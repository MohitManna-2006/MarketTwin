#include "markettwin/common/hash.hpp"

#include "markettwin/common/run_error.hpp"

#include <bit>
#include <limits>

namespace markettwin {
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
    0xc67178f2U};
}

// Every subscript below is bounded by a fixed-size std::array and its loop or block counter.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)
void Sha256::compress() {
  std::array<std::uint32_t, 64> words{};
  for (std::size_t index = 0; index < 16; ++index) {
    const auto offset = index * 4;
    words[index] = (std::uint32_t{buffer_[offset]} << 24) |
                   (std::uint32_t{buffer_[offset + 1]} << 16) |
                   (std::uint32_t{buffer_[offset + 2]} << 8) | buffer_[offset + 3];
  }
  for (std::size_t index = 16; index < words.size(); ++index) {
    const auto earlier = words[index - 15];
    const auto later = words[index - 2];
    const auto sigma0 = std::rotr(earlier, 7) ^ std::rotr(earlier, 18) ^ (earlier >> 3);
    const auto sigma1 = std::rotr(later, 17) ^ std::rotr(later, 19) ^ (later >> 10);
    words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
  }
  auto [a, b, c, d, e, f, g, h] = state_;
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
  const std::array<std::uint32_t, 8> working{a, b, c, d, e, f, g, h};
  for (std::size_t index = 0; index < state_.size(); ++index) {
    state_[index] += working[index];
  }
}

void Sha256::update(std::string_view bytes) {
  constexpr auto kMaxBytes = std::numeric_limits<std::uint64_t>::max() / 8;
  if (bytes.size() > kMaxBytes - bytes_) {
    throw RunError{"HashLengthOverflow", "SHA-256 bit length exceeds 64 bits"};
  }
  bytes_ += bytes.size();
  for (const auto byte : bytes) {
    buffer_[used_++] = static_cast<std::uint8_t>(byte);
    if (used_ == buffer_.size()) {
      compress();
      used_ = 0;
    }
  }
}

std::string Sha256::digest() const {
  auto copy = *this;
  copy.buffer_[copy.used_++] = 0x80;
  if (copy.used_ > 56) {
    while (copy.used_ < 64) {
      copy.buffer_[copy.used_++] = 0;
    }
    copy.compress();
    copy.used_ = 0;
  }
  while (copy.used_ < 56) {
    copy.buffer_[copy.used_++] = 0;
  }
  const auto bits = bytes_ * 8;
  for (std::size_t index = 0; index < 8; ++index) {
    copy.buffer_[63 - index] = static_cast<std::uint8_t>(bits >> (index * 8));
  }
  copy.compress();
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string output;
  output.reserve(64);
  for (const auto word : copy.state_) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      output.push_back(kHex[(word >> shift) & 0x0fU]);
    }
  }
  return output;
}

// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

std::string sha256(std::string_view bytes) {
  Sha256 hash;
  hash.update(bytes);
  return hash.digest();
}

} // namespace markettwin
