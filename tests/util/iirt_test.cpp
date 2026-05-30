/// @file iirt_test.cpp
/// @brief Smoke tests for librosa.iirt-style filterbank energy.

#include "core/iirt.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "core/audio.h"
#include "util/constants.h"

using namespace sonare;
using namespace sonare::constants;

namespace {

Audio make_tone(float freq, int sr, float duration) {
  const size_t n = static_cast<size_t>(duration * sr);
  std::vector<float> y(n);
  const double tp = static_cast<double>(constants::kTwoPi);
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(sr);
    y[i] = static_cast<float>(std::sin(tp * static_cast<double>(freq) * t));
  }
  return Audio::from_vector(std::move(y), sr);
}

}  // namespace

TEST_CASE("iirt returns expected row-major shape", "[iirt][unit][smoke]") {
  Audio audio = make_tone(440.0f, 22050, 2.0f);

  IirtConfig cfg;  // defaults: 87 filters starting at MIDI 21, sr=22050.
  cfg.sr = audio.sample_rate();
  auto out = iirt(audio, cfg);
  REQUIRE(!out.empty());
  REQUIRE(out.size() % static_cast<size_t>(cfg.n_filters) == 0);
}

TEST_CASE("iirt peak row is near MIDI 69 (A4) for 440Hz tone", "[iirt][unit][smoke]") {
  const int sr = 22050;
  Audio audio = make_tone(440.0f, sr, 2.0f);

  IirtConfig cfg;
  cfg.sr = sr;
  // Keep defaults: n_filters=87, midi_start=21.
  auto out = iirt(audio, cfg);
  REQUIRE(!out.empty());

  const int n_frames = static_cast<int>(out.size()) / cfg.n_filters;
  REQUIRE(n_frames > 0);

  // Sum per row, find argmax row.
  std::vector<float> row_sums(cfg.n_filters, 0.0f);
  for (int r = 0; r < cfg.n_filters; ++r) {
    float s = 0.0f;
    for (int t = 0; t < n_frames; ++t) s += out[r * n_frames + t];
    row_sums[r] = s;
  }
  int peak_row = static_cast<int>(
      std::distance(row_sums.begin(), std::max_element(row_sums.begin(), row_sums.end())));
  // MIDI 69 (A4) corresponds to row 69 - 21 = 48.
  const int expected_row = 69 - cfg.midi_start;
  CAPTURE(peak_row, expected_row);
  // Filterbank Q is moderate, so allow ±2 semitones of slack.
  REQUIRE(std::abs(peak_row - expected_row) <= 2);
}
