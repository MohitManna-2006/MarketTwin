#ifndef MARKETTWIN_TELEMETRY_METRICS_HPP
#define MARKETTWIN_TELEMETRY_METRICS_HPP

#include "markettwin/accounting/money.hpp"
#include "markettwin/exchange/execution.hpp"
#include "markettwin/exchange/order_book.hpp"

#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace markettwin {

struct TopOfBook {
  std::optional<BookLevel> bid;
  std::optional<BookLevel> ask;
  bool operator==(const TopOfBook&) const = default;
};

enum class BookState { Empty, BidOnly, AskOnly, TwoSided };
[[nodiscard]] BookState book_state(const TopOfBook& top) noexcept;

// One agent's order flow as the exchange saw it. Quantities are shares.
//   submitted_quantity = filled + canceled + expired + still resting, for accepted orders
struct AgentFlow {
  AgentId agent;
  std::uint64_t orders_submitted = 0; // reached the exchange, accepted or not
  std::uint64_t orders_accepted = 0;
  std::uint64_t orders_rejected = 0;
  Quantity submitted_quantity; // original quantity of accepted orders
  std::uint64_t maker_fills = 0;
  std::uint64_t taker_fills = 0;
  Quantity maker_quantity;
  Quantity taker_quantity;
  Quantity expired_quantity; // IOC and market residuals the exchange canceled on arrival
  std::uint64_t cancels_requested = 0;
  std::uint64_t cancels_succeeded = 0;
  Quantity canceled_quantity; // removed by the agent's own successful cancels
  // Edge against the midpoint the aggressor met, in half-tick shares:
  //   (2 * price - (bid + ask)) * quantity when the maker sold, its negation when it bought.
  // Positive is earned by the maker and paid by the taker. Fills that arrived at a book
  // without both sides have no midpoint and are counted but not measured.
  Int128 maker_edge_half_tick_shares = 0;
  Int128 taker_cost_half_tick_shares = 0;
  std::uint64_t maker_fills_measured = 0;
  std::uint64_t taker_fills_measured = 0;
  bool operator==(const AgentFlow&) const = default;
};

// Whole-market statistics. A metric with no valid observation is empty, never zero.
// Fixed-point suffixes: milli = 1/1000 of the unit; ppm and ppb are parts per million and
// billion; millibps = 1/1000 of a basis point. Divisions round half away from zero.
struct MarketMetrics {
  // Exact, from every execution.
  std::uint64_t trade_count = 0;
  Quantity volume;                             // shares, each trade counted once
  std::optional<std::int64_t> vwap_milliticks; // sum(price * quantity) / volume
  std::optional<PriceTicks> last_trade_price;
  std::optional<PriceTicks> high_trade_price;
  std::optional<PriceTicks> low_trade_price;

  // Exact, from every order and cancel that reached the exchange.
  std::uint64_t orders_submitted = 0;
  std::uint64_t orders_accepted = 0;
  std::uint64_t orders_rejected = 0;
  std::vector<std::pair<RejectReason, std::uint64_t>> rejections; // nonzero reasons, in enum order
  std::uint64_t cancels_requested = 0;
  std::uint64_t cancels_succeeded = 0;
  Quantity submitted_quantity; // accepted orders
  Quantity filled_quantity;    // both sides of every trade: twice the volume
  Quantity expired_quantity;
  Quantity canceled_quantity;
  std::optional<std::uint64_t> fill_rate_ppm; // filled_quantity / submitted_quantity

  // Exact logical time the book spent in each state, in ns. The four sum to total_ns.
  std::uint64_t total_ns = 0;
  std::uint64_t two_sided_ns = 0;
  std::uint64_t bid_only_ns = 0;
  std::uint64_t ask_only_ns = 0;
  std::uint64_t empty_ns = 0;

  // From the sampling schedule. Each mean states its own denominator.
  std::uint64_t samples = 0;
  std::uint64_t two_sided_samples = 0;
  std::uint64_t bid_samples = 0; // samples with a resting bid
  std::uint64_t ask_samples = 0;
  std::optional<std::int64_t> mean_spread_milliticks;             // over two-sided samples
  std::optional<std::int64_t> mean_relative_spread_millibps;      // spread / midpoint, same
  std::optional<std::int64_t> mean_best_bid_quantity_millishares; // over samples with a bid
  std::optional<std::int64_t> mean_best_ask_quantity_millishares; // over samples with an ask
  // Resting shares within the first depth_levels price levels of a side, over every
  // sample: an empty side contributes zero shares. Empty when samples used mixed depths.
  std::optional<std::size_t> depth_levels;
  std::optional<std::int64_t> mean_bid_depth_millishares;
  std::optional<std::int64_t> mean_ask_depth_millishares;
  // Sample standard deviation (n - 1) of simple midpoint returns between consecutive
  // two-sided samples, per sampling interval and not annualized. Needs two returns.
  std::uint64_t return_observations = 0;
  std::optional<std::uint64_t> midpoint_return_volatility_ppb;
  bool operator==(const MarketMetrics&) const = default;
};

// Collects market statistics from exchange outputs. It only reads what it is handed and
// schedules nothing, so observing a run cannot change it.
class MarketAnalytics {
public:
  explicit MarketAnalytics(std::span<const AgentId> agents);

  // An order reached the exchange; before is the top of book it met.
  void on_order(const TopOfBook& before, const ExecutionReport& report);
  void on_cancel(const CancelResult& result);
  // The top of book after a command, for exact time-in-state totals.
  void on_book(SimTime now, const TopOfBook& top);
  // One observation on the sampling schedule: the first depth levels of each side.
  void on_sample(const OrderBookSnapshot& snapshot, std::size_t depth);
  // Closes the open time-in-state interval at the end of the run.
  void finish(SimTime end);

  [[nodiscard]] MarketMetrics metrics() const;
  [[nodiscard]] const std::vector<AgentFlow>& flows() const noexcept { return flows_; } // by ID
  [[nodiscard]] std::uint64_t trade_count() const noexcept { return totals_.trade_count; }
  [[nodiscard]] Quantity volume() const noexcept { return totals_.volume; }
  [[nodiscard]] std::optional<PriceTicks> last_trade_price() const noexcept { return last_; }

private:
  [[nodiscard]] AgentFlow* flow(AgentId agent) noexcept;
  void on_trade(const TopOfBook& before, const Trade& trade);
  void elapse(SimTime now);

  // The 128-bit accumulators come first so the class carries no padding between them.
  Int128 traded_tick_shares_ = 0;
  Int128 spread_sum_ = 0;
  Int128 relative_spread_ppb_sum_ = 0;
  Int128 best_bid_quantity_sum_ = 0;
  Int128 best_ask_quantity_sum_ = 0;
  Int128 bid_depth_sum_ = 0;
  Int128 ask_depth_sum_ = 0;
  Int128 return_sum_ = 0;
  Int128 return_square_sum_ = 0;
  MarketMetrics totals_;                  // the exact counters; metrics() derives the rest
  std::vector<AgentFlow> flows_;          // sorted by agent ID
  std::vector<std::uint64_t> rejections_; // indexed by RejectReason
  std::optional<PriceTicks> last_;
  std::optional<std::int64_t> previous_midpoint_twice_;
  SimTime state_since_;
  BookState state_ = BookState::Empty;
  bool mixed_depths_ = false;
  bool returns_out_of_range_ = false;
};

} // namespace markettwin
#endif // MARKETTWIN_TELEMETRY_METRICS_HPP
