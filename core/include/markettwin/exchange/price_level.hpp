#pragma once

#include "markettwin/exchange/order.hpp"

#include <list>

namespace markettwin {

struct PriceLevel {
  std::list<Order> orders;
  Quantity total_quantity;
};

} // namespace markettwin
