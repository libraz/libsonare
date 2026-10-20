/// @file tempo_curve_outlier_test.cpp
/// @brief One dominant onset must not rescale the activation weights of every other beat.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "analysis/tempo_curve.h"

using namespace sonare;

namespace {

constexpr int kSampleRate = 22050;
constexpr int kHopLength = 512;

/// @brief 16 beats at 0.5 s then 16 at 0.4 s.
std::vector<Beat> tempo_step_beats() {
  std::vector<Beat> beats;
  double t = 0.0;
  for (int i = 0; i < 32; ++i) {
    beats.push_back({static_cast<float>(t), 0, 1.0f});
    t += i < 16 ? 0.5 : 0.4;
  }
  return beats;
}

/// @brief Envelope of 1.0 at every beat frame; beat @p loud_beat (if >= 0) is raised to @p loud.
std::vector<float> envelope_for(const std::vector<Beat>& beats, int loud_beat, float loud) {
  const double frames_per_sec = static_cast<double>(kSampleRate) / kHopLength;
  std::vector<float> env(static_cast<size_t>(beats.back().time * frames_per_sec) + 8, 0.0f);
  for (size_t i = 0; i < beats.size(); ++i) {
    env[static_cast<size_t>(std::lround(beats[i].time * frames_per_sec))] =
        static_cast<int>(i) == loud_beat ? loud : 1.0f;
  }
  return env;
}

}  // namespace

TEST_CASE("tempo curve follows a tempo step with uniform activation",
          "[analysis][tempo_curve][outlier]") {
  const std::vector<Beat> beats = tempo_step_beats();
  const std::vector<float> curve =
      estimate_beat_local_bpm(beats, envelope_for(beats, -1, 1.0f), kSampleRate, kHopLength);

  REQUIRE(curve.size() == beats.size());
  REQUIRE(curve[2] < 125.0f);
  REQUIRE(curve[20] > 140.0f);
}

TEST_CASE("tempo curve ignores one dominant beat far from a tempo change",
          "[analysis][tempo_curve][outlier]") {
  const std::vector<Beat> beats = tempo_step_beats();
  const std::vector<float> plain =
      estimate_beat_local_bpm(beats, envelope_for(beats, -1, 1.0f), kSampleRate, kHopLength);
  const std::vector<float> loud =
      estimate_beat_local_bpm(beats, envelope_for(beats, 4, 50.0f), kSampleRate, kHopLength);

  REQUIRE(loud.size() == plain.size());
  for (size_t i : {size_t{10}, size_t{20}, size_t{28}}) {
    INFO("beat " << i);
    REQUIRE(std::abs(loud[i] - plain[i]) < 0.5f);
  }
}
