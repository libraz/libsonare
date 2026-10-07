/// @file beat_analyzer_test.cpp
/// @brief Tests for beat analyzer.

#include "analysis/beat_analyzer.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "analysis/meter_analyzer.h"
#include "quick.h"
#include "support/rate_material.h"
#include "util/constants.h"

using namespace sonare;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates a click track at specified BPM.
Audio create_click_track(float bpm, int sr = 22050, float duration = 4.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float beat_interval = 60.0f / bpm;
  int click_length = sr / 100;  // 10ms click

  for (float t = 0.0f; t < duration; t += beat_interval) {
    int start = static_cast<int>(t * sr);
    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * 0.8f;
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a drum pattern with accented downbeats.
Audio create_drum_pattern(float bpm, int beats_per_bar, int sr = 22050, float duration = 4.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float beat_interval = 60.0f / bpm;
  int click_length = sr / 100;

  int beat_count = 0;
  for (float t = 0.0f; t < duration; t += beat_interval) {
    int start = static_cast<int>(t * sr);

    // Downbeat (first beat of bar) is louder
    float amplitude = (beat_count % beats_per_bar == 0) ? 1.0f : 0.5f;

    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * amplitude;
    }
    beat_count++;
  }

  return Audio::from_vector(std::move(samples), sr);
}

float average_interval(const std::vector<int>& frames, size_t begin, size_t end) {
  if (end <= begin + 1) return 0.0f;

  float total = 0.0f;
  for (size_t i = begin + 1; i < end; ++i) {
    total += static_cast<float>(frames[i] - frames[i - 1]);
  }
  return total / static_cast<float>(end - begin - 1);
}

}  // namespace

TEST_CASE("BeatAnalyzer basic", "[beat_analyzer]") {
  Audio audio = create_click_track(120.0f);

  BeatConfig config;
  config.bpm_min = 60.0f;
  config.bpm_max = 200.0f;
  config.start_bpm = 120.0f;

  BeatAnalyzer analyzer(audio, config);

  REQUIRE(analyzer.bpm() >= config.bpm_min);
  REQUIRE(analyzer.bpm() <= config.bpm_max);
  REQUIRE(analyzer.count() > 0);
}

TEST_CASE("BeatAnalyzer beat times", "[beat_analyzer]") {
  float bpm = 120.0f;
  float duration = 4.0f;
  Audio audio = create_click_track(bpm, 22050, duration);

  BeatConfig config;
  config.start_bpm = bpm;
  BeatAnalyzer analyzer(audio, config);

  auto times = analyzer.beat_times();

  // Should have approximately bpm * duration / 60 beats
  float expected_beats = bpm * duration / 60.0f;
  REQUIRE(times.size() >= static_cast<size_t>(expected_beats * 0.5f));
  REQUIRE(times.size() <= static_cast<size_t>(expected_beats * 1.5f));

  // Times should be monotonically increasing
  for (size_t i = 1; i < times.size(); ++i) {
    REQUIRE(times[i] > times[i - 1]);
  }
}

TEST_CASE("BeatAnalyzer beat frames", "[beat_analyzer]") {
  Audio audio = create_click_track(120.0f);

  BeatAnalyzer analyzer(audio);

  auto frames = analyzer.beat_frames();
  auto times = analyzer.beat_times();

  REQUIRE(frames.size() == times.size());

  // Frames should be monotonically increasing
  for (size_t i = 1; i < frames.size(); ++i) {
    REQUIRE(frames[i] > frames[i - 1]);
  }
}

TEST_CASE("BeatAnalyzer 120 BPM tracking", "[beat_analyzer]") {
  Audio audio = create_click_track(120.0f, 22050, 5.0f);

  BeatConfig config;
  config.start_bpm = 120.0f;
  BeatAnalyzer analyzer(audio, config);

  // Estimated BPM should be close to 120
  REQUIRE_THAT(analyzer.bpm(), WithinRel(120.0f, 0.1f));

  // Beat intervals should be approximately 0.5 seconds
  auto times = analyzer.beat_times();
  if (times.size() >= 2) {
    float avg_interval = 0.0f;
    for (size_t i = 1; i < times.size(); ++i) {
      avg_interval += times[i] - times[i - 1];
    }
    avg_interval /= static_cast<float>(times.size() - 1);

    REQUIRE_THAT(avg_interval, WithinRel(0.5f, 0.15f));
  }
}

