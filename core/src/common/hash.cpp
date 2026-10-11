#include "markettwin/common/hash.hpp"

#include "markettwin/common/run_error.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

#ifdef __ARM_FEATURE_SHA2
#include <arm_neon.h>
#endif

namespace markettwin {
// The compression loops index fixed-size arrays with loop counters whose bounds are the
// array sizes. Checked access here costs more than the hashing itself.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,
// cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
namespace {
constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U,
};

#ifdef __ARM_FEATURE_SHA2
// ARMv8 SHA-2 instructions: four rounds per step, with the state split as ABCD and EFGH.
void compress_accelerated(Sha256State& state, const Sha256Block& block) noexcept {
  auto abcd = vld1q_u32(state.data());
  auto efgh = vld1q_u32(state.data() + 4);
  const auto abcd_saved = abcd;
  const auto efgh_saved = efgh;
  std::array<uint32x4_t, 4> message{};
  for (std::size_t lane = 0; lane < message.size(); ++lane) {
    message[lane] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block.data() + (lane * 16))));
  }
  for (std::size_t step = 0; step < 16; ++step) {
    const auto lane = step & 3U;
    const auto scheduled = vaddq_u32(message[lane], vld1q_u32(kRoundConstants.data() + (step * 4)));
    const auto previous = abcd;
    abcd = vsha256hq_u32(abcd, efgh, scheduled);
    efgh = vsha256h2q_u32(efgh, previous, scheduled);
    if (step < 12) {
      message[lane] = vsha256su1q_u32(vsha256su0q_u32(message[lane], message[(step + 1) & 3U]),
                                      message[(step + 2) & 3U], message[(step + 3) & 3U]);
    }
  }
  vst1q_u32(state.data(), vaddq_u32(abcd, abcd_saved));
  vst1q_u32(state.data() + 4, vaddq_u32(efgh, efgh_saved));
}
#endif

} // namespace

namespace detail {

void sha256_compress_portable(Sha256State& state, const Sha256Block& block) noexcept {
  std::array<std::uint32_t, 64> words{};
  for (std::size_t index = 0; index < 16; ++index) {
    const auto offset = index * 4;
    words[index] = (std::uint32_t{block[offset]} << 24U) |
                   (std::uint32_t{block[offset + 1]} << 16U) |
                   (std::uint32_t{block[offset + 2]} << 8U) | std::uint32_t{block[offset + 3]};
  }
  for (std::size_t index = 16; index < words.size(); ++index) {
    const auto earlier = words[index - 15];
    const auto later = words[index - 2];
    const auto sigma0 = std::rotr(earlier, 7) ^ std::rotr(earlier, 18) ^ (earlier >> 3U);
    const auto sigma1 = std::rotr(later, 17) ^ std::rotr(later, 19) ^ (later >> 10U);
    words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
  }
  auto [a, b, c, d, e, f, g, h] = state;
  for (std::size_t index = 0; index < words.size(); ++index) {
    const auto sigma1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const auto choose = (e & f) ^ (~e & g);
    const auto first = h + sigma1 + choose + kRoundConstants[index] + words[index];
    const auto sigma0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const auto majority = (a & b) ^ (a & c) ^ (b & c);
    h = g;
    g = f;
    f = e;
    e = d + first;
    d = c;
    c = b;
    b = a;
    a = first + sigma0 + majority;
  }
  const Sha256State working{a, b, c, d, e, f, g, h};
  for (std::size_t index = 0; index < state.size(); ++index) {
    state[index] += working[index];
  }
}

void sha256_compress(Sha256State& state, const Sha256Block& block) noexcept {
#ifdef __ARM_FEATURE_SHA2
  compress_accelerated(state, block);
#else
  sha256_compress_portable(state, block);
#endif
}

} // namespace detail
// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,
// cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

void Sha256::update(std::string_view bytes) {
  constexpr auto kMaxBytes = std::numeric_limits<std::uint64_t>::max() / 8;
  if (bytes.size() > kMaxBytes - bytes_) {
    throw RunError{"HashLengthOverflow", "SHA-256 bit length exceeds 64 bits"};
  }
  bytes_ += bytes.size();
  while (!bytes.empty()) {
    const auto take = std::min(bytes.size(), buffer_.size() - used_);
    std::memcpy(buffer_.data() + used_, bytes.data(), take);
    used_ += take;
    bytes.remove_prefix(take);
    if (used_ == buffer_.size()) {
      detail::sha256_compress(state_, buffer_);
      used_ = 0;
    }
  }
}

std::string Sha256::digest() const {
  auto copy = *this;
  copy.buffer_.at(copy.used_++) = 0x80;
  if (copy.used_ > 56) {
    while (copy.used_ < 64) {
      copy.buffer_.at(copy.used_++) = 0;
    }
    detail::sha256_compress(copy.state_, copy.buffer_);
    copy.used_ = 0;
  }
  while (copy.used_ < 56) {
    copy.buffer_.at(copy.used_++) = 0;
  }
  const auto bits = bytes_ * 8;
  for (std::size_t index = 0; index < 8; ++index) {
    copy.buffer_.at(63 - index) = static_cast<std::uint8_t>(bits >> (index * 8));
  }
  detail::sha256_compress(copy.state_, copy.buffer_);
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string output;
  output.reserve(64);
  for (const auto word : copy.state_) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      output.push_back(kHex.at((word >> static_cast<unsigned>(shift)) & 0x0fU));
    }
  }
  return output;
}

std::string sha256(std::string_view bytes) {
  Sha256 hash;
  hash.update(bytes);
  return hash.digest();
}

} // namespace markettwin
