#include "codec.hpp"

namespace markettwin::codec {
namespace {

Json money(std::optional<Money> value) {
  return value ? Json(value->value()) : Json(nullptr);
}

template <typename Number> Json optional_number(const std::optional<Number>& value) {
  return value ? Json(*value) : Json(nullptr);
}

Json price(std::optional<PriceTicks> value) {
  return value ? Json(value->value()) : Json(nullptr);
}

Json mark(const Mark& value) {
  return {
      {"source", mark_source_name(value.source)},
      {
          "twice_price_ticks",
          value.source == MarkSource::None ? Json(nullptr) : Json(value.twice_price_ticks),
      },
  };
}

} // namespace

Json accounting(const AccountingConfig& value) {
  auto sorted = value.accounts;
  std::ranges::sort(sorted, {}, &AccountConfig::agent);
  auto accounts = Json::array();
  for (const auto& account : sorted) {
    accounts.push_back({
        {"agent_id", account.agent.value()},
        {"initial_cash_minor", account.initial_cash.value()},
        {"credit_limit_minor", money(account.credit_limit)},
    });
  }
  return {
      {
          "fees",
          {{"maker_ppm", value.fees.maker_ppm}, {"taker_ppm", value.fees.taker_ppm}},
      },
      {"default_initial_cash_minor", value.default_initial_cash.value()},
      {"accounts", accounts},
  };
}

AccountingConfig read_accounting(const Json& value) {
  fields(value, {"fees", "default_initial_cash_minor", "accounts"});
  const auto& fees = value.at("fees");
  fields(fees, {"maker_ppm", "taker_ppm"});
  AccountingConfig result{
      .fees =
          {
              .maker_ppm = signed_integer(fees.at("maker_ppm")),
              .taker_ppm = signed_integer(fees.at("taker_ppm")),
          },
      .default_initial_cash = Money{signed_integer(value.at("default_initial_cash_minor"))},
      .accounts = {},
  };
  if (!value.at("accounts").is_array()) {
    invalid("accounting accounts must be an array");
  }
  for (const auto& item : value.at("accounts")) {
    fields(item, {"agent_id", "initial_cash_minor", "credit_limit_minor"});
    const auto& limit = item.at("credit_limit_minor");
    result.accounts.push_back({
        .agent = AgentId{unsigned_integer(item.at("agent_id"))},
        .initial_cash = Money{signed_integer(item.at("initial_cash_minor"))},
        .credit_limit =
            limit.is_null() ? std::nullopt : std::optional{Money{signed_integer(limit)}},
    });
  }
  return result;
}

Json accounting_header(const RunConfig& config, std::string_view config_digest,
                       const std::vector<Account>& accounts) {
  auto agents = Json::array();
  for (const auto& account : accounts) {
    agents.push_back(account.agent.value());
  }
  return {
      {"schema_version", kAccountingSchema},
      {"kind", "Header"},
      {"channel", "accounting"},
      {"run_id", config.run_id},
      {"config_digest", config_digest},
      {"hash_algorithm", "sha256"},
      {
          "units",
          {
              {"money", "minor"},
              {"price", "ticks"},
              {"mark", "twice_ticks"},
              {"quantity", "shares"},
              {"time", "ns"},
              {"tick_size_minor", config.tick_size_minor},
          },
      },
      {"accounting", accounting(config.accounting)},
      {"accounting_basis", "average_cost"},
      {"mark_policy", {"Midpoint", "LastTrade", "Reference"}},
      {"rounding", "half_away_from_zero"},
      // Sample records list one row per agent, in this agent order and column order.
      {"agent_ids", agents},
      {
          "agent_columns",
          {
              "cash_minor",
              "position",
              "cost_basis_minor",
              "realized_pnl_minor",
              "unrealized_pnl_minor",
              "equity_minor",
          },
      },
  };
}

Json market_metrics(const RunReport& value) {
  const auto& market = value.market;
  auto rejections = Json::object();
  for (const auto& [reason, count] : market.rejections) {
    rejections.emplace(reject_reason_name(reason), count);
  }
  return {
      {"trade_count", market.trade_count},
      {"volume", market.volume.value()},
      {"traded_notional_minor", value.traded_notional.value()},
      {"vwap_milliticks", optional_number(market.vwap_milliticks)},
      {"last_trade_price_ticks", price(market.last_trade_price)},
      {"high_trade_price_ticks", price(market.high_trade_price)},
      {"low_trade_price_ticks", price(market.low_trade_price)},
      {"orders_submitted", market.orders_submitted},
      {"orders_accepted", market.orders_accepted},
      {"orders_rejected", market.orders_rejected},
      {"rejections", rejections},
      {"cancels_requested", market.cancels_requested},
      {"cancels_succeeded", market.cancels_succeeded},
      {"submitted_quantity", market.submitted_quantity.value()},
      {"filled_quantity", market.filled_quantity.value()},
      {"expired_quantity", market.expired_quantity.value()},
      {"canceled_quantity", market.canceled_quantity.value()},
      {"fill_rate_ppm", optional_number(market.fill_rate_ppm)},
      {"total_ns", market.total_ns},
      {"two_sided_ns", market.two_sided_ns},
      {"bid_only_ns", market.bid_only_ns},
      {"ask_only_ns", market.ask_only_ns},
      {"empty_ns", market.empty_ns},
      {"samples", market.samples},
      {"two_sided_samples", market.two_sided_samples},
      {"bid_samples", market.bid_samples},
      {"ask_samples", market.ask_samples},
      {"mean_spread_milliticks", optional_number(market.mean_spread_milliticks)},
      {"mean_relative_spread_millibps", optional_number(market.mean_relative_spread_millibps)},
      {
          "mean_best_bid_quantity_millishares",
          optional_number(market.mean_best_bid_quantity_millishares),
      },
      {
          "mean_best_ask_quantity_millishares",
          optional_number(market.mean_best_ask_quantity_millishares),
      },
      {"depth_levels", optional_number(market.depth_levels)},
      {"mean_bid_depth_millishares", optional_number(market.mean_bid_depth_millishares)},
      {"mean_ask_depth_millishares", optional_number(market.mean_ask_depth_millishares)},
      {"return_observations", market.return_observations},
      {"midpoint_return_volatility_ppb", optional_number(market.midpoint_return_volatility_ppb)},
  };
}

Json agent_report(const AgentReport& value) {
  const auto& account = value.account;
  const auto& flow = value.flow;
  const auto valued = [&](Money Valuation::* member) {
    return value.valuation ? Json(((*value.valuation).*member).value()) : Json(nullptr);
  };
  return {
      {"agent_id", account.agent.value()},
      {"policy", value.policy.empty() ? Json(nullptr) : Json(value.policy)},
      {"initial_cash_minor", account.initial_cash.value()},
      {"cash_minor", account.cash.value()},
      {"min_cash_minor", account.min_cash.value()},
      {"credit_limit_minor", money(value.credit_limit)},
      {"cash_refusals", value.cash_refusals},
      {"position", account.position},
      {"max_abs_position", account.max_abs_position},
      {"mean_abs_position_millishares", optional_number(value.mean_abs_position_millishares)},
      {"cost_basis_minor", account.cost_basis.value()},
      {"realized_pnl_minor", account.realized_pnl.value()},
      {"fees_paid_minor", account.fees_paid.value()},
      {"market_value_minor", valued(&Valuation::market_value)},
      {"unrealized_pnl_minor", valued(&Valuation::unrealized_pnl)},
      {"equity_minor", valued(&Valuation::equity)},
      {"total_pnl_minor", valued(&Valuation::total_pnl)},
      {"equity_observations", value.equity_observations},
      {"peak_equity_minor", money(value.peak_equity)},
      {"max_drawdown_minor", money(value.max_drawdown)},
      {"gross_notional_minor", account.gross_notional.value()},
      {"bought_quantity", account.bought.value()},
      {"sold_quantity", account.sold.value()},
      {"fills", account.fills},
      {"orders_submitted", flow.orders_submitted},
      {"orders_accepted", flow.orders_accepted},
      {"orders_rejected", flow.orders_rejected},
      {"submitted_quantity", flow.submitted_quantity.value()},
      {"maker_fills", flow.maker_fills},
      {"taker_fills", flow.taker_fills},
      {"maker_quantity", flow.maker_quantity.value()},
      {"taker_quantity", flow.taker_quantity.value()},
      {"expired_quantity", flow.expired_quantity.value()},
      {"canceled_quantity", flow.canceled_quantity.value()},
      {"resting_quantity", value.resting_quantity.value()},
      {"cancels_requested", flow.cancels_requested},
      {"cancels_succeeded", flow.cancels_succeeded},
      {"fill_rate_ppm", optional_number(value.fill_rate_ppm)},
      {"maker_fills_measured", flow.maker_fills_measured},
      {"taker_fills_measured", flow.taker_fills_measured},
      {"maker_spread_capture_minor", value.maker_spread_capture.value()},
      {"taker_spread_cost_minor", value.taker_spread_cost.value()},
  };
}

Json metrics(const RunConfig& config, const RunSummary& summary) {
  const auto& report = summary.report;
  auto agents = Json::array();
  for (const auto& agent : report.agents) {
    agents.push_back(agent_report(agent));
  }
  auto marks = Json::object();
  for (std::size_t source = 0; source < report.samples_by_mark.size(); ++source) {
    marks.emplace(mark_source_name(static_cast<MarkSource>(source)),
                  report.samples_by_mark.at(source));
  }
  return {
      {"schema_version", kMetricsSchema},
      {"kind", "RunMetrics"},
      {"run_id", config.run_id},
      {"config_digest", summary.config_digest},
      {"final_time_ns", summary.final_time.value()},
      {
          "units",
          {
              {"money", "minor"},
              {"price", "ticks"},
              {"mark", "twice_ticks"},
              {"quantity", "shares"},
              {"time", "ns"},
              {"tick_size_minor", config.tick_size_minor},
          },
      },
      {"accounting_basis", "average_cost"},
      {"mark_policy", {"Midpoint", "LastTrade", "Reference"}},
      {"rounding", "half_away_from_zero"},
      {
          "fees",
          {
              {"maker_ppm", config.accounting.fees.maker_ppm},
              {"taker_ppm", config.accounting.fees.taker_ppm},
          },
      },
      {"final_mark", mark(report.final_mark)},
      {"samples_by_mark", marks},
      {
          "conservation",
          {
              {"initial_cash_minor", report.totals.initial_cash.value()},
              {"cash_minor", report.totals.cash.value()},
              {"fee_sink_minor", report.totals.fee_sink.value()},
              {"initial_position", report.totals.initial_position},
              {"position", report.totals.position},
          },
      },
      {"market", market_metrics(report)},
      {"agents", agents},
  };
}

} // namespace markettwin::codec
