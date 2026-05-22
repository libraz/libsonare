/// @file remix_test.cpp
/// @brief Unit tests for effects/remix.

#include "effects/remix.h"

#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace sonare;

TEST_CASE("remix concatenates intervals without alignment", "[remix][util]") {
  std::vector<float> y{0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f};
  std::vector<std::pair<int, int>> intervals{{2, 4}, {7, 9}};
  auto out = remix(y, intervals, /*align_zeros=*/false);
  REQUIRE(out.size() == 4);
  REQUIRE(out[0] == 2.0f);
  REQUIRE(out[1] == 3.0f);
  REQUIRE(out[2] == 7.0f);
  REQUIRE(out[3] == 8.0f);
}

TEST_CASE("remix reverses intervals", "[remix][util]") {
  std::vector<float> y{1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  std::vector<std::pair<int, int>> intervals{{3, 5}, {0, 3}};
  auto out = remix(y, intervals, /*align_zeros=*/false);
  REQUIRE(out.size() == 5);
  REQUIRE(out[0] == 4.0f);
  REQUIRE(out[1] == 5.0f);
  REQUIRE(out[2] == 1.0f);
  REQUIRE(out[3] == 2.0f);
  REQUIRE(out[4] == 3.0f);
}

TEST_CASE("remix handles empty interval list", "[remix][util][edge]") {
  std::vector<float> y{1.0f, 2.0f, 3.0f};
  auto out = remix(y, {}, /*align_zeros=*/false);
  REQUIRE(out.empty());
}

TEST_CASE("remix clamps interval bounds to signal length", "[remix][util][edge]") {
  std::vector<float> y{1.0f, 2.0f, 3.0f};
  std::vector<std::pair<int, int>> intervals{{1, 10}};
  auto out = remix(y, intervals, /*align_zeros=*/false);
  REQUIRE(out.size() == 2);
  REQUIRE(out[0] == 2.0f);
  REQUIRE(out[1] == 3.0f);
}
