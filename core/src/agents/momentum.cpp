#include "markettwin/agents/momentum.hpp"

#include <algorithm>

namespace markettwin {
namespace {
constexpr std::int64_t kHalfTickMilliticks = kMilliticks / 2;
} // namespace

MomentumAgent::MomentumAgent(const MomentumParams& params)
    : params_(params), samples_(params.long_window, 0) {}

std::optional<std::int64_t> MomentumAgent::signal_milliticks() const noexcept {
  if (count_ < params_.long_window) {
    return std::nullopt;
  }
  const auto short_average =
      (short_sum_ * kHalfTickMilliticks) / static_cast<std::int64_t>(params_.short_window);
  const auto long_average =
      (long_sum_ * kHalfTickMilliticks) / static_cast<std::int64_t>(params_.long_window);
  return short_average - long_average;
}

void MomentumAgent::on_timer(const AgentObservation& observation, AgentActions& actions) {
  const auto& market = observation.market;
  if (!market || !has_market_price(observation)) {
    return; // the scenario reference is not a market observation and is never sampled
  }
  const auto size = params_.long_window;
  const auto newest = count_ % size;
  const auto sample = reference_twice(observation);
  long_sum_ += sample - samples_.at(newest); // an unfilled slot holds zero
  if (count_ >= params_.short_window) {
    short_sum_ -= samples_.at((count_ - params_.short_window) % size);
  }
  short_sum_ += sample;
  samples_.at(newest) = sample;
  ++count_;

  const auto signal = signal_milliticks();
  const auto threshold = to_signed(params_.threshold_milliticks);
  if (!signal || std::abs(*signal) <= threshold) {
    return;
  }
  if (last_order_ && observation.now.value() - last_order_->value() < params_.cooldown.value()) {
    return;
  }
  const auto side = *signal > 0 ? Side::Buy : Side::Sell;
  const auto& touch = side == Side::Buy ? market->best_ask : market->best_bid;
  const auto quantity = std::min(params_.order_size.value(), side_capacity(observation, side));
  if (!touch || quantity == 0) {
    return;
  }
  const auto slippage = to_signed(params_.slippage_ticks);
  const auto price = std::clamp<std::int64_t>(
      touch->price.value() + (side == Side::Buy ? slippage : -slippage), 1, kMaxScenarioPriceTicks);
  actions.submit({
      .side = side,
      .type = OrderType::Limit,
      .limit_price = PriceTicks{price},
      .quantity = Quantity{quantity},
      .tif = TimeInForce::Ioc,
  });
  last_order_ = observation.now;
}

} // namespace markettwin
