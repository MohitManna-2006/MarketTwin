#pragma once

#include "markettwin/common/types.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace markettwin {

class RunError : public std::runtime_error {
public:
  // Stable machine-readable code first, human-readable detail second.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  RunError(std::string code, const std::string& message, Sequence sequence = {})
      : std::runtime_error(message), code_(std::move(code)), sequence_(sequence) {}
  [[nodiscard]] const std::string& code() const noexcept { return code_; }
  [[nodiscard]] Sequence sequence() const noexcept { return sequence_; }

private:
  std::string code_;
  Sequence sequence_;
};

class SequenceCounter {
public:
  explicit SequenceCounter(Sequence last = {}) : last_(last) {}
  [[nodiscard]] Sequence next() {
    if (last_.value() == std::numeric_limits<std::uint64_t>::max()) {
      throw RunError{"SequenceOverflow", "sequence space exhausted", last_};
    }
    last_ = Sequence{last_.value() + 1};
    return last_;
  }

private:
  Sequence last_;
};

} // namespace markettwin
