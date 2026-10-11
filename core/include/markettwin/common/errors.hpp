#ifndef MARKETTWIN_COMMON_ERRORS_HPP
#define MARKETTWIN_COMMON_ERRORS_HPP

#include <stdexcept>

namespace markettwin {

enum class RejectReason {
  None,
  InvalidOrderId,
  DuplicateOrderId,
  UnknownSymbol,
  InvalidOwner,
  InvalidSide,
  InvalidOrderType,
  InvalidTimeInForce,
  InvalidPrice,
  InvalidQuantity,
  QuantityLimit,
  InvalidArrival,
  SelfTrade,
  QuantityOverflow,
  ActiveOrderLimit,
  AcceptedOrderLimit,
  TradeLimit,
  OrderNotFound,
  NotOwner,
  InvalidConfig,
  InvalidDepth,
};

class ExchangeError : public std::invalid_argument {
public:
  ExchangeError(RejectReason reason, const char* message)
      : std::invalid_argument(message), reason_(reason) {}
  [[nodiscard]] RejectReason reason() const noexcept { return reason_; }

private:
  RejectReason reason_;
};

} // namespace markettwin
#endif // MARKETTWIN_COMMON_ERRORS_HPP
