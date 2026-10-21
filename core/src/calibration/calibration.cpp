#include "markettwin/calibration/calibration.hpp"

#include "markettwin/accounting/money.hpp"
#include "markettwin/common/run_error.hpp"
#include "markettwin/simulation/kernel.hpp"
#include "markettwin/telemetry/replay.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <utility>

namespace markettwin::calibration {
namespace {
// nlohmann::json documents are built with operator[] and compact initializer lists.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,
// readability-trailing-comma)
using Json = nlohmann::json;

constexpr std::int64_t kMicroUsdPerCent = 10'000;
constexpr std::int64_t kMinuteNs = 60'000'000'000;
constexpr std::int64_t kPpm = 1'000'000;
constexpr std::uint64_t kMinTrades = 30;
constexpr std::int64_t kMinTwoSidedPpm = 500'000;
constexpr std::int64_t kScaleFloorPpm = 50'000; // 5% of |mean|

[[noreturn]] void invalid(const std::string& message) {
  throw RunError{"InvalidCalibrationInput", message};
}

std::size_t index(Fact fact) noexcept {
  return static_cast<std::size_t>(fact);
}

std::int64_t to_int64(Int128 value) {
  return narrow(value);
}

// numerator * 1e6 / denominator, halving both while the product would overflow. The
// halving is deterministic and costs far less than a millionth of the ratio.
std::int64_t ratio_ppm(Int128 numerator, Int128 denominator) {
  constexpr Int128 kLimit = std::numeric_limits<Int128>::max() / kPpm;
  while (numerator > kLimit || numerator < -kLimit) {
    numerator /= 2;
    denominator /= 2;
  }
  if (denominator <= 0) {
    invalid("calibration ratio has no positive denominator");
  }
  return to_int64(divide_round(numerator * kPpm, denominator));
}

// The smallest value whose cumulative weight reaches quantile_ppm of the total.
std::optional<std::int64_t>
weighted_quantile(std::vector<std::pair<std::int64_t, std::uint64_t>>& values, std::uint64_t total,
                  std::uint64_t quantile_ppm) {
  if (total == 0) {
    return std::nullopt;
  }
  std::ranges::sort(values);
  UInt128 cumulative = 0;
  const auto needed = static_cast<UInt128>(total) * quantile_ppm;
  for (const auto& [value, weight] : values) {
    cumulative += weight;
    if (cumulative * static_cast<UInt128>(kPpm) >= needed) {
      return value;
    }
  }
  return values.back().first;
}

// Sample standard deviation (n - 1) around the mean rounded to an integer.
std::optional<std::int64_t> deviation(std::span<const std::int64_t> values) {
  if (values.size() < 2) {
    return std::nullopt;
  }
  Int128 sum = 0;
  for (const auto value : values) {
    sum += value;
  }
  const auto mean = divide_round(sum, static_cast<Int128>(values.size()));
  UInt128 squares = 0;
  for (const auto value : values) {
    const auto difference = static_cast<Int128>(value) - mean;
    const auto magnitude =
        difference < 0 ? static_cast<UInt128>(-difference) : static_cast<UInt128>(difference);
    const auto square = magnitude * magnitude;
    if (squares > ~UInt128{0} - square) {
      invalid("calibration deviation overflow");
    }
    squares += square;
  }
  return to_int64(static_cast<Int128>(integer_sqrt(squares / (values.size() - 1))));
}

std::int64_t mean(std::span<const std::int64_t> values) {
  Int128 sum = 0;
  for (const auto value : values) {
    sum += value;
  }
  return to_int64(divide_round(sum, static_cast<Int128>(values.size())));
}

std::int64_t per_minute_milli(std::uint64_t count, std::uint64_t window_ns) {
  return to_int64(
      divide_round(static_cast<Int128>(count) * kMinuteNs * 1000, static_cast<Int128>(window_ns)));
}

std::optional<std::int64_t> share_ppm(std::uint64_t part, std::uint64_t whole) {
  if (whole == 0) {
    return std::nullopt;
  }
  return to_int64(divide_round(static_cast<Int128>(part) * kPpm, static_cast<Int128>(whole)));
}

std::optional<std::int64_t> interarrival_cv(std::span<const std::int64_t> times) {
  if (times.size() < 3) {
    return std::nullopt;
  }
  std::vector<std::int64_t> gaps;
  gaps.reserve(times.size() - 1);
  for (std::size_t i = 1; i < times.size(); ++i) {
    gaps.push_back(times[i] - times[i - 1]);
  }
  const auto average = mean(gaps);
  const auto spread = deviation(gaps);
  if (average <= 0 || !spread) {
    return std::nullopt;
  }
  return ratio_ppm(*spread, average);
}

// Lag-1 autocorrelation of consecutive trade-price changes, computed on n * c_i - sum so
// the mean is exact.
std::optional<std::int64_t> lag1_autocorrelation(std::span<const std::int64_t> prices) {
  if (prices.size() < 4) {
    return std::nullopt;
  }
  std::vector<Int128> changes;
  changes.reserve(prices.size() - 1);
  Int128 sum = 0;
  for (std::size_t i = 1; i < prices.size(); ++i) {
    changes.push_back(static_cast<Int128>(prices[i]) - prices[i - 1]);
    sum += changes.back();
  }
  const auto count = static_cast<Int128>(changes.size());
  constexpr Int128 kBound = Int128{1} << 62U;
  for (auto& change : changes) {
    change = change * count - sum;
    if (change > kBound || change < -kBound) {
      invalid("trade price changes are out of range");
    }
  }
  Int128 numerator = 0;
  Int128 denominator = 0;
  for (std::size_t i = 0; i < changes.size(); ++i) {
    denominator += changes[i] * changes[i];
    if (i + 1 < changes.size()) {
      numerator += changes[i] * changes[i + 1];
    }
  }
  if (denominator == 0) {
    return std::nullopt;
  }
  return ratio_ppm(numerator, denominator);
}

std::optional<std::int64_t> median(std::vector<std::int64_t> values) {
  if (values.empty()) {
    return std::nullopt;
  }
  std::ranges::sort(values);
  const auto middle = values.size() / 2;
  if (values.size() % 2 == 1) {
    return values.at(middle);
  }
  return to_int64(divide_round(static_cast<Int128>(values.at(middle - 1)) + values.at(middle), 2));
}

Json nullable(const std::optional<std::int64_t>& value) {
  return value ? Json(*value) : Json(nullptr);
}

std::optional<std::int64_t> optional_int(const Json& value) {
  if (value.is_null()) {
    return std::nullopt;
  }
  if (!value.is_number_integer()) {
    invalid("calibration values must be integers or null");
  }
  return value.get<std::int64_t>();
}

Json values_json(const FactValues& values) {
  Json object = Json::object();
  for (std::size_t i = 0; i < kFactCount; ++i) {
    object[std::string{fact_table().at(i).name}] = nullable(values.at(i));
  }
  return object;
}

FactValues parse_values(const Json& object) {
  if (!object.is_object()) {
    invalid("calibration facts must be an object");
  }
  FactValues values;
  for (std::size_t i = 0; i < kFactCount; ++i) {
    const std::string name{fact_table().at(i).name};
    if (!object.contains(name)) {
      invalid("calibration facts are missing " + name);
    }
    values.at(i) = optional_int(object.at(name));
  }
  return values;
}

Json parse(std::string_view text) {
  try {
    return Json::parse(text);
  } catch (const Json::exception&) {
    invalid("calibration input is not valid JSON");
  }
}

std::vector<Facts> parse_facts_array(std::string_view text) {
  const auto array = parse(text);
  if (!array.is_array()) {
    invalid("expected a JSON array of facts documents");
  }
  std::vector<Facts> facts;
  facts.reserve(array.size());
  for (const auto& item : array) {
    facts.push_back(parse_facts(item.dump()));
  }
  return facts;
}

// Ticks are cents times tick_size_minor; the projection is in micro-dollars.
class Projection final : public MarketObserver {
public:
  Projection(std::int64_t end_ns, std::uint64_t tick_size_minor)
      : builder_(0, end_ns),
        micro_per_tick_(static_cast<std::int64_t>(tick_size_minor) * kMicroUsdPerCent),
        end_(end_ns) {}

