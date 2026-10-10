#pragma once

#include "markettwin/common/types.hpp"

namespace markettwin {

using OrderId = StrongInteger<struct OrderIdTag, std::uint64_t>;
using TradeId = StrongInteger<struct TradeIdTag, std::uint64_t>;
using AgentId = StrongInteger<struct AgentIdTag, std::uint64_t>;
using SymbolId = StrongInteger<struct SymbolIdTag, std::uint64_t>;

} // namespace markettwin
