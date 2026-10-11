#pragma once

#include "markettwin/simulation/config.hpp"
#include "markettwin/simulation/simulation.hpp"

#include <initializer_list>
#include <nlohmann/json.hpp>
#include <string_view>

namespace markettwin::codec {

using Json = nlohmann::json;
inline constexpr std::size_t kMaxConfigBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kMaxRecordBytes = 1024 * 1024;

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
[[nodiscard]] Json trade(const Trade& value);
[[nodiscard]] Json status(const OrderStatus& value);
[[nodiscard]] Json snapshot(const OrderBookSnapshot& value);
[[nodiscard]] Json terminal(const RunSummary& value);
[[nodiscard]] Json summary(const RunSummary& value);
[[nodiscard]] std::string book_digest(const MatchingEngine& exchange);
[[nodiscard]] std::string read_file(const std::filesystem::path& path, std::size_t limit);

} // namespace markettwin::codec
