#include "codec.hpp"

#include "markettwin/common/hash.hpp"
#include "markettwin/common/run_error.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <set>

namespace markettwin::codec {
namespace {

constexpr std::array<std::string_view, 2> kSides{"Buy", "Sell"};
constexpr std::array<std::string_view, 2> kOrderTypes{"Limit", "Market"};
constexpr std::array<std::string_view, 2> kTifs{"Gtc", "Ioc"};
constexpr std::array<std::string_view, 5> kStates{
    "Rejected", "Resting", "PartiallyFilled", "Filled", "Canceled",
};
constexpr std::array<std::string_view, 21> kReasons{
    "None",         "InvalidOrderId",   "DuplicateOrderId", "UnknownSymbol",
    "InvalidOwner", "InvalidSide",      "InvalidOrderType", "InvalidTimeInForce",
    "InvalidPrice", "InvalidQuantity",  "QuantityLimit",    "InvalidArrival",
    "SelfTrade",    "QuantityOverflow", "ActiveOrderLimit", "AcceptedOrderLimit",
    "TradeLimit",   "OrderNotFound",    "NotOwner",         "InvalidConfig",
    "InvalidDepth",
};
constexpr std::array<std::string_view, 4> kStops{
    "QueueEmpty",
    "Horizon",
    "EventLimit",
    "Requested",
};

Order read_order(const Json& value) {
  fields(value,
         {"kind", "id", "owner", "symbol", "side", "type", "limit_price_ticks", "quantity", "tif"});
  Order result;
  result.id = OrderId{unsigned_integer(value.at("id"))};
  result.owner = AgentId{unsigned_integer(value.at("owner"))};
  result.symbol = SymbolId{unsigned_integer(value.at("symbol"))};
  result.side = read_enum<Side>(value.at("side"), kSides);
  result.type = read_enum<OrderType>(value.at("type"), kOrderTypes);
  result.limit_price = PriceTicks{signed_integer(value.at("limit_price_ticks"))};
  result.original_quantity = Quantity{unsigned_integer(value.at("quantity"))};
  result.remaining_quantity = result.original_quantity;
  result.tif = read_enum<TimeInForce>(value.at("tif"), kTifs);
  return result;
}

Json exchange_config(const ExchangeConfig& value) {
  auto agents = value.agents;
  std::ranges::sort(agents);
  auto ids = Json::array();
  for (const auto id : agents) {
    ids.push_back(id.value());
  }
  return {
      {"symbol", value.symbol.value()},
      {"agents", ids},
      {"max_order_quantity", value.max_order_quantity.value()},
      {"max_active_orders", value.max_active_orders},
      {"max_accepted_orders", value.max_accepted_orders},
      {"max_trades", value.max_trades},
      {"max_snapshot_depth", value.max_snapshot_depth},
  };
}

ExchangeConfig read_exchange(const Json& value) {
  fields(value, {
                    "symbol",
                    "agents",
                    "max_order_quantity",
                    "max_active_orders",
                    "max_accepted_orders",
                    "max_trades",
                    "max_snapshot_depth",
                });
  ExchangeConfig result;
  result.symbol = SymbolId{unsigned_integer(value.at("symbol"))};
  const auto& agents = value.at("agents");
  if (!agents.is_array()) {
    invalid("agents must be an array");
  }
  for (const auto& id : agents) {
    result.agents.emplace_back(unsigned_integer(id));
  }
  result.max_order_quantity = Quantity{unsigned_integer(value.at("max_order_quantity"))};
  result.max_active_orders = size_integer(value.at("max_active_orders"));
  result.max_accepted_orders = size_integer(value.at("max_accepted_orders"));
  result.max_trades = unsigned_integer(value.at("max_trades"));
  result.max_snapshot_depth = size_integer(value.at("max_snapshot_depth"));
  return result;
}

constexpr std::array<std::string_view, 10> kScriptedFields{
    "schema_version", "run_id",     "seed",      "tick_size_minor", "exchange",
    "horizon_ns",     "max_events", "max_queue", "max_trace_bytes", "commands",
};
constexpr std::array<std::string_view, 11> kScenarioFields{
    "schema_version",  "run_id",     "seed",       "tick_size_minor",
    "exchange",        "horizon_ns", "max_events", "max_queue",
    "max_trace_bytes", "commands",   "scenario",
};
constexpr std::array<std::string_view, 12> kAccountingFields{
    "schema_version",  "run_id",     "seed",       "tick_size_minor",
    "exchange",        "horizon_ns", "max_events", "max_queue",
    "max_trace_bytes", "commands",   "scenario",   "accounting",
};
constexpr std::array<std::string_view, 13> kForkFields{
    "schema_version", "run_id",     "seed",      "tick_size_minor", "exchange",
    "horizon_ns",     "max_events", "max_queue", "max_trace_bytes", "commands",
    "scenario",       "accounting", "forks",
};

void require_fields(const Json& value, std::span<const std::string_view> required) {
  if (!value.is_object() || value.size() != required.size()) {
    invalid("object has missing or unknown fields");
  }
  for (const auto name : required) {
    if (!value.contains(name)) {
      invalid("missing field: " + std::string{name});
    }
  }
}

} // namespace

std::string_view reject_reason_name(RejectReason value) {
  return enum_name(value, kReasons);
}

void invalid(std::string_view message) {
  throw RunError{"InvalidConfig", std::string{message}};
}

Json parse(std::string_view input) {
  // Object stack catches duplicate keys before the DOM could silently overwrite them.
  std::vector<std::set<std::string>> keys;
  const auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
    if (depth > 32) {
      throw RunError{"InvalidJson", "JSON nesting exceeds 32 levels"};
    }
    if (event == Json::parse_event_t::object_start) {
      keys.emplace_back();
    } else if (event == Json::parse_event_t::object_end) {
      keys.pop_back();
    } else if (event == Json::parse_event_t::key &&
               !keys.back().insert(parsed.get<std::string>()).second) {
      throw RunError{"InvalidJson", "duplicate JSON object key"};
    }
    return true;
  };
  try {
    return Json::parse(input, callback);
  } catch (const Json::exception& error) {
    throw RunError{"InvalidJson", error.what()};
  }
}

