#include "markettwin/telemetry/metrics.hpp"

#include <algorithm>

namespace markettwin {
namespace {

constexpr Int128 kPpb = 1'000'000'000;
// A midpoint return beyond 1,000 % in one sampling interval is outside what the
// variance accumulators are sized for; the volatility is then reported as missing.
constexpr Int128 kMaxReturnPpb = 10 * kPpb;
constexpr std::uint64_t kMaxReturnObservations = 1'000'000'000;
constexpr auto kAccumulatorLimit = static_cast<Int128>(UInt128{1} << 120U);

void accumulate(Int128& total, Int128 value) {
  if (value >= kAccumulatorLimit || value <= -kAccumulatorLimit || total >= kAccumulatorLimit ||
      total <= -kAccumulatorLimit) {
    throw RunError{"AccountingOverflow", "market statistic accumulator exceeds its range"};
  }
  total += value;
}

void add(Quantity& total, Quantity quantity) {
  total = checked_add(total, quantity);
}

Int128 wide(Quantity quantity) {
  return static_cast<Int128>(quantity.value());
}

Int128 level_shares(const std::vector<BookLevel>& levels) {
  Int128 total = 0;
  for (const auto& level : levels) {
    total += wide(level.quantity);
  }
  return total;
}

std::optional<std::int64_t> mean_milli(Int128 sum, std::uint64_t count) {
  if (count == 0) {
    return std::nullopt;
  }
  return narrow(divide_round(sum * 1'000, static_cast<Int128>(count)));
}

} // namespace

BookState book_state(const TopOfBook& top) noexcept {
  if (top.bid) {
    return top.ask ? BookState::TwoSided : BookState::BidOnly;
  }
  return top.ask ? BookState::AskOnly : BookState::Empty;
}

MarketAnalytics::MarketAnalytics(std::span<const AgentId> agents)
    : rejections_(static_cast<std::size_t>(RejectReason::InvalidDepth) + 1) {
  flows_.reserve(agents.size());
  for (const auto agent : agents) {
    flows_.emplace_back().agent = agent;
  }
  std::ranges::sort(flows_, {}, &AgentFlow::agent);
}

AgentFlow* MarketAnalytics::flow(AgentId agent) noexcept {
  const auto found = std::ranges::lower_bound(flows_, agent, {}, &AgentFlow::agent);
  return found == flows_.end() || found->agent != agent ? nullptr : &*found;
}

void MarketAnalytics::on_trade(const TopOfBook& before, const Trade& trade) {
  ++totals_.trade_count;
  add(totals_.volume, trade.quantity);
  add(totals_.filled_quantity, trade.quantity);
  add(totals_.filled_quantity, trade.quantity);
  accumulate(traded_tick_shares_, Int128{trade.price_ticks.value()} * wide(trade.quantity));
  last_ = trade.price_ticks;
  totals_.high_trade_price = totals_.high_trade_price
                                 ? std::max(*totals_.high_trade_price, trade.price_ticks)
                                 : trade.price_ticks;
  totals_.low_trade_price = totals_.low_trade_price
                                ? std::min(*totals_.low_trade_price, trade.price_ticks)
                                : trade.price_ticks;

  auto* maker = flow(trade.maker_agent_id);
  auto* taker = flow(trade.taker_agent_id);
  if (maker != nullptr) {
    ++maker->maker_fills;
    add(maker->maker_quantity, trade.quantity);
  }
  if (taker != nullptr) {
    ++taker->taker_fills;
    add(taker->taker_quantity, trade.quantity);
  }
  if (!before.bid || !before.ask) {
    return;
  }
  const auto midpoint_twice = Int128{before.bid->price.value()} + before.ask->price.value();
  const auto above = (2 * Int128{trade.price_ticks.value()}) - midpoint_twice;
  const bool maker_sold = trade.sell_order_id == trade.resting_order_id;
  const auto edge = (maker_sold ? above : -above) * wide(trade.quantity);
  if (maker != nullptr) {
    accumulate(maker->maker_edge_half_tick_shares, edge);
    ++maker->maker_fills_measured;
  }
  if (taker != nullptr) {
    accumulate(taker->taker_cost_half_tick_shares, edge);
    ++taker->taker_fills_measured;
  }
}

void MarketAnalytics::on_order(const TopOfBook& before, const ExecutionReport& report) {
  const auto& incoming = report.incoming;
  auto* owner = flow(incoming.owner);
  ++totals_.orders_submitted;
  if (owner != nullptr) {
    ++owner->orders_submitted;
  }
  if (!report.accepted()) {
    ++totals_.orders_rejected;
    ++rejections_.at(static_cast<std::size_t>(incoming.reason));
    if (owner != nullptr) {
      ++owner->orders_rejected;
    }
    return;
  }
  const auto original = checked_add(
      checked_add(incoming.filled_quantity, incoming.canceled_quantity), incoming.resting_quantity);
  ++totals_.orders_accepted;
  add(totals_.submitted_quantity, original);
  add(totals_.expired_quantity, incoming.canceled_quantity);
  if (owner != nullptr) {
    ++owner->orders_accepted;
    add(owner->submitted_quantity, original);
    add(owner->expired_quantity, incoming.canceled_quantity);
  }
  for (const auto& trade : report.trades) {
    on_trade(before, trade);
  }
}

void MarketAnalytics::on_cancel(const CancelResult& result) {
  auto* owner = flow(result.order.owner);
  ++totals_.cancels_requested;
  if (owner != nullptr) {
    ++owner->cancels_requested;
  }
  if (!result.canceled()) {
    return;
  }
  ++totals_.cancels_succeeded;
  add(totals_.canceled_quantity, result.order.canceled_quantity);
  if (owner != nullptr) {
    ++owner->cancels_succeeded;
    add(owner->canceled_quantity, result.order.canceled_quantity);
  }
}

void MarketAnalytics::elapse(SimTime now) {
  if (now < state_since_) {
    throw RunError{"InternalError", "market analytics observed time running backwards"};
  }
  const auto elapsed = now.value() - state_since_.value();
  switch (state_) {
  case BookState::Empty:
    totals_.empty_ns += elapsed;
    break;
  case BookState::BidOnly:
    totals_.bid_only_ns += elapsed;
    break;
  case BookState::AskOnly:
    totals_.ask_only_ns += elapsed;
    break;
  case BookState::TwoSided:
    totals_.two_sided_ns += elapsed;
    break;
  }
  totals_.total_ns += elapsed;
  state_since_ = now;
}

void MarketAnalytics::on_book(SimTime now, const TopOfBook& top) {
  const auto state = book_state(top);
  if (state != state_) {
    elapse(now);
    state_ = state;
  }
}

void MarketAnalytics::finish(SimTime end) {
  elapse(end);
}

void MarketAnalytics::on_sample(const OrderBookSnapshot& snapshot, std::size_t depth) {
  if (totals_.samples == 0) {
    totals_.depth_levels = depth;
  } else if (totals_.depth_levels != depth) {
    mixed_depths_ = true;
  }
  ++totals_.samples;
  bid_depth_sum_ += level_shares(snapshot.bids);
  ask_depth_sum_ += level_shares(snapshot.asks);
  if (!snapshot.bids.empty()) {
    ++totals_.bid_samples;
    best_bid_quantity_sum_ += wide(snapshot.bids.front().quantity);
  }
  if (!snapshot.asks.empty()) {
    ++totals_.ask_samples;
    best_ask_quantity_sum_ += wide(snapshot.asks.front().quantity);
  }
  if (snapshot.bids.empty() || snapshot.asks.empty()) {
    previous_midpoint_twice_.reset(); // a return needs two consecutive two-sided samples
    return;
  }
  const auto bid = snapshot.bids.front().price.value();
  const auto ask = snapshot.asks.front().price.value();
  const auto midpoint_twice = checked_add(bid, ask);
  const auto spread = Int128{ask} - bid;
  ++totals_.two_sided_samples;
  accumulate(spread_sum_, spread);
  // spread / midpoint = 2 * spread / (bid + ask)
  accumulate(relative_spread_ppb_sum_, divide_round(2 * spread * kPpb, midpoint_twice));
  if (previous_midpoint_twice_) {
    const auto change = Int128{midpoint_twice} - *previous_midpoint_twice_;
    const auto simple_return = divide_round(change * kPpb, *previous_midpoint_twice_);
    if (simple_return > kMaxReturnPpb || simple_return < -kMaxReturnPpb ||
        totals_.return_observations >= kMaxReturnObservations) {
      returns_out_of_range_ = true;
    } else {
      ++totals_.return_observations;
      return_sum_ += simple_return;
      return_square_sum_ += simple_return * simple_return;
    }
  }
  previous_midpoint_twice_ = midpoint_twice;
}

MarketMetrics MarketAnalytics::metrics() const {
  auto result = totals_;
  result.last_trade_price = last_;
  for (std::size_t reason = 0; reason < rejections_.size(); ++reason) {
    if (rejections_.at(reason) != 0) {
      result.rejections.emplace_back(static_cast<RejectReason>(reason), rejections_.at(reason));
    }
  }
  if (result.volume != Quantity{}) {
    result.vwap_milliticks = narrow(divide_round(traded_tick_shares_ * 1'000, wide(result.volume)));
  }
  if (result.submitted_quantity != Quantity{}) {
    result.fill_rate_ppm = static_cast<std::uint64_t>(narrow(
        divide_round(wide(result.filled_quantity) * 1'000'000, wide(result.submitted_quantity))));
  }
  result.mean_spread_milliticks = mean_milli(spread_sum_, result.two_sided_samples);
  if (result.two_sided_samples != 0) {
    // ppb to thousandths of a basis point: one millibps is 100 ppb.
    result.mean_relative_spread_millibps = narrow(divide_round(
        relative_spread_ppb_sum_, static_cast<Int128>(result.two_sided_samples) * 100));
  }
  result.mean_best_bid_quantity_millishares =
      mean_milli(best_bid_quantity_sum_, result.bid_samples);
  result.mean_best_ask_quantity_millishares =
      mean_milli(best_ask_quantity_sum_, result.ask_samples);
  if (mixed_depths_) {
    result.depth_levels.reset();
  } else {
    result.mean_bid_depth_millishares = mean_milli(bid_depth_sum_, result.samples);
    result.mean_ask_depth_millishares = mean_milli(ask_depth_sum_, result.samples);
  }
  const auto count = static_cast<Int128>(result.return_observations);
  if (count >= 2 && !returns_out_of_range_) {
    const auto numerator = (count * return_square_sum_) - (return_sum_ * return_sum_);
    const auto variance = numerator / (count * (count - 1));
    result.midpoint_return_volatility_ppb =
        static_cast<std::uint64_t>(integer_sqrt(static_cast<UInt128>(variance)));
  }
  return result;
}

} // namespace markettwin
