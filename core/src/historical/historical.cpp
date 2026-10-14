#include "markettwin/historical/historical.hpp"

#include "markettwin/accounting/money.hpp"
#include "markettwin/common/hash.hpp"
#include "markettwin/common/run_error.hpp"
#include "markettwin/common/version.hpp"

#include "observed_values.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace markettwin::historical {
namespace {
// nlohmann::json intentionally exposes object insertion through operator[] and compact
// schema documents are clearer without a comma after every final initializer element.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,
// readability-trailing-comma)
using Json = nlohmann::json;
constexpr std::size_t kMaxRecords = 500'000;
constexpr std::uintmax_t kMaxBytes = 256'000'000;

struct Event {
  Json value;
  std::int64_t timestamp = 0;
  std::uint64_t provider_sequence = std::numeric_limits<std::uint64_t>::max();
  std::size_t ordinal = 0;
};

[[noreturn]] void invalid(const std::string& message) {
  throw RunError{"InvalidHistoricalData", message};
}

std::int64_t price(const Json& record, const char* field, bool allow_zero = false) {
  if (!record.contains(field) || !record.at(field).is_string()) {
    invalid(std::string{"missing decimal-string price: "} + field);
  }
  return detail::parse_price_micro_usd(record.at(field).get<std::string>(), field, allow_zero);
}

std::pair<std::int64_t, int> timestamp(const std::string& text) {
  const auto parsed = detail::parse_utc_timestamp(text);
  return {parsed.nanoseconds, parsed.precision_digits};
}

std::string file_digest(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    throw RunError{"IoError", "cannot read historical artifact"};
  }
  Sha256 hash;
  std::array<char, 65536> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    hash.update(std::string_view{buffer.data(), static_cast<std::size_t>(input.gcount())});
  }
  return hash.digest();
}

void write_line(std::ofstream& output, const Json& record) {
  output << record.dump() << '\n';
  if (!output) {
    throw RunError{"IoError", "failed writing historical artifact"};
  }
}

Json observation(const Json& event, std::optional<std::int64_t>& bid,
                 std::optional<std::int64_t>& ask, std::optional<std::int64_t>& last,
                 std::optional<std::int64_t>& bid_size, std::optional<std::int64_t>& ask_size) {
  if (event.at("kind") == "quote") {
    bid = event.at("bid_micro_usd").get<std::int64_t>();
    ask = event.at("ask_micro_usd").get<std::int64_t>();
    bid_size = event.at("bid_size").get<std::int64_t>();
    ask_size = event.at("ask_size").get<std::int64_t>();
    if (*bid == 0 || *bid_size == 0) {
      bid.reset();
      bid_size.reset();
    }
    if (*ask == 0 || *ask_size == 0) {
      ask.reset();
      ask_size.reset();
    }
  } else {
    last = event.at("price_micro_usd").get<std::int64_t>();
  }
  std::string state = "empty";
  if (bid && ask) {
    if (*bid < *ask) {
      state = "two_sided";
    } else {
      state = *bid == *ask ? "locked" : "crossed";
    }
  } else if (bid) {
    state = "bid_only";
  } else if (ask) {
    state = "ask_only";
  }
  const auto nullable = [](const std::optional<std::int64_t>& value) -> Json {
    return value ? Json(*value) : Json(nullptr);
  };
  return {{"schema_version", kHistoricalSchema},
          {"sequence", event.at("sequence")},
          {"timestamp_ns", event.at("timestamp_ns")},
          {"last_trade_micro_usd", nullable(last)},
          {"bid_micro_usd", nullable(bid)},
          {"ask_micro_usd", nullable(ask)},
          {"bid_size", nullable(bid_size)},
          {"ask_size", nullable(ask_size)},
          {"quote_state", state}};
}

