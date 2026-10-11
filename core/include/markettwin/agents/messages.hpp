#ifndef MARKETTWIN_AGENTS_MESSAGES_HPP
#define MARKETTWIN_AGENTS_MESSAGES_HPP

#include "markettwin/exchange/execution.hpp"
#include "markettwin/exchange/order_book.hpp"

#include <optional>

namespace markettwin {

// Top of book and last print as published by the exchange at exchange_time. An agent
// sees an update only after its market-data latency has elapsed.
struct MarketDataUpdate {
  Sequence sequence;
  SimTime exchange_time;
  std::optional<BookLevel> best_bid;
  std::optional<BookLevel> best_ask;
  std::optional<PriceTicks> last_trade_price;
  Quantity cumulative_volume;
  bool operator==(const MarketDataUpdate&) const = default;
};

enum class ReportKind { Order, Cancel };

// Exchange response to an agent's own submit or cancel, with cumulative quantities.
struct OrderReport {
  ReportKind kind = ReportKind::Order;
  OrderStatus status;
  SimTime exchange_time;
  bool operator==(const OrderReport&) const = default;
};

// One side of a trade, as seen by the agent that owns order_id.
struct Fill {
  OrderId order_id;
  TradeId trade_id;
  Side side = Side::Buy;
  PriceTicks price;
  Quantity quantity;
  bool maker = false;
  SimTime exchange_time;
  bool operator==(const Fill&) const = default;
};

} // namespace markettwin
#endif // MARKETTWIN_AGENTS_MESSAGES_HPP