void fields(const Json& value, std::initializer_list<std::string_view> required) {
  require_fields(value, required);
}

std::uint64_t unsigned_integer(const Json& value) {
  if (value.is_number_unsigned()) {
    return value.get<std::uint64_t>();
  }
  if (value.is_number_integer()) {
    const auto number = value.get<std::int64_t>();
    if (number >= 0) {
      return static_cast<std::uint64_t>(number);
    }
  }
  invalid("expected an unsigned 64-bit integer");
}

std::int64_t signed_integer(const Json& value) {
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    if (number <= std::numeric_limits<std::int64_t>::max()) {
      return static_cast<std::int64_t>(number);
    }
  } else if (value.is_number_integer()) {
    return value.get<std::int64_t>();
  }
  invalid("expected a signed 64-bit integer");
}

std::size_t size_integer(const Json& value) {
  const auto number = unsigned_integer(value);
  if (number > std::numeric_limits<std::size_t>::max()) {
    invalid("size exceeds platform capacity");
  }
  return number;
}

std::string text(const Json& value) {
  if (!value.is_string()) {
    invalid("expected a string");
  }
  return value.get<std::string>();
}

Json command(const Command& value) {
  if (const auto* input = std::get_if<Order>(&value)) {
    return {
        {"kind", "Submit"},
        {"id", input->id.value()},
        {"owner", input->owner.value()},
        {"symbol", input->symbol.value()},
        {"side", enum_name(input->side, kSides)},
        {"type", enum_name(input->type, kOrderTypes)},
        {"limit_price_ticks", input->limit_price.value()},
        {"quantity", input->original_quantity.value()},
        {"tif", enum_name(input->tif, kTifs)},
    };
  }
  if (const auto* input = std::get_if<CancelCommand>(&value)) {
    return {
        {"kind", "Cancel"},
        {"order_id", input->order_id.value()},
        {"owner", input->owner.value()},
    };
  }
  if (const auto* input = std::get_if<SnapshotCommand>(&value)) {
    return {{"kind", "Snapshot"}, {"depth", input->depth}};
  }
  if (!std::holds_alternative<StopCommand>(value)) {
    invalid("invalid command variant");
  }
  return {{"kind", "Stop"}};
}

