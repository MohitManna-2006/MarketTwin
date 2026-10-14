#include "markettwin/live/live.hpp"

#include "markettwin/common/run_error.hpp"

#include "../historical/observed_values.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace markettwin::live {
namespace {
// nlohmann::json intentionally exposes object insertion through operator[] and compact
// schema documents are clearer without a comma after every final initializer element.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,
// readability-trailing-comma)
using Json = nlohmann::json;
namespace observed = historical::detail;

constexpr std::int64_t kDefaultStaleAfterNs = 15'000'000'000;
constexpr std::int64_t kDefaultSkewAlertNs = 10'000'000'000;
constexpr std::int64_t kDefaultMaxSymbols = 64;
constexpr std::int64_t kDefaultDuplicateWindow = 2'048;

[[noreturn]] void bad_batch(const std::string& message) {
  throw RunError{"InvalidLiveBatch", message};
}

[[noreturn]] void bad_record(const std::string& message) {
  throw RunError{"InvalidLiveRecord", message};
}

struct Config {
  std::string provider;
  std::string feed;
  std::int64_t stale_after_ns = kDefaultStaleAfterNs;
  std::int64_t skew_alert_ns = kDefaultSkewAlertNs;
  std::size_t max_symbols = static_cast<std::size_t>(kDefaultMaxSymbols);
  std::size_t duplicate_window = static_cast<std::size_t>(kDefaultDuplicateWindow);
};

std::int64_t bounded(const Json& document, const char* field, std::int64_t fallback,
                     std::int64_t low, std::int64_t high) {
  if (!document.contains(field)) {
    return fallback;
  }
  const auto& value = document.at(field);
  if (!value.is_number_integer() || value.is_number_float() ||
      (value.is_number_unsigned() &&
       value.get<std::uint64_t>() >
           static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) {
    throw RunError{"InvalidConfig", std::string{"live setting must be an integer: "} + field};
  }
  const auto number = value.get<std::int64_t>();
  if (number < low || number > high) {
    throw RunError{"InvalidConfig", std::string{"live setting is out of range: "} + field};
  }
  return number;
}

Config parse_config(const std::string& text) {
  Json document;
  try {
    document = Json::parse(text);
  } catch (const Json::exception&) {
    throw RunError{"InvalidConfig", "live normalizer configuration is not valid JSON"};
  }
  if (!document.is_object() || !document.contains("provider") ||
      !document.at("provider").is_string() || !document.contains("feed") ||
      !document.at("feed").is_string() || document.at("provider").get<std::string>().empty() ||
      document.at("feed").get<std::string>().empty()) {
    throw RunError{"InvalidConfig", "live normalizer needs provider and feed strings"};
  }
  constexpr auto kLimit = std::numeric_limits<std::int64_t>::max();
  Config config;
  config.provider = document.at("provider").get<std::string>();
  config.feed = document.at("feed").get<std::string>();
  config.stale_after_ns = bounded(document, "stale_after_ns", kDefaultStaleAfterNs, 1, kLimit);
  config.skew_alert_ns = bounded(document, "skew_alert_ns", kDefaultSkewAlertNs, 1, kLimit);
  config.max_symbols =
      static_cast<std::size_t>(bounded(document, "max_symbols", kDefaultMaxSymbols, 1, 4'096));
  config.duplicate_window = static_cast<std::size_t>(
      bounded(document, "duplicate_window", kDefaultDuplicateWindow, 0, 65'536));
  return config;
}

// A wall-clock instant crosses the boundary as decimal text, like every UTC nanosecond
// value in the observed-market schemas. A JSON integer is accepted as well.
std::optional<std::int64_t> wall_ns(const Json& value) {
  if (value.is_number_integer() && !value.is_number_float()) {
    if (value.is_number_unsigned() &&
        value.get<std::uint64_t>() >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return std::nullopt;
    }
    const auto number = value.get<std::int64_t>();
    return number < 0 ? std::nullopt : std::optional{number};
  }
  if (!value.is_string()) {
    return std::nullopt;
  }
  const auto& text = value.get_ref<const std::string&>();
  std::int64_t number = 0;
  const auto* const last = text.data() + text.size();
  const auto [end, error] = std::from_chars(text.data(), last, number);
  if (text.empty() || error != std::errc{} || end != last || number < 0) {
    return std::nullopt;
  }
  return number;
}

std::optional<std::int64_t> monotonic_ns(const Json& value) {
  if (!value.is_number_integer() || value.is_number_float() ||
      (value.is_number_unsigned() &&
       value.get<std::uint64_t>() >
           static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) {
    return std::nullopt;
  }
  return value.get<std::int64_t>();
}

struct Clock {
  std::int64_t wall = 0;
  std::int64_t monotonic = 0;
};

struct Frame {
  std::uint64_t number = 0;
  Clock receipt;
  const Json* messages = nullptr;
};

// Checks the whole batch before anything is changed, so a malformed batch is refused
// without a partial update.
std::pair<Clock, std::vector<Frame>> checked_batch(const Json& batch) {
  if (!batch.is_object() || !batch.contains("now_wall_ns") || !batch.contains("now_monotonic_ns") ||
      !batch.contains("frames") || !batch.at("frames").is_array()) {
    bad_batch("a live batch needs now_wall_ns, now_monotonic_ns, and a frames array");
  }
  const auto now_wall = wall_ns(batch.at("now_wall_ns"));
  const auto now_monotonic = monotonic_ns(batch.at("now_monotonic_ns"));
  if (!now_wall || !now_monotonic) {
    bad_batch("live batch clocks must be nonnegative integer nanoseconds");
  }
  std::vector<Frame> frames;
  frames.reserve(batch.at("frames").size());
  for (const auto& item : batch.at("frames")) {
    if (!item.is_object() || !item.contains("frame") || !item.at("frame").is_number_unsigned() ||
        !item.contains("receipt_wall_ns") || !item.contains("receipt_monotonic_ns") ||
        !item.contains("messages") || !item.at("messages").is_array()) {
      bad_batch("a live frame needs frame, receipt_wall_ns, receipt_monotonic_ns, and messages");
    }
    const auto wall = wall_ns(item.at("receipt_wall_ns"));
    const auto monotonic = monotonic_ns(item.at("receipt_monotonic_ns"));
    if (!wall || !monotonic) {
      bad_batch("live frame receipt clocks must be nonnegative integer nanoseconds");
    }
    Frame frame;
    frame.number = item.at("frame").get<std::uint64_t>();
    frame.receipt.wall = *wall;
    frame.receipt.monotonic = *monotonic;
    frame.messages = &item.at("messages");
    frames.push_back(frame);
  }
  Clock now;
  now.wall = *now_wall;
  now.monotonic = *now_monotonic;
  return {now, std::move(frames)};
}

std::string optional_text(const Json& message, const char* field) {
  if (!message.contains(field) || message.at(field).is_null()) {
    return {};
  }
  if (!message.at(field).is_string()) {
    bad_record(std::string{"field must be a string: "} + field);
  }
  return message.at(field).get<std::string>();
}

Json conditions(const Json& message) {
  if (!message.contains("c") || message.at("c").is_null()) {
    return Json::array();
  }
  const auto& value = message.at("c");
  if (!value.is_array()) {
    bad_record("conditions must be an array");
  }
  for (const auto& item : value) {
    if (!item.is_string()) {
      bad_record("conditions must be strings");
    }
  }
  return value;
}

std::int64_t price(const Json& message, const char* field, bool allow_zero) {
  // A JSON number here would already have passed through binary floating point.
  if (!message.contains(field) || !message.at(field).is_string()) {
    bad_record(std::string{"missing decimal-string price: "} + field);
  }
  return observed::parse_price_micro_usd(message.at(field).get_ref<const std::string&>(), field,
                                         allow_zero);
}

std::int64_t size(const Json& message, const char* field, std::int64_t minimum) {
  if (!message.contains(field)) {
    bad_record(std::string{"missing size: "} + field);
  }
  const auto& value = message.at(field);
  if (!value.is_number_integer() || value.is_number_float()) {
    bad_record(std::string{"size must be an integer: "} + field);
  }
  if (value.is_number_unsigned() &&
      value.get<std::uint64_t>() >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    bad_record(std::string{"size overflow: "} + field);
  }
  const auto number = value.get<std::int64_t>();
  if (number < minimum) {
    bad_record(minimum > 0 ? std::string{"trade quantity must be positive"}
                           : std::string{"quote sizes must be nonnegative integers"});
  }
  return number;
}

Json nullable(const std::optional<std::int64_t>& value) {
  return value ? Json(*value) : Json(nullptr);
}

std::string quote_state(const std::optional<std::int64_t>& bid,
                        const std::optional<std::int64_t>& ask) {
  if (bid && ask) {
    if (*bid < *ask) {
      return "two_sided";
    }
    return *bid == *ask ? "locked" : "crossed";
  }
  if (bid) {
    return "bid_only";
  }
  return ask ? "ask_only" : "empty";
}

struct Instrument {
  Json last_trade = nullptr;
  Json quote = nullptr;
  std::optional<std::int64_t> bid;
  std::optional<std::int64_t> ask;
  // Newest source timestamp applied, per kind. Older arrivals do not replace state.
  std::optional<std::int64_t> trade_time;
  std::optional<std::int64_t> quote_time;
  std::optional<Clock> trade_receipt;
  std::optional<Clock> quote_receipt;
  std::optional<Clock> update_receipt;
  std::uint64_t trades = 0;
  std::uint64_t quotes = 0;
  std::uint64_t out_of_order = 0;
  std::uint64_t duplicates = 0;
  std::deque<std::string> recent;
  std::unordered_set<std::string> seen;
};

std::int64_t age(const std::optional<Clock>& receipt, const Clock& now) {
  // Both values come from the caller's monotonic clock, so this is a local interval.
  return now.monotonic > receipt->monotonic ? now.monotonic - receipt->monotonic : 0;
}

Json state_json(const Config& config, const std::string& symbol, const Instrument& instrument,
                const Clock& now) {
  const auto both = instrument.bid && instrument.ask;
  const auto state = quote_state(instrument.bid, instrument.ask);
  const auto quote_stale =
      instrument.quote_receipt && age(instrument.quote_receipt, now) > config.stale_after_ns;
  const auto trade_stale =
      instrument.trade_receipt && age(instrument.trade_receipt, now) > config.stale_after_ns;
  std::optional<std::int64_t> skew;
  if (instrument.trade_time && instrument.quote_time) {
    skew = *instrument.trade_time > *instrument.quote_time
               ? *instrument.trade_time - *instrument.quote_time
               : *instrument.quote_time - *instrument.trade_time;
  }
  std::optional<std::int64_t> newest_source = instrument.trade_time;
  if (instrument.quote_time && (!newest_source || *instrument.quote_time > *newest_source)) {
    newest_source = instrument.quote_time;
  }
  std::string status = "awaiting";
  if (instrument.update_receipt) {
    status = age(instrument.update_receipt, now) > config.stale_after_ns ? "stale" : "fresh";
  }
  std::string spread_quality = "unavailable";
  if (both) {
    spread_quality = "current";
    if (quote_stale) {
      spread_quality = "stale_quote";
    } else if (state != "two_sided") {
      spread_quality = "locked_or_crossed";
    }
  }
  Json freshness = {
      {"status", status},
      {"trade_age_ns",
       instrument.trade_receipt ? Json(age(instrument.trade_receipt, now)) : Json(nullptr)},
      {"quote_age_ns",
       instrument.quote_receipt ? Json(age(instrument.quote_receipt, now)) : Json(nullptr)},
      {"update_age_ns",
       instrument.update_receipt ? Json(age(instrument.update_receipt, now)) : Json(nullptr)},
      {"trade_stale", static_cast<bool>(trade_stale)},
      {"quote_stale", static_cast<bool>(quote_stale)},
      {"skew_alert", skew && *skew > config.skew_alert_ns},
      {"spread_quality", spread_quality},
      // Wall clock minus source clock: includes unknown clock offset and transit time.
      {"source_age_ns", newest_source ? Json(now.wall - *newest_source) : Json(nullptr)}};
  return {
      {"schema_version", kLiveSchema},
      {"symbol", symbol},
      {"provider", config.provider},
      {"feed", config.feed},
      {"last_trade", instrument.last_trade},
      {"quote", instrument.quote},
      {"quote_state", state},
      {"spread_micro_usd", both ? Json(*instrument.ask - *instrument.bid) : Json(nullptr)},
      // Rounded down to a whole micro-USD; exact when both prices have at most five
      // decimal places. Withheld for a crossed quote.
      {"midpoint_micro_usd", both && *instrument.bid <= *instrument.ask
                                 ? Json(*instrument.bid + ((*instrument.ask - *instrument.bid) / 2))
                                 : Json(nullptr)},
      {"quote_trade_skew_ns", nullable(skew)},
      {"last_update_receipt_wall_ns", instrument.update_receipt
                                          ? Json(std::to_string(instrument.update_receipt->wall))
                                          : Json(nullptr)},
      {"counts",
       {{"trades", instrument.trades},
        {"quotes", instrument.quotes},
        {"out_of_order", instrument.out_of_order},
        {"duplicates", instrument.duplicates}}},
      {"freshness", std::move(freshness)}};
}

} // namespace

struct LiveNormalizer::Impl {
  explicit Impl(const std::string& config_json) : config(parse_config(config_json)) {}

  Config config;
  mutable std::mutex mutex;
  std::map<std::string, Instrument> instruments;
  std::map<std::string, std::uint64_t> unsupported;
  std::uint64_t sequence = 0;
  std::uint64_t batches = 0;
  std::uint64_t frames = 0;
  std::uint64_t messages = 0;
  std::uint64_t events = 0;
  std::uint64_t rejected = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t out_of_order = 0;
  std::atomic<std::int64_t> last_elapsed_ns{0};

  // Returns the instrument a valid message belongs to, creating it within capacity.
  Instrument& instrument(const std::string& symbol) {
    const auto found = instruments.find(symbol);
    if (found != instruments.end()) {
      return found->second;
    }
    if (instruments.size() >= config.max_symbols) {
      bad_record("instrument capacity exceeded");
    }
    return instruments[symbol];
  }

  bool repeated(Instrument& instrument, const Json& message) const {
    if (config.duplicate_window == 0) {
      return false;
    }
    auto fingerprint = message.dump();
    if (instrument.seen.contains(fingerprint)) {
      return true;
    }
    if (instrument.recent.size() >= config.duplicate_window) {
      instrument.seen.erase(instrument.recent.front());
      instrument.recent.pop_front();
    }
    instrument.seen.insert(fingerprint);
    instrument.recent.push_back(std::move(fingerprint));
    return false;
  }

  // Validates one trade or quote completely, then classifies and applies it. Throws
  // RunError for a record that is refused; nothing has been changed when it does.
  Json accept(const Json& message, const std::string& type, const Frame& frame) {
    if (!message.contains("S") || !message.at("S").is_string() ||
        !observed::valid_equity_symbol(message.at("S").get_ref<const std::string&>())) {
      bad_record("invalid equity symbol");
    }
    if (!message.contains("t") || !message.at("t").is_string()) {
      bad_record("record needs a UTC timestamp");
    }
    const auto symbol = message.at("S").get<std::string>();
    const auto time = observed::parse_utc_timestamp(message.at("t").get_ref<const std::string&>());
    const auto is_trade = type == "t";
    Json event = {{"schema_version", kLiveSchema},
                  {"kind", is_trade ? "trade" : "quote"},
                  {"symbol", symbol},
                  {"provider", config.provider},
                  {"feed", config.feed},
                  {"timestamp_ns", std::to_string(time.nanoseconds)},
                  {"timestamp_precision_digits", time.precision_digits},
                  {"source_timestamp", message.at("t")},
                  {"receipt_wall_ns", std::to_string(frame.receipt.wall)},
                  {"receipt_monotonic_ns", frame.receipt.monotonic},
                  {"frame", frame.number},
                  {"conditions", conditions(message)},
                  {"tape", optional_text(message, "z")}};
    std::optional<std::int64_t> bid;
    std::optional<std::int64_t> ask;
    if (is_trade) {
      if (message.contains("i") && !message.at("i").is_null() && !message.at("i").is_string() &&
          !(message.at("i").is_number_integer() && !message.at("i").is_number_float())) {
        bad_record("trade ID must be a string, integer, or null");
      }
      event["price_micro_usd"] = price(message, "p", false);
      event["source_price"] = message.at("p");
      event["quantity"] = size(message, "s", 1);
      event["trade_id"] = message.value("i", Json(nullptr));
      event["venue"] = optional_text(message, "x");
    } else {
      const auto bid_price = price(message, "bp", true);
      const auto ask_price = price(message, "ap", true);
      const auto bid_size = size(message, "bs", 0);
      const auto ask_size = size(message, "as", 0);
      if ((bid_price == 0) != (bid_size == 0) || (ask_price == 0) != (ask_size == 0)) {
        bad_record("absent quote side must have zero price and size");
      }
      if (bid_price != 0) {
        bid = bid_price;
      }
      if (ask_price != 0) {
        ask = ask_price;
      }
      // An absent side is null. Zero is how the provider spells absence, not a price.
      event["bid_micro_usd"] = nullable(bid);
      event["ask_micro_usd"] = nullable(ask);
      event["bid_size"] = bid ? Json(bid_size) : Json(nullptr);
      event["ask_size"] = ask ? Json(ask_size) : Json(nullptr);
      event["source_bid_price"] = message.at("bp");
      event["source_ask_price"] = message.at("ap");
      event["bid_venue"] = optional_text(message, "bx");
      event["ask_venue"] = optional_text(message, "ax");
      event["quote_state"] = quote_state(bid, ask);
    }

    auto& target = instrument(symbol);
    // Nothing below this line can fail: the record is accepted.
    auto& newest = is_trade ? target.trade_time : target.quote_time;
    std::string classification = "in_order";
    if (repeated(target, message)) {
      classification = "duplicate";
      ++target.duplicates;
      ++duplicates;
    } else if (newest && time.nanoseconds < *newest) {
      classification = "out_of_order";
      ++target.out_of_order;
      ++out_of_order;
    }
    const auto applied = classification == "in_order";
    event["live_sequence"] = ++sequence;
    event["classification"] = classification;
    event["applied"] = applied;
    ++events;
    ++(is_trade ? target.trades : target.quotes);
    target.update_receipt = frame.receipt;
    if (applied) {
      newest = time.nanoseconds;
      if (is_trade) {
        target.trade_receipt = frame.receipt;
        target.last_trade = event;
      } else {
        target.quote_receipt = frame.receipt;
        target.quote = event;
        target.bid = bid;
        target.ask = ask;
      }
    }
    return event;
  }

  std::string process(const std::string& batch_json) {
    Json batch;
    try {
      batch = Json::parse(batch_json);
    } catch (const Json::exception&) {
      bad_batch("live batch is not valid JSON");
    }
    const auto [now, checked] = checked_batch(batch);
    Json emitted = Json::array();
    Json rejects = Json::array();
    Json skipped = Json::object();
    std::set<std::string> touched;
    ++batches;
    for (const auto& frame : checked) {
      ++frames;
      std::size_t index = 0;
      for (const auto& message : *frame.messages) {
        ++messages;
        const auto type =
            message.is_object() && message.contains("T") && message.at("T").is_string()
                ? message.at("T").get<std::string>()
                : std::string{};
        if (!type.empty() && type != "t" && type != "q") {
          // Corrections, cancels, bars, and statuses are counted, never guessed at.
          ++unsupported[type];
          skipped[type] = skipped.value(type, std::uint64_t{0}) + 1;
        } else {
          try {
            if (type.empty()) {
              bad_record("message has no type");
            }
            auto event = accept(message, type, frame);
            touched.insert(event.at("symbol").get<std::string>());
            emitted.push_back(std::move(event));
          } catch (const RunError& error) {
            ++rejected;
            rejects.push_back({{"frame", frame.number},
                               {"index", index},
                               {"type", type},
                               {"symbol", message.is_object() && message.contains("S") &&
                                                  message.at("S").is_string()
                                              ? message.at("S")
                                              : Json(nullptr)},
                               {"code", error.code()},
                               {"reason", error.what()}});
          }
        }
        ++index;
      }
    }
    Json states = Json::object();
    for (const auto& symbol : touched) {
      states[symbol] = state_json(config, symbol, instruments.at(symbol), now);
    }
    return Json{{"schema_version", kLiveSchema},
                {"events", std::move(emitted)},
                {"rejects", std::move(rejects)},
                {"unsupported", std::move(skipped)},
                {"states", std::move(states)}}
        .dump();
  }
};

LiveNormalizer::LiveNormalizer(const std::string& config_json)
    : impl_(std::make_unique<Impl>(config_json)) {}
LiveNormalizer::~LiveNormalizer() = default;
LiveNormalizer::LiveNormalizer(LiveNormalizer&&) noexcept = default;
LiveNormalizer& LiveNormalizer::operator=(LiveNormalizer&&) noexcept = default;

std::string LiveNormalizer::process_batch(const std::string& batch_json) {
  const std::scoped_lock lock{impl_->mutex};
  const auto started = std::chrono::steady_clock::now();
  auto result = impl_->process(batch_json);
  impl_->last_elapsed_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - started)
                                   .count());
  return result;
}

std::string LiveNormalizer::snapshot(std::int64_t now_wall_ns,
                                     std::int64_t now_monotonic_ns) const {
  const std::scoped_lock lock{impl_->mutex};
  Clock now;
  now.wall = now_wall_ns;
  now.monotonic = now_monotonic_ns;
  Json states = Json::object();
  for (const auto& [symbol, instrument] : impl_->instruments) {
    states[symbol] = state_json(impl_->config, symbol, instrument, now);
  }
  Json unsupported = Json::object();
  for (const auto& [type, count] : impl_->unsupported) {
    unsupported[type] = count;
  }
  return Json{{"schema_version", kLiveSchema},
              {"provider", impl_->config.provider},
              {"feed", impl_->config.feed},
              {"stale_after_ns", impl_->config.stale_after_ns},
              {"skew_alert_ns", impl_->config.skew_alert_ns},
              {"states", std::move(states)},
              {"counters",
               {{"batches", impl_->batches},
                {"frames", impl_->frames},
                {"messages", impl_->messages},
                {"events", impl_->events},
                {"rejected", impl_->rejected},
                {"duplicates", impl_->duplicates},
                {"out_of_order", impl_->out_of_order},
                {"unsupported", std::move(unsupported)},
                {"live_sequence", impl_->sequence}}}}
      .dump();
}

void LiveNormalizer::retain_symbols(const std::string& symbols_json) {
  Json symbols;
  try {
    symbols = Json::parse(symbols_json);
  } catch (const Json::exception&) {
    bad_batch("retained symbols are not valid JSON");
  }
  if (!symbols.is_array()) {
    bad_batch("retained symbols must be an array");
  }
  std::unordered_set<std::string> keep;
  for (const auto& symbol : symbols) {
    if (!symbol.is_string()) {
      bad_batch("retained symbols must be strings");
    }
    keep.insert(symbol.get<std::string>());
  }
  const std::scoped_lock lock{impl_->mutex};
  std::erase_if(impl_->instruments, [&](const auto& item) { return !keep.contains(item.first); });
}

std::int64_t LiveNormalizer::last_batch_elapsed_ns() const noexcept {
  return impl_->last_elapsed_ns.load();
}

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,
// readability-trailing-comma)
} // namespace markettwin::live
