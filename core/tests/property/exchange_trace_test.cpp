#include "markettwin/exchange/matching_engine.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <map>
#include <random>
#include <ranges>
#include <set>
#include <vector>

namespace markettwin {
namespace {

// Deliberately slow independent oracle: a flat vector, a linear best-order scan,
// and value erasure. It shares no book/index/matching implementation with production.
class ReferenceExchange {
public:
  ExecutionReport submit(const Order& incoming) {
    ExecutionReport report;
    report.incoming.order_id = incoming.id;
    report.incoming.owner = incoming.owner;
    auto working = orders_;
    auto remaining = incoming.original_quantity.value();
    if (ids_.contains(incoming.id)) {
      report.incoming.reason = RejectReason::DuplicateOrderId;
      report.order_updates.push_back(report.incoming);
      return report;
    }
    while (remaining > 0) {
      const auto best = best_order(working, incoming);
      if (best == working.end()) {
        break;
      }
      if (best->owner == incoming.owner) {
        report.trades.clear();
        report.order_updates.clear();
        report.incoming.reason = RejectReason::SelfTrade;
        report.order_updates.push_back(report.incoming);
        return report;
      }
      const auto quantity = std::min(remaining, best->remaining_quantity.value());
      const auto ordinal = trades_ + report.trades.size() + 1;
      report.trades.push_back(
          {.id = TradeId{ordinal},
           .symbol = incoming.symbol,
           .buy_order_id = incoming.side == Side::Buy ? incoming.id : best->id,
           .sell_order_id = incoming.side == Side::Sell ? incoming.id : best->id,
           .aggressor_order_id = incoming.id,
           .resting_order_id = best->id,
           .maker_agent_id = best->owner,
           .taker_agent_id = incoming.owner,
           .price_ticks = best->limit_price,
           .quantity = Quantity{quantity},
           .timestamp = incoming.arrival_time,
           .sequence = Sequence{ordinal},
           .arrival_sequence = incoming.arrival_sequence});
      remaining -= quantity;
      best->remaining_quantity = Quantity{best->remaining_quantity.value() - quantity};
      report.order_updates.push_back(
          {.order_id = best->id,
           .owner = best->owner,
           .state = best->remaining_quantity.value() == 0 ? OrderState::Filled
                                                          : OrderState::PartiallyFilled,
           .reason = RejectReason::None,
           .filled_quantity =
               Quantity{best->original_quantity.value() - best->remaining_quantity.value()},
           .canceled_quantity = {},
           .resting_quantity = best->remaining_quantity});
      if (best->remaining_quantity.value() == 0) {
        working.erase(best);
      }
    }
    report.incoming.filled_quantity = Quantity{incoming.original_quantity.value() - remaining};
    if (remaining == 0) {
      report.incoming.state = OrderState::Filled;
    } else if (incoming.type == OrderType::Limit && incoming.tif == TimeInForce::Gtc) {
      auto resting = incoming;
      resting.remaining_quantity = Quantity{remaining};
      working.push_back(resting);
      report.incoming.resting_quantity = Quantity{remaining};
      report.incoming.state = remaining == incoming.original_quantity.value()
                                  ? OrderState::Resting
                                  : OrderState::PartiallyFilled;
    } else {
      report.incoming.state = OrderState::Canceled;
      report.incoming.canceled_quantity = Quantity{remaining};
    }
    report.order_updates.push_back(report.incoming);
    orders_ = std::move(working);
    ids_.insert(incoming.id);
    trades_ += report.trades.size();
    return report;
  }

  CancelResult cancel(OrderId id, AgentId owner) {
    CancelResult result;
    result.order.order_id = id;
    result.order.owner = owner;
    const auto found = std::ranges::find(orders_, id, &Order::id);
    if (found == orders_.end()) {
      result.order.reason = RejectReason::OrderNotFound;
    } else if (found->owner != owner) {
      result.order.reason = RejectReason::NotOwner;
    } else {
      result.order.state = OrderState::Canceled;
      result.order.filled_quantity =
          Quantity{found->original_quantity.value() - found->remaining_quantity.value()};
      result.order.canceled_quantity = found->remaining_quantity;
      orders_.erase(found);
    }
    return result;
  }

  [[nodiscard]] OrderBookSnapshot snapshot() const {
    OrderBookSnapshot result{.symbol = SymbolId{1}, .bids = {}, .asks = {}};
    std::map<PriceTicks, BookLevel> bids;
    std::map<PriceTicks, BookLevel> asks;
    for (const auto& order : orders_) {
      auto& level = order.side == Side::Buy ? bids[order.limit_price] : asks[order.limit_price];
      level.price = order.limit_price;
      level.quantity = Quantity{level.quantity.value() + order.remaining_quantity.value()};
      ++level.order_count;
    }
    for (const auto& [price, level] : bids | std::views::reverse) {
      result.bids.push_back(level);
    }
    for (const auto& [price, level] : asks) {
      result.asks.push_back(level);
    }
    return result;
  }

