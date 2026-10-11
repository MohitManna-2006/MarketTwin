#include "markettwin/agents/value.hpp"

#include <algorithm>

namespace markettwin {
namespace {
constexpr std::int64_t kMaxMultiple = 1'000;
} // namespace

void ValueAgent::on_timer(const AgentObservation& observation, AgentActions& actions) {
  if (!observation.fundamental_estimate_milliticks || !observation.market) {
    return;
  }
  const auto estimate = *observation.fundamental_estimate_milliticks;
  const auto threshold = to_signed(params_.threshold_milliticks);
  const auto slippage = to_signed(params_.slippage_ticks);
  const auto& market = *observation.market;
  // Demand is linear in the mispricing: one order_size per threshold of edge at the touch.
  const auto send = [&](Side side, PriceTicks limit, std::int64_t edge) {
    const auto price = limit.value();
    const auto multiple =
        threshold > 0 ? static_cast<std::uint64_t>(std::min(edge / threshold, kMaxMultiple)) : 1U;
    const auto wanted = std::min(params_.order_size.value() * std::max<std::uint64_t>(multiple, 1),
                                 observation.limits.max_order_quantity.value());
    const auto quantity = std::min(wanted, side_capacity(observation, side));
    if (quantity == 0 || price < 1 || price > kMaxScenarioPriceTicks) {
      return;
    }
    actions.submit({
        .side = side,
        .type = OrderType::Limit,
        .limit_price = PriceTicks{price},
        .quantity = Quantity{quantity},
        .tif = TimeInForce::Ioc,
    });
  };
  // The limit never pays away the required edge, whatever the slippage allowance.
  if (market.best_ask && (market.best_ask->price.value() * kMilliticks) + threshold <= estimate) {
    const auto ask = market.best_ask->price.value();
    send(Side::Buy,
         PriceTicks{std::min(ask + slippage, floor_div(estimate - threshold, kMilliticks))},
         estimate - (ask * kMilliticks));
  } else if (market.best_bid &&
             (market.best_bid->price.value() * kMilliticks) - threshold >= estimate) {
    const auto bid = market.best_bid->price.value();
    send(Side::Sell,
         PriceTicks{std::max(bid - slippage, ceil_div(estimate + threshold, kMilliticks))},
         (bid * kMilliticks) - estimate);
  }
}

} // namespace markettwin
