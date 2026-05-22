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
  // Index 3: walks back while energy[i-1] <= energy[i] -> already at local min
  // for the strictly increasing prefix, so backtracks to 0.
  REQUIRE(out[0] == 0);
  // Index 7: energy[6]=0.5 <= 0.7, energy[5]=0.0 <= 0.5 -> walks to 5.
  REQUIRE(out[1] == 5);
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