Json checked_metadata(const std::string& text) {
  Json metadata;
  try {
    metadata = Json::parse(text);
  } catch (const Json::exception&) {
    invalid("historical metadata is not valid JSON");
  }
  if (!metadata.is_object() || !metadata.contains("symbol") || !metadata.at("symbol").is_string() ||
      !metadata.contains("provider") || !metadata.at("provider").is_string() ||
      !metadata.contains("feed") || !metadata.at("feed").is_string() ||
      !metadata.contains("acquired_at") || !metadata.at("acquired_at").is_string()) {
    invalid("metadata requires symbol, provider, feed, and acquired_at strings");
  }
  if (!detail::valid_equity_symbol(metadata.at("symbol").get<std::string>())) {
    invalid("invalid historical equity symbol");
  }
  return metadata;
}

std::optional<std::uint64_t> return_volatility(const std::vector<std::int64_t>& returns_ppb) {
  if (returns_ppb.size() < 2) {
    return std::nullopt;
  }
  Int128 sum = 0;
  for (const auto value : returns_ppb) {
    sum += value;
  }
  const auto mean = divide_round(sum, static_cast<Int128>(returns_ppb.size()));
  UInt128 squared = 0;
  for (const auto value : returns_ppb) {
    const auto difference = static_cast<Int128>(value) - mean;
    const auto magnitude =
        difference < 0 ? static_cast<UInt128>(-difference) : static_cast<UInt128>(difference);
    const auto square = magnitude * magnitude;
    if (squared > ~UInt128{0} - square) {
      invalid("historical return volatility overflow");
    }
    squared += square;
  }
  const auto variance = squared / static_cast<UInt128>(returns_ppb.size() - 1);
  return static_cast<std::uint64_t>(integer_sqrt(variance));
}

} // namespace

