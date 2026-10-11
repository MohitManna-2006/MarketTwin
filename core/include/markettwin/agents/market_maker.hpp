#ifndef MARKETTWIN_AGENTS_MARKET_MAKER_HPP
#define MARKETTWIN_AGENTS_MARKET_MAKER_HPP

#include "markettwin/agents/agent.hpp"

namespace markettwin {

struct MarketMakerParams {
  std::uint64_t half_spread_ticks = 2; // minimum distance from the reference to each quote
  Quantity quote_size{100};
  std::size_t levels = 1; // resting quotes per side
  std::uint64_t level_spacing_ticks = 1;
  bool operator==(const MarketMakerParams&) const = default;
};

// Stylized inventory control in the shape of Avellaneda-Stoikov, not a calibrated
// implementation of it: reservation = reference - q * risk, where
// risk = risk_aversion * variance * remaining-horizon fraction, in milliticks per share.
struct InventoryMarketMakerParams {
  MarketMakerParams quoting;
  std::uint64_t risk_aversion_ppm = 20;        // per share and per squared millitick of variance
  std::uint64_t volatility_alpha_ppm = 50'000; // EWMA weight of the newest squared move
  std::uint64_t volatility_floor_milliticks = 500; // used until, and below, the estimate
  std::uint64_t max_skew_ticks = 2;                // clamp on the reservation shift and on widening
  bool operator==(const InventoryMarketMakerParams&) const = default;
};

// Where a maker wants its innermost quotes, in milliticks. Exposed for tests.
struct QuoteTarget {
  std::int64_t reservation_milliticks = 0;
  std::int64_t half_spread_milliticks = 0;
  bool operator==(const QuoteTarget&) const = default;
};

// Posts symmetric quotes around the delivered reference price.
class MarketMaker final : public ITradingAgent {
public:
  explicit MarketMaker(const MarketMakerParams& params) : params_(params) {}
  void on_market_data(const AgentObservation& /*observation*/, AgentActions& /*actions*/) override {
  }
  void on_timer(const AgentObservation& observation, AgentActions& actions) override;
  void on_order_report(const OrderReport& /*report*/) override {}
  void on_execution(const Fill& /*fill*/) override {}

private:
  MarketMakerParams params_;
};

// Shifts its quotes against its inventory and widens them when the reference is volatile.
class InventoryMarketMaker final : public ITradingAgent {
public:
  explicit InventoryMarketMaker(const InventoryMarketMakerParams& params) : params_(params) {}
  void on_market_data(const AgentObservation& /*observation*/, AgentActions& /*actions*/) override {
  }
  void on_timer(const AgentObservation& observation, AgentActions& actions) override;
  void on_order_report(const OrderReport& /*report*/) override {}
  void on_execution(const Fill& /*fill*/) override {}

  // Updates the volatility estimate with this observation and returns the quote target.
  [[nodiscard]] QuoteTarget target(const AgentObservation& observation);
  [[nodiscard]] std::uint64_t variance_milliticks2() const noexcept { return variance_; }

private:
  InventoryMarketMakerParams params_;
  std::optional<std::int64_t> last_reference_twice_;
  std::uint64_t variance_ = 0;
};

// Shared reconciliation: cancel working quotes that are no longer wanted, keep those
// already at a wanted price, and place the rest within the inventory limit.
void requote(const AgentObservation& observation, const MarketMakerParams& params,
             const QuoteTarget& target, AgentActions& actions);

} // namespace markettwin
#endif // MARKETTWIN_AGENTS_MARKET_MAKER_HPP
