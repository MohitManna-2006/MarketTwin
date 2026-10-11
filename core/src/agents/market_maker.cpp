#include "markettwin/agents/market_maker.hpp"

#include <algorithm>
#include <limits>

namespace markettwin {
namespace {

constexpr std::int64_t kHalfTickMilliticks = kMilliticks / 2;
constexpr std::int64_t kMaxMoveMilliticks = 1'000'000; // per wakeup, before squaring
constexpr std::uint64_t kMaxVariance = 1'000'000'000'000;

// Fraction of the run still ahead, in ppm, without overflowing for very long horizons.
std::uint64_t remaining_horizon_ppm(SimTime now, SimTime horizon) noexcept {
  if (now >= horizon) {
    return 0;
  }
  const auto left = horizon.value() - now.value();
  if (horizon.value() <= std::numeric_limits<std::uint64_t>::max() / kPpm) {
    return (left * kPpm) / horizon.value();
  }
  return left / (horizon.value() / kPpm);
}

struct Wanted {
  PriceTicks price;
  Quantity quantity;
  bool satisfied = false;
};

std::vector<Wanted> ladder(const MarketMakerParams& params, Side side, PriceTicks inner_quote,
                           Quantity size) {
  std::vector<Wanted> wanted;
  if (size == Quantity{}) {
    return wanted;
  }
  const auto inner = inner_quote.value();
  const auto spacing = to_signed(params.level_spacing_ticks);
  for (std::size_t level = 0; level < params.levels; ++level) {
    const auto offset = static_cast<std::int64_t>(level) * spacing;
    const auto price = side == Side::Buy ? inner - offset : inner + offset;
    if (price < 1 || price > kMaxScenarioPriceTicks) {
      break;
    }
    wanted.push_back({.price = PriceTicks{price}, .quantity = size});
  }
  return wanted;
}

void reconcile_side(const AgentObservation& observation, Side side, std::vector<Wanted> wanted,
                    AgentActions& actions) {
  for (const auto& order : observation.open_orders) {
    if (order.side != side || order.cancel_requested) {
      continue;
    }
    // A partially filled quote keeps its queue position; an oversized one is replaced.
    const auto match = std::ranges::find_if(wanted, [&](const Wanted& item) {
      return !item.satisfied && item.price == order.price && order.remaining <= item.quantity;
    });
    if (match == wanted.end()) {
      actions.cancel(order.id);
    } else {
      match->satisfied = true;
    }
  }
  auto capacity = side_capacity(observation, side);
  for (const auto& item : wanted) {
    if (item.satisfied) {
      continue;
    }
    const auto quantity = std::min(item.quantity.value(), capacity);
    if (quantity == 0) {
      break;
    }
    actions.submit({
        .side = side,
        .type = OrderType::Limit,
        .limit_price = item.price,
        .quantity = Quantity{quantity},
        .tif = TimeInForce::Gtc,
    });
    capacity -= quantity;
  }
}

} // namespace

void requote(const AgentObservation& observation, const MarketMakerParams& params,
             const QuoteTarget& target, AgentActions& actions) {
  // Round each quote to the nearest tick, with ties going away from the reservation
  // price. Widening by less than half a tick does not move a quote, and the quoted
  // spread is never inside twice the configured half spread.
  auto bid =
      ceil_div(target.reservation_milliticks - target.half_spread_milliticks - kHalfTickMilliticks,
               kMilliticks);
  auto ask =
      floor_div(target.reservation_milliticks + target.half_spread_milliticks + kHalfTickMilliticks,
                kMilliticks);
  bid = std::clamp<std::int64_t>(bid, 1, kMaxScenarioPriceTicks - 1);
  ask = std::clamp<std::int64_t>(ask, bid + 1, kMaxScenarioPriceTicks);
  const auto controlled = (params.quote_size.value() * observation.controls.quote_size_ppm) / kPpm;
  const Quantity size{std::min(controlled, observation.limits.max_order_quantity.value())};
  reconcile_side(observation, Side::Buy, ladder(params, Side::Buy, PriceTicks{bid}, size), actions);
  reconcile_side(observation, Side::Sell, ladder(params, Side::Sell, PriceTicks{ask}, size),
                 actions);
}

void MarketMaker::on_timer(const AgentObservation& observation, AgentActions& actions) {
  const QuoteTarget target{
      .reservation_milliticks = reference_twice(observation) * kHalfTickMilliticks,
      .half_spread_milliticks = to_signed(params_.half_spread_ticks) * kMilliticks,
  };
  requote(observation, params_, target, actions);
}

QuoteTarget InventoryMarketMaker::target(const AgentObservation& observation) {
  const auto reference = reference_twice(observation);
  if (last_reference_twice_) {
    const auto move = std::clamp((reference - *last_reference_twice_) * kHalfTickMilliticks,
                                 -kMaxMoveMilliticks, kMaxMoveMilliticks);
    const auto squared = static_cast<std::uint64_t>(move * move);
    // Integer EWMA. The estimate starts at zero and the floor covers the warm-up.
    if (squared >= variance_) {
      variance_ += ((squared - variance_) * params_.volatility_alpha_ppm) / kPpm;
    } else {
      variance_ -= ((variance_ - squared) * params_.volatility_alpha_ppm) / kPpm;
    }
  }
  last_reference_twice_ = reference;

  const auto floor = params_.volatility_floor_milliticks * params_.volatility_floor_milliticks;
  const auto variance = std::min(std::max(variance_, floor), kMaxVariance);
  const auto remaining = remaining_horizon_ppm(observation.now, observation.horizon);
  // Milliticks of reservation shift per share held. Each step stays below 1e18.
  const auto risk = to_signed((((variance * params_.risk_aversion_ppm) / kPpm) * remaining) / kPpm);
  const auto cap = to_signed(observation.limits.max_position);
  const auto inventory = std::clamp(observation.position, -cap, cap);
  const auto max_shift = to_signed(params_.max_skew_ticks) * kMilliticks;
  const auto skew = std::clamp(inventory * risk, -max_shift, max_shift);
  const auto widening =
      std::min((risk * to_signed(params_.quoting.quote_size.value())) / 2, max_shift);
  return {
      .reservation_milliticks = (reference * kHalfTickMilliticks) - skew,
      .half_spread_milliticks =
          (to_signed(params_.quoting.half_spread_ticks) * kMilliticks) + widening,
  };
}

void InventoryMarketMaker::on_timer(const AgentObservation& observation, AgentActions& actions) {
  requote(observation, params_.quoting, target(observation), actions);
}

} // namespace markettwin
