#include "markettwin/exchange/order_book.hpp"

#include "markettwin/common/errors.hpp"

#include <iterator>
#include <stdexcept>
#include <unordered_set>

namespace markettwin {
namespace {

void verify_levels(const auto& levels, Side side, SymbolId symbol, const auto& index, auto& seen) {
  for (const auto& [price, level] : levels) {
    if (price.value() <= 0 || level.orders.empty()) {
      throw std::logic_error("invalid or empty price level");
    }
    Quantity total;
    const Order* previous = nullptr;
    for (auto iter = level.orders.begin(); iter != level.orders.end(); ++iter) {
      const auto& order = *iter;
      if (order.id.value() == 0 || order.owner.value() == 0 || order.symbol != symbol ||
          order.side != side || order.type != OrderType::Limit || order.tif != TimeInForce::Gtc ||
          order.limit_price != price || order.remaining_quantity == Quantity{} ||
          order.remaining_quantity > order.original_quantity ||
          order.arrival_sequence == Sequence{}) {
        throw std::logic_error("invalid resting order");
      }
      if (previous != nullptr && (order.arrival_time < previous->arrival_time ||
                                  order.arrival_sequence <= previous->arrival_sequence)) {
        throw std::logic_error("invalid FIFO priority");
      }
      previous = &order;
      total = checked_add(total, order.remaining_quantity);
      const auto indexed = index.find(order.id.value());
      if (!seen.insert(order.id.value()).second || indexed == index.end() ||
          indexed->second.side != side || indexed->second.price != price ||
          indexed->second.order != iter) {
        throw std::logic_error("invalid order index");
      }
    }
    if (total != level.total_quantity) {
      throw std::logic_error("incorrect level aggregate");
    }
  }
}

} // namespace

OrderBook::OrderBook(SymbolId symbol, std::size_t max_snapshot_depth)
    : symbol_(symbol), max_snapshot_depth_(max_snapshot_depth) {
  if (symbol.value() == 0 || max_snapshot_depth == 0) {
    throw ExchangeError{RejectReason::InvalidConfig,
                        "book requires a symbol and positive depth cap"};
  }
}

std::optional<PriceTicks> OrderBook::best_bid() const noexcept {
  if (bids_.empty()) {
    return std::nullopt;
  }
  return bids_.begin()->first;
}

std::optional<PriceTicks> OrderBook::best_ask() const noexcept {
  if (asks_.empty()) {
    return std::nullopt;
  }
  return asks_.begin()->first;
}

OrderBookSnapshot OrderBook::snapshot(std::size_t depth) const {
  if (depth == 0 || depth > max_snapshot_depth_) {
    throw ExchangeError{RejectReason::InvalidDepth, "snapshot depth outside configured bounds"};
  }
  OrderBookSnapshot result{.symbol = symbol_, .bids = {}, .asks = {}};
  const auto append = [depth](const auto& levels, auto& output) {
    for (const auto& [price, level] : levels) {
      if (output.size() == depth) {
        break;
      }
      output.push_back(BookLevel{price, level.total_quantity, level.orders.size()});
    }
  };
  append(bids_, result.bids);
  append(asks_, result.asks);
  return result;
}

std::optional<Order> OrderBook::find(OrderId id) const {
  const auto found = index_.find(id.value());
  if (found == index_.end()) {
    return std::nullopt;
  }
  return *found->second.order;
}

std::size_t OrderBook::size() const noexcept {
  return index_.size();
}

Quantity OrderBook::level_quantity(Side side, PriceTicks price) const {
  const auto lookup = [price](const auto& levels) {
    const auto found = levels.find(price);
    return found == levels.end() ? Quantity{} : found->second.total_quantity;
  };
  return side == Side::Buy ? lookup(bids_) : lookup(asks_);
}

void OrderBook::rest(const Order& order) {
  const auto insert = [&](auto& levels) {
    auto& level = levels[order.limit_price];
    const auto total = checked_add(level.total_quantity, order.remaining_quantity);
    level.orders.push_back(order);
    index_.emplace(order.id.value(),
                   Handle{order.side, order.limit_price, std::prev(level.orders.end())});
    level.total_quantity = total;
  };
  if (order.side == Side::Buy) {
    insert(bids_);
  } else {
    insert(asks_);
  }
}

void OrderBook::reduce(OrderId id, Quantity quantity) {
  const auto found = index_.find(id.value());
  if (found == index_.end()) {
    throw std::logic_error("reducing an unindexed order");
  }
  const auto handle = found->second;
  const auto remove = [&](auto& levels) {
    const auto level = levels.find(handle.price);
    auto& order = *handle.order;
    order.remaining_quantity = checked_subtract(order.remaining_quantity, quantity);
    level->second.total_quantity = checked_subtract(level->second.total_quantity, quantity);
    if (order.remaining_quantity == Quantity{}) {
      level->second.orders.erase(handle.order);
      index_.erase(found);
      if (level->second.orders.empty()) {
        levels.erase(level);
      }
    }
  };
  if (handle.side == Side::Buy) {
    remove(bids_);
  } else {
    remove(asks_);
  }
}

void OrderBook::verify_invariants() const {
  if (best_bid() && best_ask() && *best_bid() >= *best_ask()) {
    throw std::logic_error("crossed resting book");
  }
  std::unordered_set<std::uint64_t> seen;
  verify_levels(bids_, Side::Buy, symbol_, index_, seen);
  verify_levels(asks_, Side::Sell, symbol_, index_, seen);
  if (seen.size() != index_.size()) {
    throw std::logic_error("orphaned order index entry");
  }
}

} // namespace markettwin
