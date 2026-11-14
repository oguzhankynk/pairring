#ifndef PAIRRING_REGION_HPP
#define PAIRRING_REGION_HPP

/// @file
/// @brief The lower layer contract: what a channel needs from the memory below it.

#include <cstddef>
#include <cstdint>

namespace pairring {

/// @brief One piece of a gathered write.
struct Piece {
  const void* data = nullptr;  ///< Source bytes.
  std::size_t len = 0;         ///< Byte count.
};

/// @brief Memory this endpoint reads with plain loads. Only the peer writes into it.
struct LocalRegion {
  void* base = nullptr;  ///< Start of the region; at least 8-byte aligned (64 recommended).
  std::size_t size = 0;  ///< Size in bytes; at least region_size() of the channel's Config.
};

/// @brief Write access to the peer's local region. Implemented by the integrator.
///
/// Contract (every implementation must provide it):
/// - write(): bulk copy; no atomicity, no ordering against other writes.
/// - store64(): `off` is 8-aligned; single-copy atomic; release: every earlier
///   operation on this object is visible to the peer no later than the value.
/// - notify(): doorbell; every earlier operation is visible before the peer sees
///   it. Sticky: a doorbell raised while the peer is not waiting is not lost.
/// - Ordering holds per object only. There are no remote reads.
/// - An operation that fails (peer gone, link down) returns false.
///
/// How the region was created and connected is outside this contract. Calls on
/// one object may come from the channel's sending and receiving threads at the
/// same time; implementations must allow that.
class RemoteRegion {
public:
  virtual ~RemoteRegion() = default;

  /// @brief Copies bytes into the peer's region.
  /// @param off Offset in the peer's region.
  /// @param src Source bytes.
  /// @param len Byte count.
  /// @return False if the operation failed.
  virtual bool write(std::uint64_t off, const void* src, std::size_t len) noexcept = 0;

  /// @brief Writes pieces back to back starting at `off`.
  ///
  /// Optional: the default issues one write() per piece. Override it when the
  /// layer below can send several pieces as one operation.
  /// @param off Offset in the peer's region.
  /// @param pieces Pieces, in order.
  /// @param count Number of pieces.
  /// @return False if the operation failed.
  virtual bool writev(std::uint64_t off, const Piece* pieces, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
      if (!write(off, pieces[i].data, pieces[i].len)) {
        return false;
      }
      off += pieces[i].len;
    }
    return true;
  }

  /// @brief Atomic, release-ordered 8-byte store.
  /// @param off 8-aligned offset in the peer's region.
  /// @param value Value.
  /// @return False if the operation failed.
  virtual bool store64(std::uint64_t off, std::uint64_t value) noexcept = 0;

  /// @brief Rings the peer's doorbell.
  /// @return False if the operation failed.
  virtual bool notify() noexcept = 0;

  /// @brief Size of the peer's region.
  /// @return Bytes.
  virtual std::size_t size() const noexcept = 0;

protected:
  RemoteRegion() = default;
  /// @brief Copyable by derived classes.
  RemoteRegion(const RemoteRegion&) = default;
  /// @brief Copyable by derived classes.
  /// @return *this.
  RemoteRegion& operator=(const RemoteRegion&) = default;
  /// @brief Movable by derived classes.
  RemoteRegion(RemoteRegion&&) = default;
  /// @brief Movable by derived classes.
  /// @return *this.
  RemoteRegion& operator=(RemoteRegion&&) = default;
};

}  // namespace pairring

#endif  // PAIRRING_REGION_HPP
