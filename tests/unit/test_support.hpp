#ifndef PAIRRING_TESTS_UNIT_TEST_SUPPORT_HPP
#define PAIRRING_TESTS_UNIT_TEST_SUPPORT_HPP

// Test doubles for the layer below a channel: plain memory, a region that
// counts and fails on demand, and one that delivers operations late.

#include "pairring/pairring.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <vector>

namespace pairring::test {

// Zeroed, 8-byte aligned memory standing in for a local region.
class Memory {
public:
  explicit Memory(std::uint64_t bytes) : words_((bytes + 7) / 8, 0), bytes_(bytes) {}
  void* data() noexcept {
    return words_.data();
  }
  std::size_t bytes() const noexcept {
    return bytes_;
  }
  unsigned char* at(std::uint64_t off) noexcept {
    return reinterpret_cast<unsigned char*>(words_.data()) + off;
  }
  LocalRegion region() noexcept {
    return {data(), bytes_};
  }
  void clear() noexcept {
    std::fill(words_.begin(), words_.end(), 0);
  }

private:
  std::vector<std::uint64_t> words_;
  std::size_t bytes_;
};

// Writes straight into the peer's Memory; counts operations; can fail on demand.
class TestRegion final : public RemoteRegion {
public:
  explicit TestRegion(Memory& peer) : inner_(peer.data(), peer.bytes(), &TestRegion::ring, this) {}

  bool write(std::uint64_t off, const void* src, std::size_t len) noexcept override {
    ++writes;
    hello_writes += off == layout::kHello ? 1U : 0U;
    return !fail() && inner_.write(off, src, len);
  }
  bool writev(std::uint64_t off, const Piece* pieces, std::size_t count) noexcept override {
    if (!gather) {
      return RemoteRegion::writev(off, pieces, count);
    }
    ++writevs;
    if (fail()) {
      return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
      if (!inner_.write(off, pieces[i].data, pieces[i].len)) {
        return false;
      }
      off += pieces[i].len;
    }
    return true;
  }
  bool store64(std::uint64_t off, std::uint64_t value) noexcept override {
    ++stores;
    return !fail() && inner_.store64(off, value);
  }
  bool notify() noexcept override {
    return !fail() && inner_.notify();
  }
  std::size_t size() const noexcept override {
    return size_override != 0 ? size_override : inner_.size();
  }

  std::atomic<std::uint64_t> writes{0};
  std::atomic<std::uint64_t> hello_writes{0};  // announcements written into the peer's region
  std::atomic<std::uint64_t> writevs{0};
  std::atomic<std::uint64_t> stores{0};
  std::atomic<std::uint64_t> doorbells{0};
  bool gather = false;            // implement writev natively
  int fail_after = -1;            // >= 0: operations left before every operation fails
  std::size_t size_override = 0;  // pretend the peer region has this size

private:
  static bool ring(void* self) noexcept {
    ++static_cast<TestRegion*>(self)->doorbells;
    return true;
  }
  bool fail() noexcept {
    if (fail_after < 0) {
      return false;
    }
    if (fail_after == 0) {
      return true;
    }
    --fail_after;
    return false;
  }

  MappedRemoteRegion inner_;
};

// Queues every operation and applies it only when pumped, in order: a layer
// below whose writes arrive late, like a network.
class DelayedRegion final : public RemoteRegion {
public:
  explicit DelayedRegion(Memory& peer) : inner_(peer.data(), peer.bytes()) {}

  bool write(std::uint64_t off, const void* src, std::size_t len) noexcept override {
    hello_writes += off == layout::kHello ? 1U : 0U;
    const auto* p = static_cast<const unsigned char*>(src);
    ops_.push_back({false, off, std::vector<unsigned char>(p, p + len), 0});
    return true;
  }
  bool store64(std::uint64_t off, std::uint64_t value) noexcept override {
    ops_.push_back({true, off, {}, value});
    return true;
  }
  bool notify() noexcept override {
    return true;
  }
  std::size_t size() const noexcept override {
    return inner_.size();
  }

  // Applies up to `count` queued operations; returns how many were applied.
  std::size_t pump(std::size_t count) {
    std::size_t done = 0;
    for (; done < count && !ops_.empty(); ++done) {
      const Op& op = ops_.front();
      if (op.store) {
        (void)inner_.store64(op.off, op.value);
      } else {
        (void)inner_.write(op.off, op.bytes.data(), op.bytes.size());
      }
      ops_.pop_front();
    }
    return done;
  }
  std::size_t pending() const noexcept {
    return ops_.size();
  }

  std::uint64_t hello_writes = 0;  // announcements written into the peer's region

private:
  struct Op {
    bool store;
    std::uint64_t off;
    std::vector<unsigned char> bytes;
    std::uint64_t value;
  };
  std::deque<Op> ops_;
  MappedRemoteRegion inner_;
};

// Two endpoints, A and B, each writing into the other's Memory.
template <typename Region = TestRegion>
struct Pair {
  explicit Pair(const Config& config_a = Config{}, const Config& config_b = Config{})
      : ca(config_a),
        cb(config_b),
        mem_a(region_size(ca)),
        mem_b(region_size(cb)),
        to_a(mem_a),
        to_b(mem_b) {
    a.emplace(mem_a.region(), to_b, ca);
    b.emplace(mem_b.region(), to_a, cb);
  }

  // Polls both sides until both are established (or gives up).
  bool establish(int rounds = 100) {
    for (int i = 0; i < rounds; ++i) {
      const State sa = a->poll();
      const State sb = b->poll();
      if (sa == State::kEstablished && sb == State::kEstablished) {
        return true;
      }
    }
    return false;
  }

  Config ca;
  Config cb;
  Memory mem_a;
  Memory mem_b;
  Region to_a;  // B's handle on A's memory
  Region to_b;  // A's handle on B's memory
  std::optional<Channel> a;
  std::optional<Channel> b;
};

// A message whose bytes encode (seq, len), so the receiver can check both.
inline std::vector<unsigned char> make_message(std::uint32_t seq, std::size_t len) {
  std::vector<unsigned char> m(len);
  for (std::size_t i = 0; i < len; ++i) {
    m[i] = static_cast<unsigned char>((seq * 131U + i * 7U) & 0xFFU);
  }
  if (len >= 4) {
    std::memcpy(m.data(), &seq, 4);
  }
  return m;
}

inline bool check_message(const void* p, std::size_t len, std::uint32_t seq) {
  const auto expect = make_message(seq, len);
  return std::memcmp(p, expect.data(), len) == 0;
}

}  // namespace pairring::test

#endif  // PAIRRING_TESTS_UNIT_TEST_SUPPORT_HPP