TEST_CASE("BeatAnalyzer 90 BPM tracking", "[beat_analyzer]") {
  Audio audio = create_click_track(90.0f, 22050, 5.0f);

  BeatConfig config;
  config.start_bpm = 90.0f;
  BeatAnalyzer analyzer(audio, config);

  // Should detect BPM within 5% of 90 or its double-tempo octave (180).
  // Each branch is a tight 5% relative bound rather than a wide absolute union.
  float detected = analyzer.bpm();
  bool close_to_90 = std::abs(detected - 90.0f) <= 0.05f * 90.0f;
  bool close_to_180 = std::abs(detected - 180.0f) <= 0.05f * 180.0f;

  CAPTURE(detected);
  REQUIRE((close_to_90 || close_to_180));
}

TEST_CASE("BeatAnalyzer time signature 4/4", "[beat_analyzer]") {
  Audio audio = create_drum_pattern(120.0f, 4, 22050, 8.0f);

  BeatConfig config;
  config.start_bpm = 120.0f;
  BeatAnalyzer analyzer(audio, config);

  TimeSignature ts = analyzer.time_signature();

  REQUIRE(ts.denominator == 4);
  REQUIRE(ts.confidence >= 0.0f);
  REQUIRE(ts.confidence <= 1.0f);
}

TEST_CASE("BeatAnalyzer time signature 3/4", "[beat_analyzer]") {
  Audio audio = create_drum_pattern(120.0f, 3, 22050, 8.0f);

  BeatConfig config;
  config.start_bpm = 120.0f;
  BeatAnalyzer analyzer(audio, config);

  TimeSignature ts = analyzer.time_signature();
  MeterAnalyzer meter(analyzer.onset_strength(), analyzer.beats());
  std::vector<float> tracked_strengths;
  for (const auto& beat : analyzer.beats()) {
    tracked_strengths.push_back(beat.strength);
  }

  // Must detect a triple meter: 3 beats per bar, or 6 as its compound
  // equivalent. Accepting 4 was removed so a detector that always returns 4/4
  // can no longer pass.
  CAPTURE(ts.numerator, ts.denominator, analyzer.beats().size(), meter.result().candidate_scores,
          tracked_strengths);
  REQUIRE((ts.numerator == 3 || ts.numerator == 6));
  REQUIRE(ts.denominator == 4);
}

TEST_CASE("BeatAnalyzer onset strength", "[beat_analyzer]") {
  Audio audio = create_click_track(120.0f);

  BeatAnalyzer analyzer(audio);

  const auto& onset = analyzer.onset_strength();

  REQUIRE(!onset.empty());
  REQUIRE(analyzer.sample_rate() == audio.sample_rate());
}

TEST_CASE("BeatAnalyzer from onset strength", "[beat_analyzer]") {
  Audio audio = create_click_track(120.0f);

  // First create analyzer from audio
  BeatAnalyzer analyzer1(audio);

  // Then create from onset strength
  BeatAnalyzer analyzer2(analyzer1.onset_strength(), audio.sample_rate(), 512);

  // Both should detect similar BPM
  REQUIRE_THAT(analyzer2.bpm(), WithinRel(analyzer1.bpm(), 0.2f));

  // The onset-strength constructor must also populate downbeats via the base
  // estimate (it cannot use low-frequency-energy observations without audio, but
  // it should not leave downbeats empty when beats were tracked).
  if (!analyzer2.beats().empty()) {
    REQUIRE_FALSE(analyzer2.downbeats().empty());
    REQUIRE_FALSE(analyzer2.downbeat_indices().empty());
  }
}

TEST_CASE("BeatAnalyzer prepends a missed initial grid beat", "[beat_analyzer]") {
  std::vector<float> onset(45, 0.0f);
  for (int frame = 10; frame <= 30; frame += 10) {
    onset[static_cast<size_t>(frame)] = 1.0f;
  }

  BeatConfig config;
  config.start_bpm = 60.0f;
  config.bpm_min = 50.0f;
  config.bpm_max = 70.0f;
  config.trim = false;

  BeatAnalyzer analyzer(onset, 100, 10, config);

  REQUIRE(analyzer.count() >= 4);
  REQUIRE(analyzer.beat_frames().front() == 0);
}

