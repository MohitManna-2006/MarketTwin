#ifndef MARKETTWIN_AGENTS_NOISE_HPP
#define MARKETTWIN_AGENTS_NOISE_HPP

#include "markettwin/agents/agent.hpp"

namespace markettwin {

struct NoiseParams {
  std::uint64_t buy_probability_ppm = 500'000;
  std::uint64_t market_order_ppm = 300'000; // otherwise a GTC limit order
  Quantity min_quantity{1};
  Quantity max_quantity{20};
  // Limit prices are the reference plus a uniform offset in [-limit_offset_ticks,
  // +limit_offset_ticks]; positive offsets are more aggressive on either side.
  std::uint64_t limit_offset_ticks = 5;
  std::size_t max_resting_orders = 4; // the oldest is canceled to make room
  bool operator==(const NoiseParams&) const = default;
};

// Uninformed order flow. Every wakeup consumes the same four draws in the same order
// whatever the market looks like, so the agent's random choices stay aligned across runs
// that differ only in other agents' strategies.
class NoiseAgent final : public ITradingAgent {
public:
  explicit NoiseAgent(const NoiseParams& params) : params_(params) {}
  void on_market_data(const AgentObservation& /*observation*/, AgentActions& /*actions*/) override {
  }
  void on_timer(const AgentObservation& observation, AgentActions& actions) override;
  void on_order_report(const OrderReport& /*report*/) override {}
  void on_execution(const Fill& /*fill*/) override {}

private:
  NoiseParams params_;
};

} // namespace markettwin
#endif // MARKETTWIN_AGENTS_NOISE_HPP
