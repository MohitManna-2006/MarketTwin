#ifndef MARKETTWIN_EXCHANGE_EXECUTION_HPP
#define MARKETTWIN_EXCHANGE_EXECUTION_HPP

#include "markettwin/common/errors.hpp"
#include "markettwin/exchange/order.hpp"

#include <vector>

namespace markettwin {

enum class OrderState { Rejected, Resting, PartiallyFilled, Filled, Canceled };

struct OrderStatus {
  OrderId order_id;
  AgentId owner;
  OrderState state = OrderState::Rejected;
  RejectReason reason = RejectReason::None;
  Quantity filled_quantity;
  Quantity canceled_quantity;
  Quantity resting_quantity;
  bool operator==(const OrderStatus&) const = default;
};

struct Trade {
  TradeId id;
  SymbolId symbol;
  OrderId buy_order_id;
  OrderId sell_order_id;
  OrderId aggressor_order_id;
  OrderId resting_order_id;
  AgentId maker_agent_id;
  AgentId taker_agent_id;
  PriceTicks price_ticks;
  Quantity quantity;
  SimTime timestamp;
  Sequence sequence; // Exchange-local execution ordinal, not a kernel event ID.
  Sequence arrival_sequence;
  bool operator==(const Trade&) const = default;
};

struct ExecutionReport {
  OrderStatus incoming;
  std::vector<Trade> trades;
  std::vector<OrderStatus> order_updates;
  [[nodiscard]] bool accepted() const noexcept { return incoming.state != OrderState::Rejected; }
  bool operator==(const ExecutionReport&) const = default;
};

struct CancelResult {
  OrderStatus order;
  [[nodiscard]] bool canceled() const noexcept { return order.state == OrderState::Canceled; }
  bool operator==(const CancelResult&) const = default;
};

} // namespace markettwin
#endif // MARKETTWIN_EXCHANGE_EXECUTION_HPP