std::string import_jsonl(
    const std::filesystem::path& input, const std::filesystem::path& output,
    const std::string& metadata_json) { // NOLINT(readability-function-cognitive-complexity)
  const Json metadata = checked_metadata(metadata_json);
  if (!std::filesystem::is_regular_file(input) || std::filesystem::file_size(input) > kMaxBytes) {
    throw RunError{"ResourceLimit", "raw input is missing or exceeds 256 MB"};
  }
  std::ifstream source{input, std::ios::binary};
  std::vector<Event> events;
  std::set<std::string> seen;
  Json quality = {{"schema_version", kHistoricalSchema},
                  {"duplicates", 0},
                  {"out_of_order", 0},
                  {"locked_quotes", 0},
                  {"crossed_quotes", 0},
                  {"gaps_over_five_minutes", 0}};
  quality["gap_markers"] = Json::array();
  std::optional<std::int64_t> previous_trade;
  std::optional<std::int64_t> previous_quote;
  std::string line;
  std::size_t source_ordinal = 0;
  while (std::getline(source, line)) {
    ++source_ordinal;
    if (events.size() >= kMaxRecords) {
      throw RunError{"ResourceLimit", "historical input exceeds 500000 records"};
    }
    if (line.empty()) {
      invalid("empty historical record");
    }
    Json raw;
    try {
      raw = Json::parse(line);
    } catch (const Json::exception&) {
      invalid("historical input contains invalid JSON");
    }
    if (!raw.is_object() || !raw.contains("kind") || !raw.at("kind").is_string() ||
        !raw.contains("t") || !raw.at("t").is_string()) {
      invalid("historical record needs kind and UTC timestamp");
    }
    const auto kind = raw.at("kind").get<std::string>();
    if (kind != "trade" && kind != "quote") {
      invalid("only historical trades and L1 quotes are supported");
    }
    if (raw.contains("symbol") && raw.at("symbol") != metadata.at("symbol")) {
      invalid("record symbol differs from requested symbol");
    }
    if (raw.contains("c") && !raw.at("c").is_array()) {
      invalid("conditions must be an array");
    }
    const auto [time, precision] = timestamp(raw.at("t").get<std::string>());
    auto& previous = kind == "trade" ? previous_trade : previous_quote;
    if (previous && time < *previous) {
      quality["out_of_order"] = quality.at("out_of_order").get<int>() + 1;
    }
    if (previous && time > *previous + 300'000'000'000LL) {
      quality["gaps_over_five_minutes"] = quality.at("gaps_over_five_minutes").get<int>() + 1;
      quality["gap_markers"].push_back({{"kind", kind},
                                        {"from_timestamp_ns", std::to_string(*previous)},
                                        {"to_timestamp_ns", std::to_string(time)},
                                        {"duration_ns", std::to_string(time - *previous)}});
    }
    previous = time;
    const auto fingerprint = raw.dump();
    if (!seen.insert(fingerprint).second) {
      quality["duplicates"] = quality.at("duplicates").get<int>() + 1;
      continue;
    }
    Json canonical = {{"schema_version", kHistoricalSchema},
                      {"kind", kind},
                      {"timestamp_ns", std::to_string(time)},
                      {"timestamp_precision_digits", precision},
                      {"source_timestamp", raw.at("t")},
                      {"source_ordinal", source_ordinal},
                      {"symbol", metadata.at("symbol")},
                      {"venue", raw.value("x", std::string{})},
                      {"conditions", raw.value("c", Json::array())}};
    std::uint64_t provider_sequence = std::numeric_limits<std::uint64_t>::max();
    if (raw.contains("sequence")) {
      if (!raw.at("sequence").is_number_unsigned()) {
        invalid("provider sequence must be a nonnegative integer");
      }
      provider_sequence = raw.at("sequence").get<std::uint64_t>();
      canonical["provider_sequence"] = provider_sequence;
    } else {
      canonical["provider_sequence"] = nullptr;
    }
    if (kind == "trade") {
      if (!raw.contains("s") || !raw.at("s").is_number_integer() ||
          raw.at("s").get<std::int64_t>() <= 0) {
        invalid("trade quantity must be positive");
      }
      if (raw.contains("i") && !raw.at("i").is_null() && !raw.at("i").is_string() &&
          !raw.at("i").is_number_integer()) {
        invalid("trade ID must be a string, integer, or null");
      }
      canonical["price_micro_usd"] = price(raw, "p");
      canonical["source_price"] = raw.at("p");
      canonical["quantity"] = raw.at("s");
      canonical["trade_id"] = raw.value("i", Json(nullptr));
    } else {
      const auto bid = price(raw, "bp", true);
      const auto ask = price(raw, "ap", true);
      if (!raw.contains("bs") || !raw.contains("as") || !raw.at("bs").is_number_integer() ||
          !raw.at("as").is_number_integer() || raw.at("bs").get<std::int64_t>() < 0 ||
          raw.at("as").get<std::int64_t>() < 0) {
        invalid("quote sizes must be nonnegative integers");
      }
      const auto bid_size = raw.at("bs").get<std::int64_t>();
      const auto ask_size = raw.at("as").get<std::int64_t>();
      if ((bid == 0) != (bid_size == 0) || (ask == 0) != (ask_size == 0)) {
        invalid("absent quote side must have zero price and size");
      }
      if (bid != 0 && ask != 0 && bid_size != 0 && ask_size != 0) {
        if (bid == ask) {
          quality["locked_quotes"] = quality.at("locked_quotes").get<int>() + 1;
        } else if (bid > ask) {
          quality["crossed_quotes"] = quality.at("crossed_quotes").get<int>() + 1;
        }
      }
      canonical["bid_micro_usd"] = bid;
      canonical["ask_micro_usd"] = ask;
      canonical["source_bid_price"] = raw.at("bp");
      canonical["source_ask_price"] = raw.at("ap");
      canonical["bid_size"] = bid_size;
      canonical["ask_size"] = ask_size;
      canonical["bid_venue"] = raw.value("bx", std::string{});
      canonical["ask_venue"] = raw.value("ax", std::string{});
    }
    events.push_back(Event{.value = std::move(canonical),
                           .timestamp = time,
                           .provider_sequence = provider_sequence,
                           .ordinal = source_ordinal});
  }
  if (!source.eof()) {
    throw RunError{"IoError", "failed reading raw historical input"};
  }
  std::ranges::sort(events, [](const Event& left, const Event& right) {
    return std::tie(left.timestamp, left.provider_sequence, left.ordinal) <
           std::tie(right.timestamp, right.provider_sequence, right.ordinal);
  });
  if (!output.parent_path().empty()) {
    std::filesystem::create_directories(output.parent_path());
  }
  if (std::filesystem::exists(output)) {
    throw RunError{"OutputExists", "historical session directory already exists"};
  }
  auto staging = output;
  staging += ".partial";
  if (!std::filesystem::create_directory(staging)) {
    throw RunError{"OutputExists", "historical staging directory already exists"};
  }
  try {
    std::filesystem::copy_file(input, staging / "raw.jsonl");
    std::ofstream canonical{staging / "historical_events.jsonl", std::ios::binary};
    std::ofstream samples{staging / "historical_samples.jsonl", std::ios::binary};
    std::optional<std::int64_t> bid;
    std::optional<std::int64_t> ask;
    std::optional<std::int64_t> last;
    std::optional<std::int64_t> bid_size;
    std::optional<std::int64_t> ask_size;
    std::uint64_t trades = 0;
    std::uint64_t quotes = 0;
    std::uint64_t volume = 0;
    UInt128 traded_price_quantity = 0;
    Int128 spread_sum = 0;
    std::uint64_t spread_observations = 0;
    std::optional<std::int64_t> high_trade;
    std::optional<std::int64_t> low_trade;
    std::optional<std::int64_t> previous_timestamp;
    std::optional<std::int64_t> previous_trade_price;
    std::vector<std::int64_t> returns_ppb;
    std::string quote_state = "empty";
    std::array<std::pair<std::string_view, std::uint64_t>, 6> state_time{{
        {"empty", 0},
        {"bid_only", 0},
        {"ask_only", 0},
        {"two_sided", 0},
        {"locked", 0},
        {"crossed", 0},
    }};
    std::size_t index = 0;
    for (auto& item : events) {
      auto& event = item.value;
      if (previous_timestamp) {
        const auto elapsed = item.timestamp - *previous_timestamp;
        auto* const state = std::ranges::find_if(
            state_time, [&](const auto& entry) { return entry.first == quote_state; });
        if (state != state_time.end()) {
          state->second += static_cast<std::uint64_t>(elapsed);
        }
      }
      ++index;
      event["sequence"] = index;
      write_line(canonical, event);
      const auto sample = observation(event, bid, ask, last, bid_size, ask_size);
      write_line(samples, sample);
      quote_state = sample.at("quote_state").get<std::string>();
      previous_timestamp = item.timestamp;
      if (event.at("kind") == "trade") {
        ++trades;
        const auto quantity = event.at("quantity").get<std::uint64_t>();
        if (volume > std::numeric_limits<std::uint64_t>::max() - quantity) {
          invalid("historical volume overflow");
        }
        volume += quantity;
        const auto trade_price = event.at("price_micro_usd").get<std::int64_t>();
        if (previous_trade_price) {
          returns_ppb.push_back(narrow(divide_round(
              (static_cast<Int128>(trade_price) - static_cast<Int128>(*previous_trade_price)) *
                  1'000'000'000,
              static_cast<Int128>(*previous_trade_price))));
        }
        previous_trade_price = trade_price;
        traded_price_quantity += static_cast<UInt128>(trade_price) * static_cast<UInt128>(quantity);
        high_trade = high_trade ? std::max(*high_trade, trade_price) : trade_price;
        low_trade = low_trade ? std::min(*low_trade, trade_price) : trade_price;
      } else {
        ++quotes;
        const auto quote_bid = event.at("bid_micro_usd").get<std::int64_t>();
        const auto quote_ask = event.at("ask_micro_usd").get<std::int64_t>();
        if (quote_bid != 0 && quote_ask != 0) {
          spread_sum += static_cast<Int128>(quote_ask) - static_cast<Int128>(quote_bid);
          ++spread_observations;
        }
      }
    }
    canonical.close();
    samples.close();
    if (!canonical || !samples) {
      throw RunError{"IoError", "failed closing historical artifacts"};
    }
    quality["records_imported"] = events.size();
    quality["trade_records"] = trades;
    quality["quote_records"] = quotes;
    quality["traded_quantity"] = volume;
    const bool has_warnings = quality.at("duplicates").get<int>() != 0 ||
                              quality.at("out_of_order").get<int>() != 0 ||
                              quality.at("locked_quotes").get<int>() != 0 ||
                              quality.at("crossed_quotes").get<int>() != 0 ||
                              quality.at("gaps_over_five_minutes").get<int>() != 0;
    quality["status"] = has_warnings ? "warnings" : "ok";
    std::ofstream quality_file{staging / "quality_report.json", std::ios::binary};
    write_line(quality_file, quality);
    quality_file.close();
    if (!quality_file) {
      throw RunError{"IoError", "failed closing historical quality artifact"};
    }
    const auto nullable = [](const std::optional<std::int64_t>& value) -> Json {
      return value ? Json(*value) : Json(nullptr);
    };
    Json state_durations = Json::object();
    for (const auto& [state, duration] : state_time) {
      state_durations[std::string{state} + "_ns"] = duration;
    }
    const auto coverage_ns =
        events.size() < 2
            ? std::uint64_t{0}
            : static_cast<std::uint64_t>(events.back().timestamp - events.front().timestamp);
    const auto rate_millihertz = [coverage_ns](std::uint64_t count) -> Json {
      if (coverage_ns == 0) {
        return nullptr;
      }
      return static_cast<std::uint64_t>((static_cast<UInt128>(count) * 1'000'000'000'000ULL) /
                                        coverage_ns);
    };
    const auto volatility_ppb = return_volatility(returns_ppb);
    const Json metrics = {
        {"schema_version", kHistoricalSchema},
        {"units",
         {{"price", "micro_usd"},
          {"quantity", "reported_shares_or_lots"},
          {"duration", "utc_nanoseconds"}}},
        {"scope", metadata.value("scope", std::string{"provider_feed_unspecified"})},
        {"trade_count", trades},
        {"traded_quantity", volume},
        {"last_trade_micro_usd", nullable(last)},
        {"high_trade_micro_usd", nullable(high_trade)},
        {"low_trade_micro_usd", nullable(low_trade)},
        {"vwap_micro_usd",
         volume == 0 ? Json(nullptr)
                     : Json(static_cast<std::uint64_t>(
                           (traded_price_quantity + static_cast<UInt128>(volume / 2)) / volume))},
        {"quote_update_count", quotes},
        {"trade_arrival_rate_millihertz", rate_millihertz(trades)},
        {"quote_update_rate_millihertz", rate_millihertz(quotes)},
        {"spread_observations", spread_observations},
        {"mean_quoted_spread_micro_usd",
         spread_observations == 0
             ? Json(nullptr)
             : Json(narrow(divide_round(spread_sum, static_cast<Int128>(spread_observations))))},
        {"coverage_ns", coverage_ns},
        {"trade_to_trade_return_observations", returns_ppb.size()},
        {"trade_to_trade_return_volatility_ppb",
         volatility_ppb ? Json(*volatility_ppb) : Json(nullptr)},
        {"quote_state_time", state_durations},
        {"methodology",
         {{"ordering",
           "event timestamp, provider sequence when present, then stable source ordinal"},
          {"quote_state_time",
           "last observation carried forward between canonical events; no trailing duration"},
          {"vwap", "reported trade price weighted by reported trade quantity"},
          {"missing", "unavailable values are null, never zero"},
          {"return_volatility",
           "sample standard deviation of consecutive reported-trade simple returns in parts per "
           "billion; unavailable with fewer than three trades"},
          {"arrival_rates", "record count divided by canonical coverage, in millihertz"}}}};
    std::ofstream metrics_file{staging / "historical_metrics.json", std::ios::binary};
    write_line(metrics_file, metrics);
    metrics_file.close();
    if (!metrics_file) {
      throw RunError{"IoError", "failed closing historical metrics artifact"};
    }
    const Json manifest = {
        {"schema_version", kHistoricalSchema},
        {"normalization_version", kHistoricalNormalizationVersion},
        {"engine_version", version()},
        {"git_revision", MARKETTWIN_GIT_REVISION},
        {"compiler", MARKETTWIN_COMPILER},
        {"build_type", MARKETTWIN_BUILD_TYPE},
        {"kind", "observed_historical_market"},
        {"metadata", metadata},
        {"record_count", events.size()},
        {"first_timestamp_ns",
         events.empty() ? Json(nullptr) : Json(std::to_string(events.front().timestamp))},
        {"last_timestamp_ns",
         events.empty() ? Json(nullptr) : Json(std::to_string(events.back().timestamp))},
        {"raw_sha256", file_digest(staging / "raw.jsonl")},
        {"canonical_sha256", file_digest(staging / "historical_events.jsonl")},
        {"samples_sha256", file_digest(staging / "historical_samples.jsonl")},
        {"quality_sha256", file_digest(staging / "quality_report.json")},
        {"metrics_sha256", file_digest(staging / "historical_metrics.json")}};
    std::ofstream manifest_file{staging / "manifest.json", std::ios::binary};
    write_line(manifest_file, manifest);
    manifest_file.close();
    if (!manifest_file) {
      throw RunError{"IoError", "failed closing historical manifest"};
    }
    std::filesystem::rename(staging, output);
    return manifest.dump();
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove_all(staging, ignored);
    throw;
  }
}

