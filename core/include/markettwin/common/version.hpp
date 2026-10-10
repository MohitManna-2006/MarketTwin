#pragma once

#include <string_view>

namespace markettwin {

// Engine version configured from the top-level CMake project version.
[[nodiscard]] std::string_view version() noexcept;

} // namespace markettwin
