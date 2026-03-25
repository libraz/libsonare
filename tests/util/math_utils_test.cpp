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

TEST_CASE("power_to_db basic", "[math_utils]") {
  SECTION("ref=1.0 default") {
    std::vector<float> power = {1.0f, 0.1f, 0.01f};
    std::vector<float> db(3);
    power_to_db(power.data(), 3, 1.0f, 1e-10f, 80.0f, db.data());
    REQUIRE_THAT(db[0], WithinAbs(0.0f, 0.01f));
    REQUIRE_THAT(db[1], WithinAbs(-10.0f, 0.01f));
    REQUIRE_THAT(db[2], WithinAbs(-20.0f, 0.01f));
  }

  SECTION("ref=2.0") {
    std::vector<float> power = {2.0f};
    std::vector<float> db(1);
    power_to_db(power.data(), 1, 2.0f, 1e-10f, -1.0f, db.data());
    REQUIRE_THAT(db[0], WithinAbs(0.0f, 0.01f));
  }

  SECTION("top_db clipping") {
    std::vector<float> power = {1.0f, 1e-20f};
    std::vector<float> db(2);
    power_to_db(power.data(), 2, 1.0f, 1e-10f, 80.0f, db.data());
    REQUIRE(db[1] >= -80.0f);
  }

  SECTION("in-place conversion") {
    std::vector<float> data = {1.0f, 0.1f, 0.01f};
    power_to_db(data.data(), 3, 1.0f, 1e-10f, 80.0f, data.data());
    REQUIRE_THAT(data[0], WithinAbs(0.0f, 0.01f));
    REQUIRE_THAT(data[1], WithinAbs(-10.0f, 0.01f));
    REQUIRE_THAT(data[2], WithinAbs(-20.0f, 0.01f));
  }
}

TEST_CASE("compute_autocorrelation", "[math_utils]") {
  SECTION("sine wave autocorrelation") {
    // Autocorrelation of a sine wave should be a cosine.
    // Use 16 cycles in 1024 samples => period = 64 samples.
    int n = 1024;
    int num_cycles = 16;
    std::vector<float> signal(n);
    for (int i = 0; i < n; ++i) {
      signal[i] =
          std::sin(2.0f * 3.14159f * static_cast<float>(num_cycles) * static_cast<float>(i) / n);
    }
    int max_lag = n / 2;
    std::vector<float> result(max_lag);
    compute_autocorrelation(signal.data(), n, max_lag, result.data());
    // Lag 0 should be ~1.0 (normalized autocorrelation)
    REQUIRE_THAT(result[0], WithinAbs(1.0f, 0.05f));
    // Autocorrelation at one period (lag = n/num_cycles = 64) should be high
    int period = n / num_cycles;
    REQUIRE(result[period] > 0.8f);
    // Autocorrelation at half-period should be negative (anti-correlated)
    REQUIRE(result[period / 2] < -0.5f);
  }

  SECTION("constant signal returns zeros") {
    std::vector<float> signal(128, 5.0f);
    std::vector<float> result(64);
    compute_autocorrelation(signal.data(), 128, 64, result.data());
    // Constant signal has zero variance, so autocorrelation should be all zeros
    for (int i = 0; i < 64; ++i) {
      REQUIRE_THAT(result[i], WithinAbs(0.0f, 1e-6f));
    }
  }
}
