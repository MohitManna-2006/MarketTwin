#include "markettwin/common/version.hpp"

namespace markettwin {

std::string_view version() noexcept {
  return std::string_view{MARKETTWIN_VERSION};
}

} // namespace markettwin
