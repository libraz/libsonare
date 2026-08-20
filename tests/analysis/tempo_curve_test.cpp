/// @file tempo_curve_test.cpp
/// @brief Tests for per-beat local tempo decoding and its analysis opt-in.

#include "analysis/tempo_curve.h"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "analysis/music_analyzer.h"

using namespace sonare;

namespace {

/// @brief Beats at a constant BPM, with no jitter.
std::vector<Beat> constant_beats(double bpm, int count) {
  std::vector<Beat> beats;
  const double period = 60.0 / bpm;
  for (int i = 0; i < count; ++i) {
    Beat b;
    b.time = static_cast<float>(i * period);
    b.frame = 0;
    b.strength = 1.0f;
    beats.push_back(b);
  }
  return beats;
}

/// @brief Beats accelerating linearly from bpm0 to bpm1.
std::vector<Beat> ramp_beats(double bpm0, double bpm1, int count) {
  std::vector<Beat> beats;
  double t = 0.0;
  for (int i = 0; i < count; ++i) {
    Beat b;
    b.time = static_cast<float>(t);
    b.frame = 0;
    b.strength = 1.0f;
    beats.push_back(b);
    const double frac = static_cast<double>(i) / std::max(1, count - 1);
    t += 60.0 / (bpm0 + (bpm1 - bpm0) * frac);
  }
  return beats;
}

/// @brief A click track whose tempo sweeps linearly from bpm0 to bpm1.
Audio sweeping_clicks(double bpm0, double bpm1, float duration, int sr = 22050) {
  const int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(static_cast<size_t>(n_samples), 0.0f);
  const int click_length = sr / 100;
  double t = 0.0;
  while (t < duration) {
    const int start = static_cast<int>(t * sr);
    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      const float envelope = 1.0f - static_cast<float>(i) / static_cast<float>(click_length);
      samples[static_cast<size_t>(start + i)] = envelope * 0.9f;
    }
    const double frac = t / duration;
    t += 60.0 / (bpm0 + (bpm1 - bpm0) * frac);
  }
  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("tempo curve reports one tempo per beat", "[analysis][tempo_curve]") {
  const std::vector<Beat> beats = constant_beats(120.0, 24);
  const std::vector<float> curve = estimate_beat_local_bpm(beats, {}, 22050, 512);
  REQUIRE(curve.size() == beats.size());
  for (float bpm : curve) {
    REQUIRE(std::isfinite(bpm));
    REQUIRE(bpm > 0.0f);
  }
}

TEST_CASE("tempo curve needs an interval to describe", "[analysis][tempo_curve]") {
  // A tempo is a property of the span between two beats, so a lone beat -- or
  // none -- has no tempo to report. Returning an empty curve rather than a
  // one-entry guess keeps a caller from plotting a number nothing measured.
  REQUIRE(estimate_beat_local_bpm({}, {}, 22050, 512).empty());
  REQUIRE(estimate_beat_local_bpm(constant_beats(120.0, 1), {}, 22050, 512).empty());
  REQUIRE(estimate_beat_local_bpm(constant_beats(120.0, 2), {}, 22050, 512).size() == 2);
}

TEST_CASE("tempo curve holds flat on a constant tempo", "[analysis][tempo_curve]") {
  const std::vector<float> curve =
      estimate_beat_local_bpm(constant_beats(132.0, 40), {}, 22050, 512);
  REQUIRE(!curve.empty());
  const auto bounds = std::minmax_element(curve.begin(), curve.end());
  // Within one step of the log-spaced 64-state grid over [40, 240], which is
  // about 2.9% -- the decoder cannot resolve finer than its own grid.
  REQUIRE((*bounds.second - *bounds.first) / *bounds.first < 0.03);
  REQUIRE(static_cast<double>(curve.front()) == Catch::Approx(132.0).epsilon(0.03));
}

TEST_CASE("tempo curve follows a tempo that moves", "[analysis][tempo_curve]") {
  const std::vector<float> curve =
      estimate_beat_local_bpm(ramp_beats(90.0, 150.0, 64), {}, 22050, 512);
  REQUIRE(curve.size() == 64);
  REQUIRE(static_cast<double>(curve.front()) == Catch::Approx(90.0).epsilon(0.05));
  REQUIRE(static_cast<double>(curve.back()) == Catch::Approx(150.0).epsilon(0.05));
  // Monotone up to the grid's resolution: the decoder may hold a state across
  // several beats, but it must never step back down on a rising tempo.
  for (size_t i = 1; i < curve.size(); ++i) {
    REQUIRE(curve[i] >= curve[i - 1]);
  }
}

TEST_CASE("tempo curve down-weights beats landing on silence", "[analysis][tempo_curve]") {
  // One interval is stretched to half tempo. With every beat trusted equally the
  // curve dips into it; with the stretched beat landing where the envelope is
  // silent, the smoothing prior carries across it instead. This is the whole
  // point of the activation weight, so it is worth pinning rather than assuming.
  std::vector<Beat> beats = constant_beats(120.0, 32);
  for (size_t i = 16; i < beats.size(); ++i) {
    beats[i].time += 0.5f;
  }

  const std::vector<float> unweighted = estimate_beat_local_bpm(beats, {}, 22050, 512);

  const int sr = 22050;
  const int hop = 512;
  const double frames_per_sec = static_cast<double>(sr) / static_cast<double>(hop);
  std::vector<float> envelope(static_cast<size_t>(beats.back().time * frames_per_sec) + 4, 0.0f);
  for (size_t i = 0; i < beats.size(); ++i) {
    // Every beat is struck except the one closing the stretched interval.
    if (i == 16) continue;
    envelope[static_cast<size_t>(std::lround(beats[i].time * frames_per_sec))] = 1.0f;
  }
  const std::vector<float> weighted = estimate_beat_local_bpm(beats, envelope, sr, hop);

  REQUIRE(unweighted.size() == weighted.size());
  const float base = unweighted[0];
  REQUIRE(unweighted[15] < base * 0.95f);
  REQUIRE(weighted[15] == Catch::Approx(base).epsilon(0.03));
}

TEST_CASE("analysis withholds the tempo curve until asked", "[analysis][tempo_curve]") {
  const Audio audio = sweeping_clicks(120.0, 120.0, 6.0f);

  MusicAnalyzer plain(audio);
  const AnalysisResult without = plain.analyze();
  REQUIRE(!without.beats.empty());
  REQUIRE(without.beat_local_bpm.empty());

  MusicAnalyzerConfig config;
  config.compute_tempo_curve = true;
  MusicAnalyzer opted_in(audio, config);
  const AnalysisResult with = opted_in.analyze();
  REQUIRE(with.beat_local_bpm.size() == with.beats.size());
  // Asking for the curve adds an output; it must not move the analysis under it.
  REQUIRE(with.beats.size() == without.beats.size());
  REQUIRE(static_cast<double>(with.bpm) == Catch::Approx(without.bpm));
}

TEST_CASE("analysis tempo curve tracks a sweep when beat tracking follows",
          "[analysis][tempo_curve]") {
  // Two settings, two jobs: compute_tempo_curve decides whether a curve is
  // reported, adaptive_tempo decides whether the beat grid underneath it moved
  // at all. Holding the first on and toggling the second shows the curve is
  // faithful to its grid in both cases -- nearly flat when the tracker held one
  // tempo, sweeping when it did not -- which is the trap the option's docs warn
  // about, pinned as behaviour.
  const Audio audio = sweeping_clicks(90.0, 150.0, 25.0f);

  MusicAnalyzerConfig fixed;
  fixed.compute_tempo_curve = true;
  const AnalysisResult held = MusicAnalyzer(audio, fixed).analyze();

  MusicAnalyzerConfig following = fixed;
  following.adaptive_tempo = true;
  const AnalysisResult tracked = MusicAnalyzer(audio, following).analyze();

  REQUIRE(held.beat_local_bpm.size() == held.beats.size());
  REQUIRE(tracked.beat_local_bpm.size() == tracked.beats.size());

  const auto span = [](const std::vector<float>& curve) {
    const auto bounds = std::minmax_element(curve.begin(), curve.end());
    return (*bounds.second - *bounds.first) / *bounds.first;
  };
  REQUIRE(span(tracked.beat_local_bpm) > 0.25);
  REQUIRE(span(tracked.beat_local_bpm) > span(held.beat_local_bpm) * 2.0);
  REQUIRE(static_cast<double>(tracked.beat_local_bpm.front()) == Catch::Approx(90.0).epsilon(0.15));
  REQUIRE(static_cast<double>(tracked.beat_local_bpm.back()) == Catch::Approx(150.0).epsilon(0.15));
}
