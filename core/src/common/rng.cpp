#include "markettwin/common/rng.hpp"

#include "markettwin/common/run_error.hpp"

#include <algorithm>
#include <bit>

namespace markettwin {
namespace {

constexpr std::uint64_t kGolden = 0x9e3779b97f4a7c15ULL;
constexpr std::uint64_t kLn2Q32 = 2'977'044'472ULL; // round(ln 2 * 2^32)
constexpr std::uint64_t kLow32 = 0xffffffffULL;
constexpr unsigned kUniformBits = 53;

std::uint64_t mix(std::uint64_t value) noexcept {
  return splitmix64(value);
}

// -ln(U) in Q32 for U = x / 2^53, x in [1, 2^53], via a bitwise fixed-point log2.
std::uint64_t negative_log_q32(std::uint64_t bits) noexcept {
  const std::uint64_t x = (bits >> (64U - kUniformBits)) + 1;
  const auto exponent = static_cast<unsigned>(std::bit_width(x)) - 1U;
  std::uint64_t mantissa = exponent >= 31U ? x >> (exponent - 31U) : x << (31U - exponent); // Q31
  std::uint64_t fraction = 0;
  for (unsigned bit = 32; bit-- > 0;) {
    mantissa = (mantissa * mantissa) >> 31U;
    if (mantissa >= (1ULL << 32U)) {
      mantissa >>= 1U;
      fraction |= 1ULL << bit;
    }
  }
  const std::uint64_t log2_q32 = (std::uint64_t{exponent} << 32U) | fraction;
  const std::uint64_t negative = (std::uint64_t{kUniformBits} << 32U) - log2_q32;
  return ((negative >> 32U) * kLn2Q32) + (((negative & kLow32) * kLn2Q32) >> 32U);
}

} // namespace

std::uint64_t splitmix64(std::uint64_t& state) noexcept {
  state += kGolden;
  std::uint64_t z = state;
  z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31U);
}

Rng::Rng(std::uint64_t seed, RngStream stream, std::uint64_t stream_id) noexcept {
  std::uint64_t key = mix(mix(mix(seed) ^ static_cast<std::uint64_t>(stream)) ^ stream_id);
  for (auto& word : state_) {
    word = splitmix64(key);
  }
  if (std::ranges::all_of(state_, [](std::uint64_t word) { return word == 0; })) {
    state_.front() = kGolden; // xoshiro's single forbidden state
  }
}

Rng::Rng(const State& state) : state_(state) {
  if (std::ranges::all_of(state_, [](std::uint64_t word) { return word == 0; })) {
    throw RunError{"InvalidRng", "xoshiro256** state must not be all zero"};
  }
}

std::uint64_t Rng::next() noexcept {
  auto& [first, second, third, fourth] = state_;
  const std::uint64_t result = std::rotl(second * 5, 7) * 9;
  const std::uint64_t shifted = second << 17U;
  third ^= first;
  fourth ^= second;
  second ^= third;
  first ^= fourth;
  third ^= shifted;
  fourth = std::rotl(fourth, 45);
  return result;
}

std::uint64_t Rng::uniform_below(std::uint64_t bound) {
  if (bound == 0) {
    throw RunError{"InvalidRng", "uniform bound must be positive"};
  }
  // Mask-and-reject: unbiased, at most two expected draws, and no 128-bit arithmetic.
  const std::uint64_t mask = std::bit_ceil(bound) - 1;
  for (;;) {
    const std::uint64_t candidate = next() & mask;
    if (candidate < bound) {
      return candidate;
    }
  }
}

std::int64_t Rng::uniform_between(std::int64_t low, std::int64_t high) {
  if (low > high) {
    throw RunError{"InvalidRng", "uniform range is empty"};
  }
  const std::uint64_t span = static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low);
  const std::uint64_t offset = span == ~0ULL ? next() : uniform_below(span + 1);
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(low) + offset);
}

bool Rng::bernoulli_ppm(std::uint64_t probability_ppm) {
  if (probability_ppm > 1'000'000) {
    throw RunError{"InvalidRng", "probability exceeds one million ppm"};
  }
  return uniform_below(1'000'000) < probability_ppm;
}

std::uint64_t Rng::exponential(std::uint64_t mean) {
  if (mean >= (1ULL << 56U)) {
    throw RunError{"InvalidRng", "exponential mean exceeds 2^56"};
  }
  const std::uint64_t unit = negative_log_q32(next()); // below 37 * 2^32
  const std::uint64_t whole = unit >> 32U;
  const std::uint64_t part = unit & kLow32;
  return (mean * whole) + ((mean >> 32U) * part) + (((mean & kLow32) * part) >> 32U);
}

std::int64_t Rng::normal_q16() noexcept {
  std::int64_t sum = 0;
  for (int draw = 0; draw < 3; ++draw) {
    std::uint64_t bits = next();
    for (int lane = 0; lane < 4; ++lane) {
      sum += static_cast<std::int64_t>(bits & 0xffffULL);
      bits >>= 16U;
    }
  }
  return sum - 393'210; // twelve lanes with mean 32767.5
}

} // namespace markettwin