TEST_CASE("BeatAnalyzer adaptive tempo follows synthetic tempo change", "[beat_analyzer]") {
  std::vector<float> onset(220, 0.0f);
  int frame = 5;
  for (int beat = 0; beat < 8; ++beat) {
    onset[static_cast<size_t>(frame)] = 1.0f;
    frame += 10;
  }
  for (int beat = 0; beat < 16 && frame < static_cast<int>(onset.size()); ++beat) {
    onset[static_cast<size_t>(frame)] = 1.0f;
    frame += 6;
  }

  BeatConfig config;
  config.start_bpm = 60.0f;
  config.bpm_min = 50.0f;
  config.bpm_max = 130.0f;
  config.tightness = 25.0f;
  config.trim = false;
  config.adaptive_tempo = true;
  config.tempo_update_interval_beats = 4;

  BeatAnalyzer analyzer(onset, 100, 10, config);
  const auto frames = analyzer.beat_frames();

  REQUIRE(frames.size() >= 12);

  const size_t midpoint = frames.size() / 2;
  const float early_interval = average_interval(frames, 1, midpoint);
  const float late_interval = average_interval(frames, midpoint, frames.size() - 1);

  REQUIRE(early_interval > 0.0f);
  REQUIRE(late_interval > 0.0f);
  REQUIRE(late_interval < early_interval);
  REQUIRE(late_interval < 8.0f);
}

TEST_CASE("BeatAnalyzer refines downbeats from beat-level observations", "[beat_analyzer]") {
  std::vector<float> onset(96, 0.0f);
  for (int frame = 4; frame < static_cast<int>(onset.size()); frame += 8) {
    onset[static_cast<size_t>(frame)] = 1.0f;
  }

  BeatConfig config;
  config.start_bpm = 75.0f;
  config.bpm_min = 60.0f;
  config.bpm_max = 90.0f;
  config.trim = false;

  BeatAnalyzer analyzer(onset, 100, 10, config);
  REQUIRE(analyzer.count() >= 8);

  const int numerator = std::max(2, analyzer.time_signature().numerator);
  const int observed_phase = std::min(2, numerator - 1);
  std::vector<float> low_frequency(analyzer.count(), 0.0f);
  for (size_t i = static_cast<size_t>(observed_phase); i < low_frequency.size();
       i += static_cast<size_t>(numerator)) {
    low_frequency[i] = 1.0f;
  }

  analyzer.refine_downbeats(low_frequency);
  const auto& downbeat_indices = analyzer.downbeat_indices();

  REQUIRE(!downbeat_indices.empty());
  REQUIRE(downbeat_indices.front() == observed_phase);
  for (int index : downbeat_indices) {
    REQUIRE(index % numerator == observed_phase);
  }
}

TEST_CASE("BeatAnalyzer beat strength", "[beat_analyzer]") {
  Audio audio = create_drum_pattern(120.0f, 4, 22050, 4.0f);

  BeatAnalyzer analyzer(audio);

  const auto& beats = analyzer.beats();

  for (const auto& beat : beats) {
    REQUIRE(beat.time >= 0.0f);
    REQUIRE(beat.frame >= 0);
    REQUIRE(beat.strength >= 0.0f);
  }
}

TEST_CASE("detect_beats quick function", "[beat_analyzer]") {
  Audio audio = create_click_track(120.0f);

  auto times = detect_beats(audio);

  REQUIRE(!times.empty());

  // Times should be monotonically increasing
  for (size_t i = 1; i < times.size(); ++i) {
    REQUIRE(times[i] > times[i - 1]);
  }
}

TEST_CASE("BeatAnalyzer trim option", "[beat_analyzer]") {
  Audio audio = create_click_track(120.0f);

  BeatConfig config_trim;
  config_trim.trim = true;
  BeatAnalyzer analyzer_trim(audio, config_trim);

  BeatConfig config_no_trim;
  config_no_trim.trim = false;
  BeatAnalyzer analyzer_no_trim(audio, config_no_trim);

  // Both should have beats
  REQUIRE(analyzer_trim.count() > 0);
  REQUIRE(analyzer_no_trim.count() > 0);
}

TEST_CASE("BeatAnalyzer short audio", "[beat_analyzer]") {
  // Very short audio (1 second)
  Audio audio = create_click_track(120.0f, 22050, 1.0f);

  BeatConfig config;
  BeatAnalyzer analyzer(audio, config);

  // Should still work, though may have fewer beats
  REQUIRE(analyzer.bpm() > 0.0f);
}

