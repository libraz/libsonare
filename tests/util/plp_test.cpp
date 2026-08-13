/// @file plp_test.cpp
/// @brief Smoke tests for Predominant Local Pulse (PLP).

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "feature/onset.h"
#include "feature/rhythm.h"

using namespace sonare;

namespace {

std::vector<float> impulse_train(int sr, float duration, float bpm) {
  std::vector<float> y(static_cast<size_t>(duration * sr), 0.0f);
  const int spb = static_cast<int>(60.0f / bpm * sr);
  for (size_t i = 0; i < y.size(); i += spb) y[i] = 1.0f;
  return y;
}

}  // namespace

TEST_CASE("plp from audio returns a pulse curve of matching length", "[plp][unit][smoke]") {
  auto y = impulse_train(22050, 4.0f, 120.0f);
  Audio audio = Audio::from_vector(std::move(y), 22050);

  PlpConfig cfg;
  cfg.sr = 22050;
  cfg.hop_length = 512;
  cfg.win_length = 384;
  cfg.tempo_min = 30.0f;
  cfg.tempo_max = 300.0f;

  // Length must equal the onset envelope's length.
  MelConfig mcfg;
  mcfg.hop_length = cfg.hop_length;
  OnsetConfig ocfg;
  ocfg.center = true;
  auto env = compute_onset_strength(audio, mcfg, ocfg);

  auto pulse = plp(audio, cfg);
  REQUIRE(pulse.size() == env.size());
  REQUIRE(!pulse.empty());
}

TEST_CASE("plp values are non-negative and bounded in [0, 1]", "[plp][unit][smoke]") {
  auto y = impulse_train(22050, 4.0f, 120.0f);
  Audio audio = Audio::from_vector(std::move(y), 22050);

  PlpConfig cfg;
  auto pulse = plp(audio, cfg);
  REQUIRE(!pulse.empty());
  for (float v : pulse) {
    REQUIRE(v >= 0.0f);
    REQUIRE(v <= 1.0f + 1e-5f);
  }
}

TEST_CASE("plp pulse is phase-locked to the onset envelope", "[plp][unit]") {
  // A pulse curve rebuilt from the tempogram magnitude alone places a cosine
  // peak at every frame centre, so the overlap-add of those mutually
  // misaligned cosines smears into a near-flat curve with no relation to the
  // onsets. Only the tempogram phase ties the maxima to the beat, which is
  // what makes plp() usable for peak-picking beats.
  constexpr int kFrames = 512;
  constexpr int kPeriod = 16;  // onset-envelope frames per beat
  std::vector<float> env(kFrames, 0.0f);
  for (int i = 0; i < kFrames; i += kPeriod) {
    env[i] = 1.0f;
  }

  PlpConfig cfg;
  cfg.sr = 22050;
  cfg.hop_length = 512;
  cfg.win_length = 128;  // exactly eight beat cycles per analysis window
  cfg.tempo_min = 30.0f;
  cfg.tempo_max = 300.0f;

  const auto pulse = plp(env, cfg);
  REQUIRE(pulse.size() == env.size());

  // Restrict to the steady section, one analysis window in from each edge.
  const int first = cfg.win_length;
  const int last = kFrames - cfg.win_length;

  float first_peak = -1.0f;
  float last_peak = -1.0f;
  for (int onset = first; onset < last; onset += kPeriod) {
    int best = onset - kPeriod / 2;
    for (int i = best; i <= onset + kPeriod / 2; ++i) {
      if (pulse[i] > pulse[best]) {
        best = i;
      }
    }
    CAPTURE(onset, best, pulse[best]);
    // The maximum of each beat-wide neighbourhood must land on the onset.
    REQUIRE(std::abs(best - onset) <= 1);
    REQUIRE(pulse[best] > 0.5f);
    if (first_peak < 0.0f) {
      first_peak = pulse[best];
    }
    last_peak = pulse[best];
  }

  // The pulse must not decay over a steady-tempo stretch.
  CAPTURE(first_peak, last_peak);
  REQUIRE(last_peak > 0.7f * first_peak);

  // ...and it must retain contrast rather than settling near its own maximum.
  double interior_sum = 0.0;
  for (int i = first; i < last; ++i) {
    interior_sum += pulse[i];
  }
  const double interior_mean = interior_sum / static_cast<double>(last - first);
  CAPTURE(interior_mean);
  REQUIRE(interior_mean < 0.5);
}

TEST_CASE("plp from onset envelope is consistent with audio overload", "[plp][unit][smoke]") {
  auto y = impulse_train(22050, 4.0f, 120.0f);
  Audio audio = Audio::from_vector(std::move(y), 22050);

  PlpConfig cfg;
  cfg.sr = 22050;
  cfg.hop_length = 512;
  cfg.win_length = 384;

  MelConfig mcfg;
  mcfg.hop_length = cfg.hop_length;
  OnsetConfig ocfg;
  ocfg.center = true;
  auto env = compute_onset_strength(audio, mcfg, ocfg);

  auto pulse_env = plp(env, cfg);
  REQUIRE(pulse_env.size() == env.size());
}
