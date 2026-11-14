#ifndef PAIRRING_CHANNEL_HPP
#define PAIRRING_CHANNEL_HPP

/// @file
/// @brief The point-to-point message channel. See docs/protocol.md.

#include "pairring/config.hpp"
#include "pairring/crc32c.hpp"
#include "pairring/region.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "pairring: the wire layout is little endian; big-endian targets are not supported yet"
#endif

namespace pairring {

/// @brief Session state of a channel.
enum class State : std::uint8_t {
  kHandshaking,   ///< Waiting for the peer; send() fails, nothing is received.
  kEstablished,   ///< Session up.
  kBroken,        ///< The layer below failed or the ring was corrupt; call restart().
  kIncompatible,  ///< The peer uses another version or an unusable geometry.
};

/// @brief Name of a state.
/// @param s State.
/// @return Static string.
inline const char* to_string(State s) noexcept {
  switch (s) {
    case State::kHandshaking:
      return "handshaking";
    case State::kEstablished:
      return "established";
    case State::kBroken:
      return "broken";
    case State::kIncompatible:
      return "incompatible";
  }
  return "unknown";
}

/// @brief Counters of a channel (a snapshot).
struct Stats {
  std::uint64_t tx_messages = 0;         ///< Messages sent.
  std::uint64_t tx_bytes = 0;            ///< Payload bytes sent.
  std::uint64_t tx_full = 0;             ///< send() refused: no room in the peer's ring.
  std::uint64_t tx_too_large = 0;        ///< send() refused: empty or above the peer's max_message.
  std::uint64_t tx_errors = 0;           ///< The layer below failed while sending.
  std::uint64_t rx_messages = 0;         ///< Messages delivered.
  std::uint64_t rx_bytes = 0;            ///< Payload bytes delivered.
  std::uint64_t rx_crc_errors = 0;       ///< Records whose checksum did not match.
  std::uint64_t rx_protocol_errors = 0;  ///< Malformed records (the session broke).
  std::uint64_t sessions = 0;            ///< Sessions established.
  std::uint64_t resets = 0;              ///< Times the peer started a new session.
};

namespace detail {

inline constexpr std::uint64_t kMagic = 0x474E495252494150ULL;  // "PAIRRING" in memory order
inline constexpr std::uint32_t kVersion = 1;

// Written by an endpoint into its peer's region at layout::kHello. The commit
// word at layout::kHelloCommit is stored last; `crc` lets a reader reject a
// block it read while the peer was rewriting it.
struct Hello {
  std::uint64_t magic;
  std::uint32_t version;
  std::uint32_t flags;
  std::uint32_t granule_size;
  std::uint32_t granule_count;
  std::uint32_t max_message;
  std::uint32_t reserved;
  std::uint64_t ring_offset;
  std::uint64_t epoch;
  std::uint32_t crc;
  std::uint32_t pad;
};
static_assert(sizeof(Hello) == layout::kHelloCommit, "hello block layout");

inline std::uint32_t hello_crc(const Hello& h) noexcept {
  return crc32c(&h, offsetof(Hello, crc));
}

inline std::uint64_t load_acquire(const unsigned char* p) noexcept {
  return __atomic_load_n(reinterpret_cast<const std::uint64_t*>(p), __ATOMIC_ACQUIRE);
}

// A non-zero value that differs between sessions: time, a process-wide counter
// and an address, mixed (splitmix64).
inline std::uint64_t new_epoch(const void* salt) noexcept {
  static std::atomic<std::uint64_t> counter{0};
  std::uint64_t x =
      static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()) ^
      (counter.fetch_add(1, std::memory_order_relaxed) * 0x9E3779B97F4A7C15ULL) ^
      reinterpret_cast<std::uintptr_t>(salt);
  x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
  x ^= x >> 31U;
  return x != 0 ? x : 1;
}

struct Counters {
  std::atomic<std::uint64_t> tx_messages{0};
  std::atomic<std::uint64_t> tx_bytes{0};
  std::atomic<std::uint64_t> tx_full{0};
  std::atomic<std::uint64_t> tx_too_large{0};
  std::atomic<std::uint64_t> tx_errors{0};
  std::atomic<std::uint64_t> rx_messages{0};
  std::atomic<std::uint64_t> rx_bytes{0};
  std::atomic<std::uint64_t> rx_crc_errors{0};
  std::atomic<std::uint64_t> rx_protocol_errors{0};
  std::atomic<std::uint64_t> sessions{0};
  std::atomic<std::uint64_t> resets{0};
};

inline void bump(std::atomic<std::uint64_t>& c, std::uint64_t by = 1) noexcept {
  c.fetch_add(by, std::memory_order_relaxed);
}

}  // namespace detail