TEST_CASE("BeatAnalyzer adversarial short silence + single click", "[beat_analyzer]") {
  // 0.1s of silence with a single click near the start. This exercises the
  // adaptive-DP fallback and the post-DP clamping fix: backtracer can emit
  // frame indices outside [0, n_frames) on tiny buffers.
  constexpr int sr = 22050;
  constexpr float duration = 0.1f;
  const int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);
  // Single click roughly 10ms long, starting at 10ms.
  const int click_start = sr / 100;
  const int click_len = sr / 100;
  for (int i = 0; i < click_len && click_start + i < n_samples; ++i) {
    const float envelope = 1.0f - static_cast<float>(i) / click_len;
    samples[click_start + i] = envelope * 0.8f;
  }
  Audio audio = Audio::from_vector(std::move(samples), sr);

  BeatConfig config;
  config.start_bpm = 120.0f;

  REQUIRE_NOTHROW(BeatAnalyzer(audio, config));
  BeatAnalyzer analyzer(audio, config);
  const auto& beats = analyzer.beats();
  // strength must be a finite, non-negative real (i.e. not uninitialized
  // garbage from OOB read into onset_strength_).
  for (const auto& beat : beats) {
    REQUIRE(std::isfinite(beat.strength));
    REQUIRE(beat.strength >= 0.0f);
  }
}

TEST_CASE("BeatAnalyzer all-zero onset exercises adaptive-DP fallback", "[beat_analyzer]") {
  // No onset energy anywhere — drives the adaptive-DP backtracer through its
  // fallback path. We assert it completes without crashing and yields finite
  // strengths.
  std::vector<float> onset(256, 0.0f);

  BeatConfig config;
  config.start_bpm = 120.0f;
  config.bpm_min = 60.0f;
  config.bpm_max = 200.0f;
  config.trim = false;
  config.adaptive_tempo = true;

  REQUIRE_NOTHROW(BeatAnalyzer(onset, 22050, 512, config));
  BeatAnalyzer analyzer(onset, 22050, 512, config);
  for (const auto& beat : analyzer.beats()) {
    REQUIRE(std::isfinite(beat.strength));
    REQUIRE(beat.strength >= 0.0f);
    REQUIRE(beat.frame >= 0);
  }
}

TEST_CASE("BeatAnalyzer accessors", "[beat_analyzer]") {
  Audio audio = create_click_track(120.0f);

  BeatAnalyzer analyzer(audio);

  REQUIRE(analyzer.sample_rate() == audio.sample_rate());
  REQUIRE(analyzer.hop_length() > 0);
  REQUIRE(analyzer.count() == analyzer.beats().size());
  REQUIRE(analyzer.beat_times().size() == analyzer.count());
  REQUIRE(analyzer.beat_frames().size() == analyzer.count());
}

TEST_CASE("BeatAnalyzer detects an odd meter only when its numerator is a candidate",
          "[beat_analyzer]") {
  for (int numerator : {5, 7}) {
    Audio audio = create_drum_pattern(120.0f, numerator, 22050, 10.0f);

    BeatConfig widened;
    widened.start_bpm = 120.0f;
    widened.meter_candidate_numerators = {3, 4, 5, 6, 7};
    BeatAnalyzer widened_analyzer(audio, widened);

    CAPTURE(numerator, widened_analyzer.count());
    REQUIRE(widened_analyzer.time_signature().numerator == numerator);
    REQUIRE(widened_analyzer.time_signature().denominator == 4);
    // The phase the meter estimate settled on has to address a beat inside the
    // bar it belongs to.
    REQUIRE(widened_analyzer.downbeat_phase() >= 0);
    REQUIRE(widened_analyzer.downbeat_phase() < numerator);

    // Same audio, default candidate set: the odd numerator is unreachable, so
    // the detection above is the config taking effect and not the pattern.
    BeatConfig default_config;
    default_config.start_bpm = 120.0f;
    BeatAnalyzer default_analyzer(audio, default_config);
    const int default_numerator = default_analyzer.time_signature().numerator;
    CAPTURE(default_numerator);
    REQUIRE(default_numerator != numerator);
    REQUIRE((default_numerator == 3 || default_numerator == 4 || default_numerator == 6));
  }
}

TEST_CASE("BeatAnalyzer reports the requested beat unit", "[beat_analyzer]") {
  Audio audio = create_drum_pattern(120.0f, 4, 22050, 8.0f);

  BeatConfig eighth;
  eighth.start_bpm = 120.0f;
  eighth.meter_denominator = 8;
  BeatAnalyzer eighth_analyzer(audio, eighth);

  BeatConfig default_config;
  default_config.start_bpm = 120.0f;
  BeatAnalyzer default_analyzer(audio, default_config);

  CAPTURE(eighth_analyzer.time_signature().numerator);
  REQUIRE(eighth_analyzer.time_signature().denominator == 8);
  REQUIRE(default_analyzer.time_signature().denominator == 4);
}

