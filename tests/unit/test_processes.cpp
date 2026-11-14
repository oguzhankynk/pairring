// Two processes sharing memory, with eventfd doorbells: the setup the
// examples use, end to end. The child echoes every message back.

#include "pairring/pairring.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include "test_support.hpp"

#ifdef PAIRRING_COVERAGE
// The child leaves through _exit(), which skips writing coverage data.
extern "C" void __gcov_dump();  // NOLINT(bugprone-reserved-identifier)
#endif

namespace pairring {
namespace {

bool ring_eventfd(void* context) noexcept {
  const std::uint64_t one = 1;
  return ::write(*static_cast<int*>(context), &one, sizeof(one)) == sizeof(one);
}

// Waits for a doorbell (or the timeout) and clears it.
void wait_bell(int fd, int timeout_ms) {
  pollfd pfd{fd, POLLIN, 0};
  if (::poll(&pfd, 1, timeout_ms) > 0) {
    std::uint64_t count = 0;
    (void)!::read(fd, &count, sizeof(count));
  }
}

TEST(ProcessTest, EchoThroughSharedMemory) {
  const Config cfg{64, 1024, 1000};
  const std::size_t bytes = region_size(cfg);
  void* shared =
      ::mmap(nullptr, 2 * bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(shared, MAP_FAILED);
  auto* base = static_cast<unsigned char*>(shared);
  unsigned char* region_parent = base;
  unsigned char* region_child = base + bytes;
  int bell_parent = ::eventfd(0, EFD_NONBLOCK);
  int bell_child = ::eventfd(0, EFD_NONBLOCK);
  ASSERT_GE(bell_parent, 0);
  ASSERT_GE(bell_child, 0);

  constexpr std::uint32_t kCount = 20000;
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    MappedRemoteRegion to_parent(region_parent, bytes, &ring_eventfd, &bell_parent);
    Channel ch({region_child, bytes}, to_parent, cfg);
    std::uint32_t echoed = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (echoed < kCount && std::chrono::steady_clock::now() < deadline) {
      wait_bell(bell_child, 10);
      ch.drain([&](const void* data, std::size_t n) {
        while (!ch.send(data, n)) {
          // The parent drains its ring on its own doorbells.
        }
        ++echoed;
      });
    }
#ifdef PAIRRING_COVERAGE
    __gcov_dump();
#endif
    ::_exit(echoed == kCount ? 0 : 1);
  }

  MappedRemoteRegion to_child(region_child, bytes, &ring_eventfd, &bell_child);
  Channel ch({region_parent, bytes}, to_child, cfg);
  std::uint32_t sent = 0;
  std::uint32_t received = 0;
  bool all_ok = true;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (received < kCount && std::chrono::steady_clock::now() < deadline) {
    // Keep at most a few messages in flight so neither ring can fill up.
    while (sent < kCount && sent - received < 8 && ch.state() == State::kEstablished) {
      const auto m = test::make_message(sent, 1 + sent % cfg.max_message);
      if (!ch.send(m.data(), m.size())) {
        break;
      }
      ++sent;
    }
    wait_bell(bell_parent, 10);
    ch.drain([&](const void* data, std::size_t n) {
      all_ok =
          all_ok && n == 1 + received % cfg.max_message && test::check_message(data, n, received);
      ++received;
    });
  }
  int status = 0;
  ::waitpid(child, &status, 0);
  EXPECT_EQ(received, kCount);
  EXPECT_TRUE(all_ok);
  EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  ::close(bell_parent);
  ::close(bell_child);
  ::munmap(shared, 2 * bytes);
}

}  // namespace
}  // namespace pairring
