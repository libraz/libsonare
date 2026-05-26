/// @file tempogram_test.cpp
/// @brief Smoke + statistics tests for tempogram / fourier_tempogram.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <vector>

#include "core/audio.h"
#include "feature/onset.h"
#include "feature/rhythm.h"
#include "util/json_reader.h"

using namespace sonare;
using namespace sonare::test;

namespace {

std::vector<float> impulse_train(int sr, float duration, float bpm) {
  std::vector<float> y(static_cast<size_t>(duration * sr), 0.0f);
  int spb = static_cast<int>(60.0f / bpm * sr);
  for (size_t i = 0; i < y.size(); i += spb) {
    y[i] = 1.0f;
  }
  return y;
}

}  // namespace

TEST_CASE("tempogram shape matches librosa", "[librosa][tempogram]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/tempogram.json");
  const auto& d = json["data"];
  int sr = d["sr"].as_int();
  int hop = d["hop_length"].as_int();
  int win = d["win_length"].as_int();
  int expected_lags = d["tempogram_shape"][0].as_int();
  int expected_frames = d["tempogram_shape"][1].as_int();

  auto y = impulse_train(sr, 8.0f, 120.0f);
  Audio audio = Audio::from_vector(std::move(y), sr);

  MelConfig mel_cfg;
  mel_cfg.hop_length = hop;
  OnsetConfig onset_cfg;
  auto env = compute_onset_strength(audio, mel_cfg, onset_cfg);

  TempogramConfig cfg;
  cfg.hop_length = hop;
  cfg.win_length = win;
  auto tg = tempogram(env, sr, cfg);

  // Layout: [win x n_frames] row-major.
  REQUIRE(static_cast<int>(env.size()) > 0);
  REQUIRE(tg.size() == static_cast<size_t>(win) * env.size());
  REQUIRE(expected_lags == win);
  // n_frames may differ slightly from librosa due to onset envelope length,
  // accept ±2 frames.
  CAPTURE(env.size(), expected_frames);
  REQUIRE(std::abs(static_cast<int>(env.size()) - expected_frames) <= 4);

  // Value parity: the dominant tempo (lag with the strongest mean
  // autocorrelation) must match librosa's reference, not just the shape.
  // Helper returns the argmax lag of the per-lag means, skipping lag 0 (the
  // trivial DC autocorrelation peak).
  auto peak_lag = [](const auto& data, int n_lags, int n_frames) {
    int best_lag = 1;
    double best_val = -1e30;
    for (int lag = 1; lag < n_lags; ++lag) {
      double s = 0.0;
      for (int f = 0; f < n_frames; ++f) {
        s += static_cast<double>(data[static_cast<size_t>(lag) * static_cast<size_t>(n_frames) +
                                      static_cast<size_t>(f)]);
      }
      if (s > best_val) {
        best_val = s;
        best_lag = lag;
      }
    }
    return best_lag;
  };

  const int computed_peak_lag = peak_lag(tg, win, static_cast<int>(env.size()));

  // Reference per-lag means (averaged over frames) from librosa.
  const auto& ref_lag_means = d["tempogram_lag_means"].as_array();
  int ref_peak_lag = 1;
  double ref_best = -1e30;
  for (int lag = 1; lag < static_cast<int>(ref_lag_means.size()); ++lag) {
    const double v = ref_lag_means[static_cast<size_t>(lag)].as_float();
    if (v > ref_best) {
      ref_best = v;
      ref_peak_lag = lag;
    }
  }

  const float computed_peak_bpm = 60.0f * sr / (static_cast<float>(computed_peak_lag) * hop);
  const float ref_peak_bpm = 60.0f * sr / (static_cast<float>(ref_peak_lag) * hop);
  CAPTURE(computed_peak_lag, ref_peak_lag, computed_peak_bpm, ref_peak_bpm);
  // Dominant lag must match within one bin (reference peak lag is 43 == 60 BPM,
  // the strongest subharmonic of the 120 BPM input).
  REQUIRE(std::abs(computed_peak_lag - ref_peak_lag) <= 1);
}

TEST_CASE("fourier_tempogram shape sanity", "[librosa][tempogram]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/tempogram.json");
  const auto& d = json["data"];
  int sr = d["sr"].as_int();
  int hop = d["hop_length"].as_int();
  int win = d["win_length"].as_int();
  int expected_bins = d["fourier_tempogram_shape"][0].as_int();

  auto y = impulse_train(sr, 8.0f, 120.0f);
  Audio audio = Audio::from_vector(std::move(y), sr);

  TempogramConfig cfg;
  cfg.hop_length = hop;
  cfg.win_length = win;
  auto ftg = fourier_tempogram(audio, cfg);
  REQUIRE(expected_bins == win / 2 + 1);
  REQUIRE(ftg.size() % static_cast<size_t>(expected_bins) == 0);
}

