#ifndef PAIRRING_CONFIG_HPP
#define PAIRRING_CONFIG_HPP

/// @file
/// @brief Ring geometry and region layout.

#include <cstddef>
#include <cstdint>

namespace pairring {

/// @brief Geometry of the ring an endpoint receives on (its rx ring).
///
/// The ring is `granule_count` granules of `granule_size` bytes. A message of
/// `len` bytes takes `ceil((8 + len) / granule_size)` consecutive granules.
/// - Small granules pack variable-size messages tightly ("pipe" behavior).
/// - `granule_size >= 8 + max_message` puts every message in exactly one
///   granule ("slot" behavior): a corrupt message can then be skipped.
struct Config {
  std::uint32_t granule_size = 64;      ///< Power of two, at least 16.
  std::uint32_t granule_count = 16384;  ///< Power of two, at least 2.
  std::uint32_t max_message = 4096;     ///< Largest payload; must fit in half the ring.
};

/// @brief Byte offsets inside a local region. Every control word is a u64 on its own 64-byte line.
namespace layout {
inline constexpr std::uint64_t kHello = 0;         ///< Peer's announcement.
inline constexpr std::uint64_t kHelloCommit = 56;  ///< Epoch that commits the announcement.
inline constexpr std::uint64_t kRxTail = 64;       ///< Granules the peer has published to us.
inline constexpr std::uint64_t kTxHead = 128;      ///< Granules of our tx ring the peer consumed.
inline constexpr std::uint64_t kAck = 192;         ///< Our epoch, as accepted by the peer.
inline constexpr std::uint64_t kRing = 256;        ///< Start of the rx ring.
inline constexpr std::uint32_t kRecordHeader = 8;  ///< len u32 + crc u32.
inline constexpr std::uint32_t kSkipLen = 0xFFFFFFFFU;  ///< len value of a skip record.
}  // namespace layout

/// @brief Granules a message of `len` bytes occupies.
/// @param len Payload length.
/// @param granule_size Granule size.
/// @return Granule count.
constexpr std::uint64_t granules_for(std::uint64_t len, std::uint32_t granule_size) noexcept {
  return (layout::kRecordHeader + len + granule_size - 1) / granule_size;
}

/// @brief Checks a configuration.
/// @param c Configuration.
/// @return nullptr if valid, otherwise a static description of the problem.
constexpr const char* validate(const Config& c) noexcept {
  auto pow2 = [](std::uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
  };
  if (!pow2(c.granule_size) || c.granule_size < 16) {
    return "granule_size must be a power of two and at least 16";
  }
  if (!pow2(c.granule_count) || c.granule_count < 2) {
    return "granule_count must be a power of two and at least 2";
  }
  if (c.max_message == 0) {
    return "max_message must be at least 1";
  }
  if (granules_for(c.max_message, c.granule_size) > c.granule_count / 2) {
    return "max_message must fit in half the ring";
  }
  return nullptr;
}

/// @brief Bytes a local region needs for this receive geometry.
/// @param c Configuration.
/// @return Region size.
constexpr std::uint64_t region_size(const Config& c) noexcept {
  return layout::kRing + std::uint64_t{c.granule_size} * c.granule_count;
}

}  // namespace pairring

#endif  // PAIRRING_CONFIG_HPP
