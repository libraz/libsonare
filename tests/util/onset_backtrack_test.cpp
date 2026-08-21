/// @file onset_backtrack_test.cpp
/// @brief Unit tests for onset_backtrack.

#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "feature/onset.h"

using namespace sonare;

TEST_CASE("onset_backtrack snaps to preceding local minimum", "[onset_backtrack][unit]") {
  // Energy curve with a dip at index 3 then rises to a peak at index 6.
  // Events at index 6 should backtrack to index 3.
  std::vector<float> energy = {1.0f, 0.8f, 0.5f, 0.2f, 0.6f, 0.9f, 1.2f, 0.7f};
  std::vector<int> events = {6};
  auto out = onset_backtrack(events, energy);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0] == 3);
}

TEST_CASE("onset_backtrack handles multiple events", "[onset_backtrack][unit]") {
  // Two events: one in a valley region, one near the start.
  std::vector<float> energy = {0.0f, 0.1f, 0.2f, 0.3f, 0.1f, 0.0f, 0.5f, 0.7f};
  std::vector<int> events = {3, 7};
  auto out = onset_backtrack(events, energy);
  REQUIRE(out.size() == 2);
  // The only interior local minimum is index 5: it is non-increasing into
  // (0.0 <= 0.1) and strictly rising out of (0.0 < 0.5). Index 4 fails the
  // second half (0.1 is not < 0.0), and the rising prefix has none at all.
  // Index 3 therefore falls back to the always-present candidate 0 ...
  REQUIRE(out[0] == 0);
  // ... and index 7 takes the last minimum at or before it.
  REQUIRE(out[1] == 5);
}

TEST_CASE("onset_backtrack stops at the right edge of a plateau", "[onset_backtrack][unit]") {
  // The rule that separates librosa's definition from a plain leftward walk.
  // A local minimum is non-increasing into AND strictly rising out of, so a run
  // of equal values resolves to its LAST sample, not its first. Walking back
  // while the previous sample is merely not larger crosses the whole run.
  //
  // A one-shot preceded by silence is exactly this shape, and the difference is
  // between backtracking to the real attack and backtracking to t = 0.
  std::vector<float> leading = {0.0f, 0.0f, 0.0f, 0.0f, 0.1f, 0.5f, 1.0f};
  auto out = onset_backtrack({6}, leading);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0] == 3);

  // Every event resolves to the last minimum at or before it, so events inside
  // the flat run still fall back to 0.
  auto sweep = onset_backtrack({0, 1, 2, 3, 4, 5, 6}, leading);
  REQUIRE(sweep == std::vector<int>{0, 0, 0, 3, 3, 3, 3});

  // The same rule applied to a plateau between two rises.
  std::vector<float> interior = {1.0f, 0.4f, 0.4f, 0.4f, 0.9f, 1.5f, 0.6f, 1.1f};
  REQUIRE(onset_backtrack({5, 7}, interior) == std::vector<int>{3, 6});

  // A curve with no strict rise anywhere has no interior minimum at all.
  std::vector<float> flat = {1.0f, 1.0f, 1.0f, 1.0f};
  REQUIRE(onset_backtrack({0, 3}, flat) == std::vector<int>{0, 0});
}

TEST_CASE("onset_backtrack leaves an event already on a minimum", "[onset_backtrack][unit]") {
  std::vector<float> energy = {1.0f, 0.2f, 0.9f, 0.1f, 0.8f};
  REQUIRE(onset_backtrack({1, 3}, energy) == std::vector<int>{1, 3});
}

TEST_CASE("onset_backtrack falls back to zero with no interior minimum",
          "[onset_backtrack][unit]") {
  // A monotone ramp never satisfies the non-increasing half of the rule.
  std::vector<float> ramp = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f};
  REQUIRE(onset_backtrack({0, 3, 7}, ramp) == std::vector<int>{0, 0, 0});

  // Arrays too short to have an interior index resolve the same way.
  REQUIRE(onset_backtrack({0}, {1.0f}) == std::vector<int>{0});
  REQUIRE(onset_backtrack({0, 1}, {1.0f, 0.5f}) == std::vector<int>{0, 0});
  REQUIRE(onset_backtrack({0, 1}, {0.5f, 1.0f}) == std::vector<int>{0, 0});
}

TEST_CASE("onset_backtrack clips out-of-range events", "[onset_backtrack][unit]") {
  std::vector<float> energy = {1.0f, 0.5f, 0.8f};
  std::vector<int> events = {-5, 100};
  auto out = onset_backtrack(events, energy);
  REQUIRE(out.size() == 2);
  REQUIRE(out[0] >= 0);
  REQUIRE(out[0] < static_cast<int>(energy.size()));
  REQUIRE(out[1] >= 0);
  REQUIRE(out[1] < static_cast<int>(energy.size()));
}

TEST_CASE("onset_backtrack returns empty for empty energy", "[onset_backtrack][unit]") {
  std::vector<float> energy;
  std::vector<int> events = {0, 1, 2};
  auto out = onset_backtrack(events, energy);
  REQUIRE(out.empty());
}

TEST_CASE("onset_backtrack returns empty for empty events", "[onset_backtrack][unit]") {
  std::vector<float> energy = {1.0f, 2.0f, 3.0f};
  std::vector<int> events;
  auto out = onset_backtrack(events, energy);
  REQUIRE(out.empty());
}