  [[nodiscard]] const std::vector<Order>& orders() const { return orders_; }

private:
  static std::vector<Order>::iterator best_order(std::vector<Order>& working,
                                                 const Order& incoming) {
    auto best = working.end();
    for (auto iter = working.begin(); iter != working.end(); ++iter) {
      if (iter->side == incoming.side) {
        continue;
      }
      if (incoming.type == OrderType::Limit &&
          ((incoming.side == Side::Buy && iter->limit_price > incoming.limit_price) ||
           (incoming.side == Side::Sell && iter->limit_price < incoming.limit_price))) {
        continue;
      }
      if (best == working.end() ||
          (incoming.side == Side::Buy && iter->limit_price < best->limit_price) ||
          (incoming.side == Side::Sell && iter->limit_price > best->limit_price) ||
          (iter->limit_price == best->limit_price &&
           iter->arrival_sequence < best->arrival_sequence)) {
        best = iter;
      }
    }
    return best;
  }

  std::vector<Order> orders_;
  std::set<OrderId> ids_;
  std::uint64_t trades_ = 0;
};

struct Disposition {
  std::uint64_t original = 0;
  std::uint64_t filled = 0;
  std::uint64_t canceled = 0;
};

Order random_order(std::mt19937_64& random, std::uint64_t step, AgentId owner) {
  const auto id = random() % 10 == 0 ? (random() % step) + 1 : step;
  const auto type = random() % 5 == 0 ? OrderType::Market : OrderType::Limit;
  const auto quantity = Quantity{(random() % 25) + 1};
  return Order{.id = OrderId{id},
               .owner = owner,
               .symbol = SymbolId{1},
               .side = random() % 2 == 0 ? Side::Buy : Side::Sell,
               .type = type,
               .limit_price = type == OrderType::Market
                                  ? PriceTicks{}
                                  : PriceTicks{90 + static_cast<std::int64_t>(random() % 21)},
               .original_quantity = quantity,
               .remaining_quantity = quantity,
               .tif = type == OrderType::Market || random() % 4 == 0 ? TimeInForce::Ioc
                                                                     : TimeInForce::Gtc,
               .arrival_time = SimTime{step / 10},
               .arrival_sequence = Sequence{step}};
}

class ExchangePropertyTest : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(ExchangePropertyTest, RepeatedRandomTraceMatchesReferenceAndConservesEveryOrder) {
  const auto seed = GetParam();
  std::mt19937_64 random{seed};
  const ExchangeConfig config{.symbol = SymbolId{1},
                              .agents = {AgentId{1}, AgentId{2}, AgentId{3}, AgentId{4}}};
  MatchingEngine engine{config};
  MatchingEngine repeated{config};
  ReferenceExchange reference;
  std::map<OrderId, Disposition> dispositions;
  for (std::uint64_t step = 1; step <= 1000; ++step) {
    SCOPED_TRACE(::testing::Message() << "seed=" << seed << " step=" << step);
    const auto owner = AgentId{(random() % 4) + 1};
    if (random() % 100 < 35) {
      const auto id = OrderId{(random() % step) + 1};
      const auto actual = engine.cancel(id, owner);
      ASSERT_EQ(actual, repeated.cancel(id, owner));
      ASSERT_EQ(actual, reference.cancel(id, owner));
      if (actual.canceled()) {
        dispositions.at(id).canceled += actual.order.canceled_quantity.value();
      }
    } else {
      const auto order = random_order(random, step, owner);
      const auto actual = engine.submit(order);
      ASSERT_EQ(actual, repeated.submit(order));
      ASSERT_EQ(actual, reference.submit(order));
      if (actual.accepted()) {
        dispositions.emplace(order.id,
                             Disposition{.original = order.original_quantity.value(),
                                         .filled = 0,
                                         .canceled = actual.incoming.canceled_quantity.value()});
        for (const auto& trade : actual.trades) {
          ASSERT_GT(trade.quantity.value(), 0U);
          ASSERT_NE(trade.maker_agent_id, trade.taker_agent_id);
          dispositions.at(trade.buy_order_id).filled += trade.quantity.value();
          dispositions.at(trade.sell_order_id).filled += trade.quantity.value();
        }
      }
    }
    ASSERT_NO_THROW(engine.verify_invariants());
    ASSERT_NO_THROW(repeated.verify_invariants());
    ASSERT_EQ(engine.snapshot(100), reference.snapshot());
    ASSERT_EQ(engine.snapshot(100), repeated.snapshot(100));
    ASSERT_EQ(engine.book().size(), reference.orders().size());
    for (const auto& resting : reference.orders()) {
      ASSERT_EQ(engine.book().find(resting.id), resting);
    }
    for (const auto& [id, state] : dispositions) {
      const auto resting = engine.book().find(id);
      const auto open = resting ? resting->remaining_quantity.value() : 0;
      ASSERT_EQ(state.filled + state.canceled + open, state.original);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Seeds, ExchangePropertyTest,
                         ::testing::Range(std::uint64_t{0}, std::uint64_t{32}));

} // namespace
} // namespace markettwin
