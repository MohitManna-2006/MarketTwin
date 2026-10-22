#pragma once

#include "markettwin/simulation/config.hpp"

namespace markettwin {

// Fork-stage helpers shared by validation and the kernel (ADR 0012).
[[nodiscard]] SimTime first_transition(const Shock& shock);
[[nodiscard]] SimTime last_transition(const Shock& shock);
// Whether a desk whose policy took effect at `anchor` makes a decision at `at`.
[[nodiscard]] bool decides_at(const ModelDirectedParams& desk, SimTime anchor, SimTime at) noexcept;
// Throws RunError{"InvalidConfig"} for a malformed stage and RunError{"ForkParentMismatch"}
// when the last stage does not lead back to its parent's config digest.
void validate_forks(const RunConfig& config);

} // namespace markettwin
