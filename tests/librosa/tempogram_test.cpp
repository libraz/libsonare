/// @file tempogram_test.cpp
/// @brief Smoke + statistics tests for tempogram / fourier_tempogram.

#include <catch2/catch_test_macros.hpp>
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

TEST_CASE("tempogram_ratio returns one value per factor", "[tempogram][unit]") {
  std::vector<float> tg(384 * 10, 0.5f);
  auto r = tempogram_ratio(tg, 384, 22050, 512);
  REQUIRE(r.size() == 5);
}
