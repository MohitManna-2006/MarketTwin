#ifndef MARKETTWIN_AGENTS_VALUE_HPP
#define MARKETTWIN_AGENTS_VALUE_HPP

#include "markettwin/agents/agent.hpp"

namespace markettwin {

struct ValueParams {
  // Standard deviation of the agent's error around the latent fundamental. The
  // environment draws a fresh estimate from the agent's signal stream at every wakeup.
  std::uint64_t estimate_noise_milliticks = 2'000;
  std::uint64_t threshold_milliticks = 3'000; // required edge beyond the touch
  Quantity order_size{10}; // per threshold of edge, up to the agent's order limit
  std::uint64_t slippage_ticks = 1;
  bool operator==(const ValueParams&) const = default;
};

// Buys when the ask is below its estimate by at least the threshold, and sells when the
// bid is above it, in proportion to the mispricing. It never sees the latent fundamental.
class ValueAgent final : public ITradingAgent {
public:
  explicit ValueAgent(const ValueParams& params) : params_(params) {}
  void on_market_data(const AgentObservation& /*observation*/, AgentActions& /*actions*/) override {
  }
  void on_timer(const AgentObservation& observation, AgentActions& actions) override;
  void on_order_report(const OrderReport& /*report*/) override {}
  void on_execution(const Fill& /*fill*/) override {}

private:
  ValueParams params_;
};

} // namespace markettwin
#endif // MARKETTWIN_AGENTS_VALUE_HPP
