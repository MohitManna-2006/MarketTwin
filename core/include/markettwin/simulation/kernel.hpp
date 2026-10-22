#ifndef MARKETTWIN_SIMULATION_KERNEL_HPP
#define MARKETTWIN_SIMULATION_KERNEL_HPP

#include "markettwin/simulation/simulation.hpp"
#include "markettwin/telemetry/event_log.hpp"
#include "markettwin/telemetry/metrics.hpp"

#include <span>

namespace markettwin {

// Sees the exchange's own outputs after every order and cancel: the trades it printed and
// the top of book it left. It is handed copies and schedules nothing, so observing a run
// cannot change it.
class MarketObserver {
public:
  MarketObserver() = default;
  virtual ~MarketObserver() = default;
  MarketObserver(const MarketObserver&) = default;
  MarketObserver& operator=(const MarketObserver&) = default;
  MarketObserver(MarketObserver&&) = default;
  MarketObserver& operator=(MarketObserver&&) = default;
  virtual void on_market(SimTime now, std::span<const Trade> trades, const TopOfBook& top) = 0;
};

// Each call creates fresh scheduler/exchange state. Sink failures abort the run. An AI
// desk's decisions that no fork stage fixes come from `decisions`; without one, such a
// decision fails the run with DecisionSourceMissing.
[[nodiscard]] RunSummary run_kernel(const RunConfig& config, RecordSink& sink,
                                    MarketObserver* observer = nullptr,
                                    DecisionSource* decisions = nullptr);

} // namespace markettwin
#endif // MARKETTWIN_SIMULATION_KERNEL_HPP
