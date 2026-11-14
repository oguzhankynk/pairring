// Compiles every public header on its own, with the project's warnings and
// analyzers (clang-tidy, cppcheck). The library itself is header-only; this
// translation unit is not installed.
#include "pairring/channel.hpp"
#include "pairring/config.hpp"
#include "pairring/crc32c.hpp"
#include "pairring/mapped_region.hpp"
#include "pairring/pairring.hpp"
#include "pairring/region.hpp"
#include "pairring/version.hpp"

namespace {

// Instantiates the templates so that the analyzers see their bodies.
[[maybe_unused]] std::size_t instantiate(pairring::Channel& ch) {
  std::size_t bytes = 0;
  return ch.drain([&bytes](const void* /*payload*/, std::size_t len) { bytes += len; }) + bytes;
}

}  // namespace