/// @brief A point-to-point, bidirectional message channel.
///
/// Each direction is a ring in the receiver's local region. Messages are byte
/// strings of 1..max_message bytes; within a session they arrive once, in
/// order. Nothing blocks and nothing allocates.
///
/// Threads: one thread may call send() while another calls poll(), drain(),
/// receive(), next_size() and restart() (the receiving side). With a single
/// thread, all of them may be called from it, and send() also from inside a
/// drain() callback.
class Channel {
public:
  /// @brief Bytes a local region needs for this receive geometry.
  /// @param config Receive geometry.
  /// @return Region size.
  static constexpr std::uint64_t region_size(const Config& config) noexcept {
    return pairring::region_size(config);
  }

  /// @brief Sets up the channel and announces it to the peer.
  ///
  /// Throws std::invalid_argument for an invalid config or a local region that
  /// is too small or misaligned. The local region's contents are not cleared:
  /// stale data from earlier sessions is told apart by epochs.
  /// @param local This endpoint's region (receives the peer's messages).
  /// @param remote The peer's region; must outlive the channel.
  /// @param config Geometry of this endpoint's rx ring.
  Channel(LocalRegion local, RemoteRegion& remote, const Config& config)
      : local_(static_cast<unsigned char*>(local.base)), remote_(remote), cfg_(config) {
    if (const char* why = validate(config)) {
      throw std::invalid_argument(why);
    }
    if (local.base == nullptr || reinterpret_cast<std::uintptr_t>(local.base) % 8 != 0) {
      throw std::invalid_argument("local region must be 8-byte aligned");
    }
    if (local.size < region_size(config)) {
      throw std::invalid_argument("local region is smaller than region_size(config)");
    }
    my_epoch_ = detail::new_epoch(this);
    if (!publish_hello()) {
      state_.store(State::kBroken, std::memory_order_release);
    }
  }

  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  Channel(Channel&&) = delete;
  Channel& operator=(Channel&&) = delete;
  ~Channel() = default;

  /// @brief Sends one message, all or nothing. Never blocks.
  /// @param msg Payload.
  /// @param len Payload length, 1..peer's max_message.
  /// @return False if not established, the message is empty or too large, the
  /// peer's ring has no room (try again after the peer drains), or the layer
  /// below failed (the channel is then broken).
  bool send(const void* msg, std::size_t len) noexcept {
    TxGuard guard(tx_busy_);
    if (state_.load(std::memory_order_seq_cst) != State::kEstablished) {
      return false;
    }
    const Config& pc = peer_cfg_;
    if (len == 0 || msg == nullptr || len > pc.max_message) {
      detail::bump(counters_.tx_too_large);
      return false;
    }
    const std::uint64_t n = pc.granule_count;
    const std::uint64_t g = pc.granule_size;
    const std::uint64_t k = granules_for(len, pc.granule_size);
    const std::uint64_t head = detail::load_acquire(local_ + layout::kTxHead);
    const std::uint64_t used = tail_ - head;
    if (used > n) {
      return false;  // the peer is starting a new session; poll() will notice
    }
    std::uint64_t pos = tail_ & (n - 1);
    const std::uint64_t room = n - pos;
    const std::uint64_t need = k > room ? room + k : k;
    if (need > n - used) {
      detail::bump(counters_.tx_full);
      return false;
    }
    if (k > room) {
      const std::uint32_t skip[2] = {layout::kSkipLen, 0};
      if (!remote_.write(layout::kRing + pos * g, skip, sizeof(skip))) {
        return tx_failed();
      }
      tail_ += room;
      pos = 0;
    }
    std::uint32_t hdr[2] = {static_cast<std::uint32_t>(len), 0};
    Crc32c crc;
    crc.update(&hdr[0], 4);
    crc.update(msg, len);
    hdr[1] = crc.value();
    const Piece pieces[2] = {{hdr, sizeof(hdr)}, {msg, len}};
    if (!remote_.writev(layout::kRing + pos * g, pieces, 2)) {
      return tx_failed();
    }
    tail_ += k;
    if (!remote_.store64(layout::kRxTail, tail_) || !remote_.notify()) {
      return tx_failed();
    }
    detail::bump(counters_.tx_messages);
    detail::bump(counters_.tx_bytes, len);
    return true;
  }

