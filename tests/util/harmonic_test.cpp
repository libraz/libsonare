/// @file harmonic_test.cpp
/// @brief Unit tests for core/harmonic.

#include "core/harmonic.h"

#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace sonare;

namespace {
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
