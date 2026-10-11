#pragma once

#include "markettwin/common/hash.hpp"
#include "markettwin/simulation/simulation.hpp"
#include "markettwin/telemetry/event_log.hpp"

namespace markettwin {

// The run's books. It turns authoritative exchange outputs into ledger postings, market
// statistics, and the accounting stream. It reads what the kernel hands it and schedules
// nothing, so it cannot change what a run does; it uses its own record sequence, so it
// cannot change a byte of the command or execution logs either.
class Accountant {
public:
  // The references must outlive the accountant.
  Accountant(const RunConfig& config, RecordSink& sink, std::string_view config_digest);

  // An order reached the exchange; before is the top of book it met.
  void on_order(SimTime now, const TopOfBook& before, const ExecutionReport& report);
  void on_cancel(const CancelResult& result);
  // The top of book after any command.
  void on_book(SimTime now, const TopOfBook& top);
  // One point of the run's time series, taken wherever the trace takes a depth snapshot.
  // The fundamental is recorded for research and never used as a mark.
  void sample(SimTime now, const OrderBookSnapshot& snapshot, std::size_t depth,
              std::optional<std::int64_t> fundamental_milliticks);
  // Final marks, conservation checks, the report, and the terminal accounting record.
  // cash_refusals pairs agents with buy intents their cash constraint dropped.
  void finish(RunSummary& summary, const MatchingEngine& exchange,
              std::span<const std::pair<AgentId, std::uint64_t>> cash_refusals);

private:
  [[nodiscard]] Mark mark(std::optional<PriceTicks> bid, std::optional<PriceTicks> ask) const;
  [[nodiscard]] Valuation value(const Account& account, const Mark& mark) const;
  void observe_equity(const Mark& mark);
  void write(std::string_view kind, std::string_view payload, SimTime now);
  void append(const std::string& line, Sequence sequence);

  const RunConfig* config_;
  RecordSink* sink_;
  Ledger ledger_;
  MarketAnalytics analytics_;
  std::optional<PriceTicks> reference_;
  std::vector<EquityTracker> equity_; // parallel to ledger_.accounts()
  std::vector<Int128> abs_position_sums_;
  std::array<std::uint64_t, 4> samples_by_mark_{};
  Money traded_notional_;
  SequenceCounter sequence_;
  Sha256 hash_;
  std::uint64_t bytes_ = 0;
  std::string line_;    // reused for every record
  std::string payload_; // reused for every sample
};

} // namespace markettwin
