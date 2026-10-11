#ifndef MARKETTWIN_EXCHANGE_ORDER_HPP
#define MARKETTWIN_EXCHANGE_ORDER_HPP

#include "markettwin/common/ids.hpp"

namespace markettwin {

enum class Side { Buy, Sell };
enum class OrderType { Limit, Market };
enum class TimeInForce { Gtc, Ioc };

struct Order {
  OrderId id;
  AgentId owner;
  SymbolId symbol;
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  PriceTicks limit_price;
  Quantity original_quantity;
  Quantity remaining_quantity;
  TimeInForce tif = TimeInForce::Gtc;
  SimTime arrival_time;
  Sequence arrival_sequence;
  bool operator==(const Order&) const = default;
};

} // namespace markettwin
#endif // MARKETTWIN_EXCHANGE_ORDER_HPP
