#ifndef MARKETTWIN_COMMON_VERSION_HPP
#define MARKETTWIN_COMMON_VERSION_HPP

#include <string_view>

namespace markettwin {

// Engine version configured from the top-level CMake project version.
[[nodiscard]] std::string_view version() noexcept;

} // namespace markettwin
#endif // MARKETTWIN_COMMON_VERSION_HPP
