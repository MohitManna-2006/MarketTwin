#ifndef MARKETTWIN_AGENTS_DESK_HPP
#define MARKETTWIN_AGENTS_DESK_HPP

#include "markettwin/agents/market_maker.hpp"

#include <string>

namespace markettwin {

// What the AI desk's execution layer does until the next decision (ADR 0012).
enum class DeskMode { Quote, Pause, Reduce };

// One decision. A language model chooses it; the execution layer below carries it out
// on every wakeup through the agent's own limits and latency, and the book decides every
// fill. The rationale is the model's words, stored and shown, never interpreted.
struct Directive {
  DeskMode mode = DeskMode::Quote;
  std::uint64_t half_spread_ticks = 2;
  std::int64_t skew_ticks = 0; // shifts both quotes; positive is higher
  Quantity quote_size{100};
  std::size_t levels = 1;
  std::uint64_t max_position = 1'000;
  std::string rationale;
  bool operator==(const Directive&) const = default;
};

inline constexpr std::size_t kMaxRationaleBytes = 280;
inline constexpr std::size_t kMaxMandateBytes = 2'000;

struct ModelDirectedParams {
  Directive initial;
  SimTime decision_interval{60'000'000'000};
  std::uint64_t max_half_spread_ticks = 20;
  std::uint64_t max_skew_ticks = 10;
  std::string mandate; // the owner's instructions, shown to the model with every observation
  bool operator==(const ModelDirectedParams&) const = default;
};

// Why a directive cannot be carried out by this agent, or empty when it can.
[[nodiscard]] std::string directive_error(const Directive& directive,
                                          const ModelDirectedParams& params,
                                          const AgentLimits& limits);

// The execution layer. `quote` reconciles a ladder like the baseline maker, centred on
// the delivered reference shifted by the skew; `pause` cancels every working order;
// `reduce` cancels everything and works one limit order at the touch toward zero.
class DirectedMaker final : public ITradingAgent {
public:
  explicit DirectedMaker(const ModelDirectedParams& params) : directive_(params.initial) {}
  void on_market_data(const AgentObservation& /*observation*/, AgentActions& /*actions*/) override {
  }
  void on_timer(const AgentObservation& observation, AgentActions& actions) override;
  void on_order_report(const OrderReport& /*report*/) override {}
  void on_execution(const Fill& /*fill*/) override {}

  void apply(const Directive& directive) { directive_ = directive; }
  [[nodiscard]] const Directive& directive() const noexcept { return directive_; }

private:
  Directive directive_;
};

} // namespace markettwin
#endif // MARKETTWIN_AGENTS_DESK_HPP
