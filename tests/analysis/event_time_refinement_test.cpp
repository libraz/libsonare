/// @file event_time_refinement_test.cpp
/// @brief The refined time is the steepest fine-envelope rise inside the clipped search window.

#include "analysis/event_time_refinement.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "util/constants.h"

using namespace sonare;

namespace {

constexpr double kToneHz = 1000.0;
constexpr double kDuration = 2.0;

/// @brief Fine hop the refiner uses at @p sr: 1.25 ms rounded to samples.
double fine_hop_s(int sr) { return std::lround(1.25e-3 * sr) / static_cast<double>(sr); }

/// @brief A 1 kHz tone at @p bed_amp that steps to @p amp at @p step_s, reaching it over
///        @p ramp_s (0 for an instant step).
Audio stepped_tone(int sr, double step_s, double amp, double bed_amp = 0.0, double ramp_s = 0.0) {
  std::vector<float> x(static_cast<size_t>(kDuration * sr));
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / sr;
    double a = bed_amp;
    if (t >= step_s) {
      const double frac = ramp_s > 0.0 ? std::min(1.0, (t - step_s) / ramp_s) : 1.0;
      a = bed_amp + (amp - bed_amp) * frac;
    }
    x[i] = static_cast<float>(a * std::sin(constants::kTwoPiD * kToneHz * t));
  }
  return Audio::from_vector(std::move(x), sr);
}

}  // namespace

TEST_CASE("An instant step out of silence is placed within one fine hop of the step",
          "[event_time_refinement]") {
  for (int sr : {22050, 44100, 48000}) {
    INFO("sr = " << sr);
    const double step = 1.0;
    const EventTimeRefiner refiner(stepped_tone(sr, step, 0.5));
    // The evidence frame may sit anywhere in the window; the placement must not depend on it.
    for (float frame_time : {0.97f, 1.0f, 1.03f}) {
      const auto refined = refiner.refine({frame_time}, 0.0464f);
      REQUIRE(refined[0].has_value());
      CHECK(std::abs(*refined[0] - step) < fine_hop_s(sr));
    }
  }
}

TEST_CASE("Three rates place the same step within their two fine hops of each other",
          "[event_time_refinement]") {
  std::vector<float> placed;
  for (int sr : {22050, 44100, 48000}) {
    const EventTimeRefiner refiner(stepped_tone(sr, 0.8123, 0.5));
    const auto refined = refiner.refine({0.8f}, 0.0464f);
    REQUIRE(refined[0].has_value());
    placed.push_back(*refined[0]);
  }
  for (size_t i = 1; i < placed.size(); ++i) {
    CHECK(std::abs(placed[i] - placed[0]) < fine_hop_s(22050) + fine_hop_s(48000));
  }
}

TEST_CASE("A linear ramp is placed at its start, where its level rises fastest",
          "[event_time_refinement]") {
  const int sr = 44100;
  const double step = 1.0;
  const EventTimeRefiner refiner(stepped_tone(sr, step, 0.5, 0.0, 0.03));
  const auto refined = refiner.refine({1.01f}, 0.0464f);
  REQUIRE(refined[0].has_value());
  CHECK(*refined[0] > step - fine_hop_s(sr));
  CHECK(*refined[0] < step + 0.005);
}

TEST_CASE("A step over a sustained bed is read against the local level, not silence",
          "[event_time_refinement]") {
  const int sr = 44100;
  SECTION("a 12 dB step is evidence") {
    const EventTimeRefiner refiner(stepped_tone(sr, 1.0, 0.4, 0.1));
    const auto refined = refiner.refine({1.0f}, 0.0464f);
    REQUIRE(refined[0].has_value());
    CHECK(std::abs(*refined[0] - 1.0) < fine_hop_s(sr) + 1e-6);
  }
  SECTION("a 3 dB step is not") {
    const EventTimeRefiner refiner(stepped_tone(sr, 1.0, 0.1 * std::sqrt(2.0), 0.1));
    CHECK_FALSE(refiner.refine({1.0f}, 0.0464f)[0].has_value());
  }
}

TEST_CASE("Silence and a window that misses the step hold no evidence", "[event_time_refinement]") {
  const int sr = 22050;
  CHECK_FALSE(EventTimeRefiner(stepped_tone(sr, 1.0, 0.0)).refine({1.0f}, 0.0464f)[0]);
  const EventTimeRefiner refiner(stepped_tone(sr, 1.0, 0.5));
  CHECK_FALSE(refiner.refine({0.9f}, 0.0464f)[0].has_value());
  CHECK_FALSE(refiner.refine({1.1f}, 0.0464f)[0].has_value());
}

TEST_CASE("Neighbouring events are searched only up to their midpoint and never cross",
          "[event_time_refinement]") {
  const int sr = 48000;
  const EventTimeRefiner refiner(stepped_tone(sr, 1.0, 0.5));
  const std::vector<float> frames = {0.98f, 1.02f};
  const auto refined = refiner.refine(frames, 0.0464f);
  const double midpoint = 0.5 * (frames[0] + frames[1]);
  REQUIRE(refined[1].has_value());
  CHECK(*refined[1] >= midpoint - 1e-6);
  CHECK(std::abs(*refined[1] - 1.0) < fine_hop_s(sr));
  if (refined[0]) {
    CHECK(*refined[0] < midpoint);
    CHECK(*refined[0] < *refined[1]);
  }
}

TEST_CASE("A signal already at full level at sample 0 has no rise to place",
          "[event_time_refinement]") {
  // Only the half-filled first fine frame precedes the level, a 3 dB rise.
  const EventTimeRefiner refiner(stepped_tone(22050, 0.0, 0.5));
  CHECK_FALSE(refiner.refine({0.0f}, 0.0464f)[0].has_value());
}