  /// @brief Advances the handshake. Called by drain() and receive(); call it
  /// directly while nothing is being received.
  /// @return The state afterwards.
  State poll() noexcept {
    const std::uint64_t commit = detail::load_acquire(local_ + layout::kHelloCommit);
    if (commit != 0 && commit != accepted_peer_ && commit != rejected_peer_) {
      accept_peer(commit);
    }
    if (state_.load(std::memory_order_acquire) == State::kHandshaking && accepted_peer_ != 0 &&
        detail::load_acquire(local_ + layout::kAck) == my_epoch_) {
      State expected = State::kHandshaking;
      if (state_.compare_exchange_strong(
              expected, State::kEstablished, std::memory_order_seq_cst)) {
        detail::bump(counters_.sessions);
      }
    }
    return state_.load(std::memory_order_acquire);
  }

  /// @brief Delivers every message that has arrived, without copying.
  ///
  /// Any number of doorbells may stand behind one call: the count comes from the
  /// ring, not from doorbells. The consumed space is reported back to the peer
  /// once per batch.
  /// @tparam F Callable as `on_message(const void* payload, std::size_t len)`.
  /// It must not throw and must not call drain() or receive(); it may call send().
  /// The payload is valid only during the call.
  /// @param on_message Message handler.
  /// @return Messages delivered.
  template <typename F>
  std::size_t drain(F&& on_message) noexcept {
    if (poll() != State::kEstablished) {
      return 0;
    }
    std::size_t delivered = 0;
    while (true) {
      const std::uint64_t tail = detail::load_acquire(local_ + layout::kRxTail);
      if (tail <= consumed_) {
        break;
      }
      while (consumed_ < tail) {
        Record r;
        const Next next = next_record(tail, r);
        if (next == Next::kSkipped) {
          continue;
        }
        if (next == Next::kStop) {
          return delivered;
        }
        on_message(static_cast<const void*>(r.payload), static_cast<std::size_t>(r.len));
        account(r);
        ++delivered;
      }
      if (!report_consumed()) {
        break;
      }
    }
    return delivered;
  }

  /// @brief Copies the next message out.
  /// @param buf Destination.
  /// @param cap Capacity of `buf`. A larger message is left in place (see next_size()).
  /// @return The message length, or 0 if there is no message (or it does not fit).
  std::size_t receive(void* buf, std::size_t cap) noexcept {
    Record r;
    if (!peek(r) || r.len > cap) {
      return 0;
    }
    std::memcpy(buf, r.payload, r.len);
    account(r);
    (void)report_consumed();
    return r.len;
  }

  /// @brief Length of the next message, so a buffer can be sized for receive().
  /// @return Bytes, or 0 if there is no message.
  std::size_t next_size() noexcept {
    Record r;
    return peek(r) ? static_cast<std::size_t>(r.len) : 0;
  }

  /// @brief Starts a new session with a new epoch. Messages in flight are dropped.
  ///
  /// Use it to leave kBroken, or after the layer below has reconnected. Call it
  /// from the receiving side's thread.
  void restart() noexcept {
    quiesce(State::kHandshaking);
    my_epoch_ = detail::new_epoch(this);
    accepted_peer_ = 0;
    rejected_peer_ = 0;
    consumed_ = 0;
    tail_ = 0;
    if (!publish_hello()) {
      state_.store(State::kBroken, std::memory_order_seq_cst);
    }
  }

