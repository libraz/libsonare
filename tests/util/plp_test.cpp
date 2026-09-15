/// @file plp_test.cpp
/// @brief Smoke tests for Predominant Local Pulse (PLP).

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

#include "core/audio.h"
#include "feature/onset.h"
#include "feature/rhythm.h"
#include "util/exception.h"

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

TEST_CASE("plp's tempo-band scan keeps the lowest bin of an exact tie", "[plp][unit]") {
  // plp() picks one tempo bin per frame and rebuilds the pulse from that bin alone, so the
  // selected index is the whole of what the band scan contributes. A tie leaves the
  // magnitudes identical and moves the index, which nothing asserted about the pulse's values
  // can see, so this is the one property worth stating about the scan itself.
  //
  // The surface is the library's own: fourier_tempogram() is std::abs() over the same complex
  // tempogram plp() builds internally, from the config below. The reference is the definition
  // -- the largest in-band magnitude, then the lowest bin attaining it -- rather than a copy
  // of either scan, so agreeing with it says something about the result and not about the
  // loop shape. Reconstructing the pulse a different winner would emit needs the tempogram
  // phase, which is not exported, which is why the claim lives on the index sequence.
  constexpr int kFrames = 512;
  constexpr int kPeriod = 16;
  std::vector<float> env(kFrames, 0.0f);
  for (int i = 0; i < kFrames; i += kPeriod) {
    env[i] = 1.0f;
  }

  PlpConfig cfg;
  cfg.sr = 22050;
  cfg.hop_length = 512;
  cfg.win_length = 128;
  // Narrow enough to admit two bins, so a tie between them is the whole contest.
  cfg.tempo_min = 100.0f;
  cfg.tempo_max = 140.0f;

  TempogramConfig tcfg;  // the configuration plp() builds for its own front end
  tcfg.hop_length = cfg.hop_length;
  tcfg.win_length = cfg.win_length;
  tcfg.window = WindowType::Hann;
  tcfg.center = true;
  tcfg.norm = false;
  const std::vector<float> magnitude = fourier_tempogram(env, cfg.sr, tcfg);

  const int n_bins = cfg.win_length / 2 + 1;
  const int n_frames = kFrames;
  REQUIRE(magnitude.size() == static_cast<size_t>(n_bins) * n_frames);

  std::vector<int> band;
  for (int b = 1; b < n_bins; ++b) {
    const double bpm = static_cast<double>(b) / static_cast<double>(cfg.win_length) * 60.0 *
                       static_cast<double>(cfg.sr) / static_cast<double>(cfg.hop_length);
    if (bpm >= cfg.tempo_min && bpm <= cfg.tempo_max) {
      band.push_back(b);
    }
  }
  REQUIRE(band.size() == 2);

  std::vector<int> expected(n_frames, 0);
  int tied_frames = 0;
  for (int t = 0; t < n_frames; ++t) {
    float best = -1.0f;
    for (int b : band) {
      best = std::max(best, magnitude[b * n_frames + t]);
    }
    int hits = 0;
    for (int b : band) {
      if (magnitude[b * n_frames + t] != best) continue;
      if (hits == 0) expected[t] = b;
      ++hits;
    }
    if (hits > 1) ++tied_frames;
  }
  // Random magnitudes essentially never tie, so a fixture without this count is one that runs
  // the scan hundreds of times and its tie-break not once.
  CAPTURE(tied_frames);
  REQUIRE(tied_frames > 0);

  // A frame at a time, every bin compared against that frame's incumbent.
  std::vector<int> per_frame(n_frames, 0);
  for (int t = 0; t < n_frames; ++t) {
    float best = -1.0f;
    for (int b : band) {
      const float v = magnitude[b * n_frames + t];
      if (v > best) {
        best = v;
        per_frame[t] = b;
      }
    }
  }
  // A bin at a time over its contiguous row, carrying one incumbent per frame -- the shape
  // plp() runs. Ascending bins and a strict `>` are what keep the two agreeing under a tie.
  std::vector<int> bin_major(n_frames, 0);
  std::vector<float> running(static_cast<size_t>(n_frames), -1.0f);
  for (int b : band) {
    for (int t = 0; t < n_frames; ++t) {
      const float v = magnitude[b * n_frames + t];
      if (v > running[static_cast<size_t>(t)]) {
        running[static_cast<size_t>(t)] = v;
        bin_major[t] = b;
      }
    }
  }

  for (int t = 0; t < n_frames; ++t) {
    CAPTURE(t, expected[t], per_frame[t], bin_major[t]);
    REQUIRE(per_frame[t] == expected[t]);
    REQUIRE(bin_major[t] == expected[t]);
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

TEST_CASE("plp rejects a non-finite tempo band", "[plp][unit]") {
  // The band test is a pair of relational comparisons, so a NaN bound fails both
  // and the band stops restricting anything instead of rejecting the input.
  const std::vector<float> env(1024, 1.0f);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  PlpConfig nan_min;
  nan_min.tempo_min = nan;
  REQUIRE_THROWS_AS(plp(env, nan_min), SonareException);

  PlpConfig nan_max;
  nan_max.tempo_max = nan;
  REQUIRE_THROWS_AS(plp(env, nan_max), SonareException);

  PlpConfig inf_max;
  inf_max.tempo_max = inf;
  REQUIRE_THROWS_AS(plp(env, inf_max), SonareException);

  PlpConfig inverted;
  inverted.tempo_min = 300.0f;
  inverted.tempo_max = 30.0f;
  REQUIRE_THROWS_AS(plp(env, inverted), SonareException);

  REQUIRE_NOTHROW(plp(env, PlpConfig{}));
}
