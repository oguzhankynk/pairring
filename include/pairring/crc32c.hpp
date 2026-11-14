#ifndef PAIRRING_CRC32C_HPP
#define PAIRRING_CRC32C_HPP

/// @file
/// @brief CRC-32C (Castagnoli), the checksum of every record.

#include <cstddef>
#include <cstdint>

#if defined(__SSE4_2__)
#include <nmmintrin.h>
#endif

namespace pairring {

namespace detail {

struct Crc32cTable {
  std::uint32_t entries[256];
};

constexpr Crc32cTable make_crc32c_table() noexcept {
  Crc32cTable table{};
  for (std::uint32_t i = 0; i < 256U; ++i) {
    std::uint32_t c = i;
    for (int bit = 0; bit < 8; ++bit) {
      c = (c & 1U) != 0U ? (c >> 1U) ^ 0x82F63B78U : c >> 1U;
    }
    table.entries[i] = c;
  }
  return table;
}

inline constexpr Crc32cTable kCrc32cTable = make_crc32c_table();

}  // namespace detail

/// @brief Incremental CRC-32C (polynomial 0x1EDC6F41, reflected; init and final xor 0xFFFFFFFF).
///
/// Uses the SSE4.2 instruction when the compiler targets it, a table otherwise.
class Crc32c {
public:
  /// @brief Adds bytes.
  /// @param data Bytes.
  /// @param len Byte count.
  void update(const void* data, std::size_t len) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
#if defined(__SSE4_2__)
    for (; len >= 8; len -= 8, p += 8) {
      std::uint64_t word = 0;
      __builtin_memcpy(&word, p, 8);
      state_ = static_cast<std::uint32_t>(_mm_crc32_u64(state_, word));
    }
    for (; len > 0; --len, ++p) {
      state_ = _mm_crc32_u8(state_, *p);
    }
#else
    for (; len > 0; --len, ++p) {
      state_ = detail::kCrc32cTable.entries[(state_ ^ *p) & 0xFFU] ^ (state_ >> 8U);
    }
#endif
  }

  /// @brief The checksum of everything added so far.
  /// @return CRC-32C.
  std::uint32_t value() const noexcept {
    return ~state_;
  }

private:
  std::uint32_t state_ = 0xFFFFFFFFU;
};

/// @brief CRC-32C of one buffer.
/// @param data Bytes.
/// @param len Byte count.
/// @return CRC-32C.
inline std::uint32_t crc32c(const void* data, std::size_t len) noexcept {
  Crc32c c;
  c.update(data, len);
  return c.value();
}

}  // namespace pairring

#endif  // PAIRRING_CRC32C_HPP
