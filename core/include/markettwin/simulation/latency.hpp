#ifndef MARKETTWIN_SIMULATION_LATENCY_HPP
#define MARKETTWIN_SIMULATION_LATENCY_HPP

#include "markettwin/common/types.hpp"

namespace markettwin {

// Logical-time delays between an agent and the exchange. Each of the three paths is a
// FIFO channel: a message never overtakes an earlier one from the same agent.
struct LatencyConfig {
  SimTime order_entry{0};        // agent to exchange, for orders and cancels
  SimTime order_entry_jitter{0}; // extra uniform delay in [0, jitter], per message
  SimTime report{0};             // exchange to agent, for order reports and fills
  SimTime market_data{0};        // exchange to agent, for top-of-book updates
  bool operator==(const LatencyConfig&) const = default;
};

} // namespace markettwin
#endif // MARKETTWIN_SIMULATION_LATENCY_HPP
