/// @file plp_test.cpp
/// @brief Smoke tests for Predominant Local Pulse (PLP).

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
