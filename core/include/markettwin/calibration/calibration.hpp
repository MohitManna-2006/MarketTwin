#ifndef MARKETTWIN_CALIBRATION_CALIBRATION_HPP
#define MARKETTWIN_CALIBRATION_CALIBRATION_HPP

#include "markettwin/simulation/simulation.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace markettwin::calibration {

// ADR 0011. Synthetic runs and observed sessions are reduced to one L1 projection, and the
// same function measures both. Every value is an integer; nothing here feeds back into a
// run or into the historical artifacts it reads.
inline constexpr std::uint32_t kCalibrationSchema = 1;

// One record of the projection. Prices are micro-dollars; a quote side with price 0 is
// absent. A quote record is only emitted when the best bid, best ask, or a size changes.
struct TapeRecord {
  enum class Kind : std::uint8_t { Quote, Trade };
  Kind kind = Kind::Quote;
  std::int64_t time_ns = 0;
  std::int64_t bid = 0;
  std::int64_t bid_size = 0;
  std::int64_t ask = 0;
  std::int64_t ask_size = 0;
  std::int64_t price = 0;
  std::int64_t quantity = 0;
  bool operator==(const TapeRecord&) const = default;
};

// Records in time order inside [start_ns, end_ns).
struct MarketTape {
  std::int64_t start_ns = 0;
  std::int64_t end_ns = 0;
  std::vector<TapeRecord> records;
};

// Appends records in time order and drops quotes that repeat the previous L1 state.
class TapeBuilder {
public:
  TapeBuilder(std::int64_t start_ns, std::int64_t end_ns);
  void quote(std::int64_t time_ns, std::int64_t bid, std::int64_t bid_size, std::int64_t ask,
             std::int64_t ask_size);
  void trade(std::int64_t time_ns, std::int64_t price, std::int64_t quantity);
  [[nodiscard]] MarketTape finish() &&;

private:
  void check_time(std::int64_t time_ns) const;

  MarketTape tape_;
  std::optional<TapeRecord> last_quote_;
};

// The scored facts, in ADR order. Groups weigh equally in the score.
enum class Fact : std::uint8_t {
  SpreadCentsP10,
  SpreadCentsP50,
  SpreadCentsP90,
  RelativeSpreadBpsP50,
  TradesPerMinute,
  InterarrivalCv,
  EmptyMinuteShare,
  TradeReturnLag1Autocorr,
  MidpointVol1mBps,
  TwoSidedShare,
  L1UpdatesPerMinute,
  L1MedianDurationMs,
};
inline constexpr std::size_t kFactCount = 12;
inline constexpr std::size_t kGroupCount = 4;

struct FactInfo {
  std::string_view name;
  std::string_view unit;  // what one unit of the stored integer is
  std::string_view group; // spread, arrival, returns, or quote_state
  std::int64_t floor = 0; // the absolute floor of the score scale, in stored units
};
[[nodiscard]] const std::array<FactInfo, kFactCount>& fact_table() noexcept;

using FactValues = std::array<std::optional<std::int64_t>, kFactCount>;

struct Facts {
  FactValues scored;
  std::uint64_t trades = 0;
  std::uint64_t quotes = 0;       // L1 change records
  std::uint64_t window_ns = 0;    // end - start
  std::uint64_t measured_ns = 0;  // end - first L1 record; 0 when there is none
  std::uint64_t two_sided_ns = 0; // bid < ask
  std::uint64_t locked_ns = 0;    // bid == ask; reported, not scored
  std::uint64_t crossed_ns = 0;   // bid > ask; reported, not scored
  // Midpoint of the first two-sided L1 record, micro-dollars rounded half away from zero.
  std::optional<std::int64_t> opening_midpoint;
  bool operator==(const Facts&) const = default;
};

[[nodiscard]] Facts compute_facts(const MarketTape& tape);

// Runs the config in memory and projects what its exchange printed and quoted. The window
// is [0, horizon); ticks become micro-dollars through tick_size_minor (cents per tick).
[[nodiscard]] MarketTape synthetic_tape(const RunConfig& config, RunSummary* summary = nullptr);
// Reads a completed Phase 4A session's canonical events inside [start_ns, end_ns).
[[nodiscard]] MarketTape observed_tape(const std::filesystem::path& session, std::int64_t start_ns,
                                       std::int64_t end_ns);

// Mean and floored scale of each fact across sessions. A fact any session leaves
// undefined has neither.
struct Targets {
  std::size_t sessions = 0;
  FactValues mean;
  FactValues deviation; // sample standard deviation across sessions, before the floor
  FactValues scale;     // max(deviation, 5% of |mean|, the fact's floor)
  // Median opening midpoint across sessions, in cents: where candidate markets start.
  std::optional<std::int64_t> reference_price_cents;
  bool operator==(const Targets&) const = default;
};
[[nodiscard]] Targets targets(std::span<const Facts> sessions);

struct FactScore {
  std::optional<std::int64_t> mean;         // across seeds
  std::optional<std::int64_t> deviation;    // sample standard deviation across seeds
  std::optional<std::int64_t> distance_ppm; // (mean - target) / scale, in millionths
  bool operator==(const FactScore&) const = default;
};

struct CandidateScore {
  bool valid = false;
  std::string invalid_reason; // empty when valid
  std::size_t seeds = 0;
  std::array<FactScore, kFactCount> facts;
  std::optional<std::uint64_t> score_ppm; // group-weighted RMS distance, valid candidates only
  bool operator==(const CandidateScore&) const = default;
};

// Distances use the given targets' means and the scales' scales (train for both when
// fitting; validation or test means with train scales when selecting or testing).
[[nodiscard]] CandidateScore score(const Targets& target, const Targets& scale,
                                   std::span<const Facts> seeds);

[[nodiscard]] std::string facts_json(const Facts& facts);
[[nodiscard]] Facts parse_facts(std::string_view text);
[[nodiscard]] std::string targets_json(const Targets& targets);
[[nodiscard]] Targets parse_targets(std::string_view text);
[[nodiscard]] std::string score_json(const CandidateScore& score);

// Coarse JSON boundaries for the CLI and the Python binding.
[[nodiscard]] std::string observed_facts_json(const std::filesystem::path& session,
                                              std::int64_t start_ns, std::int64_t end_ns);
// The facts plus the run's config digest and trade count.
[[nodiscard]] std::string synthetic_facts_json(std::string_view config_text);
// facts_array_text is a JSON array of facts documents.
[[nodiscard]] std::string targets_from_facts_json(std::string_view facts_array_text);
[[nodiscard]] std::string score_from_json(std::string_view target_text, std::string_view scale_text,
                                          std::string_view facts_array_text);

} // namespace markettwin::calibration

#endif // MARKETTWIN_CALIBRATION_CALIBRATION_HPP