Command read_command(const Json& value) {
  if (!value.is_object() || !value.contains("kind")) {
    invalid("command requires kind");
  }
  const auto kind = text(value.at("kind"));
  if (kind == "Submit") {
    return read_order(value);
  }
  if (kind == "Cancel") {
    fields(value, {"kind", "order_id", "owner"});
    return CancelCommand{
        .order_id = OrderId{unsigned_integer(value.at("order_id"))},
        .owner = AgentId{unsigned_integer(value.at("owner"))},
    };
  }
  if (kind == "Snapshot") {
    fields(value, {"kind", "depth"});
    return SnapshotCommand{.depth = size_integer(value.at("depth"))};
  }
  if (kind == "Stop") {
    fields(value, {"kind"});
    return StopCommand{};
  }
  invalid("unknown command kind: " + kind);
}

Json config(const RunConfig& value) {
  auto commands = Json::array();
  for (const auto& item : value.commands) {
    commands.push_back(
        {{"timestamp_ns", item.timestamp.value()}, {"command", command(item.command)}});
  }
  // The lowest schema that can express the config. A scripted or scenario config with
  // default accounting keeps its version 1 or 2 bytes, and so its earlier digests.
  const bool forked = !value.forks.empty() || has_desk(value);
  const bool accounted = forked || value.accounting != AccountingConfig{};
  auto schema = kScriptedConfigSchema;
  if (forked) {
    schema = kForkConfigSchema;
  } else if (accounted) {
    schema = kAccountingConfigSchema;
  } else if (value.scenario) {
    schema = kScenarioConfigSchema;
  }
  Json output{
      {"schema_version", schema},
      {"run_id", value.run_id},
      {"seed", value.seed},
      {"tick_size_minor", value.tick_size_minor},
      {"exchange", exchange_config(value.exchange)},
      {"horizon_ns", value.limits.horizon.value()},
      {"max_events", value.limits.max_events},
      {"max_queue", value.limits.max_queue},
      {"max_trace_bytes", value.max_trace_bytes},
      {"commands", commands},
  };
  if (accounted) {
    output.update({
        {"scenario", value.scenario ? scenario(*value.scenario) : Json(nullptr)},
        {"accounting", accounting(value.accounting)},
    });
  } else if (value.scenario) {
    output.update({{"scenario", scenario(*value.scenario)}});
  }
  if (forked) {
    auto stages = Json::array();
    for (const auto& stage : value.forks) {
      stages.push_back(fork_stage(stage));
    }
    output.update({{"forks", stages}});
  }
  return output;
}

RunConfig read_config(const Json& value) {
  const auto version = value.is_object() && value.contains("schema_version")
                           ? unsigned_integer(value.at("schema_version"))
                           : 0;
  if (version == kScriptedConfigSchema) {
    require_fields(value, kScriptedFields);
  } else if (version == kScenarioConfigSchema) {
    require_fields(value, kScenarioFields);
  } else if (version == kAccountingConfigSchema) {
    require_fields(value, kAccountingFields);
  } else if (version == kForkConfigSchema) {
    require_fields(value, kForkFields);
  } else {
    throw RunError{"UnsupportedSchema", "expected run config schema version 1, 2, 3, or 4"};
  }
  RunConfig result;
  result.run_id = text(value.at("run_id"));
  result.seed = unsigned_integer(value.at("seed"));
  result.tick_size_minor = unsigned_integer(value.at("tick_size_minor"));
  result.exchange = read_exchange(value.at("exchange"));
  result.limits.horizon = SimTime{unsigned_integer(value.at("horizon_ns"))};
  result.limits.max_events = unsigned_integer(value.at("max_events"));
  result.limits.max_queue = size_integer(value.at("max_queue"));
  result.max_trace_bytes = unsigned_integer(value.at("max_trace_bytes"));
  const auto& commands = value.at("commands");
  if (!commands.is_array()) {
    invalid("commands must be an array");
  }
  for (const auto& item : commands) {
    fields(item, {"timestamp_ns", "command"});
    result.commands.push_back({
        .timestamp = SimTime{unsigned_integer(item.at("timestamp_ns"))},
        .command = read_command(item.at("command")),
    });
  }
  // Schema 2 requires a scenario; schema 3 may carry null for a purely scripted run.
  const bool accounted = version == kAccountingConfigSchema || version == kForkConfigSchema;
  if (version == kScenarioConfigSchema || (accounted && !value.at("scenario").is_null())) {
    result.scenario = read_scenario(value.at("scenario"));
  }
  if (accounted) {
    result.accounting = read_accounting(value.at("accounting"));
  }
  if (version == kForkConfigSchema) {
    if (!value.at("forks").is_array()) {
      invalid("forks must be an array");
    }
    for (const auto& item : value.at("forks")) {
      result.forks.push_back(read_fork_stage(item));
    }
    // Schema 4 exists only for forks and desks, so each config has one spelling.
    if (result.forks.empty() && !has_desk(result)) {
      invalid("schema 4 needs a fork stage or an AI desk agent");
    }
  }
  validate_config(result);
  return result;
}