  /// @brief Current state.
  /// @return State.
  State state() const noexcept {
    return state_.load(std::memory_order_acquire);
  }

  /// @brief This endpoint's current epoch (changes on restart()).
  /// @return Epoch.
  std::uint64_t epoch() const noexcept {
    return my_epoch_;
  }

  /// @brief A snapshot of the counters.
  /// @return Counters.
  Stats stats() const noexcept {
    auto get = [](const std::atomic<std::uint64_t>& c) {
      return c.load(std::memory_order_relaxed);
    };
    Stats s;
    s.tx_messages = get(counters_.tx_messages);
    s.tx_bytes = get(counters_.tx_bytes);
    s.tx_full = get(counters_.tx_full);
    s.tx_too_large = get(counters_.tx_too_large);
    s.tx_errors = get(counters_.tx_errors);
    s.rx_messages = get(counters_.rx_messages);
    s.rx_bytes = get(counters_.rx_bytes);
    s.rx_crc_errors = get(counters_.rx_crc_errors);
    s.rx_protocol_errors = get(counters_.rx_protocol_errors);
    s.sessions = get(counters_.sessions);
    s.resets = get(counters_.resets);
    return s;
  }

private:
  struct Record {
    const unsigned char* payload = nullptr;
    std::uint32_t len = 0;
    std::uint64_t granules = 0;
  };
  enum class Next { kRecord, kSkipped, kStop };

  // Marks send() as running; the receiving side waits for it before it resets
  // the session (Dekker-style, both sides sequentially consistent).
  class TxGuard {
  public:
    explicit TxGuard(std::atomic<bool>& busy) noexcept : busy_(busy) {
      busy_.store(true, std::memory_order_seq_cst);
    }
    ~TxGuard() {
      busy_.store(false, std::memory_order_release);
    }
    TxGuard(const TxGuard&) = delete;
    TxGuard& operator=(const TxGuard&) = delete;
    TxGuard(TxGuard&&) = delete;
    TxGuard& operator=(TxGuard&&) = delete;

  private:
    std::atomic<bool>& busy_;
  };

  // Moves to `next` and waits until no send() runs in the old session.
  void quiesce(State next) noexcept {
    state_.store(next, std::memory_order_seq_cst);
    while (tx_busy_.load(std::memory_order_seq_cst)) {
      std::this_thread::yield();
    }
  }

  bool tx_failed() noexcept {
    detail::bump(counters_.tx_errors);
    State expected = State::kEstablished;
    state_.compare_exchange_strong(expected, State::kBroken, std::memory_order_seq_cst);
    return false;
  }

  void broken() noexcept {
    quiesce(State::kBroken);
  }

  bool publish_hello() noexcept {
    detail::Hello h{};
    h.magic = detail::kMagic;
    h.version = detail::kVersion;
    h.granule_size = cfg_.granule_size;
    h.granule_count = cfg_.granule_count;
    h.max_message = cfg_.max_message;
    h.ring_offset = layout::kRing;
    h.epoch = my_epoch_;
    h.crc = detail::hello_crc(h);
    return remote_.write(layout::kHello, &h, sizeof(h)) && remote_.store64(layout::kRxTail, 0) &&
           remote_.store64(layout::kTxHead, 0) &&
           remote_.store64(layout::kHelloCommit, my_epoch_) && remote_.notify();
  }

