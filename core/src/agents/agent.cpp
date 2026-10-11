#include "markettwin/agents/agent.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace markettwin {

bool has_market_price(const AgentObservation& observation) noexcept {
  return observation.market && ((observation.market->best_bid && observation.market->best_ask) ||
                                observation.market->last_trade_price);
}

std::int64_t reference_twice(const AgentObservation& observation) noexcept {
  if (observation.market) {
    const auto& market = *observation.market;
    if (market.best_bid && market.best_ask) {
      return market.best_bid->price.value() + market.best_ask->price.value();
    }
    if (market.last_trade_price) {
      return 2 * market.last_trade_price->value();
    }
  }
  return 2 * observation.reference_price.value();
}

std::uint64_t side_capacity(const AgentObservation& observation, Side side) noexcept {
  // Orders being canceled still count: the cancel can lose its race with a fill.
  std::uint64_t working = 0;
  for (const auto& order : observation.open_orders) {
    if (order.side == side) {
      working += order.remaining.value();
    }
  }
  const auto limit = static_cast<std::int64_t>(std::min<std::uint64_t>(
      observation.limits.max_position, std::numeric_limits<std::int64_t>::max() / 2));
  const auto signed_position = side == Side::Buy ? observation.position : -observation.position;
  const auto room = limit - signed_position;
  if (room <= 0 || std::cmp_less_equal(room, working)) {
    return 0;
  }
  return static_cast<std::uint64_t>(room) - working;
}

} // namespace markettwin
