#include "markettwin/agents/noise.hpp"

#include <algorithm>

namespace markettwin {

void NoiseAgent::on_timer(const AgentObservation& observation, AgentActions& actions) {
  auto& rng = actions.rng();
  const auto side = rng.bernoulli_ppm(params_.buy_probability_ppm) ? Side::Buy : Side::Sell;
  const bool market = rng.bernoulli_ppm(params_.market_order_ppm);
  const auto drawn = static_cast<std::uint64_t>(rng.uniform_between(
      to_signed(params_.min_quantity.value()), to_signed(params_.max_quantity.value())));
  const auto range = to_signed(params_.limit_offset_ticks);
  const auto offset = rng.uniform_between(-range, range);

  const auto quantity = std::min(drawn, side_capacity(observation, side));
  if (quantity == 0) {
    return;
  }
  if (market) {
    actions.submit({
        .side = side,
        .type = OrderType::Market,
        .limit_price = PriceTicks{},
        .quantity = Quantity{quantity},
        .tif = TimeInForce::Ioc,
    });
    return;
  }
  std::size_t resting = 0;
  for (const auto& order : observation.open_orders) {
    resting += order.cancel_requested ? 0U : 1U;
  }
  for (const auto& order : observation.open_orders) { // oldest first
    if (resting < params_.max_resting_orders) {
      break;
    }
    if (!order.cancel_requested) {
      actions.cancel(order.id);
      --resting;
    }
  }
  const auto reference = reference_twice(observation);
  const auto price =
      side == Side::Buy ? floor_div(reference, 2) + offset : ceil_div(reference, 2) - offset;
  actions.submit({
      .side = side,
      .type = OrderType::Limit,
      .limit_price = PriceTicks{std::clamp<std::int64_t>(price, 1, kMaxScenarioPriceTicks)},
      .quantity = Quantity{quantity},
      .tif = TimeInForce::Gtc,
  });
}

} // namespace markettwin
