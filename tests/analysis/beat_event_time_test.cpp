/// @file beat_event_time_test.cpp
/// @brief Beat and onset times mark the start of the envelope evidence, not the shifted frame.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "analysis/analysis_rate.h"
#include "analysis/beat_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "core/audio.h"
#include "core/convert.h"
#include "feature/onset.h"
#include "util/constants.h"

using namespace sonare;

namespace {

constexpr int kSampleRate = 22050;
constexpr double kBpm = 120.0;
constexpr double kLeadInSeconds = 0.5;
constexpr int kEventCount = 24;

/// @brief Event onsets at @ref kBpm after @ref kLeadInSeconds of silence.
std::vector<double> event_times() {
  std::vector<double> times;
  for (int i = 0; i < kEventCount; ++i) {
    times.push_back(kLeadInSeconds + i * 60.0 / kBpm);
  }
  return times;
}

/// @brief Short broadband clicks: 10 ms of decaying deterministic noise per event.
Audio click_train() {
  const std::vector<double> times = event_times();
  const double duration = times.back() + 1.0;
  std::vector<float> samples(static_cast<size_t>(duration * kSampleRate), 0.0f);
  uint32_t state = 12345u;
  const int length = kSampleRate / 100;
  for (double t : times) {
    const size_t start = static_cast<size_t>(std::lround(t * kSampleRate));
    for (int i = 0; i < length && start + i < samples.size(); ++i) {
      state = state * 1664525u + 1013904223u;
      const float noise =
          static_cast<float>(state >> 8) / static_cast<float>(1u << 24) * 2.0f - 1.0f;
      samples[start + i] = 0.8f * noise * std::exp(-static_cast<float>(i) / (0.25f * length));
    }
  }
  return Audio::from_vector(std::move(samples), kSampleRate);
}

/// @brief Plucked-style notes: an instant attack, then a 120 ms exponential decay.
Audio decaying_notes() {
  const std::vector<double> times = event_times();
  const double duration = times.back() + 1.0;
  std::vector<float> samples(static_cast<size_t>(duration * kSampleRate), 0.0f);
  const double pitches[] = {220.0, 277.18, 329.63, 440.0};
  const int length = static_cast<int>(0.45 * kSampleRate);
  for (size_t n = 0; n < times.size(); ++n) {
    const size_t start = static_cast<size_t>(std::lround(times[n] * kSampleRate));
    const double f = pitches[n % 4];
    for (int i = 0; i < length && start + i < samples.size(); ++i) {
      const double t = static_cast<double>(i) / kSampleRate;
      samples[start + i] +=
          static_cast<float>(0.6 * std::exp(-t / 0.12) * std::sin(constants::kTwoPiD * f * t));
    }
  }
  return Audio::from_vector(std::move(samples), kSampleRate);
}

BeatConfig beat_config() {
  BeatConfig config;
  config.start_bpm = static_cast<float>(kBpm);
  config.bpm_min = 60.0f;
  config.bpm_max = 180.0f;
  return config;
}

/// @brief Checks every beat against the nearest true onset: median within 40 ms, each within 50 ms.
void require_beats_on_onsets(const BeatAnalyzer& analyzer) {
  const std::vector<double> truth = event_times();
  REQUIRE(analyzer.count() >= static_cast<size_t>(kEventCount / 2));
  std::vector<double> errors;
  for (const Beat& beat : analyzer.beats()) {
    double best = 1.0e9;
    for (double t : truth) best = std::min(best, std::abs(static_cast<double>(beat.time) - t));
    CAPTURE(beat.frame, beat.time, best);
    REQUIRE(best <= 0.050);
    errors.push_back(best);
  }
  std::nth_element(errors.begin(), errors.begin() + errors.size() / 2, errors.end());
  CAPTURE(errors[errors.size() / 2]);
  REQUIRE(errors[errors.size() / 2] <= 0.040);
}

/// @brief Pins times to the shifted frames and the BPM to the frame span.
void require_frame_identity(const BeatAnalyzer& analyzer) {
  const int sr = analyzer.sample_rate();
  const int hop = analyzer.hop_length();
  const int shift = analyzer.onset_shift_frames();
  for (const Beat& beat : analyzer.beats()) {
    CAPTURE(beat.frame, beat.time, shift);
    REQUIRE(beat.time == envelope_frame_time(beat.frame, shift, sr, hop));
  }
  const std::vector<Beat>& beats = analyzer.beats();
  REQUIRE(beats.size() >= 2);
  const double expected_bpm = 60.0 * sr / hop * static_cast<double>(beats.size() - 1) /
                              static_cast<double>(beats.back().frame - beats.front().frame);
  REQUIRE(static_cast<double>(analyzer.bpm()) == Catch::Approx(expected_bpm).epsilon(1.0e-5));
}

}  // namespace

TEST_CASE("beat analyzer reports the front end's centering shift", "[beat][event_time]") {
  const Audio audio = click_train();
  const BeatConfig config = beat_config();
  const BeatAnalyzer analyzer(audio, config);
  const int expected = onset_center_shift_frames(
      stft_config_at_rate(config.n_fft, config.hop_length, audio.sample_rate()), true);
  REQUIRE(expected > 0);
  REQUIRE(analyzer.onset_shift_frames() == expected);
}

TEST_CASE("beats on a click train out of silence sit on the clicks", "[beat][event_time]") {
  const BeatAnalyzer analyzer(click_train(), beat_config());
  require_beats_on_onsets(analyzer);
  require_frame_identity(analyzer);
}

TEST_CASE("beats on decaying notes out of silence sit on the attacks", "[beat][event_time]") {
  const BeatAnalyzer analyzer(decaying_notes(), beat_config());
  require_beats_on_onsets(analyzer);
  require_frame_identity(analyzer);
}

TEST_CASE("an envelope without a stated shift reports frame times", "[beat][event_time]") {
  const BeatAnalyzer from_audio(click_train(), beat_config());
  const BeatAnalyzer from_envelope(from_audio.onset_strength(), from_audio.sample_rate(),
                                   from_audio.hop_length(), beat_config());
  REQUIRE(from_envelope.onset_shift_frames() == 0);
  REQUIRE(from_envelope.beat_frames() == from_audio.beat_frames());
  for (const Beat& beat : from_envelope.beats()) {
    REQUIRE(beat.time ==
            frames_to_time(beat.frame, from_envelope.sample_rate(), from_envelope.hop_length()));
  }
  require_frame_identity(from_envelope);
}

TEST_CASE("onset times sit at the start of the evidence frame", "[beat][event_time]") {
  const Audio audio = decaying_notes();
  for (bool backtrack : {false, true}) {
    OnsetDetectConfig config;
    config.backtrack = backtrack;
    const OnsetAnalyzer analyzer(audio, config);
    CAPTURE(backtrack);
    REQUIRE(analyzer.onset_shift_frames() > 0);
    REQUIRE(analyzer.count() >= static_cast<size_t>(kEventCount / 2));
    const std::vector<int> frames = analyzer.onset_frames();
    for (size_t i = 0; i < analyzer.count(); ++i) {
      const Onset& onset = analyzer.onsets()[i];
      CAPTURE(i, onset.frame, onset.time);
      REQUIRE(frames[i] == onset.frame);
      REQUIRE(onset.time == envelope_frame_time(onset.frame, analyzer.onset_shift_frames(),
                                                analyzer.sample_rate(), analyzer.hop_length()));
    }
  }
}
