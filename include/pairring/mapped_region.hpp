#ifndef PAIRRING_MAPPED_REGION_HPP
#define PAIRRING_MAPPED_REGION_HPP

/// @file
/// @brief A ready RemoteRegion for when the peer's region is mapped into this process.

#include "pairring/region.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pairring {

/// @brief RemoteRegion over memory mapped into this process: shared memory
/// between processes, a device window, or plain memory in tests.
///
/// write() is memcpy, store64() an atomic release store, notify() calls the
/// function given at construction (for example writing an eventfd or waking a
/// futex). Without a function, notify() does nothing and the peer polls.
///
/// Write-combining mappings (typical for device windows) may reorder plain
/// stores; pass `strong_ordering = true` to put a full fence before each
/// store64() and notify().
class MappedRemoteRegion final : public RemoteRegion {
public:
  /// @brief Doorbell function: returns false if the doorbell could not be rung.
  using NotifyFn = bool (*)(void* context) noexcept;

  /// @brief Wraps a mapped peer region.
  /// @param peer_base Mapped address of the peer region's offset 0; 8-byte aligned.
  /// @param size Size of the peer region.
  /// @param notify_fn Doorbell, or nullptr.
  /// @param context Passed to `notify_fn`.
  /// @param strong_ordering Full fences, for write-combining mappings.
  MappedRemoteRegion(void* peer_base,
                     std::size_t size,
                     NotifyFn notify_fn = nullptr,
                     void* context = nullptr,
                     bool strong_ordering = false) noexcept
      : base_(static_cast<unsigned char*>(peer_base)),
        size_(size),
        notify_fn_(notify_fn),
        context_(context),
        strong_(strong_ordering) {}

  /// @brief Copies bytes into the peer region.
  /// @param off Offset.
  /// @param src Source.
  /// @param len Byte count.
  /// @return False if the range is outside the region.
  bool write(std::uint64_t off, const void* src, std::size_t len) noexcept override {
    if (off > size_ || len > size_ - off) {
      return false;
    }
    std::memcpy(base_ + off, src, len);
    return true;
  }

  /// @brief Atomic release store.
  /// @param off 8-aligned offset.
  /// @param value Value.
  /// @return False if the offset is misaligned or outside the region.
  bool store64(std::uint64_t off, std::uint64_t value) noexcept override {
    if (off % 8 != 0 || off > size_ || size_ - off < 8) {
      return false;
    }
    fence();
    __atomic_store_n(reinterpret_cast<std::uint64_t*>(base_ + off), value, __ATOMIC_RELEASE);
    return true;
  }

  /// @brief Rings the doorbell, if there is one.
  /// @return The doorbell function's result, or true without one.
  bool notify() noexcept override {
    if (notify_fn_ == nullptr) {
      return true;
    }
    fence();
    return notify_fn_(context_);
  }

  /// @brief Size of the peer region.
  /// @return Bytes.
  std::size_t size() const noexcept override {
    return size_;
  }

private:
  void fence() const noexcept {
    std::atomic_thread_fence(strong_ ? std::memory_order_seq_cst : std::memory_order_release);
  }

  unsigned char* base_;
  std::size_t size_;
  NotifyFn notify_fn_;
  void* context_;
  bool strong_;
};

}  // namespace pairring

#endif  // PAIRRING_MAPPED_REGION_HPP
