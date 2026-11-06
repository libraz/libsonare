/// @file onset_shift_test.cpp
/// @brief Tests for the onset centering shift and envelope frame-to-time conversion.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "core/convert.h"
#include "core/spectrum.h"
#include "feature/onset.h"

using namespace sonare;

namespace {

/// @brief Measures the shift center_onset_strength applies from the position of an impulse.
int applied_shift(const StftConfig& geometry, bool center) {
  std::vector<float> env(64, 0.0f);
  env[3] = 1.0f;
  const std::vector<float> out = center_onset_strength(env, geometry, center);
  REQUIRE(out.size() == env.size());
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] == 1.0f) return static_cast<int>(i) - 3;
  }
  FAIL("impulse was shifted out of the envelope");
  return -1;
}

}  // namespace

TEST_CASE("onset_center_shift_frames matches the applied shift", "[onset][shift]") {
  for (int sr : {22050, 32000, 44100, 48000}) {
    CAPTURE(sr);
    // Window lengths scaled with the rate, as the rate-converted geometry does.
    const int hop = sr / 43;
    const int n_fft = 4 * hop;

    SECTION("default window") {
      StftConfig g;
      g.n_fft = n_fft;
      g.hop_length = hop;
      g.win_length = 0;
      REQUIRE(onset_center_shift_frames(g, true) == applied_shift(g, true));
      REQUIRE(onset_center_shift_frames(g, true) > 0);
    }

    SECTION("explicit windows") {
      for (int win : {hop, hop * 3 / 2, hop * 5 / 2, n_fft - 1, n_fft}) {
        CAPTURE(win);
        StftConfig g;
        g.n_fft = n_fft;
        g.hop_length = hop;
        g.win_length = win;
        REQUIRE(onset_center_shift_frames(g, true) == applied_shift(g, true));
      }
    }

    SECTION("not centered") {
      StftConfig g;
      g.n_fft = n_fft;
      g.hop_length = hop;
      REQUIRE(onset_center_shift_frames(g, false) == 0);
      REQUIRE(applied_shift(g, false) == 0);
      g.win_length = hop * 3 / 2;
      REQUIRE(onset_center_shift_frames(g, false) == 0);
      REQUIRE(applied_shift(g, false) == 0);
    }
  }
}

TEST_CASE("onset centering shifts in place across every size boundary", "[onset][shift]") {
  const std::vector<float> input{1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  const std::vector<float> partial_expected{0.0f, 0.0f, 1.0f, 2.0f, 3.0f};
  const std::vector<float> zero_expected = input;
  const std::vector<float> filled_expected(input.size(), 0.0f);

  REQUIRE(center_onset_strength(input, 1, 1, true) == zero_expected);
  REQUIRE(center_onset_strength(input, 4, 1, true) == partial_expected);
  REQUIRE(center_onset_strength(input, 10, 1, true) == filled_expected);
  REQUIRE(center_onset_strength(input, 20, 1, true) == filled_expected);
  REQUIRE(input == std::vector<float>{1.0f, 2.0f, 3.0f, 4.0f, 5.0f});

  StftConfig geometry;
  geometry.hop_length = 1;
  geometry.win_length = 4;
  REQUIRE(center_onset_strength(input, geometry, true) == partial_expected);
  geometry.win_length = 10;
  REQUIRE(center_onset_strength(input, geometry, true) == filled_expected);
  geometry.win_length = 20;
  REQUIRE(center_onset_strength(input, geometry, true) == filled_expected);
  REQUIRE(center_onset_strength(input, geometry, false) == zero_expected);

  const std::vector<float> empty;
  REQUIRE(center_onset_strength(empty, 10, 1, true).empty());
  REQUIRE(center_onset_strength(empty, geometry, true).empty());
}

TEST_CASE("envelope_frame_time converts envelope frames to event times", "[onset][shift]") {
  REQUIRE(envelope_frame_time(10, 0, 22050, 512) == Catch::Approx(frames_to_time(10, 22050, 512)));
  REQUIRE(envelope_frame_time(10, 2, 22050, 512) == Catch::Approx(frames_to_time(8, 22050, 512)));
  REQUIRE(envelope_frame_time(2, 2, 44100, 512) == 0.0f);
  REQUIRE(envelope_frame_time(1, 4, 44100, 512) == 0.0f);
  REQUIRE(envelope_frame_time(0, 0, 48000, 512) == 0.0f);
}
