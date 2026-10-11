#ifndef MARKETTWIN_COMMON_RNG_HPP
#define MARKETTWIN_COMMON_RNG_HPP

#include <array>
#include <cstdint>
#include <string_view>

namespace markettwin {

// Recorded in run artifacts. Changing a generator or sampler changes this identifier.
inline constexpr std::string_view kRngAlgorithm = "xoshiro256**-splitmix64-v1";

// Each consumer draws from its own stream, so adding draws to one never shifts another.
enum class RngStream : std::uint64_t {
  Fundamental = 1,
  AgentPolicy = 2,
  AgentWakeup = 3,
  AgentLatency = 4,
  AgentSignal = 5,
};

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t& state) noexcept;

// xoshiro256** with integer-only samplers. No floating point and no standard-library
// distributions, so a seed produces the same values on every toolchain.
class Rng {
public:
  using State = std::array<std::uint64_t, 4>;

  Rng(std::uint64_t seed, RngStream stream, std::uint64_t stream_id = 0) noexcept;
  explicit Rng(const State& state);

  [[nodiscard]] std::uint64_t next() noexcept;
  // Uniform on [0, bound). Unbiased; throws if bound is zero.
  [[nodiscard]] std::uint64_t uniform_below(std::uint64_t bound);
  // Uniform on [low, high], both inclusive.
  [[nodiscard]] std::int64_t uniform_between(std::int64_t low, std::int64_t high);
  // True with probability probability_ppm / 1e6. Always consumes one bounded draw.
  [[nodiscard]] bool bernoulli_ppm(std::uint64_t probability_ppm);
  // Exponential with the given mean, truncated near 36.7 means. Mean must be below 2^56.
  [[nodiscard]] std::uint64_t exponential(std::uint64_t mean);
  // Approximately standard normal in Q16 fixed point (65536 is one standard deviation).
  // Sum of twelve 16-bit uniforms: exactly symmetric, bounded at six standard deviations.
  [[nodiscard]] std::int64_t normal_q16() noexcept;

  [[nodiscard]] const State& state() const noexcept { return state_; }

private:
  State state_{};
};

} // namespace markettwin
#endif // MARKETTWIN_COMMON_RNG_HPP