TEST_CASE("BeatAnalyzer uses the requested beat unit for the too-few-beats default",
          "[beat_analyzer]") {
  // One second of audio cannot produce the eight beats the meter estimator
  // needs, and the low-confidence fallback still has to answer in the unit the
  // caller asked for.
  Audio audio = create_drum_pattern(120.0f, 4, 22050, 1.0f);

  BeatConfig config;
  config.start_bpm = 120.0f;
  config.meter_denominator = 8;
  BeatAnalyzer analyzer(audio, config);

  REQUIRE(analyzer.count() < 8);
  REQUIRE(analyzer.time_signature().numerator == 4);
  REQUIRE(analyzer.time_signature().denominator == 8);
  REQUIRE(analyzer.downbeat_phase() == 0);
}

TEST_CASE("BeatAnalyzer downbeat phase and indices address its own beats", "[beat_analyzer]") {
  Audio audio = create_drum_pattern(120.0f, 4, 22050, 8.0f);

  BeatConfig config;
  config.start_bpm = 120.0f;
  BeatAnalyzer analyzer(audio, config);

  const int numerator = analyzer.time_signature().numerator;
  REQUIRE(numerator > 0);
  CAPTURE(numerator, analyzer.downbeat_phase(), analyzer.count());
  REQUIRE(analyzer.downbeat_phase() >= 0);
  REQUIRE(analyzer.downbeat_phase() < numerator);

  const auto& indices = analyzer.downbeat_indices();
  REQUIRE_FALSE(indices.empty());
  REQUIRE(indices.size() == analyzer.downbeats().size());

  int previous = -1;
  for (size_t i = 0; i < indices.size(); ++i) {
    CAPTURE(i, indices[i]);
    REQUIRE(indices[i] >= 0);
    REQUIRE(indices[i] < static_cast<int>(analyzer.beats().size()));
    REQUIRE(indices[i] > previous);
    previous = indices[i];
    REQUIRE(analyzer.downbeats()[i].time == analyzer.beats()[static_cast<size_t>(indices[i])].time);
  }
}

TEST_CASE("BeatAnalyzer retains the beat-level evidence the downbeat pass scored",
          "[beat_analyzer]") {
  Audio audio = create_drum_pattern(120.0f, 4, 22050, 4.0f);
  BeatAnalyzer analyzer(audio);

  const size_t beat_count = analyzer.beats().size();
  REQUIRE(beat_count >= 4);

  // Every populated stream indexes in parallel with beats().
  REQUIRE(analyzer.beat_onset_observations().size() == beat_count);
  REQUIRE(analyzer.beat_low_frequency_observations().size() == beat_count);
  // Chord changes only arrive from a caller that has analyzed chords, which
  // BeatAnalyzer never does on its own.
  REQUIRE(analyzer.beat_chord_change_observations().empty());

  for (size_t i = 0; i < beat_count; ++i) {
    CAPTURE(i, analyzer.beat_onset_observations()[i],
            analyzer.beat_low_frequency_observations()[i]);
    REQUIRE(std::isfinite(analyzer.beat_onset_observations()[i]));
    REQUIRE(analyzer.beat_onset_observations()[i] >= 0.0f);
    REQUIRE(std::isfinite(analyzer.beat_low_frequency_observations()[i]));
    REQUIRE(analyzer.beat_low_frequency_observations()[i] >= 0.0f);
  }

  // The exposed onset observation is a beat-local window; Beat::strength is a
  // single unwindowed envelope frame. Being able to read the windowed quantity
  // is the reason the accessor exists, so the two must not collapse into one.
  bool differs = false;
  for (size_t i = 0; i < beat_count && !differs; ++i) {
    differs = analyzer.beat_onset_observations()[i] != analyzer.beats()[i].strength;
  }
  REQUIRE(differs);
}

TEST_CASE("BeatAnalyzer without audio scores onsets but has no low-frequency evidence",
          "[beat_analyzer]") {
  Audio audio = create_drum_pattern(120.0f, 4, 22050, 4.0f);
  BeatAnalyzer audio_backed(audio);
  BeatAnalyzer onset_backed(audio_backed.onset_strength(), audio.sample_rate(), 512);

  REQUIRE_FALSE(onset_backed.beats().empty());
  REQUIRE(onset_backed.beat_onset_observations().size() == onset_backed.beats().size());
  // Low-frequency energy is measured on the waveform, which this constructor
  // never receives, and chord changes need an analysis this class does not run.
  REQUIRE(onset_backed.beat_low_frequency_observations().empty());
  REQUIRE(onset_backed.beat_chord_change_observations().empty());

  // The audio-backed constructor does fill it, so the emptiness above is the
  // absent audio rather than an accessor that never populates.
  REQUIRE_FALSE(audio_backed.beat_low_frequency_observations().empty());
}

