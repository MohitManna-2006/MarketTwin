#include "markettwin/exchange/matching_engine.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace markettwin {
namespace {

OrderStatus resting_status(const Order& order) {
  const auto filled = checked_subtract(order.original_quantity, order.remaining_quantity);
  auto state = OrderState::PartiallyFilled;
  if (order.remaining_quantity == Quantity{}) {
    state = OrderState::Filled;
  } else if (filled == Quantity{}) {
    state = OrderState::Resting;
  }
  return {
      .order_id = order.id,
      .owner = order.owner,
      .state = state,
      .reason = RejectReason::None,
      .filled_quantity = filled,
      .canceled_quantity = {},
      .resting_quantity = order.remaining_quantity,
  };
}

bool crosses(const Order& order, PriceTicks price) {
  return order.type == OrderType::Market ||
         (order.side == Side::Buy ? price <= order.limit_price : price >= order.limit_price);
}

} // namespace

MatchingEngine::MatchingEngine(ExchangeConfig config)
    : config_(std::move(config)), book_(config_.symbol, config_.max_snapshot_depth) {
  if (config_.agents.empty() || config_.max_order_quantity == Quantity{} ||
      config_.max_active_orders == 0 || config_.max_accepted_orders == 0 ||
      config_.max_trades == 0) {
    throw ExchangeError{RejectReason::InvalidConfig,
                        "exchange limits and agent list must be nonempty"};
  }
  for (const auto agent : config_.agents) {
    if (agent.value() == 0 || !agents_.insert(agent.value()).second) {
      throw ExchangeError{RejectReason::InvalidConfig, "agent IDs must be nonzero and unique"};
    }
  }
}

RejectReason MatchingEngine::validate(const Order& order) const {
  if (order.id.value() == 0) {
    return RejectReason::InvalidOrderId;
  }
  if (accepted_ids_.contains(order.id.value())) {
    return RejectReason::DuplicateOrderId;
  }
  if (order.symbol != config_.symbol) {
    return RejectReason::UnknownSymbol;
  }
  if (!agents_.contains(order.owner.value())) {
    return RejectReason::InvalidOwner;
  }
  if (order.side != Side::Buy && order.side != Side::Sell) {
    return RejectReason::InvalidSide;
  }
  if (order.type != OrderType::Limit && order.type != OrderType::Market) {
    return RejectReason::InvalidOrderType;
  }
  if ((order.tif != TimeInForce::Gtc && order.tif != TimeInForce::Ioc) ||
      (order.type == OrderType::Market && order.tif != TimeInForce::Ioc)) {
    return RejectReason::InvalidTimeInForce;
  }
  if ((order.type == OrderType::Limit && order.limit_price.value() <= 0) ||
      (order.type == OrderType::Market && order.limit_price != PriceTicks{})) {
    return RejectReason::InvalidPrice;
  }
  if (order.original_quantity == Quantity{} ||
      order.remaining_quantity != order.original_quantity) {
    return RejectReason::InvalidQuantity;
  }
  if (order.original_quantity > config_.max_order_quantity) {
    return RejectReason::QuantityLimit;
  }
  if (order.arrival_time < last_arrival_time_ || order.arrival_sequence <= last_arrival_sequence_) {
    return RejectReason::InvalidArrival;
  }
  if (accepted_ids_.size() >= config_.max_accepted_orders) {
    return RejectReason::AcceptedOrderLimit;
  }
  return RejectReason::None;
}

template <typename Levels>
MatchingEngine::MatchPlan MatchingEngine::plan_matches(const Order& order,
                                                       const Levels& levels) const {
  MatchPlan plan;
  plan.remaining = order.original_quantity;
  for (const auto& [price, level] : levels) {
    if (plan.remaining == Quantity{} || !crosses(order, price)) {
      break;
    }
    for (const auto& maker : level.orders) {
      if (plan.remaining == Quantity{}) {
        break;
      }
      if (maker.owner == order.owner) {
        plan.reason = RejectReason::SelfTrade;
        return plan;
      }
      if (plan.report.trades.size() >= config_.max_trades - trade_count_) {
        plan.reason = RejectReason::TradeLimit;
        return plan;
      }
      const auto quantity = std::min(plan.remaining, maker.remaining_quantity);
      const std::uint64_t ordinal = trade_count_ + plan.report.trades.size() + 1;
      plan.report.trades.push_back({
          .id = TradeId{ordinal},
          .symbol = order.symbol,
          .buy_order_id = order.side == Side::Buy ? order.id : maker.id,
          .sell_order_id = order.side == Side::Sell ? order.id : maker.id,
          .aggressor_order_id = order.id,
          .resting_order_id = maker.id,
          .maker_agent_id = maker.owner,
          .taker_agent_id = order.owner,
          .price_ticks = price,
          .quantity = quantity,
          .timestamp = order.arrival_time,
          .sequence = Sequence{ordinal},
          .arrival_sequence = order.arrival_sequence,
      });
      auto updated = maker;
      updated.remaining_quantity = checked_subtract(maker.remaining_quantity, quantity);
      plan.report.order_updates.push_back(resting_status(updated));
      if (updated.remaining_quantity == Quantity{}) {
        ++plan.removed_orders;
      }
      plan.remaining = checked_subtract(plan.remaining, quantity);
    }
  }
  return plan;
}

