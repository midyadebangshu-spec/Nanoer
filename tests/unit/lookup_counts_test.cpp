// The table of FUSE lookup and open counts, against a std::unordered_map:
// random adds and drops over a range of inode numbers that collide, so
// probing, growth and backward-shift deletion are all exercised.

#include <random>
#include <unordered_map>

#include <gtest/gtest.h>

#include "nanoer/lookup_counts.h"

using namespace nanoer;

TEST(LookupCounts, MatchesAMap) {
  for (std::uint64_t seed : {1, 2, 3}) {
    LookupCounts t;
    std::unordered_map<InodeId, std::uint64_t> model;
    std::mt19937_64 rng(seed);
    for (int step = 0; step < 200000; ++step) {
      // Numbers spread out (as after many creations) and close together.
      InodeId ino = rng() % 2 ? 1 + rng() % 5000 : 1 + (rng() % 300) * 1024;
      int op = static_cast<int>(rng() % 10);
      if (op < 5) {
        std::uint64_t n = 1 + rng() % 3;
        t.add(ino, n);
        model[ino] += n;
      } else if (op < 9) {
        std::uint64_t n = 1 + rng() % 4;
        std::optional<std::uint64_t> left = t.drop(ino, n);
        auto it = model.find(ino);
        ASSERT_EQ(left.has_value(), it != model.end()) << "step " << step;
        if (it != model.end()) {
          it->second = it->second > n ? it->second - n : 0;
          ASSERT_EQ(*left, it->second) << "step " << step;
          if (it->second == 0) model.erase(it);
        }
      } else {
        ASSERT_EQ(t.contains(ino), model.contains(ino)) << "step " << step;
      }
      ASSERT_EQ(t.size(), model.size()) << "step " << step;
    }
    for (const auto& [ino, n] : model) {
      ASSERT_TRUE(t.contains(ino));
      ASSERT_EQ(t.drop(ino, n), std::optional<std::uint64_t>(0));
    }
    EXPECT_EQ(t.size(), 0u);
    t.add(7);
    t.clear();
    EXPECT_FALSE(t.contains(7));
  }
}