TEST_CASE("BeatAnalyzer republishes the observations of the latest refinement", "[beat_analyzer]") {
  // MusicAnalyzer refines a second time once chords exist, so the accessors have
  // to expose the evidence of the most recent pass and not the first one.
  Audio audio = create_drum_pattern(120.0f, 4, 22050, 4.0f);
  BeatAnalyzer analyzer(audio);

  const size_t beat_count = analyzer.beats().size();
  REQUIRE(beat_count >= 4);
  const std::vector<float> initial_low_frequency = analyzer.beat_low_frequency_observations();
  REQUIRE(analyzer.beat_chord_change_observations().empty());

  const std::vector<float> low_frequency(beat_count, 0.25f);
  std::vector<float> chord_changes(beat_count, 0.0f);
  for (size_t i = 0; i < beat_count; i += 4) {
    chord_changes[i] = 1.0f;
  }
  // Otherwise the equality below could hold without the refinement replacing
  // anything.
  REQUIRE(initial_low_frequency != low_frequency);

  analyzer.refine_downbeats(low_frequency, chord_changes);

  REQUIRE(analyzer.beat_low_frequency_observations() == low_frequency);
  REQUIRE(analyzer.beat_chord_change_observations() == chord_changes);
  // The onset window is recomputed by the refinement and stays one per beat.
  REQUIRE(analyzer.beat_onset_observations().size() == beat_count);
}

namespace {

/// @brief One bar of an accented groove: per-beat hit kind and peak amplitude.
struct GrooveBeat {
  bool kick;
  float amp;
};

/// @brief Renders a 120 BPM kick/noise-hit groove with eighth-note hats, starting
///        `start_beat` beats into the bar pattern so a clip can open mid-bar.
std::vector<float> render_groove(const std::vector<GrooveBeat>& bar, int total_beats,
                                 int start_beat, int sr) {
  constexpr float kBeatSeconds = 0.5f;
  const size_t length = static_cast<size_t>((total_beats * kBeatSeconds + 0.4f) * sr);
  std::vector<float> out(length, 0.0f);
  uint32_t state = 0x7a3cf611u;
  auto noise = [&state]() {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) / static_cast<float>(1u << 24) * 2.0f - 1.0f;
  };
  auto add_noise = [&](size_t start, float amp, float decay) {
    const size_t n = static_cast<size_t>(6.0f * decay * sr);
    for (size_t j = 0; j < n && start + j < length; ++j) {
      out[start + j] += amp * std::exp(-static_cast<float>(j) / sr / decay) * noise();
    }
  };
  auto add_kick = [&](size_t start, float amp) {
    constexpr float kDecay = 0.16f;
    const size_t n = static_cast<size_t>(6.0f * kDecay * sr);
    double phase = 0.0;
    for (size_t j = 0; j < n && start + j < length; ++j) {
      const float t = static_cast<float>(j) / sr;
      phase += sonare::constants::kTwoPiD * (55.0 + 90.0 * std::exp(-t / 0.03)) / sr;
      const float click = t < 0.004f ? (1.0f - t / 0.004f) * noise() * 0.5f : 0.0f;
      out[start + j] += amp * (std::exp(-t / kDecay) * static_cast<float>(std::sin(phase)) + click);
    }
  };

  const int numerator = static_cast<int>(bar.size());
  for (int k = 0; k < total_beats; ++k) {
    const GrooveBeat& beat = bar[static_cast<size_t>((k + start_beat) % numerator)];
    const size_t start = static_cast<size_t>(k * kBeatSeconds * sr);
    if (beat.kick) {
      add_kick(start, beat.amp);
    } else {
      add_noise(start, beat.amp, 0.045f);
    }
    add_noise(start, 0.09f, 0.01f);
    add_noise(start + static_cast<size_t>(0.5f * kBeatSeconds * sr), 0.063f, 0.01f);
  }
  return out;
}

/// @brief Requires the reported downbeats to be exactly the true bar starts.
/// @details A bar start inside the first 0.25 s is not required: the tracker
///          follows the reference beat tracker in reporting no beat at t = 0.
void require_bar_starts(const std::vector<float>& downbeats, float first_bar, float bar_seconds,
                        float duration) {
  constexpr float kTolerance = 0.05f;
  CAPTURE(downbeats, first_bar, bar_seconds);
  REQUIRE_FALSE(downbeats.empty());
  for (float time : downbeats) {
    const float bars = (time - first_bar) / bar_seconds;
    CAPTURE(time);
    REQUIRE(std::abs(bars - std::round(bars)) * bar_seconds <= kTolerance);
  }
  for (float bar = first_bar; bar < duration - 0.3f; bar += bar_seconds) {
    if (bar < 0.25f) continue;
    const bool found = std::any_of(downbeats.begin(), downbeats.end(), [bar](float time) {
      return std::abs(time - bar) <= kTolerance;
    });
    CAPTURE(bar);
    REQUIRE(found);
  }
}

}  // namespace