  // A committed hello with an epoch we have not seen: a new peer session.
  void accept_peer(std::uint64_t commit) noexcept {
    detail::Hello h{};
    std::memcpy(&h, local_ + layout::kHello, sizeof(h));
    if (h.epoch != commit || h.crc != detail::hello_crc(h)) {
      return;  // read while the peer was rewriting it; try again on the next poll
    }
    const Config peer{h.granule_size, h.granule_count, h.max_message};
    if (h.magic != detail::kMagic || h.version != detail::kVersion ||
        h.ring_offset != layout::kRing || validate(peer) != nullptr ||
        region_size(peer) > remote_.size()) {
      rejected_peer_ = commit;
      quiesce(State::kIncompatible);
      return;
    }
    const bool restarted = accepted_peer_ != 0;
    quiesce(State::kHandshaking);
    peer_cfg_ = peer;
    consumed_ = 0;
    tail_ = 0;
    accepted_peer_ = commit;
    if (restarted) {
      detail::bump(counters_.resets);
    }
    // Always announce again: the peer may have a fresh region that never saw
    // our hello. This also zeroes our counters in the peer's region.
    if (!publish_hello() || !remote_.store64(layout::kAck, commit) || !remote_.notify()) {
      broken();
    }
  }

  // Reads the record at `consumed_` (published up to `tail`).
  Next next_record(std::uint64_t tail, Record& out) noexcept {
    const std::uint64_t n = cfg_.granule_count;
    const std::uint64_t pos = consumed_ & (n - 1);
    const std::uint64_t room = n - pos;
    const unsigned char* rec = local_ + layout::kRing + pos * cfg_.granule_size;
    std::uint32_t hdr[2];
    std::memcpy(hdr, rec, sizeof(hdr));
    if (hdr[0] == layout::kSkipLen) {
      if (consumed_ + room > tail) {
        return corrupt();
      }
      consumed_ += room;
      return Next::kSkipped;
    }
    const std::uint64_t k = granules_for(hdr[0], cfg_.granule_size);
    if (hdr[0] == 0 || hdr[0] > cfg_.max_message || k > room || consumed_ + k > tail) {
      return corrupt();
    }
    Crc32c crc;
    crc.update(&hdr[0], 4);
    crc.update(rec + layout::kRecordHeader, hdr[0]);
    if (crc.value() != hdr[1]) {
      detail::bump(counters_.rx_crc_errors);
      if (granules_for(cfg_.max_message, cfg_.granule_size) == 1) {
        consumed_ += 1;  // single-granule records: the next one is one granule further
        return Next::kSkipped;
      }
      broken();  // the length may be what is wrong: the next record cannot be found
      return Next::kStop;
    }
    out.payload = rec + layout::kRecordHeader;
    out.len = hdr[0];
    out.granules = k;
    return Next::kRecord;
  }

  Next corrupt() noexcept {
    detail::bump(counters_.rx_protocol_errors);
    broken();
    return Next::kStop;
  }

  // The next message, if any, leaving it in place.
  bool peek(Record& r) noexcept {
    if (poll() != State::kEstablished) {
      return false;
    }
    while (true) {
      const std::uint64_t tail = detail::load_acquire(local_ + layout::kRxTail);
      if (tail <= consumed_) {
        return false;
      }
      const Next next = next_record(tail, r);
      if (next == Next::kRecord) {
        return true;
      }
      if (next == Next::kStop) {
        return false;
      }
    }
  }

  void account(const Record& r) noexcept {
    consumed_ += r.granules;
    detail::bump(counters_.rx_messages);
    detail::bump(counters_.rx_bytes, r.len);
  }

  bool report_consumed() noexcept {
    if (!remote_.store64(layout::kTxHead, consumed_) || !remote_.notify()) {
      broken();
      return false;
    }
    return true;
  }

  unsigned char* local_;
  RemoteRegion& remote_;
  Config cfg_;  // our rx ring

  // Receiving side.
  std::uint64_t my_epoch_ = 0;
  std::uint64_t accepted_peer_ = 0;
  std::uint64_t rejected_peer_ = 0;
  std::uint64_t consumed_ = 0;

  // Written by the receiving side while no send() runs, read by send().
  Config peer_cfg_{};
  // Sending side.
  std::uint64_t tail_ = 0;

  std::atomic<State> state_{State::kHandshaking};
  std::atomic<bool> tx_busy_{false};
  detail::Counters counters_;
};

}  // namespace pairring

#endif  // PAIRRING_CHANNEL_HPP