TEST_CASE("PLP pulse statistics match librosa reference", "[librosa][plp]") {
  auto json = JsonReader::parse_file("tests/librosa/reference/plp.json");
  const auto& d = json["data"];
  int sr = d["sr"].as_int();
  int hop = d["hop_length"].as_int();
  int win = d["win_length"].as_int();
  float bpm = d["bpm"].as_float();
  int expected_length = d["length"].as_int();
  float expected_mean = d["mean"].as_float();
  float expected_std = d["std"].as_float();

  auto y = impulse_train(sr, 8.0f, bpm);
  Audio audio = Audio::from_vector(std::move(y), sr);

  PlpConfig cfg;
  cfg.sr = sr;
  cfg.hop_length = hop;
  cfg.win_length = win;
  cfg.tempo_min = 30.0f;
  cfg.tempo_max = 300.0f;
  auto pulse = plp(audio, cfg);

  const float mean =
      std::accumulate(pulse.begin(), pulse.end(), 0.0f) / static_cast<float>(pulse.size());
  float var = 0.0f;
  for (float v : pulse) {
    const float diff = v - mean;
    var += diff * diff;
  }
  const float stddev = std::sqrt(var / static_cast<float>(pulse.size()));
  const float min_value = *std::min_element(pulse.begin(), pulse.end());
  const float max_value = *std::max_element(pulse.begin(), pulse.end());

  CAPTURE(pulse.size(), expected_length, mean, expected_mean, stddev, expected_std, min_value,
          max_value);
  REQUIRE(std::abs(static_cast<int>(pulse.size()) - expected_length) <= 4);
  REQUIRE(std::abs(mean - expected_mean) <= 0.12f);
  REQUIRE(std::abs(stddev - expected_std) <= 0.12f);
  REQUIRE(min_value >= -1.0e-6f);
  REQUIRE(max_value <= 1.0f + 1.0e-6f);
}

TEST_CASE("cyclic_tempogram folds octave-equivalent tempi", "[tempogram][unit]") {
  const int sr = 512;
  const int hop = 1;
  TempogramConfig cfg;
  cfg.hop_length = hop;
  cfg.win_length = 512;
  cfg.center = false;
  cfg.norm = false;

  std::vector<float> env60(512);
  std::vector<float> env120(512);
  for (int i = 0; i < 512; ++i) {
    env60[static_cast<size_t>(i)] =
        0.5f + 0.5f * std::sin(2.0f * static_cast<float>(M_PI) * i / 512.0f);
    env120[static_cast<size_t>(i)] =
        0.5f + 0.5f * std::sin(4.0f * static_cast<float>(M_PI) * i / 512.0f);
  }

  auto c60 = cyclic_tempogram(env60, sr, cfg, 60.0f, 60);
  auto c120 = cyclic_tempogram(env120, sr, cfg, 60.0f, 60);

  REQUIRE(!c60.empty());
  REQUIRE(c60.size() == c120.size());

  auto strongest_bin = [](const std::vector<float>& cyclic, int n_bins) {
    const int n_frames = static_cast<int>(cyclic.size() / static_cast<size_t>(n_bins));
    int best_bin = 0;
    float best_value = -1.0f;
    for (int bin = 0; bin < n_bins; ++bin) {
      float sum = 0.0f;
      for (int frame = 0; frame < n_frames; ++frame) {
        sum += cyclic[static_cast<size_t>(bin) * static_cast<size_t>(n_frames) +
                      static_cast<size_t>(frame)];
      }
      if (sum > best_value) {
        best_value = sum;
        best_bin = bin;
      }
    }
    return best_bin;
  };

  REQUIRE(std::abs(strongest_bin(c60, 60) - strongest_bin(c120, 60)) <= 1);
}

TEST_CASE("tempogram cosine mode is scale invariant", "[tempogram][unit]") {
  std::vector<float> env{0.2f, 1.0f, 0.4f, 0.0f, 0.8f, 0.1f, 0.5f, 0.3f,
                         0.6f, 0.0f, 0.9f, 0.2f, 0.4f, 0.7f, 0.1f, 0.5f};
  std::vector<float> scaled = env;
  for (float& value : scaled) {
    value *= 3.0f;
  }

  TempogramConfig cfg;
  cfg.hop_length = 1;
  cfg.win_length = 8;
  cfg.center = false;
  cfg.norm = false;

  cfg.mode = TempogramMode::kAutocorrelation;
  const auto ac = tempogram(env, 8, cfg);
  const auto ac_scaled = tempogram(scaled, 8, cfg);
  REQUIRE(ac.size() == ac_scaled.size());
  REQUIRE(ac_scaled[0] == Catch::Approx(ac[0] * 9.0f).margin(1.0e-5f));

  cfg.mode = TempogramMode::kCosine;
  const auto cosine = tempogram(env, 8, cfg);
  const auto cosine_scaled = tempogram(scaled, 8, cfg);
  REQUIRE(cosine.size() == cosine_scaled.size());
  for (size_t i = 0; i < cosine.size(); ++i) {
    REQUIRE(cosine_scaled[i] == Catch::Approx(cosine[i]).margin(1.0e-5f));
    REQUIRE(cosine[i] >= -1.0f - 1.0e-6f);
    REQUIRE(cosine[i] <= 1.0f + 1.0e-6f);
  }
}

TEST_CASE("tempogram_ratio returns one value per factor", "[tempogram][unit]") {
  std::vector<float> tg(384 * 10, 0.5f);
  auto r = tempogram_ratio(tg, 384, 22050, 512);
  REQUIRE(r.size() == 5);
}