TEST_CASE("detect_downbeats places the bar on the kick of a 4/4 backbeat groove",
          "[beat_analyzer][downbeat]") {
  constexpr int kSr = 32000;
  const std::vector<GrooveBeat> bar{{true, 1.0f}, {false, 0.25f}, {false, 0.18f}, {false, 0.25f}};
  const auto samples = render_groove(bar, 16, 0, kSr);
  const auto downbeats = quick::detect_downbeats(samples.data(), samples.size(), kSr);
  require_bar_starts(downbeats, 0.0f, 2.0f, 8.0f);
}

TEST_CASE("detect_downbeats places the bar on the kick of a 3/4 groove",
          "[beat_analyzer][downbeat]") {
  constexpr int kSr = 32000;
  const std::vector<GrooveBeat> bar{{true, 1.0f}, {false, 0.45f}, {false, 0.45f}};
  const auto samples = render_groove(bar, 30, 0, kSr);
  const auto downbeats = quick::detect_downbeats(samples.data(), samples.size(), kSr);
  require_bar_starts(downbeats, 0.0f, 1.5f, 15.0f);
}

TEST_CASE("detect_downbeats prefers the louder of two kicks in a short 4/4 kit groove",
          "[beat_analyzer][downbeat]") {
  constexpr int kSr = 32000;
  const std::vector<GrooveBeat> bar{{true, 1.0f}, {false, 0.6f}, {true, 0.8f}, {false, 0.6f}};
  const auto samples = render_groove(bar, 8, 0, kSr);
  const auto downbeats = quick::detect_downbeats(samples.data(), samples.size(), kSr);
  require_bar_starts(downbeats, 0.0f, 2.0f, 4.0f);
}

TEST_CASE("detect_downbeats finds the bar when a 4/4 clip opens mid-bar",
          "[beat_analyzer][downbeat]") {
  constexpr int kSr = 32000;
  const std::vector<GrooveBeat> bar{{true, 1.0f}, {false, 0.25f}, {false, 0.18f}, {false, 0.25f}};
  // The clip opens on beat 3, so bars start one second in.
  const auto samples = render_groove(bar, 18, 2, kSr);
  const auto downbeats = quick::detect_downbeats(samples.data(), samples.size(), kSr);
  require_bar_starts(downbeats, 1.0f, 2.0f, 9.0f);
}

TEST_CASE("BeatAnalyzer with trim returns no beats for silent input", "[beat_analyzer]") {
  const int sr = 22050;
  Audio silence = Audio::from_vector(std::vector<float>(static_cast<size_t>(sr) * 6, 0.0f), sr);

  BeatConfig config;
  config.trim = true;
  BeatAnalyzer analyzer(silence, config);

  REQUIRE(analyzer.beats().empty());
  REQUIRE(analyzer.time_signature().confidence == 0.0f);
}

