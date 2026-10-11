#ifndef MARKETTWIN_EXPERIMENT_STATISTICS_HPP
#define MARKETTWIN_EXPERIMENT_STATISTICS_HPP

#include <cstdint>
#include <optional>
#include <span>

namespace markettwin {

// Statistics over per-run metric values. Inputs are integers in the metric's own unit;
// results are integers in thousandths of that unit ("milli"), rounded half away from
// zero. Nothing here uses floating point, so a result is the same on every toolchain.
//
// Supported range: at most 100,000 values, each within +/- 1e12. Anything else throws
// RunError{"StatisticsRange"} instead of losing precision.
inline constexpr std::size_t kMaxStatisticValues = 100'000;
inline constexpr std::int64_t kMaxStatisticMagnitude = 1'000'000'000'000;
inline constexpr std::uint64_t kMaxBootstrapResamples = 1'000'000;

struct Interval {
  std::int64_t lower_milli = 0;
  std::int64_t upper_milli = 0;
  bool operator==(const Interval&) const = default;
};

// resamples = 0 turns the bootstrap interval off.
struct BootstrapConfig {
  std::uint64_t resamples = 10'000;
  std::uint64_t seed = 0;
  bool operator==(const BootstrapConfig&) const = default;
};

struct Summary {
  std::uint64_t count = 0;
  std::int64_t minimum = 0; // metric units
  std::int64_t maximum = 0;
  std::int64_t mean_milli = 0;
  std::int64_t median_milli = 0; // the mean of the two middle values when count is even
  // Sample standard deviation with n - 1 in the denominator. Needs two values.
  std::optional<std::int64_t> standard_deviation_milli;
  // Two-sided 95 % Student t interval for the mean: mean +/- t(0.975, n - 1) * s / sqrt(n).
  // Exact coverage assumes independent, normally distributed values. Needs two values.
  std::optional<Interval> mean_t_interval;
  // Two-sided 95 % percentile bootstrap interval for the mean: the values are resampled
  // with replacement `resamples` times, and the interval runs from the k-th smallest to
  // the k-th largest resampled mean, k = floor(0.025 * resamples) + 1. It assumes only
  // independent values, and is known to be somewhat narrow for small samples.
  std::optional<Interval> mean_bootstrap_interval;
  bool operator==(const Summary&) const = default;
};

// Empty input has no summary. stream_id selects the bootstrap's random stream, so every
// metric resamples independently and adding a metric never changes another's interval.
[[nodiscard]] std::optional<Summary> summarize(std::span<const std::int64_t> values,
                                               const BootstrapConfig& bootstrap = {.resamples = 0},
                                               std::uint64_t stream_id = 0);

// The 0.975 quantile of Student's t distribution in millionths: a table for 1 to 30
// degrees of freedom and a four-term Cornish-Fisher series beyond, accurate to 1e-6.
[[nodiscard]] std::int64_t student_t_975_micro(std::uint64_t degrees_of_freedom);

} // namespace markettwin
#endif // MARKETTWIN_EXPERIMENT_STATISTICS_HPP
