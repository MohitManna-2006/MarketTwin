#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace markettwin {

// SHA-256 modulo-2^32 arithmetic is intentional; this type never holds financial values.
class Sha256 {
public:
  void update(std::string_view bytes);
  [[nodiscard]] std::string digest() const;

private:
  void compress();
  std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t used_ = 0;
  std::uint64_t bytes_ = 0;
};
[[nodiscard]] std::string sha256(std::string_view bytes);

} // namespace markettwin
