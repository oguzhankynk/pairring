#include "pairring/crc32c.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace pairring {
namespace {

TEST(Crc32cTest, KnownVectors) {
  EXPECT_EQ(crc32c("", 0), 0x00000000U);
  EXPECT_EQ(crc32c("123456789", 9), 0xE3069283U);  // the standard check value
  const unsigned char zeros[32] = {};
  EXPECT_EQ(crc32c(zeros, sizeof(zeros)), 0x8A9136AAU);  // RFC 3720, B.4
}

TEST(Crc32cTest, IncrementalEqualsOneShot) {
  const std::string text = "The quick brown fox jumps over the lazy dog, twice over.";
  for (std::size_t split = 0; split <= text.size(); ++split) {
    Crc32c c;
    c.update(text.data(), split);
    c.update(text.data() + split, text.size() - split);
    EXPECT_EQ(c.value(), crc32c(text.data(), text.size())) << split;
  }
}

}  // namespace
}  // namespace pairring
