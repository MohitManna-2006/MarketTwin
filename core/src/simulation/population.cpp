#include "population.hpp"

#include "markettwin/common/hash.hpp"

#include "../telemetry/codec.hpp"

#include <algorithm>

namespace markettwin {
namespace {

// Never earlier than a message already in flight on the same channel.
SimTime fifo(SimTime& channel, SimTime now, std::uint64_t delay) {
  channel = std::max(channel, SimTime{now.value() + delay});
  return channel;
}

Fill fill_for(const Trade& trade, bool maker) {
  const bool maker_buys = trade.buy_order_id == trade.resting_order_id;
  return {
      .order_id = maker ? trade.resting_order_id : trade.aggressor_order_id,
      .trade_id = trade.id,
      .side = maker == maker_buys ? Side::Buy : Side::Sell,
      .price = trade.price_ticks,
      .quantity = trade.quantity,
      .maker = maker,
      .exchange_time = trade.timestamp,
  };
}

} // namespace

Population::Agent::Agent(const AgentConfig& agent, const RunConfig& run)
    : config(&agent), policy(make_policy(agent.policy)),
      policy_rng(run.seed, RngStream::AgentPolicy, agent.id.value()),
      wakeup_rng(run.seed, RngStream::AgentWakeup, agent.id.value()),
      latency_rng(run.seed, RngStream::AgentLatency, agent.id.value()),
      signal_rng(run.seed, RngStream::AgentSignal, agent.id.value()),
      credit_limit(markettwin::credit_limit(run.accounting, agent.id)),
      cash(initial_cash(run.accounting, agent.id)) {
  if (const auto* value = std::get_if<ValueParams>(&agent.policy)) {
    signal_noise_milliticks = value->estimate_noise_milliticks;
  }
  activity.id = agent.id;
  activity.policy = policy_name(agent.policy);
}

Population::Population(const RunConfig& config, const ScenarioConfig& scenario,
                       const FundamentalProcess& fundamental, Scheduler& scheduler)
    : config_(&config), scenario_(&scenario), fundamental_(&fundamental), scheduler_(&scheduler) {
  agents_.reserve(scenario.agents.size());
  for (const auto& item : scenario.agents) {
    slots_.emplace_back(item.id, agents_.size());
    agents_.emplace_back(item, config);
  }
  std::ranges::sort(slots_);
}

void Population::start() {
  for (AgentSlot slot = 0; slot < agents_.size(); ++slot) {
    static_cast<void>(
        scheduler_->schedule(agents_.at(slot).config->wakeup.first, AgentWakeup{.slot = slot}));
  }
}

AgentObservation Population::observe(const Agent& agent,
                                     std::optional<std::int64_t> estimate) const {
  return {
      .now = scheduler_->now(),
      .horizon = config_->limits.horizon,
      .reference_price = scenario_->reference_price,
      .market = agent.market,
      .open_orders = agent.orders,
      .position = agent.position,
      .limits = agent.config->limits,
      .controls = agent.controls,
      .fundamental_estimate_milliticks = estimate,
  };
}

std::optional<AgentSlot> Population::slot_of(AgentId id) const noexcept {
  const auto found =
      std::ranges::lower_bound(slots_, id, {}, &std::pair<AgentId, AgentSlot>::first);
  if (found == slots_.end() || found->first != id) {
    return std::nullopt;
  }
  return found->second;
}

void Population::wake(AgentSlot slot) {
  auto& agent = agents_.at(slot);
  ++agent.activity.wakeups;
  std::optional<std::int64_t> estimate;
  if (agent.signal_noise_milliticks) {
    // One draw per wakeup whatever the policy then does, to keep the stream aligned.
    const auto error =
        (agent.signal_rng.normal_q16() * to_signed(*agent.signal_noise_milliticks)) / 65'536;
    estimate = fundamental_->value() + error;
  }
  AgentActions actions{agent.policy_rng, intents_};
  agent.policy->on_timer(observe(agent, estimate), actions);
  flush(agent);

  const auto& wakeup = agent.config->wakeup;
  // An order-flow imbalance scales the rate; validation bounds it to keep this in range.
  const auto interval = std::max<std::uint64_t>(
      static_cast<std::uint64_t>(UInt128{wakeup.interval.value()} * kPpm / agent.arrival_rate_ppm),
      1);
  const auto delay = wakeup.distribution == WakeupDistribution::Fixed
                         ? interval
                         : std::max<std::uint64_t>(agent.wakeup_rng.exponential(interval), 1);
  static_cast<void>(scheduler_->schedule_after(SimTime{delay}, AgentWakeup{.slot = slot}));
}

void Population::deliver(AgentSlot slot, const MarketDataUpdate& update) {
  auto& agent = agents_.at(slot);
  agent.market = update;
  auto& interval = agent.interval;
  ++interval.market_updates;
  if (update.best_bid && update.best_ask) {
    const auto mid = update.best_bid->price.value() + update.best_ask->price.value();
    if (!interval.mid_open_twice) {
      interval.mid_open_twice = mid;
      interval.mid_high_twice = mid;
      interval.mid_low_twice = mid;
    }
    interval.mid_high_twice = std::max(interval.mid_high_twice, mid);
    interval.mid_low_twice = std::min(interval.mid_low_twice, mid);
    interval.mid_last_twice = mid;
  }
  AgentActions actions{agent.policy_rng, intents_};
  agent.policy->on_market_data(observe(agent), actions);
  flush(agent);
}

void Population::deliver(AgentSlot slot, const OrderReport& report) {
  auto& agent = agents_.at(slot);
  const auto found = std::ranges::find(agent.orders, report.status.order_id, &WorkingOrder::id);
  if (found != agent.orders.end()) {
    const auto state = report.status.state;
    const bool live = report.kind == ReportKind::Order &&
                      (state == OrderState::Resting || state == OrderState::PartiallyFilled);
    if (live) {
      found->acknowledged = true;
      found->remaining = report.status.resting_quantity;
    } else {
      // Finished, rejected, canceled, or a cancel that found nothing left to cancel.
      agent.orders.erase(found);
    }
  }
  agent.policy->on_order_report(report);
}

void Population::deliver(AgentSlot slot, const Fill& fill) {
  auto& agent = agents_.at(slot);
  const auto quantity = to_signed(fill.quantity.value());
  agent.position += fill.side == Side::Buy ? quantity : -quantity;
  const auto ticks = static_cast<std::uint64_t>(fill.price.value()) * fill.quantity.value();
  if (fill.side == Side::Buy) {
    agent.interval.bought += fill.quantity.value();
    agent.interval.buy_value_ticks += ticks;
  } else {
    agent.interval.sold += fill.quantity.value();
    agent.interval.sell_value_ticks += ticks;
  }
  if (agent.credit_limit) {
    const auto value = notional(fill.price, fill.quantity, config_->tick_size_minor);
    const auto& fees = config_->accounting.fees;
    const auto fee = fee_for(value, fill.maker ? fees.maker_ppm : fees.taker_ppm);
    const auto traded = fill.side == Side::Buy ? checked_negate(value) : value;
    agent.cash = checked_subtract(checked_add(agent.cash, traded), fee);
  }
  agent.policy->on_execution(fill);
}

void Population::publish(const MarketDataUpdate& update) {
  for (AgentSlot slot = 0; slot < agents_.size(); ++slot) {
    auto& agent = agents_.at(slot);
    const auto time =
        fifo(agent.market_channel, scheduler_->now(),
             agent.config->latency.market_data.value() + agent.added_market_data.value());
    static_cast<void>(
        scheduler_->schedule(time, MarketDataDelivery{.slot = slot, .update = update}));
  }
}

SimTime Population::order_arrival(Agent& agent) {
  const auto& latency = agent.config->latency;
  auto delay = latency.order_entry.value() + agent.added_order_entry.value();
  if (latency.order_entry_jitter != SimTime{}) {
    delay += agent.latency_rng.uniform_below(latency.order_entry_jitter.value() + 1);
  }
  return fifo(agent.order_channel, scheduler_->now(), delay);
}

SimTime Population::report_delivery(Agent& agent) const {
  return fifo(agent.report_channel, scheduler_->now(),
              agent.config->latency.report.value() + agent.added_report.value());
}

void Population::record_fill(AgentId id, Side side, Quantity quantity) {
  if (const auto slot = slot_of(id)) {
    auto& activity = agents_.at(*slot).activity;
    const auto shares = to_signed(quantity.value());
    ++activity.fills;
    activity.filled_quantity = checked_add(activity.filled_quantity, quantity);
    activity.net_position += side == Side::Buy ? shares : -shares;
  }
}

void Population::route(const ExecutionReport& report) {
  const auto now = scheduler_->now();
  const auto send_report = [&](const OrderStatus& status) {
    if (const auto slot = slot_of(status.owner)) {
      const OrderReport delivered{
          .kind = ReportKind::Order,
          .status = status,
          .exchange_time = now,
      };
      static_cast<void>(
          scheduler_->schedule(report_delivery(agents_.at(*slot)),
                               OrderReportDelivery{.slot = *slot, .report = delivered}));
    }
  };
  const auto send_fill = [&](AgentId owner, const Fill& fill) {
    record_fill(owner, fill.side, fill.quantity);
    if (const auto slot = slot_of(owner)) {
      static_cast<void>(scheduler_->schedule(report_delivery(agents_.at(*slot)),
                                             FillDelivery{.slot = *slot, .fill = fill}));
    }
  };
  // ADR 0001: one maker status follows each trade, then the final incoming status.
  for (std::size_t index = 0; index < report.trades.size(); ++index) {
    const auto& trade = report.trades.at(index);
    send_fill(trade.maker_agent_id, fill_for(trade, true));
    send_report(report.order_updates.at(index));
    send_fill(trade.taker_agent_id, fill_for(trade, false));
  }
  send_report(report.incoming);
}

void Population::route(const CancelResult& result) {
  if (const auto slot = slot_of(result.order.owner)) {
    const OrderReport delivered{
        .kind = ReportKind::Cancel,
        .status = result.order,
        .exchange_time = scheduler_->now(),
    };
    static_cast<void>(
        scheduler_->schedule(report_delivery(agents_.at(*slot)),
                             OrderReportDelivery{.slot = *slot, .report = delivered}));
  }
}

void Population::flush(Agent& agent) {
  for (const auto& intent : intents_) {
    const bool sent = std::visit([&](const auto& item) { return send(agent, item); }, intent);
    if (!sent) {
      ++agent.activity.intents_dropped;
    }
  }
  intents_.clear();
}

// A conservative pre-trade check, in the same place and spirit as the position limit.
// Every working buy is costed in full at its limit price plus the larger fee rate, as if
// it filled entirely as it stands. A market order has no limit, so it is costed at the
// agent's own reference price: an estimate, not a bound. Sales are never refused here.
bool Population::can_fund(const Agent& agent, const SubmitIntent& intent) const {
  if (!agent.credit_limit || intent.side != Side::Buy) {
    return true;
  }
  const auto& fees = config_->accounting.fees;
  const auto rate = std::max<std::int64_t>({fees.maker_ppm, fees.taker_ppm, 0});
  const auto reference = PriceTicks{ceil_div(reference_twice(observe(agent)), 2)};
  const auto cost = [&](OrderType type, PriceTicks limit, Quantity quantity) {
    const auto value =
        notional(type == OrderType::Limit ? limit : reference, quantity, config_->tick_size_minor);
    return Int128{value.value()} + fee_for(value, rate).value();
  };
  Int128 committed = cost(intent.type, intent.limit_price, intent.quantity);
  for (const auto& order : agent.orders) {
    if (order.side == Side::Buy) {
      committed += cost(order.type, order.price, order.remaining);
    }
  }
  return Int128{agent.cash.value()} + agent.credit_limit->value() >= committed;
}

bool Population::send(Agent& agent, const SubmitIntent& intent) {
  const auto& limits = agent.config->limits;
  const bool priced =
      intent.type == OrderType::Limit
          ? intent.limit_price.value() >= 1 && intent.limit_price.value() <= kMaxScenarioPriceTicks
          : intent.limit_price == PriceTicks{};
  if (!priced || agent.messages >= limits.max_messages || intent.quantity == Quantity{} ||
      intent.quantity > limits.max_order_quantity ||
      agent.orders.size() >= limits.max_open_orders ||
      intent.quantity.value() > side_capacity(observe(agent), intent.side)) {
    return false;
  }
  if (!can_fund(agent, intent)) {
    ++agent.cash_refusals;
    return false;
  }
  ++agent.messages;
  ++agent.orders_issued;
  ++agent.activity.orders_sent;
  const OrderId id{(agent.config->id.value() << kAgentOrderIdShift) + agent.orders_issued};
  agent.orders.push_back({
      .id = id,
      .side = intent.side,
      .type = intent.type,
      .price = intent.limit_price,
      .remaining = intent.quantity,
      .tif = intent.tif,
      .sent_at = scheduler_->now(),
  });
  static_cast<void>(
      scheduler_->schedule(order_arrival(agent), Order{
                                                     .id = id,
                                                     .owner = agent.config->id,
                                                     .symbol = config_->exchange.symbol,
                                                     .side = intent.side,
                                                     .type = intent.type,
                                                     .limit_price = intent.limit_price,
                                                     .original_quantity = intent.quantity,
                                                     .remaining_quantity = intent.quantity,
                                                     .tif = intent.tif,
                                                     .arrival_time = {},
                                                     .arrival_sequence = {},
                                                 }));
  return true;
}

bool Population::send(Agent& agent, const CancelIntent& intent) {
  const auto found = std::ranges::find(agent.orders, intent.order_id, &WorkingOrder::id);
  if (found == agent.orders.end() || found->cancel_requested ||
      agent.messages >= agent.config->limits.max_messages) {
    return false;
  }
  ++agent.messages;
  ++agent.activity.cancels_sent;
  found->cancel_requested = true;
  static_cast<void>(scheduler_->schedule(
      order_arrival(agent), CancelCommand{.order_id = intent.order_id, .owner = agent.config->id}));
  return true;
}

void Population::set_quote_size(AgentId id, std::uint64_t quote_size_ppm) {
  if (const auto slot = slot_of(id)) {
    agents_.at(*slot).controls.quote_size_ppm = quote_size_ppm;
  }
}

void Population::set_order_flow(AgentId id, const OrderFlowImbalance* imbalance) {
  if (const auto slot = slot_of(id)) {
    auto& agent = agents_.at(*slot);
    agent.arrival_rate_ppm = imbalance != nullptr ? imbalance->arrival_rate_ppm : kPpm;
    agent.controls.buy_probability_ppm =
        imbalance != nullptr ? std::optional{imbalance->buy_probability_ppm} : std::nullopt;
    agent.controls.market_order_ppm =
        imbalance != nullptr ? std::optional{imbalance->market_order_ppm} : std::nullopt;
  }
}

void Population::set_added_latency(AgentId id, const LatencyDegradation* degradation) {
  if (const auto slot = slot_of(id)) {
    auto& agent = agents_.at(*slot);
    agent.added_order_entry = degradation != nullptr ? degradation->added_order_entry : SimTime{};
    agent.added_report = degradation != nullptr ? degradation->added_report : SimTime{};
    agent.added_market_data = degradation != nullptr ? degradation->added_market_data : SimTime{};
  }
}

void Population::withdraw_quotes(AgentId id) {
  const auto slot = slot_of(id);
  if (!slot) {
    return;
  }
  auto& agent = agents_.at(*slot);
  // send() marks each order, so collect the IDs first; the order list does not change.
  std::vector<OrderId> working;
  for (const auto& order : agent.orders) {
    if (!order.cancel_requested) {
      working.push_back(order.id);
    }
  }
  for (const auto order : working) {
    if (!send(agent, CancelIntent{.order_id = order})) {
      ++agent.activity.intents_dropped;
    }
  }
}

void Population::replace_policy(AgentId id, const PolicyConfig& policy) {
  const auto slot = slot_of(id);
  if (!slot) {
    return;
  }
  auto& agent = agents_.at(*slot);
  agent.policy = make_policy(policy);
  const auto* value = std::get_if<ValueParams>(&policy);
  agent.signal_noise_milliticks =
      value != nullptr ? std::optional{value->estimate_noise_milliticks} : std::nullopt;
  agent.activity.policy = policy_name(policy);
  agent.interval = Interval{};
  agent.interval.volume_at_start = agent.market ? agent.market->cumulative_volume : Quantity{};
}

void Population::apply_directive(AgentId id, const Directive& directive) {
  if (const auto slot = slot_of(id)) {
    if (auto* desk = dynamic_cast<DirectedMaker*>(agents_.at(*slot).policy.get())) {
      desk->apply(directive);
    }
  }
}

std::string Population::desk_observation(AgentId id, SimTime at, SimTime interval_start,
                                         const ModelDirectedParams& desk,
                                         const Directive& current) {
  using codec::Json;
  auto& agent = agents_.at(slot_of(id).value());
  const auto optional_ticks = [](const std::optional<PriceTicks>& price) {
    return price ? Json(price->value()) : Json(nullptr);
  };
  const auto level = [&](const std::optional<BookLevel>& book) {
    return book ? Json{{"price_ticks", book->price.value()}, {"quantity", book->quantity.value()}}
                : Json(nullptr);
  };
  const auto milliticks = [](std::int64_t twice) { return twice * (kMilliticks / 2); };
  const auto& interval = agent.interval;
  Json market = nullptr;
  if (agent.market) {
    market = {
        {"best_bid", level(agent.market->best_bid)},
        {"best_ask", level(agent.market->best_ask)},
        {"last_trade_ticks", optional_ticks(agent.market->last_trade_price)},
        {"exchange_time_ns", agent.market->exchange_time.value()},
    };
  }
  const auto volume = agent.market ? agent.market->cumulative_volume : Quantity{};
  Json mids = nullptr;
  if (interval.mid_open_twice) {
    mids = {
        {"open", milliticks(*interval.mid_open_twice)},
        {"high", milliticks(interval.mid_high_twice)},
        {"low", milliticks(interval.mid_low_twice)},
        {"last", milliticks(interval.mid_last_twice)},
    };
  }
  std::uint64_t bids = 0;
  std::uint64_t asks = 0;
  std::uint64_t bid_quantity = 0;
  std::uint64_t ask_quantity = 0;
  for (const auto& order : agent.orders) {
    if (order.cancel_requested) {
      continue;
    }
    if (order.side == Side::Buy) {
      ++bids;
      bid_quantity += order.remaining.value();
    } else {
      ++asks;
      ask_quantity += order.remaining.value();
    }
  }
  const auto& limits = agent.config->limits;
  const Json observation{
      {"time_ns", at.value()},
      {"horizon_ns", config_->limits.horizon.value()},
      {"tick_size_minor", config_->tick_size_minor},
      {"market", market},
      {
          "interval",
          {
              {"start_ns", interval_start.value()},
              {"market_updates", interval.market_updates},
              {"mid_milliticks", mids},
              {"volume", volume.value() - interval.volume_at_start.value()},
          },
      },
      {
          "own",
          {
              {"position", agent.position},
              {"bought", interval.bought},
              {"sold", interval.sold},
              {"buy_value_ticks", interval.buy_value_ticks},
              {"sell_value_ticks", interval.sell_value_ticks},
              {"working_bids", bids},
              {"working_asks", asks},
              {"working_bid_quantity", bid_quantity},
              {"working_ask_quantity", ask_quantity},
          },
      },
      {"controls", {{"quote_size_ppm", agent.controls.quote_size_ppm}}},
      {
          "limits",
          {
              {"max_position", limits.max_position},
              {"max_order_quantity", limits.max_order_quantity.value()},
              {"max_open_orders", limits.max_open_orders},
              {"max_half_spread_ticks", desk.max_half_spread_ticks},
              {"max_skew_ticks", desk.max_skew_ticks},
          },
      },
      {"directive", codec::directive(current)},
  };
  agent.interval = Interval{};
  agent.interval.volume_at_start = volume;
  return observation.dump();
}

std::vector<AgentActivity> Population::activity() const {
  std::vector<AgentActivity> result;
  result.reserve(agents_.size());
  for (const auto& agent : agents_) {
    result.push_back(agent.activity);
  }
  return result;
}

std::vector<std::pair<AgentId, std::uint64_t>> Population::cash_refusals() const {
  std::vector<std::pair<AgentId, std::uint64_t>> result;
  result.reserve(agents_.size());
  for (const auto& agent : agents_) {
    result.emplace_back(agent.config->id, agent.cash_refusals);
  }
  return result;
}

std::vector<Rng::State> Population::rng_states() const {
  std::vector<Rng::State> result;
  result.reserve(agents_.size() * 4);
  for (const auto& agent : agents_) {
    for (const auto* rng :
         {&agent.policy_rng, &agent.wakeup_rng, &agent.latency_rng, &agent.signal_rng}) {
      result.push_back(rng->state());
    }
  }
  return result;
}

} // namespace markettwin