namespace {

constexpr int kTrimSr = 22050;
constexpr float kTrimBodySeconds = 30.0f;

/// @brief Deterministic white noise of the given RMS, mixed into @p samples over [start,
/// start+len).
void add_white_noise(std::vector<float>& samples, size_t start, size_t len, float rms,
                     std::mt19937& rng) {
  const float peak = rms * std::sqrt(3.0f);  // uniform [-peak, peak] has RMS peak / sqrt(3)
  for (size_t i = start; i < start + len && i < samples.size(); ++i) {
    const float u = static_cast<float>(rng()) / 4294967295.0f;
    samples[i] += peak * (2.0f * u - 1.0f);
  }
}

/// @brief 120 BPM 1 kHz decaying clicks over [start_sec, start_sec + seconds).
void add_click_train(std::vector<float>& samples, float start_sec, float seconds, float amplitude) {
  constexpr int kClickLength = 400;
  const float beat_interval = 0.5f;
  for (float t = 0.0f; t < seconds; t += beat_interval) {
    const size_t start = static_cast<size_t>((start_sec + t) * static_cast<float>(kTrimSr));
    for (int n = 0; n < kClickLength && start + static_cast<size_t>(n) < samples.size(); ++n) {
      const float fn = static_cast<float>(n);
      samples[start + static_cast<size_t>(n)] +=
          amplitude * std::sin(constants::kTwoPi * 1000.0f * fn / static_cast<float>(kTrimSr)) *
          std::exp(-fn / 60.0f);
    }
  }
}

int count_beats_in(const BeatAnalyzer& analyzer, float from_sec, float to_sec) {
  int count = 0;
  for (const auto& beat : analyzer.beats()) {
    if (beat.time >= from_sec && beat.time < to_sec) ++count;
  }
  return count;
}

/// @brief Body clicks + 10 s of -30 dB edge clicks, -90 dB noise floor, optional +24 dB burst.
int count_edge_beats(bool with_burst) {
  constexpr float kBodyAmp = 0.5f;
  const size_t total = static_cast<size_t>(40.0f * static_cast<float>(kTrimSr));
  std::vector<float> samples(total, 0.0f);
  std::mt19937 rng(12345);
  add_click_train(samples, 0.0f, kTrimBodySeconds, kBodyAmp);
  add_click_train(samples, kTrimBodySeconds, 10.0f, kBodyAmp * std::pow(10.0f, -30.0f / 20.0f));
  add_white_noise(samples, 0, total, std::pow(10.0f, -90.0f / 20.0f), rng);
  if (with_burst) {
    add_white_noise(samples, static_cast<size_t>(15.0f * kTrimSr), kTrimSr / 20,
                    kBodyAmp * std::pow(10.0f, 24.0f / 20.0f), rng);
  }

  BeatConfig config;
  config.trim = true;
  BeatAnalyzer analyzer(Audio::from_vector(std::move(samples), kTrimSr), config);
  const int edge = count_beats_in(analyzer, 30.2f, 41.0f);
  INFO("with_burst=" << with_burst << " edge beats=" << edge
                     << " total=" << analyzer.beats().size());
  return edge;
}

}  // namespace

TEST_CASE("a single loud burst does not trim quiet but clear edge beats", "[beat_analyzer]") {
  const int without_burst = count_edge_beats(false);
  REQUIRE(without_burst >= 15);

  const int with_burst = count_edge_beats(true);
  REQUIRE(with_burst >= 15);
}

TEST_CASE("a noise prelude is still trimmed", "[beat_analyzer]") {
  constexpr float kPreludeSeconds = 20.0f;
  const size_t prelude = static_cast<size_t>(kPreludeSeconds * static_cast<float>(kTrimSr));
  const size_t body = static_cast<size_t>(kTrimBodySeconds * static_cast<float>(kTrimSr));
  std::vector<float> samples(prelude + body, 0.0f);
  std::mt19937 rng(777);
  add_white_noise(samples, 0, prelude, std::pow(10.0f, -40.0f / 20.0f), rng);
  add_click_train(samples, kPreludeSeconds, kTrimBodySeconds, 0.5f);
  add_white_noise(samples, prelude, body, std::pow(10.0f, -90.0f / 20.0f), rng);

  BeatConfig config;
  config.trim = true;
  BeatAnalyzer analyzer(Audio::from_vector(std::move(samples), kTrimSr), config);

  INFO("beats=" << analyzer.beats().size());
  REQUIRE(!analyzer.beats().empty());
  REQUIRE(count_beats_in(analyzer, 0.0f, 19.8f) == 0);
}

TEST_CASE("beats and tempo of the same content agree across input sample rates",
          "[analysis_rate][beat_analyzer]") {
  using test::RateMaterial;
  const BeatAnalyzer base(test::make_rate_material(RateMaterial::TriadTurnaround, 22050));
  const std::vector<float> base_times = base.beat_times();
  for (int sr : {32000, 44100, 48000}) {
    const BeatAnalyzer at_rate(test::make_rate_material(RateMaterial::TriadTurnaround, sr));
    const std::vector<float> times = at_rate.beat_times();
    float max_time_dev = 0.0f;
    for (size_t i = 0; i < std::min(times.size(), base_times.size()); ++i) {
      max_time_dev = std::max(max_time_dev, std::abs(times[i] - base_times[i]));
    }
    const float bpm_dev = std::abs(at_rate.bpm() - base.bpm()) / base.bpm();
    CAPTURE(sr, base_times.size(), times.size(), base.bpm(), at_rate.bpm(), max_time_dev, bpm_dev);
    CAPTURE(base_times, times);
    CHECK(times.size() == base_times.size());
    // One frame of tie-ordering drift in each grid's DP.
    const float time_tolerance =
        static_cast<float>(base.hop_length()) / static_cast<float>(constants::kDefaultSampleRate) +
        static_cast<float>(at_rate.hop_length()) / static_cast<float>(sr);
    CAPTURE(time_tolerance);
    CHECK(max_time_dev <= time_tolerance);
    CHECK(bpm_dev <= 0.02f);
  }
}
