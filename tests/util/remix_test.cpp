/// @file remix_test.cpp
/// @brief Unit tests for effects/remix.

#include "effects/remix.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <utility>
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

namespace {

std::vector<float> tone(float hz, int sample_rate, int n, float amp = 0.3f) {
  std::vector<float> out(static_cast<size_t>(n), 0.0f);
  for (int i = 0; i < n; ++i) {
    out[static_cast<size_t>(i)] =
        amp * std::sin(2.0f * 3.14159265358979f * hz * static_cast<float>(i) /
                       static_cast<float>(sample_rate));
  }
  return out;
}

}  // namespace

TEST_CASE("remix keeps a slice when the material has no zero crossing", "[remix][util][edge]") {
  // Silence, a DC offset and any constant have no sign change, so librosa's
  // zero-crossing set degenerates to the two padded sentinels. Snapping to that
  // collapses every slice, which used to return an EMPTY signal for material as
  // ordinary as a take with a DC offset.
  const std::vector<std::pair<int, int>> intervals{{0, 3500}, {7500, 12000}};

  const std::vector<float> silence(12000, 0.0f);
  REQUIRE(remix(silence, intervals, /*align_zeros=*/false).size() == 8000);
  REQUIRE(remix(silence, intervals, /*align_zeros=*/true).size() == 8000);

  const std::vector<float> dc(12000, 0.5f);
  REQUIRE(remix(dc, intervals, /*align_zeros=*/false).size() == 8000);
  REQUIRE(remix(dc, intervals, /*align_zeros=*/true).size() == 8000);

  const std::vector<float> negative_dc(12000, -0.5f);
  REQUIRE(remix(negative_dc, intervals, /*align_zeros=*/true).size() == 8000);
}

TEST_CASE("remix falls back to the raw slice when snapping would empty it", "[remix][util][edge]") {
  // One isolated impulse: exactly two sign changes, both far from the requested
  // boundaries, so unguarded snapping pulls start and end onto the same point.
  std::vector<float> impulses(12000, 0.0f);
  impulses[6000] = 1.0f;
  impulses[6001] = -1.0f;
  const std::vector<std::pair<int, int>> intervals{{0, 1000}};
  const auto snapped = remix(impulses, intervals, /*align_zeros=*/true);
  REQUIRE(snapped.size() == 1000);
}

TEST_CASE("remix still snaps ordinary material to zero crossings", "[remix][util]") {
  // The guards must not disable snapping for a signal that genuinely crosses:
  // a 440 Hz tone still moves its boundaries.
  constexpr int kSr = 48000;
  const std::vector<float> y = tone(440.0f, kSr, 12000);
  const std::vector<std::pair<int, int>> intervals{{0, 3500}, {7500, 12000}};
  const auto plain = remix(y, intervals, /*align_zeros=*/false);
  const auto snapped = remix(y, intervals, /*align_zeros=*/true);
  REQUIRE(plain.size() == 8000);
  REQUIRE(snapped.size() != plain.size());
}

TEST_CASE("align_remix_intervals returns one cut set usable on every channel", "[remix][util]") {
  constexpr int kSr = 48000;
  // Two channels of a stereo take at different pitches: snapping each channel
  // on its own crossings lands them on different frames, so the channels come
  // out different lengths. One resolved cut set fixes that.
  const std::vector<float> left = tone(440.0f, kSr, kSr);
  const std::vector<float> right = tone(311.13f, kSr, kSr);
  const std::vector<std::pair<int, int>> intervals{{0, 12345}, {23456, kSr}};

  const auto left_only = remix(left, intervals, /*align_zeros=*/true);
  const auto right_only = remix(right, intervals, /*align_zeros=*/true);
  REQUIRE(left_only.size() != right_only.size());

  const auto cuts = align_remix_intervals(left, intervals, /*align_zeros=*/true);
  REQUIRE(cuts.size() == intervals.size());
  size_t total = 0;
  for (const auto& cut : cuts) {
    REQUIRE(cut.first >= 0);
    REQUIRE(cut.second <= kSr);
    REQUIRE(cut.second >= cut.first);
    total += static_cast<size_t>(cut.second - cut.first);
  }
  // Applying the SAME cuts to both channels yields equal lengths.
  REQUIRE(remix(left, cuts, /*align_zeros=*/false).size() == total);
  REQUIRE(remix(right, cuts, /*align_zeros=*/false).size() == total);
  // And re-slicing with the resolved cuts reproduces the single-channel result.
  REQUIRE(remix(left, cuts, /*align_zeros=*/false).size() == left_only.size());
}

TEST_CASE("align_remix_intervals passes intervals through when snapping is off", "[remix][util]") {
  const std::vector<float> y = tone(440.0f, 48000, 1000);
  const std::vector<std::pair<int, int>> intervals{{10, 200}, {-5, 5000}};
  const auto cuts = align_remix_intervals(y, intervals, /*align_zeros=*/false);
  REQUIRE(cuts.size() == 2);
  REQUIRE(cuts[0] == std::pair<int, int>{10, 200});
  REQUIRE(cuts[1] == std::pair<int, int>{0, 1000});
}
