#pragma once

#include "markettwin/simulation/config.hpp"
#include "markettwin/simulation/fundamental.hpp"
#include "markettwin/simulation/simulation.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <span>
#include <string_view>

namespace markettwin::codec {

using Json = nlohmann::json;
inline constexpr std::size_t kMaxConfigBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaxRecordBytes = 1024 * 1024;

inline constexpr std::uint64_t kScriptedConfigSchema = 1;
inline constexpr std::uint64_t kScenarioConfigSchema = 2;
// Schema 2 with a required accounting object and a scenario that may be null.
inline constexpr std::uint64_t kAccountingConfigSchema = 3;
inline constexpr std::uint64_t kManifestSchema = 2;

[[noreturn]] void invalid(std::string_view message);
[[nodiscard]] Json parse(std::string_view input);
void fields(const Json& value, std::initializer_list<std::string_view> required);
[[nodiscard]] std::uint64_t unsigned_integer(const Json& value);
[[nodiscard]] std::int64_t signed_integer(const Json& value);
[[nodiscard]] std::size_t size_integer(const Json& value);
[[nodiscard]] std::string text(const Json& value);
[[nodiscard]] Json order(const Order& value);
[[nodiscard]] Json command(const Command& value);
[[nodiscard]] Command read_command(const Json& value);
[[nodiscard]] Json config(const RunConfig& value);
[[nodiscard]] RunConfig read_config(const Json& value);
// Trades and order statuses are most of a trace, so these two write canonical text
// directly instead of building a JSON tree first.
[[nodiscard]] std::string trade(const Trade& value);
[[nodiscard]] std::string status(const OrderStatus& value);
[[nodiscard]] Json snapshot(const OrderBookSnapshot& value);
[[nodiscard]] Json terminal(const RunSummary& value);
[[nodiscard]] Json summary(const RunSummary& value);
[[nodiscard]] std::string book_digest(const MatchingEngine& exchange);
[[nodiscard]] std::string read_file(const std::filesystem::path& path, std::size_t limit);

// Writes one flat canonical object. Members must be added in sorted key order, and
// names must be plain identifiers that need no escaping.
class ObjectWriter {
public:
  template <std::integral Number> ObjectWriter& number(std::string_view key, Number value) {
    std::array<char, 24> digits{};
    const auto end = std::to_chars(digits.data(), digits.data() + digits.size(), value).ptr;
    member(key).append(digits.data(), end);
    return *this;
  }
  ObjectWriter& name(std::string_view key, std::string_view value) {
    member(key).append("\"").append(value).append("\"");
    return *this;
  }
  // A member whose value is already canonical JSON text.
  ObjectWriter& raw(std::string_view key, std::string_view value) {
    member(key).append(value);
    return *this;
  }
  [[nodiscard]] std::string finish() {
    text_.push_back('}');
    return std::move(text_);
  }

private:
  std::string& member(std::string_view key) {
    text_.push_back(text_.empty() ? '{' : ',');
    return text_.append("\"").append(key).append("\":");
  }
  std::string text_;
};

// Scenario records (scenario_codec.cpp).
[[nodiscard]] Json scenario(const ScenarioConfig& value);
[[nodiscard]] ScenarioConfig read_scenario(const Json& value);
[[nodiscard]] Json policy(const PolicyConfig& value);
[[nodiscard]] PolicyConfig read_policy(const Json& value);
[[nodiscard]] Json shock_transition(std::size_t index, ShockPhase phase, const Shock& value);
[[nodiscard]] Json fundamental_update(const FundamentalProcess& value);
[[nodiscard]] Json scenario_summary(const ScenarioSummary& value);
[[nodiscard]] std::string rng_digest(const Rng::State& fundamental,
                                     const std::vector<Rng::State>& agents);

// Accounting configuration, records, and metrics (accounting_codec.cpp).
[[nodiscard]] Json accounting(const AccountingConfig& value);
[[nodiscard]] AccountingConfig read_accounting(const Json& value);
[[nodiscard]] Json accounting_header(const RunConfig& config, std::string_view config_digest,
                                     const std::vector<Account>& accounts);
[[nodiscard]] Json market_metrics(const RunReport& value);
[[nodiscard]] Json agent_report(const AgentReport& value);
// The complete metrics.json document for a finished run.
[[nodiscard]] Json metrics(const RunConfig& config, const RunSummary& summary);
[[nodiscard]] std::string_view reject_reason_name(RejectReason value);

// Enumerators serialize by spelling, in declaration order.
template <typename Enum, std::size_t Size>
[[nodiscard]] std::string_view enum_name(Enum value,
                                         const std::array<std::string_view, Size>& names) {
  const auto index = static_cast<std::size_t>(value);
  if (index >= names.size()) {
    invalid("unknown enum value");
  }
  return names.at(index);
}

template <typename Enum, std::size_t Size>
[[nodiscard]] Enum read_enum(const Json& value, const std::array<std::string_view, Size>& names) {
  const auto name = text(value);
  const auto found = std::ranges::find(names, name);
  if (found == names.end()) {
    invalid("unknown enum spelling: " + name);
  }
  return static_cast<Enum>(std::distance(names.begin(), found));
}

} // namespace markettwin::codec
