#ifndef MARKETTWIN_EXCHANGE_MATCHING_ENGINE_HPP
#define MARKETTWIN_EXCHANGE_MATCHING_ENGINE_HPP

#include "markettwin/exchange/execution.hpp"
#include "markettwin/exchange/order_book.hpp"

#include <unordered_set>

namespace markettwin {

struct ExchangeConfig {
  SymbolId symbol{1};
  std::vector<AgentId> agents;
  Quantity max_order_quantity{1'000'000};
  std::size_t max_active_orders = 100'000;
  std::size_t max_accepted_orders = 1'000'000;
  std::uint64_t max_trades = 1'000'000;
  std::size_t max_snapshot_depth = 1'000;
};

class MatchingEngine {
public:
  explicit MatchingEngine(ExchangeConfig config);
  [[nodiscard]] ExecutionReport submit(const Order& order);
  [[nodiscard]] CancelResult cancel(OrderId id, AgentId owner);
  [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
  [[nodiscard]] OrderBookSnapshot snapshot(std::size_t depth) const {
    return book_.snapshot(depth);
  }
  [[nodiscard]] SymbolId symbol() const noexcept { return config_.symbol; }
  [[nodiscard]] std::optional<PriceTicks> best_bid() const noexcept { return book_.best_bid(); }
  [[nodiscard]] std::optional<PriceTicks> best_ask() const noexcept { return book_.best_ask(); }
  void verify_invariants() const;

private:
  struct MatchPlan {
    ExecutionReport report;
    Quantity remaining;
    std::size_t removed_orders = 0;
    RejectReason reason = RejectReason::None;
  };
  template <typename Levels>
  [[nodiscard]] MatchPlan plan_matches(const Order& order, const Levels& levels) const;

  [[nodiscard]] RejectReason validate(const Order& order) const;

  ExchangeConfig config_;
  OrderBook book_;
  std::unordered_set<std::uint64_t> agents_;
  std::unordered_set<std::uint64_t> accepted_ids_;
  SimTime last_arrival_time_;
  Sequence last_arrival_sequence_;
  std::uint64_t trade_count_ = 0;
};

} // namespace markettwin
#endif // MARKETTWIN_EXCHANGE_MATCHING_ENGINE_HPP
