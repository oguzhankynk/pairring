#include "pairring/config.hpp"

#include <gtest/gtest.h>

namespace pairring {
namespace {

TEST(ConfigTest, DefaultIsValid) {
  EXPECT_EQ(validate(Config{}), nullptr);
  EXPECT_EQ(region_size(Config{}), layout::kRing + 64U * 16384U);
}

TEST(ConfigTest, RejectsBadGeometry) {
  EXPECT_NE(validate(Config{48, 64, 8}), nullptr);   // granule not a power of two
  EXPECT_NE(validate(Config{8, 64, 8}), nullptr);    // granule too small
  EXPECT_NE(validate(Config{64, 48, 8}), nullptr);   // count not a power of two
  EXPECT_NE(validate(Config{64, 1, 8}), nullptr);    // count too small
  EXPECT_NE(validate(Config{64, 64, 0}), nullptr);   // empty max
  EXPECT_NE(validate(Config{64, 4, 200}), nullptr);  // max beyond half the ring
}

TEST(ConfigTest, MaxMessageMayFillHalfTheRing) {
  // 4 granules of 64: half the ring is 128 bytes, minus the 8-byte header.
  EXPECT_EQ(validate(Config{64, 4, 120}), nullptr);
  EXPECT_NE(validate(Config{64, 4, 121}), nullptr);
}

TEST(ConfigTest, GranulesFor) {
  EXPECT_EQ(granules_for(1, 16), 1U);
  EXPECT_EQ(granules_for(8, 16), 1U);
  EXPECT_EQ(granules_for(9, 16), 2U);
  EXPECT_EQ(granules_for(248, 256), 1U);
  EXPECT_EQ(granules_for(249, 256), 2U);
}

}  // namespace
}  // namespace pairring
