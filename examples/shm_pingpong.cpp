// Round trips between two independent processes over POSIX shared memory.
//
//   pairring_shm_pingpong <name> a [count]    # start first: creates the memory
//   pairring_shm_pingpong <name> b [count]    # in another terminal
//
// Both regions live in one shared memory object: [region of a][region of b].
// Each side reads its own region and writes the other's. There is no
// doorbell: both sides spin, which gives the lowest latency (and burns a core).

#include "pairring/pairring.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr pairring::Config kConfig{64, 4096, 1024};

void* map_shared(const std::string& name, bool create, std::size_t bytes) {
  int fd = -1;
  if (create) {
    ::shm_unlink(name.c_str());
    fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || ::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
      return nullptr;
    }
  } else {
    // Wait until side a has created and sized the object.
    for (int tries = 0; tries < 300; ++tries) {
      fd = ::shm_open(name.c_str(), O_RDWR, 0);
      struct stat st {};
      if (fd >= 0 && ::fstat(fd, &st) == 0 && static_cast<std::size_t>(st.st_size) >= bytes) {
        break;
      }
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (fd < 0) {
      return nullptr;
    }
  }
  void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  return p == MAP_FAILED ? nullptr : p;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3 || (std::string(argv[2]) != "a" && std::string(argv[2]) != "b")) {
    std::cerr << "usage: " << argv[0] << " <name> a|b [count]\n";
    return 2;
  }
  const std::string name = std::string("/") + argv[1];
  const bool is_a = std::string(argv[2]) == "a";
  const std::uint32_t count =
      argc > 3 ? static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10)) : 100000;

  const std::size_t region = pairring::region_size(kConfig);
  auto* base = static_cast<unsigned char*>(map_shared(name, is_a, 2 * region));
  if (base == nullptr) {
    std::cerr << "cannot map shared memory " << name << "\n";
    return 1;
  }
  unsigned char* mine = is_a ? base : base + region;
  unsigned char* theirs = is_a ? base + region : base;

  pairring::MappedRemoteRegion remote(theirs, region);
  pairring::Channel ch({mine, region}, remote, kConfig);
  while (ch.poll() != pairring::State::kEstablished) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::cout << "established\n";

  std::vector<double> rtt_us;
  rtt_us.reserve(count);
  std::uint32_t got = 0;
  if (is_a) {
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      while (!ch.send(&i, sizeof(i))) {
      }
      while (ch.drain([&](const void*, std::size_t) { ++got; }) == 0) {
      }
      rtt_us.push_back(
          std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    }
  } else {
    // Count messages, not drain() calls: one drain() delivers everything pending.
    while (got < count) {
      ch.drain([&](const void* p, std::size_t n) {
        while (!ch.send(p, n)) {
        }
        ++got;
      });
    }
  }

  if (is_a) {
    std::sort(rtt_us.begin(), rtt_us.end());
    std::cout << "round trip (us): min " << rtt_us.front() << "  p50 " << rtt_us[rtt_us.size() / 2]
              << "  p99 " << rtt_us[rtt_us.size() * 99 / 100] << "\n";
    ::shm_unlink(name.c_str());
  }
  ::munmap(base, 2 * region);
  return got == count ? 0 : 1;
}
