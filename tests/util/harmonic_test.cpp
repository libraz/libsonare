/// @file harmonic_test.cpp
/// @brief Unit tests for core/harmonic.

#include "core/harmonic.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <vector>

#include "util/exception.h"

using namespace sonare;

namespace {
template <typename Fn>
bool throws_invalid_parameter_with_message(Fn&& fn, const char* expected_message) {
  try {
    fn();
  } catch (const SonareException& error) {
    return error.code() == ErrorCode::InvalidParameter &&
           std::string(error.what()).find(expected_message) != std::string::npos;
  } catch (...) {
  }
  return false;
}

// 3-bin spectrum at 3 frames: peak at bin 1.
std::vector<float> make_test_spec() {
  std::vector<float> S{
      0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
  };
  return S;
}
std::vector<float> freqs{100.0f, 200.0f, 400.0f};
}  // namespace

TEST_CASE("interp_harmonics returns expected shape", "[util][harmonic]") {
  auto S = make_test_spec();
  std::vector<float> harm{1.0f, 2.0f};
  auto out = interp_harmonics(S.data(), 3, 3, freqs, harm);
  REQUIRE(out.size() == 2 * 3 * 3);
  // h=1 case: target == bin freq -> unchanged.
  for (int k = 0; k < 3; ++k) {
    for (int t = 0; t < 3; ++t) {
      REQUIRE(out[(0 * 3 + k) * 3 + t] == S[k * 3 + t]);
    }
  }
}

TEST_CASE("salience averages across harmonics", "[util][harmonic]") {
  auto S = make_test_spec();
  std::vector<float> harm{1.0f, 2.0f, 4.0f};
  auto sal = salience(S.data(), 3, 3, freqs, harm);
  REQUIRE(sal.size() == 3 * 3);
  // All non-negative.
  for (float v : sal) REQUIRE(v >= 0.0f);
}

TEST_CASE("f0_harmonics returns shape n_harmonics x n_frames", "[util][harmonic]") {
  auto S = make_test_spec();
  std::vector<float> harm{1.0f, 2.0f};
  std::vector<float> f0{100.0f, 100.0f, 200.0f};
  auto out = f0_harmonics(S.data(), 3, 3, f0, freqs, harm);
  REQUIRE(out.size() == 2 * 3);
}

TEST_CASE("harmonic interpolation defines one-bin boundaries", "[util][harmonic][edge]") {
  const std::vector<float> one_bin{440.0f};
  const std::vector<float> spectrum{3.0f, 4.0f};

  const auto interpolated = interp_harmonics(spectrum, 1, 2, one_bin, {1.0f, 2.0f});
  REQUIRE(interpolated.size() == 4);
  REQUIRE(interpolated[0] == 3.0f);
  REQUIRE(interpolated[1] == 4.0f);
  REQUIRE(interpolated[2] == 0.0f);
  REQUIRE(interpolated[3] == 0.0f);

  const auto sal = salience(spectrum, 1, 2, one_bin, {1.0f, 2.0f}, -2.0f);
  REQUIRE(sal.size() == 2);
  REQUIRE(sal[0] == Catch::Approx(0.5f));
  REQUIRE(sal[1] == Catch::Approx(1.0f));

  const auto f0 = f0_harmonics(spectrum, 1, 2, {440.0f, 880.0f}, one_bin, {1.0f});
  REQUIRE(f0.size() == 2);
  REQUIRE(f0[0] == 3.0f);
  REQUIRE(f0[1] == 0.0f);
}

TEST_CASE("harmonic vector overloads require the declared matrix size", "[util][harmonic][edge]") {
  const std::vector<float> one_bin{440.0f};
  const std::vector<float> two_bins{100.0f, 200.0f};
  const std::vector<float> harmonics{1.0f};

  REQUIRE_THROWS_AS(interp_harmonics({1.0f}, 2, 1, two_bins, harmonics), SonareException);
  REQUIRE_THROWS_AS(interp_harmonics({1.0f, 2.0f, 3.0f}, 2, 1, two_bins, harmonics),
                    SonareException);
  REQUIRE_THROWS_AS(salience({1.0f}, 2, 1, two_bins, harmonics), SonareException);
  REQUIRE_THROWS_AS(salience({1.0f, 2.0f, 3.0f}, 2, 1, two_bins, harmonics), SonareException);
  REQUIRE_THROWS_AS(f0_harmonics({1.0f}, 2, 1, {100.0f}, two_bins, harmonics), SonareException);
  REQUIRE_THROWS_AS(f0_harmonics({1.0f, 2.0f, 3.0f}, 2, 1, {100.0f}, two_bins, harmonics),
                    SonareException);

  // Preserve the pointer overloads' existing empty-harmonics early return,
  // even when the supplied vector does not match a positive matrix shape.
  REQUIRE(interp_harmonics({1.0f}, 2, 1, two_bins, {}).empty());
  REQUIRE(salience({1.0f}, 2, 1, two_bins, {}).empty());
  REQUIRE(f0_harmonics({1.0f}, 2, 1, {100.0f}, two_bins, {}).empty());

  REQUIRE(throws_invalid_parameter_with_message(
      [&] {
        interp_harmonics(std::vector<float>{}, std::numeric_limits<int>::max(), 2,
                         std::vector<float>{}, harmonics);
      },
      "interp_harmonics: x size must equal"));
  REQUIRE(throws_invalid_parameter_with_message(
      [&] {
        salience(std::vector<float>{}, std::numeric_limits<int>::max(), 2, std::vector<float>{},
                 harmonics);
      },
      "salience: S size must equal"));
  REQUIRE(throws_invalid_parameter_with_message(
      [&] {
        f0_harmonics(std::vector<float>{}, std::numeric_limits<int>::max(), 2, {100.0f},
                     std::vector<float>{}, harmonics);
      },
      "f0_harmonics: S size must equal"));
}
