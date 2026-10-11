#include "markettwin/experiment/statistics.hpp"

#include "markettwin/accounting/money.hpp"
#include "markettwin/common/rng.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace markettwin {
namespace {

// t(0.975, df) * 1e6 for df = 1..30.
constexpr std::array<std::int64_t, 30> kStudentT975Micro{
    12'706'205, 4'302'653, 3'182'446, 2'776'445, 2'570'582, 2'446'912, 2'364'624, 2'306'004,
    2'262'157,  2'228'139, 2'200'985, 2'178'813, 2'160'369, 2'144'787, 2'131'450, 2'119'905,
    2'109'816,  2'100'922, 2'093'024, 2'085'963, 2'079'614, 2'073'873, 2'068'658, 2'063'899,
    2'059'539,  2'055'529, 2'051'831, 2'048'407, 2'045'230, 2'042'272,
};
// Cornish-Fisher expansion of the same quantile around the normal one, in billionths:
//   t = z + g1 / df + g2 / df^2 + g3 / df^3 + g4 / df^4,  z = 1.959963985
constexpr std::int64_t kNormal975Nano = 1'959'963'985;
constexpr std::array<std::int64_t, 4> kCornishFisherNano{
    2'372'271'230,
    2'822'498'616,
    2'555'849'680,
    1'589'534'053,
};

// floor(numerator * scale / denominator) without forming numerator * scale, which could
// exceed 128 bits. The quotient times the scale must fit; callers bound it.
UInt128 scaled_quotient(UInt128 numerator, UInt128 denominator, UInt128 scale) {
  return ((numerator / denominator) * scale) + (((numerator % denominator) * scale) / denominator);
}

std::int64_t rounded_sqrt(UInt128 value) {
  return static_cast<std::int64_t>((integer_sqrt(value * 4) + 1) / 2);
}

std::int64_t mean_milli(Int128 sum, std::size_t count) {
  return narrow(divide_round(sum * 1'000, static_cast<Int128>(count)));
}

// Resamples the sorted values, so the interval does not depend on the order runs finished.
Interval bootstrap_interval(const std::vector<std::int64_t>& values, const BootstrapConfig& config,
                            std::uint64_t stream_id) {
  Rng rng{config.seed, RngStream::Bootstrap, stream_id};
  std::vector<std::int64_t> sums(config.resamples);
  for (auto& sum : sums) {
    for (std::size_t draw = 0; draw < values.size(); ++draw) {
      sum += values.at(rng.uniform_below(values.size())); // within 1e17 by the input bounds
    }
  }
  std::ranges::sort(sums);
  const std::size_t tail = (config.resamples * 25) / 1'000;
  return {
      .lower_milli = mean_milli(sums.at(tail), values.size()),
      .upper_milli = mean_milli(sums.at(sums.size() - 1 - tail), values.size()),
  };
}

} // namespace

std::int64_t student_t_975_micro(std::uint64_t degrees_of_freedom) {
  if (degrees_of_freedom == 0) {
    throw RunError{"StatisticsRange", "a t quantile needs at least one degree of freedom"};
  }
  if (degrees_of_freedom <= kStudentT975Micro.size()) {
    return kStudentT975Micro.at(degrees_of_freedom - 1);
  }
  const auto df = static_cast<Int128>(degrees_of_freedom);
  Int128 quantile = kNormal975Nano;
  Int128 power = 1;
  for (const auto coefficient : kCornishFisherNano) {
    power *= df;
    quantile += divide_round(coefficient, power);
  }
  return narrow(divide_round(quantile, 1'000));
}

std::optional<Summary> summarize(std::span<const std::int64_t> values,
                                 const BootstrapConfig& bootstrap, std::uint64_t stream_id) {
  if (values.empty()) {
    return std::nullopt;
  }
  const auto in_range = [](std::int64_t value) {
    return value >= -kMaxStatisticMagnitude && value <= kMaxStatisticMagnitude;
  };
  if (values.size() > kMaxStatisticValues || !std::ranges::all_of(values, in_range) ||
      bootstrap.resamples > kMaxBootstrapResamples) {
    throw RunError{"StatisticsRange",
                   "statistics support at most 100,000 values within +/- 1e12 and 1e6 resamples"};
  }
  std::vector<std::int64_t> sorted(values.begin(), values.end());
  std::ranges::sort(sorted);
  const auto count = sorted.size();
  Int128 sum = 0;
  UInt128 squares = 0;
  for (const auto value : sorted) {
    sum += value;
    squares += static_cast<UInt128>(Int128{value} * Int128{value});
  }
  Summary summary;
  summary.count = count;
  summary.minimum = sorted.front();
  summary.maximum = sorted.back();
  summary.mean_milli = mean_milli(sum, count);
  summary.median_milli = count % 2 == 1 ? sorted.at(count / 2) * 1'000
                                        : (sorted.at((count / 2) - 1) + sorted.at(count / 2)) * 500;
  if (count < 2) {
    return summary;
  }
  // variance = (n * sum(x^2) - sum(x)^2) / (n * (n - 1)), kept as an exact fraction. With
  // the input bounds the numerator is below 1e34 and the variance below 2e24.
  const auto n = static_cast<UInt128>(count);
  const auto numerator = (n * squares) - static_cast<UInt128>(sum * sum);
  const auto denominator = n * (n - 1);
  summary.standard_deviation_milli =
      rounded_sqrt(scaled_quotient(numerator, denominator, 1'000'000));
  // Standard error in millionths of a unit, so the half width keeps three decimals.
  const auto standard_error_micro =
      integer_sqrt(scaled_quotient(numerator, denominator * n, 1'000'000'000'000));
  const auto half_width = narrow(divide_round(Int128{student_t_975_micro(count - 1)} *
                                                  static_cast<Int128>(standard_error_micro),
                                              1'000'000'000));
  summary.mean_t_interval = Interval{
      .lower_milli = summary.mean_milli - half_width,
      .upper_milli = summary.mean_milli + half_width,
  };
  if (bootstrap.resamples != 0) {
    summary.mean_bootstrap_interval = bootstrap_interval(sorted, bootstrap, stream_id);
  }
  return summary;
}

} // namespace markettwin
