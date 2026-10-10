#pragma once

#include "markettwin/exchange/price_level.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace markettwin {

struct BookLevel {
  PriceTicks price;
  Quantity quantity;
  std::size_t order_count = 0;
  bool operator==(const BookLevel&) const = default;
};

struct OrderBookSnapshot {
  SymbolId symbol;
  std::vector<BookLevel> bids;
  std::vector<BookLevel> asks;
  bool operator==(const OrderBookSnapshot&) const = default;
};

class MatchingEngine;

// Only MatchingEngine may mutate the book; readers get immutable value copies.
class OrderBook {
public:
  explicit OrderBook(SymbolId symbol, std::size_t max_snapshot_depth);
  ~OrderBook() = default;
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) = delete;
  OrderBook& operator=(OrderBook&&) = delete;

  [[nodiscard]] std::optional<PriceTicks> best_bid() const noexcept;
  [[nodiscard]] std::optional<PriceTicks> best_ask() const noexcept;
  [[nodiscard]] OrderBookSnapshot snapshot(std::size_t depth) const;
  [[nodiscard]] std::optional<Order> find(OrderId id) const;
  [[nodiscard]] std::size_t size() const noexcept;
  // Throws logic_error on internal corruption. Available in all build profiles.
  void verify_invariants() const;

private:
  friend class MatchingEngine;
  struct Handle {
    Side side;
    PriceTicks price;
    std::list<Order>::iterator order;
  };

  void rest(const Order& order);
  void reduce(OrderId id, Quantity quantity);
  [[nodiscard]] Quantity level_quantity(Side side, PriceTicks price) const;

  SymbolId symbol_;
  std::size_t max_snapshot_depth_;
  std::map<PriceTicks, PriceLevel, std::greater<>> bids_;
  std::map<PriceTicks, PriceLevel> asks_;
  std::unordered_map<std::uint64_t, Handle> index_;
};

} // namespace markettwin
