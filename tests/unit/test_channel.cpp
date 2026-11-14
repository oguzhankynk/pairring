#include "pairring/pairring.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

#include "test_support.hpp"

namespace pairring {
namespace {

using test::check_message;
using test::DelayedRegion;
using test::make_message;
using test::Memory;
using test::Pair;
using test::TestRegion;

// Small geometries that wrap often.
constexpr Config kPipe{16, 64, 200};   // variable-size records, skip records at the end
constexpr Config kSlot{256, 16, 248};  // every record in one granule

std::vector<std::vector<unsigned char>> drain_all(Channel& ch) {
  std::vector<std::vector<unsigned char>> out;
  ch.drain([&](const void* p, std::size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    out.emplace_back(b, b + n);
  });
  return out;
}

// ---------------------------------------------------------------------------
// Construction and handshake
// ---------------------------------------------------------------------------

TEST(ChannelTest, RejectsInvalidSetup) {
  Memory peer(region_size(Config{}));
  TestRegion remote(peer);
  Memory small(region_size(Config{}) - 8);
  EXPECT_THROW(Channel(small.region(), remote, Config{}), std::invalid_argument);
  Memory mem(region_size(Config{}) + 8);
  LocalRegion misaligned{mem.at(4), region_size(Config{})};
  EXPECT_THROW(Channel(misaligned, remote, Config{}), std::invalid_argument);
  EXPECT_THROW(Channel(mem.region(), remote, Config{48, 16, 8}), std::invalid_argument);
}

TEST(ChannelTest, HandshakeEstablishesBothSides) {
  Pair<> p;
  EXPECT_EQ(p.a->state(), State::kHandshaking);
  EXPECT_FALSE(p.a->send("x", 1));  // not yet
  ASSERT_TRUE(p.establish());
  EXPECT_EQ(p.a->stats().sessions, 1U);
  EXPECT_EQ(p.b->stats().sessions, 1U);
  EXPECT_NE(p.a->epoch(), p.b->epoch());
  EXPECT_STREQ(to_string(p.a->state()), "established");
}

TEST(ChannelTest, DifferentGeometriesPerDirection) {
  Pair<> p(kPipe, kSlot);  // A receives on a pipe ring, B on a slot ring
  ASSERT_TRUE(p.establish());
  const auto big = make_message(1, 248);
  EXPECT_TRUE(p.a->send(big.data(), big.size()));   // B's max is 248
  EXPECT_FALSE(p.b->send(big.data(), big.size()));  // A's max is 200
  EXPECT_EQ(p.b->stats().tx_too_large, 1U);
  const auto got = drain_all(*p.b);
  ASSERT_EQ(got.size(), 1U);
  EXPECT_EQ(got[0], big);
}

// ---------------------------------------------------------------------------
// Data path
// ---------------------------------------------------------------------------

TEST(ChannelTest, RoundTripBothDirections) {
  Pair<> p;
  ASSERT_TRUE(p.establish());
  ASSERT_TRUE(p.a->send("ping", 4));
  ASSERT_TRUE(p.b->send("pong!", 5));
  auto at_b = drain_all(*p.b);
  auto at_a = drain_all(*p.a);
  ASSERT_EQ(at_b.size(), 1U);
  ASSERT_EQ(at_a.size(), 1U);
  EXPECT_EQ(std::string(at_b[0].begin(), at_b[0].end()), "ping");
  EXPECT_EQ(std::string(at_a[0].begin(), at_a[0].end()), "pong!");
  EXPECT_EQ(p.a->stats().tx_bytes, 4U);
  EXPECT_EQ(p.a->stats().rx_bytes, 5U);
}

TEST(ChannelTest, RejectsEmptyAndOversizedMessages) {
  Pair<> p(kPipe, kPipe);
  ASSERT_TRUE(p.establish());
  const std::vector<unsigned char> big(201, 1);
  EXPECT_FALSE(p.a->send(big.data(), big.size()));
  EXPECT_FALSE(p.a->send("x", 0));
  EXPECT_FALSE(p.a->send(nullptr, 3));
  EXPECT_EQ(p.a->stats().tx_too_large, 3U);
}

void stream_in_order(const Config& cfg, bool gather) {
  Pair<> p(cfg, cfg);
  p.to_b.gather = gather;
  ASSERT_TRUE(p.establish());
  std::mt19937 rng(42);
  std::uniform_int_distribution<std::size_t> size(1, cfg.max_message);
  std::uint32_t sent = 0;
  std::uint32_t received = 0;
  constexpr std::uint32_t kCount = 5000;
  while (received < kCount) {
    // Fill the peer's ring until it refuses, then drain everything.
    while (sent < kCount) {
      const auto m = make_message(sent, size(rng));
      if (!p.a->send(m.data(), m.size())) {
        break;
      }
      ++sent;
    }
    p.b->drain([&](const void* data, std::size_t n) {
      EXPECT_TRUE(check_message(data, n, received)) << "message " << received;
      ++received;
    });
  }
  EXPECT_GT(p.a->stats().tx_full, 0U);  // the ring really filled up
  EXPECT_EQ(p.b->stats().rx_messages, kCount);
  EXPECT_EQ(p.b->stats().rx_crc_errors, 0U);
}

TEST(ChannelTest, PipeStreamWrapsAndStaysInOrder) {
  stream_in_order(kPipe, false);
}

TEST(ChannelTest, SlotStreamStaysInOrder) {
  stream_in_order(kSlot, false);
}

TEST(ChannelTest, GatherWriteIsUsedWhenTheRegionHasIt) {
  stream_in_order(kPipe, true);
  Pair<> p;
  ASSERT_TRUE(p.establish());
  const auto writes_before = p.to_b.writes.load();
  ASSERT_TRUE(p.a->send("abc", 3));
  EXPECT_EQ(p.to_b.writes.load() - writes_before, 2U);  // default writev: header + payload
  p.to_b.gather = true;
  ASSERT_TRUE(p.a->send("abc", 3));
  EXPECT_EQ(p.to_b.writevs.load(), 1U);
}

TEST(ChannelTest, FullRingRefusesThenRecovers) {
  Pair<> p(kSlot, kSlot);
  ASSERT_TRUE(p.establish());
  int accepted = 0;
  while (p.a->send("m", 1)) {
    ++accepted;
  }
  EXPECT_EQ(accepted, 16);  // one record per granule
  EXPECT_EQ(p.a->stats().tx_full, 1U);
  EXPECT_EQ(drain_all(*p.b).size(), 16U);
  EXPECT_TRUE(p.a->send("m", 1));
}

TEST(ChannelTest, ManyDoorbellsOneDrain) {
  Pair<> p;
  ASSERT_TRUE(p.establish());
  const auto bells_at_b = p.to_b.doorbells.load();
  const auto bells_at_a = p.to_a.doorbells.load();
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(p.a->send("m", 1));
  }
  EXPECT_EQ(p.to_b.doorbells.load() - bells_at_b, 5U);  // one per message
  EXPECT_EQ(p.b->drain([](const void*, std::size_t) {}), 5U);
  EXPECT_EQ(p.to_a.doorbells.load() - bells_at_a, 1U);         // one feedback per batch
  EXPECT_EQ(p.b->drain([](const void*, std::size_t) {}), 0U);  // a late doorbell finds nothing
}

TEST(ChannelTest, ReceiveCopiesOneMessageAtATime) {
  Pair<> p;
  ASSERT_TRUE(p.establish());
  char buf[16] = {};
  EXPECT_EQ(p.b->next_size(), 0U);
  EXPECT_EQ(p.b->receive(buf, sizeof(buf)), 0U);
  ASSERT_TRUE(p.a->send("hello", 5));
  ASSERT_TRUE(p.a->send("world!", 6));
  EXPECT_EQ(p.b->next_size(), 5U);
  EXPECT_EQ(p.b->receive(buf, 4), 0U);  // too small: left in place
  EXPECT_EQ(p.b->receive(buf, sizeof(buf)), 5U);
  EXPECT_EQ(std::string(buf, 5), "hello");
  EXPECT_EQ(p.b->next_size(), 6U);
  EXPECT_EQ(p.b->receive(buf, sizeof(buf)), 6U);
  EXPECT_EQ(p.b->receive(buf, sizeof(buf)), 0U);
}

TEST(ChannelTest, CallbackMaySend) {
  Pair<> p;
  ASSERT_TRUE(p.establish());
  ASSERT_TRUE(p.a->send("ping", 4));
  p.b->drain([&](const void* data, std::size_t n) { EXPECT_TRUE(p.b->send(data, n)); });
  const auto echoed = drain_all(*p.a);
  ASSERT_EQ(echoed.size(), 1U);
  EXPECT_EQ(std::string(echoed[0].begin(), echoed[0].end()), "ping");
}

// ---------------------------------------------------------------------------
// Corruption
// ---------------------------------------------------------------------------

// Flips one payload byte of the first record in B's ring.
void corrupt_first_payload(Pair<>& p) {
  p.mem_b.at(layout::kRing + layout::kRecordHeader)[0] ^= 0xFFU;
}

TEST(ChannelTest, SlotRingDropsACorruptMessageAndGoesOn) {
  Pair<> p(kSlot, kSlot);
  ASSERT_TRUE(p.establish());
  ASSERT_TRUE(p.a->send("bad", 3));
  ASSERT_TRUE(p.a->send("good", 4));
  corrupt_first_payload(p);
  const auto got = drain_all(*p.b);
  ASSERT_EQ(got.size(), 1U);
  EXPECT_EQ(std::string(got[0].begin(), got[0].end()), "good");
  EXPECT_EQ(p.b->stats().rx_crc_errors, 1U);
  EXPECT_EQ(p.b->state(), State::kEstablished);
}

TEST(ChannelTest, PipeRingBreaksOnACorruptMessageAndRestartRecovers) {
  Pair<> p(kPipe, kPipe);
  ASSERT_TRUE(p.establish());
  ASSERT_TRUE(p.a->send("bad", 3));
  corrupt_first_payload(p);
  EXPECT_TRUE(drain_all(*p.b).empty());
  EXPECT_EQ(p.b->stats().rx_crc_errors, 1U);
  EXPECT_EQ(p.b->state(), State::kBroken);

  p.b->restart();  // new session for both sides
  ASSERT_TRUE(p.establish());
  EXPECT_EQ(p.a->stats().resets, 1U);
  ASSERT_TRUE(p.a->send("again", 5));
  const auto got = drain_all(*p.b);
  ASSERT_EQ(got.size(), 1U);
  EXPECT_EQ(std::string(got[0].begin(), got[0].end()), "again");
}

TEST(ChannelTest, MalformedHeaderBreaksTheSession) {
  Pair<> p;
  ASSERT_TRUE(p.establish());
  ASSERT_TRUE(p.a->send("abc", 3));
  const std::uint32_t huge = 1U << 30U;
  std::memcpy(p.mem_b.at(layout::kRing), &huge, 4);
  EXPECT_TRUE(drain_all(*p.b).empty());
  EXPECT_EQ(p.b->stats().rx_protocol_errors, 1U);
  EXPECT_EQ(p.b->state(), State::kBroken);
  EXPECT_FALSE(p.b->send("x", 1));
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

TEST(ChannelTest, PeerRestartStartsANewSession) {
  Pair<> p;
  ASSERT_TRUE(p.establish());
  ASSERT_TRUE(p.a->send("lost", 4));  // in flight when B goes away

  p.b.reset();  // B's process dies; its region keeps the old contents
  p.b.emplace(p.mem_b.region(), p.to_a, p.cb);
  ASSERT_TRUE(p.establish());
  EXPECT_EQ(p.a->stats().resets, 1U);
  EXPECT_TRUE(drain_all(*p.b).empty());  // the old session's message is gone

  ASSERT_TRUE(p.a->send("new", 3));
  ASSERT_TRUE(p.b->send("back", 4));
  EXPECT_EQ(drain_all(*p.b).size(), 1U);
  EXPECT_EQ(drain_all(*p.a).size(), 1U);
}

TEST(ChannelTest, PeerWithAFreshRegionStillConnects) {
  Pair<> p;
  ASSERT_TRUE(p.establish());
  p.b.reset();
  p.mem_b.clear();  // the region was recreated: A's hello is gone
  p.b.emplace(p.mem_b.region(), p.to_a, p.cb);
  ASSERT_TRUE(p.establish());
  ASSERT_TRUE(p.a->send("hi", 2));
  EXPECT_EQ(drain_all(*p.b).size(), 1U);
}

TEST(ChannelTest, RestartFromEitherSide) {
  Pair<> p;
  ASSERT_TRUE(p.establish());
  for (int i = 0; i < 3; ++i) {
    const std::uint64_t old_epoch = p.a->epoch();
    p.a->restart();
    EXPECT_NE(p.a->epoch(), old_epoch);
    EXPECT_FALSE(p.a->send("x", 1));
    ASSERT_TRUE(p.establish());
    ASSERT_TRUE(p.a->send("x", 1));
    ASSERT_TRUE(p.b->send("y", 1));
    EXPECT_EQ(drain_all(*p.b).size(), 1U);
    EXPECT_EQ(drain_all(*p.a).size(), 1U);
  }
  EXPECT_EQ(p.b->stats().resets, 3U);
}

TEST(ChannelTest, IncompatiblePeerIsRejected) {
  Pair<> p;
  // A hello with a valid checksum but another version.
  detail::Hello h{};
  h.magic = detail::kMagic;
  h.version = 99;
  h.granule_size = 64;
  h.granule_count = 16384;
  h.max_message = 4096;
  h.ring_offset = layout::kRing;
  h.epoch = 12345;
  h.crc = detail::hello_crc(h);
  std::memcpy(p.mem_a.at(layout::kHello), &h, sizeof(h));
  __atomic_store_n(reinterpret_cast<std::uint64_t*>(p.mem_a.at(layout::kHelloCommit)),
                   h.epoch,
                   __ATOMIC_RELEASE);
  EXPECT_EQ(p.a->poll(), State::kIncompatible);
  EXPECT_EQ(p.a->poll(), State::kIncompatible);  // rejected once, not re-examined
  EXPECT_FALSE(p.a->send("x", 1));
}

TEST(ChannelTest, PeerRingLargerThanItsRegionIsRejected) {
  Pair<> p;
  p.to_b.size_override = 1024;  // B's region looks too small for B's ring
  EXPECT_EQ(p.a->poll(), State::kIncompatible);
}

TEST(ChannelTest, TornHelloIsIgnoredUntilComplete) {
  Pair<> p;
  // Commit word set, block not yet consistent: the reader must wait.
  const std::uint64_t commit = detail::load_acquire(p.mem_a.at(layout::kHelloCommit));
  p.mem_a.at(layout::kHello + 8)[0] ^= 0xFFU;
  EXPECT_EQ(p.a->poll(), State::kHandshaking);
  p.mem_a.at(layout::kHello + 8)[0] ^= 0xFFU;
  EXPECT_EQ(detail::load_acquire(p.mem_a.at(layout::kHelloCommit)), commit);
  EXPECT_TRUE(p.establish());
}

// ---------------------------------------------------------------------------
// Failures of the layer below
// ---------------------------------------------------------------------------

TEST(ChannelTest, FailingSendBreaksTheChannel) {
  Pair<> p;
  ASSERT_TRUE(p.establish());
  p.to_b.fail_after = 0;
  EXPECT_FALSE(p.a->send("x", 1));
  EXPECT_EQ(p.a->state(), State::kBroken);
  EXPECT_EQ(p.a->stats().tx_errors, 1U);
  p.to_b.fail_after = -1;
  p.a->restart();
  EXPECT_TRUE(p.establish());
}

TEST(ChannelTest, FailingFeedbackBreaksTheChannel) {
  Pair<> p;
  ASSERT_TRUE(p.establish());
  ASSERT_TRUE(p.a->send("x", 1));
  p.to_a.fail_after = 0;  // B cannot report what it consumed
  EXPECT_EQ(drain_all(*p.b).size(), 1U);
  EXPECT_EQ(p.b->state(), State::kBroken);
}

TEST(ChannelTest, FailureDuringSetupBreaksTheChannel) {
  Memory peer(region_size(Config{}));
  Memory mine(region_size(Config{}));
  TestRegion remote(peer);
  remote.fail_after = 0;
  Channel ch(mine.region(), remote, Config{});
  EXPECT_EQ(ch.state(), State::kBroken);
}

TEST(ChannelTest, EveryFailurePointBreaksTheChannel) {
  // Skip record write fails: the next message needs a skip in a pipe ring.
  {
    Pair<> p(kPipe, kPipe);
    ASSERT_TRUE(p.establish());
    const auto m = make_message(0, 100);  // 7 granules of 16
    ASSERT_TRUE(p.a->send(m.data(), m.size()));
    drain_all(*p.b);
    ASSERT_TRUE(p.a->send(m.data(), m.size()));
    drain_all(*p.b);
    ASSERT_TRUE(p.a->send(m.data(), m.size()));
    drain_all(*p.b);
    ASSERT_TRUE(p.a->send(m.data(), m.size()));
    drain_all(*p.b);
    // 4 * 7 = 28 granules used; 36 remain before the end. Make the next one long.
    const auto big = make_message(1, 200);  // 13 granules
    ASSERT_TRUE(p.a->send(big.data(), big.size()));
    ASSERT_TRUE(p.a->send(big.data(), big.size()));
    drain_all(*p.b);
    p.to_b.fail_after = 0;  // 54 used: the next 13-granule record needs a skip
    EXPECT_FALSE(p.a->send(big.data(), big.size()));
    EXPECT_EQ(p.a->state(), State::kBroken);
  }
  // Publishing the tail fails after the record was written.
  {
    Pair<> p;
    ASSERT_TRUE(p.establish());
    p.to_b.fail_after = 2;  // header and payload succeed, the tail store fails
    EXPECT_FALSE(p.a->send("abc", 3));
    EXPECT_EQ(p.a->state(), State::kBroken);
  }
  // restart() cannot announce itself.
  {
    Pair<> p;
    ASSERT_TRUE(p.establish());
    p.to_b.fail_after = 0;
    p.a->restart();
    EXPECT_EQ(p.a->state(), State::kBroken);
  }
  // Accepting the peer cannot answer it.
  {
    Pair<> p;
    p.to_b.fail_after = 0;
    EXPECT_EQ(p.a->poll(), State::kBroken);
  }
}

TEST(ChannelTest, SkipRecordPastThePublishedTailIsCorrupt) {
  Pair<> p(kPipe, kPipe);
  ASSERT_TRUE(p.establish());
  // B's peer claims one granule, but that granule says "skip to the end".
  const std::uint32_t skip[2] = {layout::kSkipLen, 0};
  std::memcpy(p.mem_b.at(layout::kRing), skip, sizeof(skip));
  __atomic_store_n(
      reinterpret_cast<std::uint64_t*>(p.mem_b.at(layout::kRxTail)), 1U, __ATOMIC_RELEASE);
  EXPECT_TRUE(drain_all(*p.b).empty());
  EXPECT_EQ(p.b->state(), State::kBroken);
  EXPECT_EQ(p.b->stats().rx_protocol_errors, 1U);
}

TEST(ChannelTest, ReceiveHandlesWrapsAndCorruption) {
  Pair<> p(kPipe, kPipe);
  char buf[256];
  EXPECT_EQ(p.b->receive(buf, sizeof(buf)), 0U);  // not established yet
  ASSERT_TRUE(p.establish());
  std::mt19937 rng(11);
  std::uniform_int_distribution<std::size_t> size(1, kPipe.max_message);
  for (std::uint32_t i = 0; i < 500; ++i) {
    const auto m = make_message(i, size(rng));
    ASSERT_TRUE(p.a->send(m.data(), m.size()));
    const std::size_t n = p.b->receive(buf, sizeof(buf));  // crosses skip records too
    ASSERT_EQ(n, m.size());
    EXPECT_TRUE(check_message(buf, n, i));
  }
  ASSERT_TRUE(p.a->send("bad", 3));
  // Corrupt the length of whatever B reads next (the record, or a skip before it).
  const std::uint64_t consumed_granules =
      detail::load_acquire(p.mem_a.at(layout::kTxHead));  // what B reported to A
  const std::uint64_t at = layout::kRing + (consumed_granules % kPipe.granule_count) * 16;
  const std::uint32_t zero = 0;
  std::memcpy(p.mem_b.at(at), &zero, 4);
  EXPECT_EQ(p.b->next_size(), 0U);
  EXPECT_EQ(p.b->state(), State::kBroken);
}

TEST(ChannelTest, StateNames) {
  EXPECT_STREQ(to_string(State::kHandshaking), "handshaking");
  EXPECT_STREQ(to_string(State::kEstablished), "established");
  EXPECT_STREQ(to_string(State::kBroken), "broken");
  EXPECT_STREQ(to_string(State::kIncompatible), "incompatible");
  EXPECT_STREQ(to_string(static_cast<State>(42)), "unknown");
}

TEST(ChannelTest, PeerCountersFromAnotherSessionAreIgnored) {
  Pair<> p;
  ASSERT_TRUE(p.establish());
  // A's tx_head runs ahead of A's tail, as when the peer starts over: send()
  // refuses without breaking, and poll() sorts out the new session.
  __atomic_store_n(
      reinterpret_cast<std::uint64_t*>(p.mem_a.at(layout::kTxHead)), 1U << 20U, __ATOMIC_RELEASE);
  EXPECT_FALSE(p.a->send("x", 1));
  EXPECT_EQ(p.a->state(), State::kEstablished);
  EXPECT_EQ(p.a->stats().tx_errors, 0U);
}

TEST(MappedRemoteRegionTest, ChecksBoundsAndAlignment) {
  Memory mem(64);
  MappedRemoteRegion r(mem.data(), mem.bytes());
  const char b[8] = {};
  EXPECT_TRUE(r.write(56, b, 8));
  EXPECT_FALSE(r.write(57, b, 8));
  EXPECT_FALSE(r.write(65, b, 0));
  EXPECT_TRUE(r.store64(56, 1));
  EXPECT_FALSE(r.store64(60, 1));
  EXPECT_FALSE(r.store64(64, 1));
  EXPECT_TRUE(r.notify());  // no doorbell: nothing to do
  EXPECT_EQ(r.size(), 64U);
  MappedRemoteRegion strong(mem.data(), mem.bytes(), nullptr, nullptr, true);
  EXPECT_TRUE(strong.store64(0, 2));
}

// ---------------------------------------------------------------------------
// A layer below that delivers late (writes arrive in order, but whenever)
// ---------------------------------------------------------------------------

TEST(ChannelTest, LateDeliveryKeepsTheContract) {
  Pair<DelayedRegion> p(kPipe, kPipe);
  std::mt19937 rng(7);
  std::uniform_int_distribution<std::size_t> pump(0, 3);
  std::uniform_int_distribution<std::size_t> size(1, kPipe.max_message);
  std::uint32_t sent = 0;
  std::uint32_t received = 0;
  constexpr std::uint32_t kCount = 3000;
  for (int round = 0; received < kCount && round < 1000000; ++round) {
    p.to_a.pump(pump(rng));
    p.to_b.pump(pump(rng));
    p.a->poll();
    if (sent < kCount) {
      const auto m = make_message(sent, size(rng));
      if (p.a->send(m.data(), m.size())) {
        ++sent;
      }
    }
    p.b->drain([&](const void* data, std::size_t n) {
      EXPECT_TRUE(check_message(data, n, received)) << "message " << received;
      ++received;
    });
  }
  EXPECT_EQ(received, kCount);
  EXPECT_EQ(p.b->stats().rx_crc_errors, 0U);
  EXPECT_EQ(p.b->stats().rx_protocol_errors, 0U);
}

// ---------------------------------------------------------------------------
// One sending thread, one receiving thread
// ---------------------------------------------------------------------------

TEST(ChannelTest, SenderAndReceiverOnTwoThreads) {
  Pair<> p(kPipe, kPipe);
  ASSERT_TRUE(p.establish());
  constexpr std::uint32_t kCount = 100000;
  std::atomic<bool> bad{false};
  std::thread rx([&] {
    std::uint32_t received = 0;
    while (received < kCount) {
      p.b->drain([&](const void* data, std::size_t n) {
        if (!check_message(data, n, received)) {
          bad = true;
        }
        ++received;
      });
    }
  });
  std::mt19937 rng(3);
  std::uniform_int_distribution<std::size_t> size(1, kPipe.max_message);
  for (std::uint32_t i = 0; i < kCount; ++i) {
    const auto m = make_message(i, size(rng));
    while (!p.a->send(m.data(), m.size())) {
      std::this_thread::yield();
    }
  }
  rx.join();
  EXPECT_FALSE(bad.load());
}

TEST(ChannelTest, RestartWhileTheOtherSideIsSending) {
  Pair<> p(kPipe, kPipe);
  ASSERT_TRUE(p.establish());
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> accepted{0};
  std::thread tx([&] {
    while (!stop.load()) {
      if (p.a->send("payload", 7)) {
        ++accepted;
      }
    }
  });
  // A's receiving side restarts A's session while A's sender keeps going.
  for (int i = 0; i < 50; ++i) {
    p.a->restart();
    while (!p.establish(1)) {
      drain_all(*p.b);
    }
    drain_all(*p.b);
  }
  stop = true;
  tx.join();
  EXPECT_GT(accepted.load(), 0U);
  EXPECT_EQ(p.b->stats().rx_crc_errors, 0U);
  EXPECT_EQ(p.b->stats().rx_protocol_errors, 0U);
}

}  // namespace
}  // namespace pairring
