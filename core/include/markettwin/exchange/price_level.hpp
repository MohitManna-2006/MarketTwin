#ifndef MARKETTWIN_EXCHANGE_PRICE_LEVEL_HPP
#define MARKETTWIN_EXCHANGE_PRICE_LEVEL_HPP

#include "markettwin/exchange/order.hpp"

#include <list>

namespace markettwin {

struct PriceLevel {
  std::list<Order> orders;
  Quantity total_quantity;
};

} // namespace markettwin
#endif // MARKETTWIN_EXCHANGE_PRICE_LEVEL_HPP
