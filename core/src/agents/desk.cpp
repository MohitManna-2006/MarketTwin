#include "markettwin/agents/desk.hpp"

#include <algorithm>

namespace markettwin {
namespace {

constexpr std::size_t kMaxDeskLevels = 32;

void cancel_all(const AgentObservation& observation, AgentActions& actions) {
  for (const auto& order : observation.open_orders) {
    if (!order.cancel_requested) {
      actions.cancel(order.id);
    }
  }
}

} // namespace

std::string directive_error(const Directive& directive, const ModelDirectedParams& params,
                            const AgentLimits& limits) {
  if (directive.mode != DeskMode::Quote && directive.mode != DeskMode::Pause &&
      directive.mode != DeskMode::Reduce) {
    return "unknown directive mode";
  }
  if (directive.half_spread_ticks < 1 ||
      directive.half_spread_ticks > params.max_half_spread_ticks) {
    return "half_spread_ticks must be 1 to the desk's max_half_spread_ticks";
  }
  const auto skew = directive.skew_ticks;
  if (skew < -to_signed(params.max_skew_ticks) || skew > to_signed(params.max_skew_ticks)) {
    return "skew_ticks must be within the desk's max_skew_ticks";
  }
  if (directive.quote_size.value() < 1 || directive.quote_size > limits.max_order_quantity) {
    return "quote_size must be positive and within the agent's order limit";
  }
  if (directive.levels < 1 || directive.levels > kMaxDeskLevels ||
      2 * directive.levels > limits.max_open_orders) {
    return "levels must fit twice within max_open_orders";
  }
  if (directive.max_position > limits.max_position) {
    return "max_position must not exceed the agent's position limit";
  }
  if (directive.rationale.size() > kMaxRationaleBytes ||
      !std::ranges::all_of(directive.rationale, [](char ch) { return ch >= ' ' && ch <= '~'; })) {
    return "rationale must be at most 280 printable ASCII characters";
  }
  return {};
}

void DirectedMaker::on_timer(const AgentObservation& observation, AgentActions& actions) {
  switch (directive_.mode) {
  case DeskMode::Quote: {
    auto bounded = observation;
    bounded.limits.max_position =
        std::min(directive_.max_position, observation.limits.max_position);
    const MarketMakerParams params{
        .half_spread_ticks = directive_.half_spread_ticks,
        .quote_size = directive_.quote_size,
        .levels = directive_.levels,
        .level_spacing_ticks = 1,
    };
    const QuoteTarget target{
        .reservation_milliticks = (reference_twice(observation) * (kMilliticks / 2)) +
                                  (directive_.skew_ticks * kMilliticks),
        .half_spread_milliticks = to_signed(directive_.half_spread_ticks) * kMilliticks,
    };
    requote(bounded, params, target, actions);
    return;
  }
  case DeskMode::Pause:
    cancel_all(observation, actions);
    return;
  case DeskMode::Reduce:
    break;
  }
  if (observation.position == 0) {
    cancel_all(observation, actions);
    return;
  }
  const auto side = observation.position > 0 ? Side::Sell : Side::Buy;
  // The touch on the other side: selling at the bid, buying at the ask.
  std::optional<PriceTicks> touch;
  if (observation.market) {
    const auto& level =
        side == Side::Sell ? observation.market->best_bid : observation.market->best_ask;
    if (level) {
      touch = level->price;
    }
  }
  // Keep one working order toward flat at the touch; cancel everything else.
  bool working = false;
  for (const auto& order : observation.open_orders) {
    if (order.cancel_requested) {
      continue;
    }
    if (!working && order.side == side && touch && order.price == *touch) {
      working = true;
    } else {
      actions.cancel(order.id);
    }
  }
  if (working || !touch) {
    return;
  }
  const auto held = static_cast<std::uint64_t>(observation.position > 0 ? observation.position
                                                                        : -observation.position);
  const auto limit = std::min(directive_.quote_size.value(), held);
  const auto quantity = std::min(limit, observation.limits.max_order_quantity.value());
  actions.submit({
      .side = side,
      .type = OrderType::Limit,
      .limit_price = *touch,
      .quantity = Quantity{quantity},
      .tif = TimeInForce::Gtc,
  });
}

} // namespace markettwin
