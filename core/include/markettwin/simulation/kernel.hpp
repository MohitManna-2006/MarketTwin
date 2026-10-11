#ifndef MARKETTWIN_SIMULATION_KERNEL_HPP
#define MARKETTWIN_SIMULATION_KERNEL_HPP

#include "markettwin/simulation/simulation.hpp"
#include "markettwin/telemetry/event_log.hpp"

namespace markettwin {

// Each call creates fresh scheduler/exchange state. Sink failures abort the run.
[[nodiscard]] RunSummary run_kernel(const RunConfig& config, RecordSink& sink);

} // namespace markettwin
#endif // MARKETTWIN_SIMULATION_KERNEL_HPP