ExecutionReport MatchingEngine::submit(const Order& order) {
  const auto reject = [&](RejectReason reason) {
    ExecutionReport report;
    report.incoming = {
        .order_id = order.id,
        .owner = order.owner,
        .state = OrderState::Rejected,
        .reason = reason,
        .filled_quantity = {},
        .canceled_quantity = {},
        .resting_quantity = {},
    };
    report.order_updates.push_back(report.incoming);
    return report;
  };
  if (const auto reason = validate(order); reason != RejectReason::None) {
    return reject(reason);
  }

  // Preflight collects immutable outputs without changing book, counters, or IDs.
  auto plan =
      order.side == Side::Buy ? plan_matches(order, book_.asks_) : plan_matches(order, book_.bids_);
  if (plan.reason != RejectReason::None) {
    return reject(plan.reason);
  }
  auto& report = plan.report;
  const auto remaining = plan.remaining;
  const bool will_rest =
      remaining != Quantity{} && order.type == OrderType::Limit && order.tif == TimeInForce::Gtc;
  if (will_rest) {
    if (book_.size() - plan.removed_orders >= config_.max_active_orders) {
      return reject(RejectReason::ActiveOrderLimit);
    }
    const auto existing = book_.level_quantity(order.side, order.limit_price);
    if (remaining.value() > std::numeric_limits<std::uint64_t>::max() - existing.value()) {
      return reject(RejectReason::QuantityOverflow);
    }
  }
  auto incoming = order;
  incoming.remaining_quantity = remaining;
  report.incoming = resting_status(incoming);
  if (!will_rest && remaining != Quantity{}) {
    report.incoming.state = OrderState::Canceled;
    report.incoming.canceled_quantity = remaining;
    report.incoming.resting_quantity = Quantity{};
  }
  report.order_updates.push_back(report.incoming);

  // No domain validation remains after this point. Allocation failures abort the run.
  accepted_ids_.insert(order.id.value());
  for (const auto& trade : report.trades) {
    book_.reduce(trade.resting_order_id, trade.quantity);
  }
  if (will_rest) {
    book_.rest(incoming);
  }
  trade_count_ += report.trades.size();
  last_arrival_time_ = order.arrival_time;
  last_arrival_sequence_ = order.arrival_sequence;
  return std::move(report);
}

CancelResult MatchingEngine::cancel(OrderId id, AgentId owner) {
  CancelResult result{
      .order =
          {
              .order_id = id,
              .owner = owner,
              .state = OrderState::Rejected,
              .reason = RejectReason::None,
              .filled_quantity = {},
              .canceled_quantity = {},
              .resting_quantity = {},
          },
  };
  if (!agents_.contains(owner.value())) {
    result.order.reason = RejectReason::InvalidOwner;
    return result;
  }
  const auto order = book_.find(id);
  if (!order) {
    result.order.reason = RejectReason::OrderNotFound;
    return result;
  }
  if (order->owner != owner) {
    result.order.reason = RejectReason::NotOwner;
    return result;
  }
  result.order = resting_status(*order);
  result.order.state = OrderState::Canceled;
  result.order.canceled_quantity = order->remaining_quantity;
  result.order.resting_quantity = Quantity{};
  book_.reduce(id, order->remaining_quantity);
  return result;
}

void MatchingEngine::verify_invariants() const {
  book_.verify_invariants();
  if (book_.size() > config_.max_active_orders ||
      accepted_ids_.size() > config_.max_accepted_orders || trade_count_ > config_.max_trades) {
    throw std::logic_error("exchange exceeded configured limits");
  }
  for (const auto& [id, handle] : book_.index_) {
    if (!accepted_ids_.contains(id) || !agents_.contains(handle.order->owner.value())) {
      throw std::logic_error("unregistered order or owner in book");
    }
  }
}

} // namespace markettwin