std::string verify(const std::filesystem::path& directory) {
  const auto manifest_path = directory / "manifest.json";
  if (!std::filesystem::is_regular_file(manifest_path)) {
    throw RunError{"IncompleteArtifacts", "historical manifest is absent"};
  }
  Json manifest;
  std::ifstream{manifest_path} >> manifest;
  if (manifest.value("schema_version", 0U) != kHistoricalSchema ||
      manifest.value("normalization_version", 0U) != kHistoricalNormalizationVersion) {
    throw RunError{"UnsupportedSchema", "historical schema is not supported"};
  }
  if (manifest.value("kind", std::string{}) != "observed_historical_market" ||
      !manifest.contains("metadata") || !manifest.at("metadata").is_object()) {
    throw RunError{"InvalidHistoricalManifest", "historical manifest identity is invalid"};
  }
  static_cast<void>(checked_metadata(manifest.at("metadata").dump()));
  if (manifest.value("engine_version", std::string{}) != version()) {
    throw RunError{"UnsupportedVersion",
                   "historical session was produced by another engine version"};
  }
  for (const auto& [name, field] : {std::pair{"raw.jsonl", "raw_sha256"},
                                    {"historical_events.jsonl", "canonical_sha256"},
                                    {"historical_samples.jsonl", "samples_sha256"},
                                    {"quality_report.json", "quality_sha256"},
                                    {"historical_metrics.json", "metrics_sha256"}}) {
    if (file_digest(directory / name) != manifest.at(field).get<std::string>()) {
      return Json{{"verified", false}, {"code", "DigestMismatch"}, {"artifact", name}}.dump();
    }
  }
  std::ifstream events{directory / "historical_events.jsonl"};
  std::ifstream samples{directory / "historical_samples.jsonl"};
  std::optional<std::int64_t> bid;
  std::optional<std::int64_t> ask;
  std::optional<std::int64_t> last;
  std::optional<std::int64_t> bid_size;
  std::optional<std::int64_t> ask_size;
  std::string event_line;
  std::string sample_line;
  std::size_t count = 0;
  while (std::getline(events, event_line)) {
    ++count;
    if (!std::getline(samples, sample_line)) {
      return Json{{"verified", false}, {"code", "MissingSample"}, {"sequence", count}}.dump();
    }
    const Json event = Json::parse(event_line);
    if (event.at("sequence") != count ||
        observation(event, bid, ask, last, bid_size, ask_size).dump() != sample_line) {
      return Json{{"verified", false}, {"code", "PlaybackMismatch"}, {"sequence", count}}.dump();
    }
  }
  if (std::getline(samples, sample_line) || count != manifest.at("record_count")) {
    return Json{{"verified", false}, {"code", "RecordCountMismatch"}}.dump();
  }
  return Json{{"verified", true},
              {"record_count", count},
              {"canonical_sha256", manifest.at("canonical_sha256")}}
      .dump();
}

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,
// readability-trailing-comma)
} // namespace markettwin::historical
