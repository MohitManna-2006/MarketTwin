#ifndef MARKETTWIN_AGENTS_MOMENTUM_HPP
#define MARKETTWIN_AGENTS_MOMENTUM_HPP

#include "markettwin/agents/agent.hpp"

namespace markettwin {

struct MomentumParams {
  std::size_t short_window = 5; // samples, one per wakeup with a market price
  std::size_t long_window = 20; // also the warm-up length
  std::uint64_t threshold_milliticks = 1'000;
  Quantity order_size{10};
  std::uint64_t slippage_ticks = 1; // how far through the touch an order may reach
  SimTime cooldown{0};              // minimum time between orders
  bool operator==(const MomentumParams&) const = default;
};

// Trades in the direction of a short-minus-long moving-average gap, using only prices
// that were delivered at or before each wakeup.
class MomentumAgent final : public ITradingAgent {
public:
  explicit MomentumAgent(const MomentumParams& params);
  void on_market_data(const AgentObservation& /*observation*/, AgentActions& /*actions*/) override {
  }
  void on_timer(const AgentObservation& observation, AgentActions& actions) override;
  void on_order_report(const OrderReport& /*report*/) override {}
  void on_execution(const Fill& /*fill*/) override {}

  // Short average minus long average in milliticks; empty during warm-up.
  [[nodiscard]] std::optional<std::int64_t> signal_milliticks() const noexcept;

private:
  MomentumParams params_;
  std::vector<std::int64_t> samples_; // ring of reference prices in half ticks
  std::size_t count_ = 0;
  std::int64_t short_sum_ = 0;
  std::int64_t long_sum_ = 0;
  std::optional<SimTime> last_order_;
};

} // namespace markettwin
#endif // MARKETTWIN_AGENTS_MOMENTUM_HPP