Json order(const Order& value) {
  auto output = command(Command{value});
  output.erase("kind");
  output.update({
      {"remaining_quantity", value.remaining_quantity.value()},
      {"arrival_time_ns", value.arrival_time.value()},
      {"arrival_sequence", value.arrival_sequence.value()},
  });
  return output;
}

std::string trade(const Trade& value) {
  return ObjectWriter{}
      .number("aggressor_order_id", value.aggressor_order_id.value())
      .number("arrival_sequence", value.arrival_sequence.value())
      .number("buy_order_id", value.buy_order_id.value())
      .number("execution_sequence", value.sequence.value())
      .number("id", value.id.value())
      .number("maker_agent_id", value.maker_agent_id.value())
      .number("price_ticks", value.price_ticks.value())
      .number("quantity", value.quantity.value())
      .number("resting_order_id", value.resting_order_id.value())
      .number("sell_order_id", value.sell_order_id.value())
      .number("symbol", value.symbol.value())
      .number("taker_agent_id", value.taker_agent_id.value())
      .number("timestamp_ns", value.timestamp.value())
      .finish();
}

std::string status(const OrderStatus& value) {
  return ObjectWriter{}
      .number("canceled_quantity", value.canceled_quantity.value())
      .number("filled_quantity", value.filled_quantity.value())
      .number("order_id", value.order_id.value())
      .number("owner", value.owner.value())
      .name("reason", enum_name(value.reason, kReasons))
      .number("resting_quantity", value.resting_quantity.value())
      .name("state", enum_name(value.state, kStates))
      .finish();
}

Json snapshot(const OrderBookSnapshot& value) {
  const auto levels = [](const auto& input) {
    auto output = Json::array();
    for (const auto& level : input) {
      output.push_back({
          {"price_ticks", level.price.value()},
          {"quantity", level.quantity.value()},
          {"order_count", level.order_count},
      });
    }
    return output;
  };
  return {
      {"symbol", value.symbol.value()},
      {"bids", levels(value.bids)},
      {"asks", levels(value.asks)},
      {
          "best_bid_ticks",
          value.bids.empty() ? Json(nullptr) : Json(value.bids.front().price.value()),
      },
      {
          "best_ask_ticks",
          value.asks.empty() ? Json(nullptr) : Json(value.asks.front().price.value()),
      },
  };
}

Json terminal(const RunSummary& value) {
  Json output{
      {"status", "Completed"},
      {"stop_reason", enum_name(value.stop_reason, kStops)},
      {"event_count", value.event_count},
      {"trade_count", value.trade_count},
      {"seed", value.seed},
      {"final_time_ns", value.final_time.value()},
      {"resting_order_count", value.resting_order_count},
      {"book_digest", value.book_digest},
      {"trade_digest", value.trade_digest},
  };
  if (value.scenario) {
    output.update({{"scenario", scenario_summary(*value.scenario)}});
  }
  return output;
}

Json summary(const RunSummary& value) {
  auto output = terminal(value);
  output.update({
      {"config_digest", value.config_digest},
      {"commands_digest", value.commands_digest},
      {"trace_digest", value.trace_digest},
  });
  return output;
}

std::string book_digest(const MatchingEngine& exchange) {
  auto orders = Json::array();
  for (const auto& resting : exchange.book().ordered_orders()) {
    orders.push_back(order(resting));
  }
  return sha256(Json{{"symbol", exchange.symbol().value()}, {"orders", orders}}.dump());
}

std::string read_file(const std::filesystem::path& path, std::size_t limit) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    throw RunError{"IoError", "cannot open " + path.string()};
  }
  std::string output;
  std::array<char, 4096> buffer{};
  while (input) {
    input.read(buffer.data(), buffer.size());
    const auto count = static_cast<std::size_t>(input.gcount());
    if (count > limit - output.size()) {
      throw RunError{"ResourceLimit", "file exceeds byte limit: " + path.string()};
    }
    output.append(buffer.data(), count);
  }
  if (!input.eof()) {
    throw RunError{"IoError", "failed reading " + path.string()};
  }
  return output;
}

} // namespace markettwin::codec
