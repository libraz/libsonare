/// @file math_utils_test.cpp
/// @brief Tests for math utility functions.

#include "util/math_utils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("mean", "[math_utils]") {
  std::vector<float> data = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  REQUIRE_THAT(mean(data.data(), data.size()), WithinAbs(3.0f, 1e-6f));

  std::vector<float> empty;
  REQUIRE_THAT(mean(empty.data(), empty.size()), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("variance", "[math_utils]") {
  std::vector<float> data = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  REQUIRE_THAT(variance(data.data(), data.size()), WithinAbs(2.0f, 1e-6f));
}

TEST_CASE("stddev", "[math_utils]") {
  std::vector<float> data = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  REQUIRE_THAT(stddev(data.data(), data.size()), WithinAbs(std::sqrt(2.0f), 1e-5f));
}

TEST_CASE("argmax", "[math_utils]") {
  std::vector<float> data = {1.0f, 5.0f, 3.0f, 2.0f};
  REQUIRE(argmax(data.data(), data.size()) == 1);

  std::vector<float> empty;
  REQUIRE(argmax(empty.data(), empty.size()) == 0);
}

TEST_CASE("cosine_similarity", "[math_utils]") {
  std::vector<float> a = {1.0f, 0.0f, 0.0f};
  std::vector<float> b = {1.0f, 0.0f, 0.0f};
  REQUIRE_THAT(cosine_similarity(a.data(), b.data(), a.size()), WithinAbs(1.0f, 1e-6f));

  std::vector<float> c = {0.0f, 1.0f, 0.0f};
  REQUIRE_THAT(cosine_similarity(a.data(), c.data(), a.size()), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("median", "[math_utils]") {
  std::vector<float> odd = {3.0f, 1.0f, 2.0f};
  REQUIRE_THAT(median(odd.data(), odd.size()), WithinAbs(2.0f, 1e-6f));

  std::vector<float> even = {4.0f, 1.0f, 3.0f, 2.0f};
  REQUIRE_THAT(median(even.data(), even.size()), WithinAbs(2.5f, 1e-6f));
}

TEST_CASE("percentile", "[math_utils]") {
  std::vector<float> data = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  REQUIRE_THAT(percentile(data.data(), data.size(), 0.0f), WithinAbs(1.0f, 1e-6f));
  REQUIRE_THAT(percentile(data.data(), data.size(), 50.0f), WithinAbs(3.0f, 1e-6f));
  REQUIRE_THAT(percentile(data.data(), data.size(), 100.0f), WithinAbs(5.0f, 1e-6f));
}

TEST_CASE("next_power_of_2", "[math_utils]") {
  SECTION("powers of 2") {
    REQUIRE(next_power_of_2(1) == 1);
    REQUIRE(next_power_of_2(2) == 2);
    REQUIRE(next_power_of_2(4) == 4);
    REQUIRE(next_power_of_2(1024) == 1024);
  }

  SECTION("non-powers of 2") {
    REQUIRE(next_power_of_2(3) == 4);
    REQUIRE(next_power_of_2(5) == 8);
    REQUIRE(next_power_of_2(7) == 8);
    REQUIRE(next_power_of_2(100) == 128);
    REQUIRE(next_power_of_2(1000) == 1024);
    REQUIRE(next_power_of_2(2000) == 2048);
  }

  SECTION("edge cases") {
    REQUIRE(next_power_of_2(0) == 1);
    REQUIRE(next_power_of_2(-1) == 1);
    REQUIRE(next_power_of_2(-100) == 1);
  }
}