  void on_market(SimTime now, std::span<const Trade> trades, const TopOfBook& top) override {
    const auto time = static_cast<std::int64_t>(now.value());
    if (time >= end_) {
      return;
    }
    for (const auto& trade : trades) {
      builder_.trade(time, price(trade.price_ticks),
                     static_cast<std::int64_t>(trade.quantity.value()));
    }
    builder_.quote(time, top.bid ? price(top.bid->price) : 0,
                   top.bid ? static_cast<std::int64_t>(top.bid->quantity.value()) : 0,
                   top.ask ? price(top.ask->price) : 0,
                   top.ask ? static_cast<std::int64_t>(top.ask->quantity.value()) : 0);
  }

  [[nodiscard]] MarketTape finish() && { return std::move(builder_).finish(); }

private:
  [[nodiscard]] std::int64_t price(PriceTicks ticks) const {
    return to_int64(static_cast<Int128>(ticks.value()) * micro_per_tick_);
  }

  TapeBuilder builder_;
  std::int64_t micro_per_tick_;
  std::int64_t end_;
};

} // namespace

TapeBuilder::TapeBuilder(std::int64_t start_ns, std::int64_t end_ns) {
  if (start_ns >= end_ns) {
    invalid("calibration window must have start before end");
  }
  tape_.start_ns = start_ns;
  tape_.end_ns = end_ns;
}

void TapeBuilder::check_time(std::int64_t time_ns) const {
  if (time_ns < tape_.start_ns || time_ns >= tape_.end_ns) {
    invalid("calibration record lies outside its window");
  }
  if (!tape_.records.empty() && time_ns < tape_.records.back().time_ns) {
    invalid("calibration records are not in time order");
  }
}

void TapeBuilder::quote(std::int64_t time_ns, std::int64_t bid, std::int64_t bid_size,
                        std::int64_t ask, std::int64_t ask_size) {
  check_time(time_ns);
  if (bid < 0 || ask < 0 || bid_size < 0 || ask_size < 0) {
    invalid("calibration quotes must be nonnegative");
  }
  // A side with no price or no size is absent.
  if (bid == 0 || bid_size == 0) {
    bid = 0;
    bid_size = 0;
  }
  if (ask == 0 || ask_size == 0) {
    ask = 0;
    ask_size = 0;
  }
  const TapeRecord record{.kind = TapeRecord::Kind::Quote,
                          .time_ns = time_ns,
                          .bid = bid,
                          .bid_size = bid_size,
                          .ask = ask,
                          .ask_size = ask_size};
  if (last_quote_ && last_quote_->bid == bid && last_quote_->bid_size == bid_size &&
      last_quote_->ask == ask && last_quote_->ask_size == ask_size) {
    return;
  }
  last_quote_ = record;
  tape_.records.push_back(record);
}

void TapeBuilder::trade(std::int64_t time_ns, std::int64_t price, std::int64_t quantity) {
  check_time(time_ns);
  if (price <= 0 || quantity <= 0) {
    invalid("calibration trades need a positive price and quantity");
  }
  tape_.records.push_back(TapeRecord{
      .kind = TapeRecord::Kind::Trade, .time_ns = time_ns, .price = price, .quantity = quantity});
}

MarketTape TapeBuilder::finish() && {
  return std::move(tape_);
}

const std::array<FactInfo, kFactCount>& fact_table() noexcept {
  static const std::array<FactInfo, kFactCount> table{{
      {.name = "spread_cents_p10", .unit = "1/1000 cent", .group = "spread", .floor = 100},
      {.name = "spread_cents_p50", .unit = "1/1000 cent", .group = "spread", .floor = 100},
      {.name = "spread_cents_p90", .unit = "1/1000 cent", .group = "spread", .floor = 100},
      {.name = "relative_spread_bps_p50", .unit = "1/1000 bp", .group = "spread", .floor = 100},
      {.name = "trades_per_minute",
       .unit = "1/1000 trade per minute",
       .group = "arrival",
       .floor = 1000},
      {.name = "interarrival_cv", .unit = "ppm", .group = "arrival", .floor = 10'000},
      {.name = "empty_minute_share", .unit = "ppm", .group = "arrival", .floor = 10'000},
      {.name = "trade_return_lag1_autocorr", .unit = "ppm", .group = "returns", .floor = 10'000},
      {.name = "midpoint_vol_1m_bps", .unit = "1/1000 bp", .group = "returns", .floor = 100},
      {.name = "two_sided_share", .unit = "ppm", .group = "quote_state", .floor = 1000},
      {.name = "l1_updates_per_minute",
       .unit = "1/1000 update per minute",
       .group = "quote_state",
       .floor = 1000},
      {.name = "l1_median_duration_ms", .unit = "1/1000 ms", .group = "quote_state", .floor = 1000},
  }};
  return table;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one pass over the tape.
Facts compute_facts(const MarketTape& tape) {
  if (tape.start_ns >= tape.end_ns) {
    invalid("calibration window must have start before end");
  }
  Facts facts;
  facts.window_ns = static_cast<std::uint64_t>(tape.end_ns - tape.start_ns);
  const std::size_t minutes = facts.window_ns / static_cast<std::uint64_t>(kMinuteNs);

  std::vector<std::int64_t> trade_times;
  std::vector<std::int64_t> trade_prices;
  std::vector<bool> minute_traded(minutes, false);
  std::vector<std::int64_t> quote_gaps;
  std::vector<std::pair<std::int64_t, std::uint64_t>> spreads;         // micro-dollars, ns
  std::vector<std::pair<std::int64_t, std::uint64_t>> relatives;       // millibps, ns
  std::vector<std::optional<std::int64_t>> boundary_mid2(minutes + 1); // bid + ask

  std::optional<TapeRecord> state;
  std::optional<std::int64_t> first_quote;
  std::size_t next_boundary = 0;
  const auto boundary_time = [&](std::size_t k) {
    return tape.start_ns + static_cast<std::int64_t>(k) * kMinuteNs;
  };
  const auto two_sided = [](const TapeRecord& quote) {
    return quote.bid != 0 && quote.ask != 0 && quote.bid < quote.ask;
  };
  // State at each minute boundary is the last L1 record at or before it.
  const auto sample_until = [&](std::int64_t time, bool inclusive) {
    while (next_boundary <= minutes && (boundary_time(next_boundary) < time ||
                                        (inclusive && boundary_time(next_boundary) == time))) {
      if (state && two_sided(*state)) {
        boundary_mid2.at(next_boundary) = state->bid + state->ask;
      }
      ++next_boundary;
    }
  };
  const auto elapse = [&](std::int64_t until) {
    if (!state || until <= state->time_ns) {
      return;
    }
    const auto duration = static_cast<std::uint64_t>(until - state->time_ns);
    if (state->bid == 0 || state->ask == 0) {
      return;
    }
    if (state->bid < state->ask) {
      facts.two_sided_ns += duration;
      const auto spread = state->ask - state->bid;
      spreads.emplace_back(spread, duration);
      relatives.emplace_back(to_int64(divide_round(static_cast<Int128>(spread) * 20'000'000,
                                                   static_cast<Int128>(state->bid) + state->ask)),
                             duration);
    } else if (state->bid == state->ask) {
      facts.locked_ns += duration;
    } else {
      facts.crossed_ns += duration;
    }
  };

  for (const auto& record : tape.records) {
    if (record.time_ns < tape.start_ns || record.time_ns >= tape.end_ns) {
      invalid("calibration record lies outside its window");
    }
    sample_until(record.time_ns, false);
    if (record.kind == TapeRecord::Kind::Trade) {
      ++facts.trades;
      trade_times.push_back(record.time_ns);
      trade_prices.push_back(record.price);
      const auto minute = static_cast<std::size_t>((record.time_ns - tape.start_ns) / kMinuteNs);
      if (minute < minutes) {
        minute_traded.at(minute) = true;
      }
      continue;
    }
    ++facts.quotes;
    elapse(record.time_ns);
    if (state) {
      quote_gaps.push_back(record.time_ns - state->time_ns);
    } else {
      first_quote = record.time_ns;
    }
    state = record;
    if (!facts.opening_midpoint && two_sided(record)) {
      facts.opening_midpoint =
          to_int64(divide_round(static_cast<Int128>(record.bid) + record.ask, 2));
    }
    // A record exactly on a boundary applies from that instant.
    sample_until(record.time_ns, true);
  }
  elapse(tape.end_ns);
  sample_until(tape.end_ns, true);
  if (first_quote) {
    facts.measured_ns = static_cast<std::uint64_t>(tape.end_ns - *first_quote);
  }

  auto& scored = facts.scored;
  const auto two_sided_total = facts.two_sided_ns;
  const auto spread_cents = [&](std::uint64_t quantile) -> std::optional<std::int64_t> {
    const auto micro = weighted_quantile(spreads, two_sided_total, quantile);
    if (!micro) {
      return std::nullopt;
    }
    return to_int64(divide_round(*micro, kMicroUsdPerCent / 1000));
  };
  scored.at(index(Fact::SpreadCentsP10)) = spread_cents(100'000);
  scored.at(index(Fact::SpreadCentsP50)) = spread_cents(500'000);
  scored.at(index(Fact::SpreadCentsP90)) = spread_cents(900'000);
  scored.at(index(Fact::RelativeSpreadBpsP50)) =
      weighted_quantile(relatives, two_sided_total, 500'000);

  scored.at(index(Fact::TradesPerMinute)) = per_minute_milli(facts.trades, facts.window_ns);
  scored.at(index(Fact::InterarrivalCv)) = interarrival_cv(trade_times);
  if (minutes > 0) {
    const auto empty = static_cast<std::uint64_t>(std::ranges::count(minute_traded, false));
    scored.at(index(Fact::EmptyMinuteShare)) = share_ppm(empty, minutes);
  }

  scored.at(index(Fact::TradeReturnLag1Autocorr)) = lag1_autocorrelation(trade_prices);
  std::vector<std::int64_t> returns_ppb;
  for (std::size_t k = 0; k + 1 < boundary_mid2.size(); ++k) {
    const auto& from = boundary_mid2.at(k);
    const auto& to = boundary_mid2.at(k + 1);
    if (from && to) {
      returns_ppb.push_back(
          to_int64(divide_round((static_cast<Int128>(*to) - *from) * 1'000'000'000, *from)));
    }
  }
  if (const auto volatility = deviation(returns_ppb)) {
    scored.at(index(Fact::MidpointVol1mBps)) = to_int64(divide_round(*volatility, 100));
  }

  scored.at(index(Fact::TwoSidedShare)) = share_ppm(facts.two_sided_ns, facts.measured_ns);
  scored.at(index(Fact::L1UpdatesPerMinute)) = per_minute_milli(facts.quotes, facts.window_ns);
  if (const auto gap = median(quote_gaps)) {
    scored.at(index(Fact::L1MedianDurationMs)) = to_int64(divide_round(*gap, 1000));
  }
  return facts;
}

MarketTape synthetic_tape(const RunConfig& config, RunSummary* summary) {
  const auto end = config.limits.horizon.value();
  if (end == 0 || end > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    invalid("synthetic calibration needs a positive horizon");
  }
  Projection projection{static_cast<std::int64_t>(end), config.tick_size_minor};
  NullSink sink;
  auto result = run_kernel(config, sink, &projection);
  if (summary != nullptr) {
    *summary = std::move(result);
  }
  return std::move(projection).finish();
}

MarketTape observed_tape(const std::filesystem::path& session, std::int64_t start_ns,
                         std::int64_t end_ns) {
  std::ifstream input{session / "historical_events.jsonl", std::ios::binary};
  if (!input) {
    throw RunError{"IoError", "cannot read historical events"};
  }
  TapeBuilder builder{start_ns, end_ns};
  std::string line;
  while (std::getline(input, line)) {
    const auto event = parse(line);
    const auto time = std::stoll(event.at("timestamp_ns").get<std::string>());
    if (time < start_ns || time >= end_ns) {
      continue;
    }
    if (event.at("kind") == "trade") {
      builder.trade(time, event.at("price_micro_usd").get<std::int64_t>(),
                    event.at("quantity").get<std::int64_t>());
    } else {
      builder.quote(time, event.at("bid_micro_usd").get<std::int64_t>(),
                    event.at("bid_size").get<std::int64_t>(),
                    event.at("ask_micro_usd").get<std::int64_t>(),
                    event.at("ask_size").get<std::int64_t>());
    }
  }
  if (!input.eof()) {
    throw RunError{"IoError", "failed reading historical events"};
  }
  return std::move(builder).finish();
}

Targets targets(std::span<const Facts> sessions) {
  Targets result;
  result.sessions = sessions.size();
  std::vector<std::int64_t> openings;
  for (const auto& session : sessions) {
    if (session.opening_midpoint) {
      openings.push_back(*session.opening_midpoint);
    }
  }
  if (!openings.empty() && openings.size() == sessions.size()) {
    if (const auto middle = median(openings)) {
      result.reference_price_cents = to_int64(divide_round(*middle, kMicroUsdPerCent));
    }
  }
  for (std::size_t i = 0; i < kFactCount; ++i) {
    std::vector<std::int64_t> values;
    for (const auto& session : sessions) {
      if (!session.scored.at(i)) {
        values.clear();
        break;
      }
      values.push_back(*session.scored.at(i));
    }
    if (values.empty()) {
      continue;
    }
    const auto average = mean(values);
    const auto spread = deviation(values);
    const auto relative_floor = to_int64(
        divide_round(static_cast<Int128>(average < 0 ? -average : average) * kScaleFloorPpm, kPpm));
    result.mean.at(i) = average;
    result.deviation.at(i) = spread;
    result.scale.at(i) = std::max({spread.value_or(0), relative_floor, fact_table().at(i).floor});
  }
  return result;
}

CandidateScore score(const Targets& target, const Targets& scale, std::span<const Facts> seeds) {
  CandidateScore result;
  result.seeds = seeds.size();
  for (std::size_t i = 0; i < kFactCount; ++i) {
    std::vector<std::int64_t> values;
    for (const auto& seed : seeds) {
      if (seed.scored.at(i)) {
        values.push_back(*seed.scored.at(i));
      }
    }
    auto& fact = result.facts.at(i);
    if (!values.empty() && values.size() == seeds.size()) {
      fact.mean = mean(values);
      fact.deviation = deviation(values);
      if (target.mean.at(i) && scale.scale.at(i)) {
        fact.distance_ppm = to_int64(divide_round(
            (static_cast<Int128>(*fact.mean) - *target.mean.at(i)) * kPpm, *scale.scale.at(i)));
      }
    }
  }
  if (seeds.empty()) {
    result.invalid_reason = "no seeds";
    return result;
  }
  for (const auto& seed : seeds) {
    if (seed.trades < kMinTrades) {
      result.invalid_reason = "a seed printed fewer than 30 trades";
      return result;
    }
    const auto two_sided = seed.scored.at(index(Fact::TwoSidedShare));
    if (!two_sided || *two_sided < kMinTwoSidedPpm) {
      result.invalid_reason = "a seed was two-sided for less than half the window";
      return result;
    }
  }
  for (std::size_t i = 0; i < kFactCount; ++i) {
    if (!result.facts.at(i).mean) {
      result.invalid_reason = "a seed left " + std::string{fact_table().at(i).name} + " undefined";
      return result;
    }
    if (!result.facts.at(i).distance_ppm) {
      result.invalid_reason =
          "the targets leave " + std::string{fact_table().at(i).name} + " undefined";
      return result;
    }
  }
  // Each group weighs 1 / kGroupCount and each fact an equal share of its group:
  // sum of d^2 * (12 / group size), divided by 12 * kGroupCount.
  constexpr std::int64_t kCommon = 12;
  Int128 weighted = 0;
  for (std::size_t i = 0; i < kFactCount; ++i) {
    const auto& group = fact_table().at(i).group;
    const auto size = std::ranges::count_if(
        fact_table(), [&](const FactInfo& info) { return info.group == group; });
    const auto distance = static_cast<Int128>(*result.facts.at(i).distance_ppm);
    weighted += distance * distance * (kCommon / size);
  }
  const auto mean_square = divide_round(weighted, kCommon * static_cast<Int128>(kGroupCount));
  result.score_ppm = static_cast<std::uint64_t>(integer_sqrt(static_cast<UInt128>(mean_square)));
  result.valid = true;
  return result;
}

std::string facts_json(const Facts& facts) {
  Json units = Json::object();
  Json groups = Json::object();
  for (const auto& info : fact_table()) {
    units[std::string{info.name}] = info.unit;
    groups[std::string{info.name}] = info.group;
  }
  const Json document = {
      {"schema_version", kCalibrationSchema},
      {"facts", values_json(facts.scored)},
      {"units", units},
      {"groups", groups},
      {"counts",
       {{"trades", facts.trades},
        {"l1_changes", facts.quotes},
        {"window_ns", facts.window_ns},
        {"measured_ns", facts.measured_ns},
        {"two_sided_ns", facts.two_sided_ns},
        {"locked_ns", facts.locked_ns},
        {"crossed_ns", facts.crossed_ns}}},
      {"unscored",
       {{"locked_share_ppm", nullable(share_ppm(facts.locked_ns, facts.measured_ns))},
        {"crossed_share_ppm", nullable(share_ppm(facts.crossed_ns, facts.measured_ns))},
        {"opening_midpoint_micro_usd", nullable(facts.opening_midpoint)}}}};
  return document.dump();
}

Facts parse_facts(std::string_view text) {
  const auto document = parse(text);
  if (!document.is_object() || !document.contains("facts") || !document.contains("counts") ||
      !document.contains("unscored")) {
    invalid("a facts document needs facts, counts, and unscored values");
  }
  try {
    const auto& counts = document.at("counts");
    return Facts{.scored = parse_values(document.at("facts")),
                 .trades = counts.at("trades").get<std::uint64_t>(),
                 .quotes = counts.at("l1_changes").get<std::uint64_t>(),
                 .window_ns = counts.at("window_ns").get<std::uint64_t>(),
                 .measured_ns = counts.at("measured_ns").get<std::uint64_t>(),
                 .two_sided_ns = counts.at("two_sided_ns").get<std::uint64_t>(),
                 .locked_ns = counts.at("locked_ns").get<std::uint64_t>(),
                 .crossed_ns = counts.at("crossed_ns").get<std::uint64_t>(),
                 .opening_midpoint =
                     optional_int(document.at("unscored").at("opening_midpoint_micro_usd"))};
  } catch (const Json::exception&) {
    invalid("a facts document has missing or malformed counts");
  }
}

std::string targets_json(const Targets& targets) {
  const Json document = {{"schema_version", kCalibrationSchema},
                         {"sessions", targets.sessions},
                         {"reference_price_cents", nullable(targets.reference_price_cents)},
                         {"mean", values_json(targets.mean)},
                         {"deviation", values_json(targets.deviation)},
                         {"scale", values_json(targets.scale)}};
  return document.dump();
}

Targets parse_targets(std::string_view text) {
  const auto document = parse(text);
  if (!document.is_object() || !document.contains("sessions") || !document.contains("mean") ||
      !document.contains("deviation") || !document.contains("scale") ||
      !document.at("sessions").is_number_unsigned()) {
    invalid("a targets document needs sessions, mean, deviation, and scale");
  }
  Targets targets{.sessions = document.at("sessions").get<std::size_t>(),
                  .mean = parse_values(document.at("mean")),
                  .deviation = parse_values(document.at("deviation")),
                  .scale = parse_values(document.at("scale")),
                  .reference_price_cents =
                      optional_int(document.value("reference_price_cents", Json(nullptr)))};
  for (const auto& value : targets.scale) {
    if (value && *value <= 0) {
      invalid("calibration scales must be positive");
    }
  }
  return targets;
}

std::string score_json(const CandidateScore& score) {
  Json facts = Json::object();
  for (std::size_t i = 0; i < kFactCount; ++i) {
    const auto& fact = score.facts.at(i);
    facts[std::string{fact_table().at(i).name}] = {{"mean", nullable(fact.mean)},
                                                   {"deviation", nullable(fact.deviation)},
                                                   {"distance_ppm", nullable(fact.distance_ppm)}};
  }
  const Json document = {
      {"schema_version", kCalibrationSchema},
      {"valid", score.valid},
      {"invalid_reason", score.invalid_reason.empty() ? Json(nullptr) : Json(score.invalid_reason)},
      {"seeds", score.seeds},
      {"facts", facts},
      {"score_ppm", score.score_ppm ? Json(*score.score_ppm) : Json(nullptr)}};
  return document.dump();
}

std::string observed_facts_json(const std::filesystem::path& session, std::int64_t start_ns,
                                std::int64_t end_ns) {
  return facts_json(compute_facts(observed_tape(session, start_ns, end_ns)));
}

std::string synthetic_facts_json(std::string_view config_text) {
  const auto config = parse_config(config_text);
  RunSummary summary;
  const auto facts = compute_facts(synthetic_tape(config, &summary));
  auto document = Json::parse(facts_json(facts));
  document["run"] = {{"run_id", config.run_id},
                     {"seed", summary.seed},
                     {"config_digest", summary.config_digest},
                     {"trade_count", summary.trade_count},
                     {"event_count", summary.event_count}};
  return document.dump();
}

std::string targets_from_facts_json(std::string_view facts_array_text) {
  return targets_json(targets(parse_facts_array(facts_array_text)));
}

std::string score_from_json(std::string_view target_text, std::string_view scale_text,
                            std::string_view facts_array_text) {
  return score_json(score(parse_targets(target_text), parse_targets(scale_text),
                          parse_facts_array(facts_array_text)));
}

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,
// readability-trailing-comma)
} // namespace markettwin::calibration
