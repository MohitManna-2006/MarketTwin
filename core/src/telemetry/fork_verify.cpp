#include "markettwin/telemetry/fork.hpp"

#include "markettwin/common/hash.hpp"
#include "markettwin/telemetry/replay.hpp"

#include "artifacts.hpp"
#include "codec.hpp"

#include <array>
#include <fstream>
#include <functional>
#include <map>
#include <optional>

namespace markettwin {
namespace {
using codec::Json;

struct Record {
  std::uint64_t sequence = 0;
  std::string line; // without the newline
};

std::uint64_t sequence_of(std::string_view line) {
  constexpr std::string_view kKey = R"(,"sequence":)";
  const auto at = line.rfind(kKey);
  if (at == std::string_view::npos) {
    throw RunError{"TraceMismatch", "a trace record has no sequence"};
  }
  std::uint64_t value = 0;
  for (auto index = at + kKey.size(); index < line.size() && line.at(index) >= '0' &&
                                      line.at(index) <= '9';
       ++index) {
    value = (value * 10) + static_cast<std::uint64_t>(line.at(index) - '0');
  }
  return value;
}

// The text between `open` and the next `close`, or the last `close` when `last` is set.
std::string_view between(std::string_view line, std::string_view open, std::string_view close,
                         bool last = false) {
  const auto start = line.find(open);
  if (start == std::string_view::npos) {
    throw RunError{"TraceMismatch", "a trace record is not in canonical form"};
  }
  const auto from = start + open.size();
  const auto end = last ? line.rfind(close) : line.find(close, from);
  if (end == std::string_view::npos || end < from) {
    throw RunError{"TraceMismatch", "a trace record is not in canonical form"};
  }
  return line.substr(from, end - from);
}

std::string_view kind_of(std::string_view line) {
  return between(line, R"(,"kind":")", R"(",")");
}

// What a record says about the market, without the fields a fork legitimately shifts:
// the run ID, recorder and scheduler sequences, causation, and digests. A trade's
// arrival_sequence is a recorder sequence too.
std::string comparable(std::string_view line) {
  std::string payload{between(line, R"(","payload":)", R"(,"payload_digest":")", true)};
  if (kind_of(line) == "Trade") {
    constexpr std::string_view kArrival = R"("arrival_sequence":)";
    if (const auto at = payload.find(kArrival); at != std::string::npos) {
      payload.erase(at, payload.find(',', at) + 1 - at);
    }
  }
  std::string key{kind_of(line)};
  // The envelope's timestamp is its last member; a trade payload has one of its own.
  constexpr std::string_view kTimestamp = R"(,"timestamp_ns":)";
  const auto stamp = line.rfind(kTimestamp);
  key.append("|").append(line.substr(stamp + kTimestamp.size()));
  key.append("|").append(between(line, R"({"actor_id":)", R"(,"causation_id":)"));
  key.append("|").append(between(line, R"(,"correlation_id":)", R"(,"kind":")"));
  key.append("|").append(payload);
  return key;
}

// Both channels of one run in global sequence order, headers skipped.
class Stream {
public:
  explicit Stream(const RunArtifacts& paths)
      : commands_(paths.commands, std::ios::binary), trace_(paths.trace, std::ios::binary) {
    if (!commands_ || !trace_) {
      throw RunError{"IncompleteArtifacts", "command or execution log is missing"};
    }
    std::string header;
    std::getline(commands_, header);
    std::getline(trace_, header);
    command_ = read(commands_);
    execution_ = read(trace_);
  }

  // The next record, and whether it came from the commands channel.
  std::optional<std::pair<Record, bool>> next() {
    if (!command_ && !execution_) {
      return std::nullopt;
    }
    const bool command = command_ && (!execution_ || command_->sequence < execution_->sequence);
    auto& slot = command ? command_ : execution_;
    auto result = std::make_pair(std::move(*slot), command);
    slot = read(command ? commands_ : trace_);
    return result;
  }

private:
  static std::optional<Record> read(std::ifstream& input) {
    std::string line;
    if (!std::getline(input, line)) {
      return std::nullopt;
    }
    return Record{.sequence = sequence_of(line), .line = std::move(line)};
  }
  std::ifstream commands_;
  std::ifstream trace_;
  std::optional<Record> command_;
  std::optional<Record> execution_;
};

std::string replace_run_id(std::string line, std::string_view from, std::string_view to) {
  const auto key = R"("run_id":")" + std::string{from} + '"';
  if (const auto at = line.find(key); at != std::string::npos) {
    line.replace(at, key.size(), R"("run_id":")" + std::string{to} + '"');
  }
  return line;
}

bool is_market(std::string_view kind) {
  return kind == "OrderArrival" || kind == "CancelArrival" || kind == "Trade";
}

Json divergence(const std::optional<Record>& parent, const std::optional<Record>& fork) {
  const auto parse = [](const std::optional<Record>& item) {
    return item ? codec::parse(item->line) : Json(nullptr);
  };
  // The earlier of the two records: where one run did something the other had not yet.
  std::optional<std::uint64_t> time;
  for (const auto* item : {&parent, &fork}) {
    if (*item) {
      const auto stamp = codec::unsigned_integer(codec::parse((*item)->line).at("timestamp_ns"));
      time = time ? std::min(*time, stamp) : stamp;
    }
  }
  return {
      {"timestamp_ns", time ? Json(*time) : Json(nullptr)},
      {"parent", parse(parent)},
      {"fork", parse(fork)},
  };
}

// The first pair of records after the prefix that `keep` selects and that differ.
Json first_difference(const RunArtifacts& parent, const RunArtifacts& fork,
                      std::uint64_t fork_point,
                      const std::function<bool(std::string_view)>& keep) {
  Stream left{parent};
  Stream right{fork};
  const auto advance = [&](Stream& stream, bool skip_point) -> std::optional<Record> {
    while (auto item = stream.next()) {
      auto& record = item->first;
      const auto kind = kind_of(record.line);
      if (record.sequence < fork_point || (skip_point && record.sequence == fork_point) ||
          kind == "RunCompleted" || !keep(kind)) {
        continue;
      }
      return std::move(record);
    }
    return std::nullopt;
  };
  while (true) {
    auto a = advance(left, false);
    auto b = advance(right, true);
    if (!a && !b) {
      return nullptr;
    }
    if (!a || !b || comparable(a->line) != comparable(b->line)) {
      return divergence(a, b);
    }
  }
}

struct Sample {
  std::uint64_t timestamp = 0;
  Json best_bid;
  Json best_ask;
};

std::vector<Sample> samples_after(const RunArtifacts& paths, std::uint64_t at) {
  std::vector<Sample> result;
  Stream stream{paths};
  while (auto item = stream.next()) {
    const auto& line = item->first.line;
    if (kind_of(line) != "BookSnapshot") {
      continue;
    }
    const auto record = codec::parse(line);
    const auto timestamp = codec::unsigned_integer(record.at("timestamp_ns"));
    if (timestamp >= at) {
      const auto& payload = record.at("payload");
      result.push_back({
          .timestamp = timestamp,
          .best_bid = payload.at("best_bid_ticks"),
          .best_ask = payload.at("best_ask_ticks"),
      });
    }
  }
  return result;
}

Json market_sample(const Json& bid, const Json& ask) {
  if (bid.is_null() || ask.is_null()) {
    return {{"mid_milliticks", nullptr}, {"spread_ticks", nullptr}};
  }
  const auto low = codec::signed_integer(bid);
  const auto high = codec::signed_integer(ask);
  return {{"mid_milliticks", (low + high) * (kMilliticks / 2)}, {"spread_ticks", high - low}};
}

Json compare_values(const Json& parent, const Json& fork) {
  const bool numbers = parent.is_number_integer() && fork.is_number_integer();
  return {
      {"parent", parent},
      {"fork", fork},
      {"difference",
       numbers ? Json(codec::signed_integer(fork) - codec::signed_integer(parent)) : Json(nullptr)},
  };
}

Json agent_differences(const Json& parent, const Json& fork) {
  constexpr std::array<std::string_view, 9> kFields{
      "total_pnl_minor",  "realized_pnl_minor", "unrealized_pnl_minor",
      "max_drawdown_minor", "position",         "max_abs_position",
      "mean_abs_position_millishares", "fills", "maker_spread_capture_minor",
  };
  auto result = Json::array();
  for (const auto& left : parent.at("agents")) {
    const auto id = left.at("agent_id");
    const auto right = std::ranges::find_if(
        fork.at("agents"), [&](const Json& item) { return item.at("agent_id") == id; });
    if (right == fork.at("agents").end()) {
      continue;
    }
    Json values = Json::object();
    for (const auto name : kFields) {
      const std::string key{name};
      values[key] = compare_values(left.value(key, Json(nullptr)), right->value(key, Json(nullptr)));
    }
    result.push_back({
        {"agent_id", id},
        {"parent_policy", left.at("policy")},
        {"fork_policy", right->at("policy")},
        {"values", values},
    });
  }
  return result;
}

// Each agent's accounting sample columns at every sample time, keyed by time.
struct Accounts {
  std::vector<std::uint64_t> agents;
  std::vector<std::string> columns;
  std::map<std::uint64_t, Json> samples;
};

Accounts read_accounts(const RunArtifacts& paths) {
  Accounts result;
  std::ifstream input{paths.accounting, std::ios::binary};
  std::string line;
  while (std::getline(input, line)) {
    if (line.starts_with(R"({"kind":"Sample")")) {
      const auto record = codec::parse(line);
      result.samples.emplace(codec::unsigned_integer(record.at("timestamp_ns")),
                             record.at("payload").at("agents"));
    } else if (line.find(R"("kind":"Header")") != std::string::npos) {
      const auto header = codec::parse(line);
      for (const auto& id : header.at("agent_ids")) {
        result.agents.push_back(codec::unsigned_integer(id));
      }
      for (const auto& name : header.at("agent_columns")) {
        result.columns.push_back(codec::text(name));
      }
    }
  }
  return result;
}

// Every agent's accounting values in both runs at the last sample time they share, so a
// fork with a shorter horizon is compared over the same window as its parent.
Json window_differences(const RunArtifacts& parent, const RunArtifacts& fork) {
  const auto left = read_accounts(parent);
  const auto right = read_accounts(fork);
  std::optional<std::uint64_t> end;
  for (const auto& [time, values] : right.samples) {
    if (left.samples.contains(time)) {
      end = time;
    }
  }
  if (!end || left.agents != right.agents || left.columns != right.columns) {
    return nullptr;
  }
  const auto& a = left.samples.at(*end);
  const auto& b = right.samples.at(*end);
  auto agents = Json::array();
  for (std::size_t index = 0; index < left.agents.size(); ++index) {
    Json values = Json::object();
    for (std::size_t column = 0; column < left.columns.size(); ++column) {
      values[left.columns.at(column)] =
          compare_values(a.at(index).at(column), b.at(index).at(column));
    }
    agents.push_back({{"agent_id", left.agents.at(index)}, {"values", values}});
  }
  return {{"end_ns", *end}, {"agents", agents}};
}

Json digests(const ReplayResult& replay) {
  const auto& summary = replay.summary;
  return {
      {"config", summary.config_digest},   {"commands", summary.commands_digest},
      {"trace", summary.trace_digest},     {"book", summary.book_digest},
      {"trade", summary.trade_digest},     {"accounting", summary.accounting_digest},
      {"metrics", summary.metrics_digest},
  };
}

ForkVerification fail(std::string code, std::string message) {
  return {.verified = false, .code = std::move(code), .message = std::move(message), .report = {}};
}

} // namespace

ForkVerification verify_fork(const std::filesystem::path& parent_directory,
                             const std::filesystem::path& fork_directory) {
  try {
    const auto parent_replay = verify_replay(parent_directory);
    if (!parent_replay.verified) {
      return fail("ParentNotVerified", parent_replay.code + ": " + parent_replay.message);
    }
    const auto fork_replay = verify_replay(fork_directory);
    if (!fork_replay.verified) {
      return fail("ForkNotVerified", fork_replay.code + ": " + fork_replay.message);
    }
    const auto parent = artifact_paths(parent_directory);
    const auto fork = artifact_paths(fork_directory);
    const auto parent_config = load_config(parent.config);
    const auto fork_config = load_config(fork.config);
    if (fork_config.forks.empty()) {
      return fail("NotAFork", "the second run has no fork stage");
    }
    const auto stage_index = fork_config.forks.size() - 1;
    const auto& stage = fork_config.forks.back();
    if (stage.parent_run_id != parent_config.run_id ||
        stage.parent_config_digest != parent_replay.summary.config_digest) {
      return fail("ParentMismatch", "the fork's last stage names a different parent");
    }

    // The prefix: every record before the fork's last ForkPoint, in both channels.
    Stream left{parent};
    Stream right{fork};
    Sha256 prefix;
    std::uint64_t commands = 0;
    std::uint64_t executions = 0;
    std::uint64_t fork_point = 0;
    while (true) {
      auto item = right.next();
      if (!item) {
        return fail("ForkPointMissing", "the fork's trace has no ForkPoint for its last stage");
      }
      const auto& line = item->first.line;
      if (kind_of(line) == "ForkPoint" &&
          codec::parse(line).at("payload").at("stage") == Json(stage_index)) {
        fork_point = item->first.sequence;
        break;
      }
      const auto other = left.next();
      if (!other || replace_run_id(other->first.line, parent_config.run_id,
                                   fork_config.run_id) != line) {
        return fail("PrefixMismatch", "the traces differ before the fork at sequence " +
                                          std::to_string(item->first.sequence));
      }
      prefix.update(line);
      prefix.update("\n");
      ++(item->second ? commands : executions);
    }

    const auto at = stage.at.value();
    const auto everything = [](std::string_view /*kind*/) { return true; };
    const auto first = first_difference(parent, fork, fork_point, everything);
    const auto market = first_difference(parent, fork, fork_point, is_market);

    const auto parent_samples = samples_after(parent, at);
    const auto fork_samples = samples_after(fork, at);
    auto samples = Json::array();
    for (std::size_t index = 0, other = 0; index < parent_samples.size(); ++index) {
      const auto& left_sample = parent_samples.at(index);
      while (other < fork_samples.size() &&
             fork_samples.at(other).timestamp < left_sample.timestamp) {
        ++other;
      }
      if (other == fork_samples.size() || fork_samples.at(other).timestamp != left_sample.timestamp) {
        continue;
      }
      const auto& right_sample = fork_samples.at(other);
      const auto a = market_sample(left_sample.best_bid, left_sample.best_ask);
      const auto b = market_sample(right_sample.best_bid, right_sample.best_ask);
      Json gap = nullptr;
      if (!a.at("mid_milliticks").is_null() && !b.at("mid_milliticks").is_null()) {
        const auto difference = codec::signed_integer(b.at("mid_milliticks")) -
                                codec::signed_integer(a.at("mid_milliticks"));
        gap = difference < 0 ? -difference : difference;
      }
      samples.push_back({
          {"timestamp_ns", left_sample.timestamp},
          {"parent", a},
          {"fork", b},
          {"mid_gap_milliticks", gap},
      });
    }

    const auto parent_metrics = codec::parse(codec::read_file(parent.metrics, codec::kMaxConfigBytes));
    const auto fork_metrics = codec::parse(codec::read_file(fork.metrics, codec::kMaxConfigBytes));
    const auto& parent_summary = parent_replay.summary;
    const auto& fork_summary = fork_replay.summary;
    const Json report{
        {"schema_version", 1},
        {"kind", "ForkReport"},
        {"label", "ONE SAMPLE PATH"},
        {"parent_run_id", parent_config.run_id},
        {"fork_run_id", fork_config.run_id},
        {"stage", stage_index},
        {"at_ns", at},
        {"fork", codec::fork_stage(stage)},
        {
            "prefix",
            {
                {"commands", commands},
                {"execution", executions},
                {"records", commands + executions},
                {"digest", prefix.digest()},
                {"fork_point_sequence", fork_point},
            },
        },
        {"divergence", {{"first", first}, {"first_market", market}}},
        {"agents", agent_differences(parent_metrics, fork_metrics)},
        {"window", window_differences(parent, fork)},
        {
            "market",
            {
                {"trades", compare_values(Json(parent_summary.trade_count),
                                          Json(fork_summary.trade_count))},
                {"events", compare_values(Json(parent_summary.event_count),
                                          Json(fork_summary.event_count))},
                {"samples", samples},
            },
        },
        {"digests", {{"parent", digests(parent_replay)}, {"fork", digests(fork_replay)}}},
    };
    return {
        .verified = true,
        .code = "Verified",
        .message = "both runs replay, the fork names its parent, and the prefix is identical",
        .report = report.dump(),
    };
  } catch (const RunError& error) {
    return fail(error.code(), error.what());
  } catch (const std::filesystem::filesystem_error& error) {
    return fail("IoError", error.what());
  }
}

Directive parse_directive(std::string_view json) {
  return codec::read_directive(codec::parse(json));
}

std::string directive_json(const Directive& directive) {
  return codec::directive(directive).dump();
}

std::vector<RecordedDecision> read_decisions(const std::filesystem::path& run) {
  std::vector<RecordedDecision> result;
  std::ifstream input{artifact_paths(run).trace, std::ios::binary};
  if (!input) {
    throw RunError{"IncompleteArtifacts", "execution log is missing"};
  }
  std::string line;
  while (std::getline(input, line)) {
    if (line.find(R"("kind":"Decision")") == std::string::npos) {
      continue;
    }
    const auto record = codec::parse(line);
    if (codec::text(record.at("kind")) == "Decision") {
      result.push_back(codec::read_decision_fields(
          record.at("payload"), SimTime{codec::unsigned_integer(record.at("timestamp_ns"))}));
    }
  }
  return result;
}

ForkStage parse_stage_request(std::string_view json) {
  auto value = codec::parse(json);
  if (!value.is_object() || !value.contains("at_ns")) {
    codec::invalid("a fork request needs at_ns");
  }
  const Json defaults{
      {"parent_run_id", ""},
      {"parent_config_digest", ""},
      {"add_shocks", Json::array()},
      {"remove_shocks", Json::array()},
      {"policy_overrides", Json::array()},
      {"horizon_ns", nullptr},
      {"decisions", "hold"},
      {"recorded_decisions", Json::array()},
      {"directive_overrides", Json::array()},
  };
  for (const auto& [key, fallback] : defaults.items()) {
    if (!value.contains(key)) {
      value[key] = fallback;
    }
  }
  return codec::read_fork_stage(value);
}

RunConfig fork_of(const std::filesystem::path& parent, ForkStage stage, std::string run_id) {
  auto config = load_config(artifact_paths(parent).config);
  stage.parent_run_id = config.run_id;
  stage.parent_config_digest = sha256(canonical_config(config));
  stage.recorded = read_decisions(parent);
  config.run_id = std::move(run_id);
  config.forks.push_back(std::move(stage));
  validate_config(config);
  return config;
}

std::string fork_verification_json(const ForkVerification& result) {
  return Json{
      {"verified", result.verified},
      {"code", result.code},
      {"message", result.message},
      {"report", result.verified ? codec::parse(result.report) : Json(nullptr)},
  }
      .dump();
}

} // namespace markettwin
