#include "accountant.hpp"

#include "../telemetry/codec.hpp"

#include <charconv>

namespace markettwin {
namespace {

std::vector<AccountOpening> openings(const RunConfig& config) {
  std::vector<AccountOpening> result;
  result.reserve(config.exchange.agents.size());
  for (const auto agent : config.exchange.agents) {
    result.push_back({
        .agent = agent,
        .cash = initial_cash(config.accounting, agent),
        .position = 0,
        .cost_basis = {},
    });
  }
  return result;
}

template <std::integral Number> void append_number(std::string& text, Number value) {
  std::array<char, 24> digits{};
  const auto* end = std::to_chars(digits.data(), digits.data() + digits.size(), value).ptr;
  text.append(digits.data(), static_cast<std::size_t>(end - digits.data()));
}

template <std::integral Number>
void append_optional(std::string& text, const std::optional<Number>& value) {
  if (value) {
    append_number(text, *value);
  } else {
    text.append("null");
  }
}

void append_level(std::string& text, const std::vector<BookLevel>& levels) {
  if (levels.empty()) {
    text.append("null");
    return;
  }
  text.push_back('[');
  append_number(text, levels.front().price.value());
  text.push_back(',');
  append_number(text, levels.front().quantity.value());
  text.push_back(']');
}

std::uint64_t level_shares(const std::vector<BookLevel>& levels) {
  Quantity total;
  for (const auto& level : levels) {
    total = checked_add(total, level.quantity);
  }
  return total.value();
}

std::string party(const Account& account, Money fee) {
  return codec::ObjectWriter{}
      .number("agent_id", account.agent.value())
      .number("cash_minor", account.cash.value())
      .number("cost_basis_minor", account.cost_basis.value())
      .number("fee_minor", fee.value())
      .number("position", account.position)
      .number("realized_pnl_minor", account.realized_pnl.value())
      .finish();
}

std::optional<PriceTicks> price_of(const std::optional<BookLevel>& level) {
  return level ? std::optional{level->price} : std::nullopt;
}

std::optional<PriceTicks> best(const std::vector<BookLevel>& levels) {
  return levels.empty() ? std::nullopt : std::optional{levels.front().price};
}

} // namespace

Accountant::Accountant(const RunConfig& config, RecordSink& sink, std::string_view config_digest)
    : config_(&config), sink_(&sink),
      ledger_(openings(config), config.accounting.fees, config.tick_size_minor),
      analytics_(config.exchange.agents),
      reference_(config.scenario ? std::optional{config.scenario->reference_price} : std::nullopt),
      equity_(ledger_.accounts().size()), abs_position_sums_(ledger_.accounts().size()) {
  append(codec::accounting_header(config, config_digest, ledger_.accounts()).dump() + '\n', {});
  // Every account starts flat, so its opening equity is its cash and needs no mark.
  observe_equity({});
}

void Accountant::append(const std::string& line, Sequence sequence) {
  if (line.size() > codec::kMaxRecordBytes || line.size() > config_->max_trace_bytes - bytes_) {
    throw RunError{"ResourceLimit", "accounting stream exceeds byte budget", sequence};
  }
  sink_->append(TraceChannel::Accounting, line, sequence);
  hash_.update(line);
  bytes_ += line.size();
}

// A lighter envelope than the trace's: the stream is ordered by its own sequence and
// covered as a whole by its file digest.
void Accountant::write(std::string_view kind, std::string_view payload, SimTime now) {
  const auto sequence = sequence_.next();
  line_.assign(R"({"kind":")").append(kind).append(R"(","payload":)").append(payload);
  line_.append(R"(,"sequence":)");
  append_number(line_, sequence.value());
  line_.append(R"(,"timestamp_ns":)");
  append_number(line_, now.value());
  line_.append("}\n");
  append(line_, sequence);
}

Mark Accountant::mark(std::optional<PriceTicks> bid, std::optional<PriceTicks> ask) const {
  return select_mark(bid, ask, analytics_.last_trade_price(), reference_);
}

Valuation Accountant::value(const Account& account, const Mark& mark) const {
  const auto valuation = value_account(account, mark, config_->tick_size_minor);
  if (!valuation) {
    // A position can only come from a trade, and a trade always leaves a last price.
    throw RunError{"InternalError", "open position without a mark"};
  }
  return *valuation;
}

void Accountant::observe_equity(const Mark& mark) {
  const auto& accounts = ledger_.accounts();
  for (std::size_t index = 0; index < accounts.size(); ++index) {
    equity_.at(index).observe(value(accounts.at(index), mark).equity);
  }
}

void Accountant::on_order(SimTime now, const TopOfBook& before, const ExecutionReport& report) {
  analytics_.on_order(before, report);
  for (const auto& trade : report.trades) {
    const auto entry = ledger_.apply(trade);
    if (!entry) {
      throw RunError{"InternalError", "exchange reported a trade twice", trade.arrival_sequence};
    }
    traded_notional_ = checked_add(traded_notional_, entry->notional);
    const auto payload =
        codec::ObjectWriter{}
            .raw("buyer", party(ledger_.account(entry->buyer), entry->buyer_fee))
            .number("notional_minor", entry->notional.value())
            .number("price_ticks", trade.price_ticks.value())
            .number("quantity", trade.quantity.value())
            .raw("seller", party(ledger_.account(entry->seller), entry->seller_fee))
            .number("trade_id", trade.id.value())
            .finish();
    write("LedgerEntry", payload, now);
  }
}

void Accountant::on_cancel(const CancelResult& result) {
  analytics_.on_cancel(result);
}

void Accountant::on_book(SimTime now, const TopOfBook& top) {
  analytics_.on_book(now, top);
}

void Accountant::sample(SimTime now, const OrderBookSnapshot& snapshot, std::size_t depth,
                        std::optional<std::int64_t> fundamental_milliticks) {
  analytics_.on_sample(snapshot, depth);
  const auto bid = best(snapshot.bids);
  const auto ask = best(snapshot.asks);
  const auto current = mark(bid, ask);
  ++samples_by_mark_.at(static_cast<std::size_t>(current.source));

  auto& text = payload_;
  text.assign(R"({"agents":[)");
  const auto& accounts = ledger_.accounts();
  for (std::size_t index = 0; index < accounts.size(); ++index) {
    const auto& account = accounts.at(index);
    const auto valuation = value(account, current);
    equity_.at(index).observe(valuation.equity);
    abs_position_sums_.at(index) +=
        account.position < 0 ? -Int128{account.position} : Int128{account.position};
    text.append(index == 0 ? "[" : ",[");
    for (const auto column : {
             account.cash.value(),
             account.position,
             account.cost_basis.value(),
             account.realized_pnl.value(),
             valuation.unrealized_pnl.value(),
             valuation.equity.value(),
         }) {
      append_number(text, column);
      text.push_back(',');
    }
    text.back() = ']';
  }
  text.append(R"(],"ask":)");
  append_level(text, snapshot.asks);
  text.append(R"(,"ask_depth":)");
  append_number(text, level_shares(snapshot.asks));
  text.append(R"(,"bid":)");
  append_level(text, snapshot.bids);
  text.append(R"(,"bid_depth":)");
  append_number(text, level_shares(snapshot.bids));
  text.append(R"(,"depth_levels":)");
  append_number(text, depth);
  text.append(R"(,"fundamental_milliticks":)");
  append_optional(text, fundamental_milliticks);
  text.append(R"(,"mark_source":")").append(mark_source_name(current.source));
  text.append(R"(","mark_twice_ticks":)");
  append_optional(text, current.source == MarkSource::None
                            ? std::nullopt
                            : std::optional{current.twice_price_ticks});
  text.append(R"(,"spread_ticks":)");
  append_optional(text, bid && ask ? std::optional{ask->value() - bid->value()} : std::nullopt);
  text.append(R"(,"trade_count":)");
  append_number(text, analytics_.trade_count());
  text.append(R"(,"volume":)");
  append_number(text, analytics_.volume().value());
  text.push_back('}');
  write("Sample", text, now);
}

void Accountant::finish(RunSummary& summary, const MatchingEngine& exchange,
                        std::span<const std::pair<AgentId, std::uint64_t>> cash_refusals) {
  analytics_.finish(summary.final_time);
  const auto bid = exchange.book().top(Side::Buy);
  const auto ask = exchange.book().top(Side::Sell);
  const auto final_mark = mark(price_of(bid), price_of(ask));
  observe_equity(final_mark);
  ledger_.verify();

  auto& report = summary.report;
  report = {
      .final_mark = final_mark,
      .market = analytics_.metrics(),
      .traded_notional = traded_notional_,
      .totals = ledger_.totals(),
      .samples_by_mark = samples_by_mark_,
      .agents = {},
  };
  const auto& accounts = ledger_.accounts();
  const auto resting = exchange.book().ordered_orders();
  for (std::size_t index = 0; index < accounts.size(); ++index) {
    const auto& account = accounts.at(index);
    const auto& flow = analytics_.flows().at(index);
    AgentReport agent{
        .policy = {},
        .account = account,
        .valuation = value(account, final_mark),
        .equity_observations = equity_.at(index).observations(),
        .peak_equity = equity_.at(index).peak(),
        .max_drawdown = equity_.at(index).max_drawdown(),
        .mean_abs_position_millishares = {},
        .credit_limit = credit_limit(config_->accounting, account.agent),
        .cash_refusals = 0,
        .flow = flow,
        .resting_quantity = {},
        .fill_rate_ppm = {},
        .maker_spread_capture =
            half_tick_shares_to_money(flow.maker_edge_half_tick_shares, config_->tick_size_minor),
        .taker_spread_cost =
            half_tick_shares_to_money(flow.taker_cost_half_tick_shares, config_->tick_size_minor),
    };
    if (config_->scenario) {
      const auto& agents = config_->scenario->agents;
      if (const auto found = std::ranges::find(agents, account.agent, &AgentConfig::id);
          found != agents.end()) {
        agent.policy = policy_name(found->policy);
      }
    }
    if (report.market.samples != 0) {
      agent.mean_abs_position_millishares = narrow(divide_round(
          abs_position_sums_.at(index) * 1'000, static_cast<Int128>(report.market.samples)));
    }
    if (const auto found = std::ranges::find(cash_refusals, account.agent,
                                             &std::pair<AgentId, std::uint64_t>::first);
        found != cash_refusals.end()) {
      agent.cash_refusals = found->second;
    }
    for (const auto& order : resting) {
      if (order.owner == account.agent) {
        agent.resting_quantity = checked_add(agent.resting_quantity, order.remaining_quantity);
      }
    }
    if (flow.submitted_quantity != Quantity{}) {
      const auto filled = checked_add(flow.maker_quantity, flow.taker_quantity);
      agent.fill_rate_ppm = static_cast<std::uint64_t>(
          narrow(divide_round(static_cast<Int128>(filled.value()) * 1'000'000,
                              static_cast<Int128>(flow.submitted_quantity.value()))));
    }
    report.agents.push_back(std::move(agent));
  }
  // The stream ends by committing to the metrics document, which is published beside it.
  summary.metrics_digest = sha256(codec::metrics(*config_, summary).dump() + '\n');
  const auto terminal = codec::ObjectWriter{}
                            .number("cash_minor", report.totals.cash.value())
                            .number("fee_sink_minor", report.totals.fee_sink.value())
                            .number("initial_cash_minor", report.totals.initial_cash.value())
                            .name("metrics_digest", summary.metrics_digest)
                            .number("position", report.totals.position)
                            .number("samples", report.market.samples)
                            .number("trade_count", report.market.trade_count)
                            .finish();
  write("AccountingCompleted", terminal, summary.final_time);
  summary.accounting_digest = hash_.digest();
}

} // namespace markettwin
