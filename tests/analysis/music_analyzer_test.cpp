/// @file music_analyzer_test.cpp
/// @brief Tests for unified music analyzer.

#include "analysis/music_analyzer.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "analysis/beat_analyzer.h"
#include "support/section_form.h"
#include "util/constants.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// @brief Creates a simple test audio signal.
Audio create_test_audio(int sr = 22050, float duration = 5.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  // Create a rhythmic pattern at 120 BPM
  float beat_interval = 60.0f / 120.0f;  // 0.5 seconds per beat
  int click_length = sr / 100;

  for (float t = 0.0f; t < duration; t += beat_interval) {
    int start = static_cast<int>(t * sr);
    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * 0.8f;
    }
  }

  // Add some harmonic content
  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / sr;
    samples[i] += 0.3f * std::sin(2.0f * sonare::constants::kPiD * 440.0f * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a click track whose first beat of every bar is accented.
Audio create_accented_audio(float bpm, int beats_per_bar, int sr = 22050, float duration = 10.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float beat_interval = 60.0f / bpm;
  int click_length = sr / 100;
  int beat_count = 0;

  for (float t = 0.0f; t < duration; t += beat_interval) {
    int start = static_cast<int>(t * sr);
    float amplitude = (beat_count % beats_per_bar == 0) ? 1.0f : 0.5f;

    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * amplitude;
    }
    beat_count++;
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates a click track that accelerates linearly between two tempi.
Audio create_tempo_ramp(float start_bpm, float end_bpm, int sr = 22050, float duration = 8.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);
  int click_length = sr / 100;

  for (float t = 0.0f; t < duration;) {
    int start = static_cast<int>(t * sr);
    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * 0.9f;
    }
    const float bpm = start_bpm + (end_bpm - start_bpm) * (t / duration);
    t += 60.0f / bpm;
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Clicks on every beat with chord changes offset from bar starts.
Audio create_offset_chord_audio(float bpm, int beats_per_bar, int chord_offset_beats, int sr,
                                float duration) {
  const int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);
  const float beat_interval = 60.0f / bpm;
  const int click_length = sr / 100;

  for (float t = 0.0f; t < duration; t += beat_interval) {
    const int start = static_cast<int>(t * sr);
    for (int i = 0; i < click_length && start + i < n_samples; ++i) {
      const float envelope = 1.0f - static_cast<float>(i) / click_length;
      samples[start + i] = envelope * 0.7f;
    }
  }

  // Triads change every bar, but the change lands chord_offset_beats after the
  // accented bar start.
  static const float kTriads[4][3] = {{261.63f, 329.63f, 392.00f},
                                      {349.23f, 440.00f, 523.25f},
                                      {293.66f, 349.23f, 440.00f},
                                      {220.00f, 261.63f, 329.63f}};
  const float bar_seconds = beat_interval * static_cast<float>(beats_per_bar);
  const float offset_seconds = beat_interval * static_cast<float>(chord_offset_beats);
  for (int i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    const float shifted = t - offset_seconds;
    if (shifted < 0.0f) continue;
    const int bar = static_cast<int>(shifted / bar_seconds);
    const float* triad = kTriads[bar % 4];
    for (int voice = 0; voice < 3; ++voice) {
      samples[i] += 0.22f * std::sin(2.0f * sonare::constants::kPiD * triad[voice] * t);
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Mean frame distance between consecutive beats over a half-open range.
float mean_beat_interval(const std::vector<int>& frames, size_t begin, size_t end) {
  if (end <= begin + 1) return 0.0f;
  float total = 0.0f;
  for (size_t i = begin + 1; i < end; ++i) {
    total += static_cast<float>(frames[i] - frames[i - 1]);
  }
  return total / static_cast<float>(end - begin - 1);
}

}  // namespace

TEST_CASE("MusicAnalyzer basic", "[music_analyzer]") {
  Audio audio = create_test_audio();

  MusicAnalyzerConfig config;
  MusicAnalyzer analyzer(audio, config);

  // Should create analyzer without crashing
  REQUIRE(analyzer.audio().size() == audio.size());
}

TEST_CASE("MusicAnalyzer bpm", "[music_analyzer]") {
  Audio audio = create_test_audio();

  MusicAnalyzer analyzer(audio);

  float bpm = analyzer.bpm();

  REQUIRE(bpm >= 60.0f);
  REQUIRE(bpm <= 200.0f);
}

TEST_CASE("MusicAnalyzer key", "[music_analyzer]") {
  Audio audio = create_test_audio();

  MusicAnalyzer analyzer(audio);

  Key key = analyzer.key();

  REQUIRE(key.confidence >= 0.0f);
  REQUIRE(key.confidence <= 1.0f);
}

TEST_CASE("MusicAnalyzer beat_times", "[music_analyzer]") {
  Audio audio = create_test_audio();

  MusicAnalyzer analyzer(audio);

  auto times = analyzer.beat_times();

  // Should have beats
  REQUIRE(!times.empty());

  // Times should be sorted
  for (size_t i = 1; i < times.size(); ++i) {
    REQUIRE(times[i] > times[i - 1]);
  }
}

TEST_CASE("MusicAnalyzer analyze aligns beats with the direct beat detector", "[music_analyzer]") {
  Audio audio = create_test_audio();
  MusicAnalyzerConfig config;
  MusicAnalyzer analyzer(audio, config);
  const AnalysisResult analysis = analyzer.analyze();

  BeatConfig direct_config;
  direct_config.bpm_min = config.bpm_min;
  direct_config.bpm_max = config.bpm_max;
  direct_config.start_bpm = config.start_bpm;
  direct_config.n_fft = config.n_fft;
  direct_config.hop_length = config.hop_length;
  const std::vector<float> direct_beats = detect_beats(audio, direct_config);

  REQUIRE(analysis.beats.size() == direct_beats.size());
  for (size_t i = 0; i < direct_beats.size(); ++i) {
    REQUIRE(analysis.beats[i].time == Catch::Approx(direct_beats[i]).margin(1e-6));
  }
}

TEST_CASE("MusicAnalyzer chords", "[.][slow][music_analyzer]") {
  Audio audio = create_test_audio();

  MusicAnalyzer analyzer(audio);

  auto chords = analyzer.chords();

  // May have chords
  // Chords should have valid timing
  for (const auto& chord : chords) {
    REQUIRE(chord.start >= 0.0f);
    REQUIRE(chord.end > chord.start);
  }
}

TEST_CASE("MusicAnalyzer refines downbeats after chord analysis", "[.][slow][music_analyzer]") {
  // Pins the documented SEQUENTIAL contract (see the MusicAnalyzer class
  // warning). chord_analyzer()'s lazy initializer calls refine_downbeats() a
  // second time on the BeatAnalyzer that beat_analyzer() already published, so:
  //
  //   - used sequentially, this is supported and the reference stays valid;
  //   - used concurrently, it is a data race, because the revision reassigns
  //     the analyzer's vectors while another thread may be reading them.
  //
  // The pointer check below is not incidental: it IS the mechanism. If a future
  // change makes the second refine a no-op, this test fails and the class
  // warning has to be revisited rather than silently going stale.
  Audio audio = create_test_audio();

  MusicAnalyzer analyzer(audio);
  auto& beat = analyzer.beat_analyzer();
  const size_t initial_downbeats = beat.downbeat_indices().size();
  REQUIRE(initial_downbeats > 0);
  const int* initial_storage = beat.downbeat_indices().data();

  (void)analyzer.chord_analyzer().count();

  // Still a valid, populated result when the two are used in sequence.
  REQUIRE(!beat.downbeat_indices().empty());
  REQUIRE(beat.downbeat_indices().size() == initial_downbeats);
  // ... and the storage a concurrent reader would have been holding is gone:
  // the second refine move-assigned a freshly built vector over it.
  REQUIRE(beat.downbeat_indices().data() != initial_storage);
}

TEST_CASE("MusicAnalyzer form", "[music_analyzer]") {
  Audio audio = create_test_audio();

  MusicAnalyzer analyzer(audio);

  std::string form = analyzer.form();

  // Form should be a string of section characters
  REQUIRE(!form.empty());
}

TEST_CASE("MusicAnalyzer native-rate section path", "[music_analyzer]") {
  // A source above the 22050 Hz analysis rate is downsampled internally; section
  // analysis must run on that shared analysis-rate signal (same rate the
  // boundaries were detected on) and still yield a valid, non-empty form whose
  // characters are legal section labels.
  Audio audio = create_test_audio(44100, 8.0f);

  MusicAnalyzer analyzer(audio);

  REQUIRE(analyzer.boundary_detector().sample_rate() == 22050);

  std::string form = analyzer.form();
  REQUIRE(!form.empty());
  INFO("form " << form);
  REQUIRE(sonare::test::is_section_form(form));

  const auto& sections = analyzer.section_analyzer().sections();
  REQUIRE(!sections.empty());
  float prev_end = 0.0f;
  for (const auto& section : sections) {
    REQUIRE(section.end > section.start);
    REQUIRE(section.energy_level >= 0.0f);
    REQUIRE(section.energy_level <= 1.0f);
    // Sections stay ordered and within the native track duration despite the
    // internal resample to the analysis rate.
    REQUIRE(section.start >= prev_end - 0.1f);
    REQUIRE(section.end <= audio.duration() + 0.1f);
    prev_end = section.end;
  }
}

TEST_CASE("MusicAnalyzer analyzer access", "[.][slow][music_analyzer]") {
  Audio audio = create_test_audio();

  MusicAnalyzer analyzer(audio);

  // Access all analyzers (lazy initialization)
  REQUIRE(analyzer.bpm_analyzer().bpm() > 0.0f);
  REQUIRE(analyzer.key_analyzer().key().confidence >= 0.0f);
  (void)analyzer.beat_analyzer().count();
  (void)analyzer.chord_analyzer().count();
  (void)analyzer.onset_analyzer().count();
  REQUIRE(analyzer.dynamics_analyzer().dynamics().dynamic_range_db >= 0.0f);
  REQUIRE(!analyzer.rhythm_analyzer().groove_type().empty());
  REQUIRE(analyzer.timbre_analyzer().brightness() >= 0.0f);
  (void)analyzer.section_analyzer().count();
  REQUIRE(analyzer.boundary_detector().sample_rate() > 0);
}

TEST_CASE("MusicAnalyzer analyze", "[.][slow][music_analyzer]") {
  Audio audio = create_test_audio();

  MusicAnalyzer analyzer(audio);

  AnalysisResult result = analyzer.analyze();

  // Check result fields
  REQUIRE(result.bpm > 0.0f);
  REQUIRE(result.bpm_confidence >= 0.0f);
  REQUIRE(result.bpm_confidence <= 1.0f);
  REQUIRE_FALSE(result.bpm_candidates.empty());
  REQUIRE(result.bpm_candidates.front().relation == BpmCandidateRelation::Primary);
  REQUIRE(result.bpm_candidates.front().value == result.bpm);
  REQUIRE(result.key.confidence >= 0.0f);
  REQUIRE(result.time_signature.numerator > 0);
  REQUIRE_FALSE(result.time_signature_candidates.empty());
  REQUIRE(result.timbre.brightness >= 0.0f);
  REQUIRE(result.dynamics.dynamic_range_db >= 0.0f);
  REQUIRE(!result.form.empty());
}

TEST_CASE("MusicAnalyzer regular analysis skips the cancellation callback",
          "[.][slow][music_analyzer]") {
  MusicAnalyzer analyzer(create_test_audio());
  int cancel_queries = 0;
  analyzer.set_cancel_callback([&] {
    ++cancel_queries;
    return true;
  });

  const AnalysisResult result = analyzer.analyze();

  REQUIRE(result.bpm > 0.0f);
  REQUIRE(cancel_queries == 0);
}

TEST_CASE("MusicAnalyzer lazy initialization", "[music_analyzer]") {
  Audio audio = create_test_audio();

  MusicAnalyzer analyzer(audio);

  // First access
  float bpm1 = analyzer.bpm();

  // Second access (should return cached result)
  float bpm2 = analyzer.bpm();

  REQUIRE_THAT(bpm1, WithinAbs(bpm2, 0.01f));
}

TEST_CASE("MusicAnalyzer config", "[music_analyzer]") {
  Audio audio = create_test_audio();

  MusicAnalyzerConfig config;
  config.bpm_min = 80.0f;
  config.bpm_max = 160.0f;
  config.start_bpm = 120.0f;
  config.use_chord_hmm = true;
  config.use_chord_key_context = true;
  config.chord_hmm_beam_width = 8;
  config.detect_chord_inversions = true;

  MusicAnalyzer analyzer(audio, config);

  REQUIRE(analyzer.config().bpm_min == 80.0f);
  REQUIRE(analyzer.config().bpm_max == 160.0f);
  REQUIRE(analyzer.config().use_chord_hmm);
  REQUIRE(analyzer.config().use_chord_key_context);
  REQUIRE(analyzer.config().chord_hmm_beam_width == 8);
  REQUIRE(analyzer.config().detect_chord_inversions);
}

TEST_CASE("MusicAnalyzer can opt into chord HMM, key context, and inversion detection",
          "[.][slow][music_analyzer][chord_analyzer]") {
  Audio audio = create_test_audio();

  MusicAnalyzerConfig config;
  config.use_chord_hmm = true;
  config.use_chord_key_context = true;
  config.chord_hmm_beam_width = 8;
  config.detect_chord_inversions = true;

  MusicAnalyzer analyzer(audio, config);
  const auto chords = analyzer.chords();

  for (const auto& chord : chords) {
    REQUIRE(chord.confidence >= 0.0f);
    REQUIRE(chord.confidence <= 1.0f);
    REQUIRE(chord.end > chord.start);
  }
}

TEST_CASE("MusicAnalyzer melody analyzer", "[music_analyzer]") {
  Audio audio = create_test_audio();

  MusicAnalyzer analyzer(audio);

  auto& melody = analyzer.melody_analyzer();

  // Just verify it doesn't crash
  (void)melody.count();
}

TEST_CASE("AnalysisResult struct", "[music_analyzer]") {
  AnalysisResult result;

  result.bpm = 120.0f;
  result.bpm_confidence = 0.9f;
  result.bpm_candidates.push_back({120.0f, 0.9f, BpmCandidateRelation::Primary});
  result.key.root = PitchClass::C;
  result.key.mode = Mode::Major;
  result.key.confidence = 0.8f;
  result.time_signature.numerator = 4;
  result.time_signature.denominator = 4;
  result.time_signature_candidates.push_back({4, 4, 0.8f});
  result.form = "IABABCO";

  REQUIRE(result.bpm == 120.0f);
  REQUIRE(result.bpm_candidates.front().relation == BpmCandidateRelation::Primary);
  REQUIRE(result.key.root == PitchClass::C);
  REQUIRE(result.form == "IABABCO");
}

TEST_CASE("MusicAnalyzer progress callback", "[.][slow][music_analyzer]") {
  Audio audio = create_test_audio();

  MusicAnalyzer analyzer(audio);

  std::vector<float> progress_values;
  std::vector<std::string> stages;

  analyzer.set_progress_callback([&](float progress, const char* stage) {
    progress_values.push_back(progress);
    stages.push_back(stage);
  });

  AnalysisResult result = analyzer.analyze();

  // Should have received progress updates
  REQUIRE(!progress_values.empty());
  REQUIRE(!stages.empty());

  // Progress should be monotonically increasing
  for (size_t i = 1; i < progress_values.size(); ++i) {
    REQUIRE(progress_values[i] >= progress_values[i - 1]);
  }

  // Should start at 0 and end at 1
  REQUIRE(progress_values.front() == 0.0f);
  REQUIRE(progress_values.back() == 1.0f);

  // Should have expected stages
  REQUIRE(stages.front() == "features");
  REQUIRE(stages.back() == "complete");

  // Result should still be valid
  REQUIRE(result.bpm > 0.0f);
}

TEST_CASE("MusicAnalyzer analyze deterministic", "[.][slow][music_analyzer]") {
  Audio audio = create_test_audio(22050, 2.0f);

  MusicAnalyzer analyzer1(audio);
  AnalysisResult r1 = analyzer1.analyze();

  MusicAnalyzer analyzer2(audio);
  AnalysisResult r2 = analyzer2.analyze();

  REQUIRE(r1.bpm == r2.bpm);
  REQUIRE(r1.key.root == r2.key.root);
  REQUIRE(r1.key.mode == r2.key.mode);
  REQUIRE(r1.key.confidence == r2.key.confidence);
  REQUIRE(r1.beats.size() == r2.beats.size());
  REQUIRE(r1.chords.size() == r2.chords.size());
  REQUIRE(r1.timbre.brightness == r2.timbre.brightness);
  REQUIRE(r1.dynamics.dynamic_range_db == r2.dynamics.dynamic_range_db);
  REQUIRE(r1.form == r2.form);
}

TEST_CASE("MusicAnalyzer analyze multiple instances", "[.][slow][music_analyzer]") {
  Audio audio = create_test_audio(22050, 2.0f);

  MusicAnalyzer a(audio);
  MusicAnalyzer b(audio);

  AnalysisResult ra = a.analyze();
  AnalysisResult rb = b.analyze();

  REQUIRE(ra.bpm == rb.bpm);
  REQUIRE(ra.key.root == rb.key.root);
  REQUIRE(ra.key.mode == rb.key.mode);
  REQUIRE(ra.beats.size() == rb.beats.size());
  REQUIRE(ra.chords.size() == rb.chords.size());
  REQUIRE(ra.timbre.brightness == rb.timbre.brightness);
  REQUIRE(ra.dynamics.dynamic_range_db == rb.dynamics.dynamic_range_db);
  REQUIRE(ra.form == rb.form);
}

TEST_CASE("MusicAnalyzer progress callback thread safety", "[.][slow][music_analyzer]") {
  Audio audio = create_test_audio();

  MusicAnalyzer analyzer(audio);

  std::atomic<int> counter{0};
  std::vector<float> recorded_progress;

  analyzer.set_progress_callback([&](float progress, const char* /*stage*/) {
    counter.fetch_add(1, std::memory_order_relaxed);
    recorded_progress.push_back(progress);
  });

  analyzer.analyze();

  REQUIRE(counter.load() > 0);

  for (float p : recorded_progress) {
    REQUIRE(p >= 0.0f);
    REQUIRE(p <= 1.0f);
  }
}

TEST_CASE("MusicAnalyzer precompute then lazy access", "[.][slow][music_analyzer]") {
  Audio audio = create_test_audio();

  MusicAnalyzer analyzer(audio);

  // analyze() calls precompute_features() internally
  AnalysisResult result = analyzer.analyze();
  REQUIRE(result.bpm > 0.0f);

  // Lazy accessors should return valid results from cached features
  REQUIRE(analyzer.bpm_analyzer().bpm() > 0.0f);
  REQUIRE(analyzer.key_analyzer().key().confidence >= 0.0f);
  REQUIRE(analyzer.timbre_analyzer().brightness() >= 0.0f);
  REQUIRE(analyzer.dynamics_analyzer().dynamics().dynamic_range_db >= 0.0f);
  // count() returns size_t; just verify the calls succeed without throwing
  (void)analyzer.beat_analyzer().count();
  (void)analyzer.chord_analyzer().count();
  REQUIRE(!analyzer.rhythm_analyzer().groove_type().empty());
  (void)analyzer.section_analyzer().count();
  (void)analyzer.melody_analyzer().count();
}

TEST_CASE("MusicAnalyzer reports downbeats as ascending in-range beat indices",
          "[.][slow][music_analyzer]") {
  Audio audio = create_accented_audio(120.0f, 4, 22050, 6.0f);

  MusicAnalyzerConfig config;
  config.start_bpm = 120.0f;
  MusicAnalyzer analyzer(audio, config);
  const AnalysisResult result = analyzer.analyze();

  REQUIRE_FALSE(result.beats.empty());
  REQUIRE_FALSE(result.downbeat_indices.empty());
  // One entry per measure start, not one per beat.
  REQUIRE(result.downbeat_indices.size() <= result.beats.size());

  int previous = -1;
  for (size_t i = 0; i < result.downbeat_indices.size(); ++i) {
    const int index = result.downbeat_indices[i];
    CAPTURE(i, index, result.beats.size());
    REQUIRE(index >= 0);
    REQUIRE(index < static_cast<int>(result.beats.size()));
    REQUIRE(index > previous);
    previous = index;
  }

  CAPTURE(result.downbeat_phase, result.time_signature.numerator);
  REQUIRE(result.time_signature.numerator > 0);
  REQUIRE(result.downbeat_phase >= 0);
  REQUIRE(result.downbeat_phase < result.time_signature.numerator);
}

TEST_CASE("MusicAnalyzer publishes the chord-refined downbeats", "[.][slow][music_analyzer]") {
  // Clicks are uniform and the chord changes land one beat after the bar start,
  // so the beat analyzer's own downbeat estimate and the chord-informed one
  // disagree. Downbeat refinement runs a second time when the chord analyzer is
  // first used, and the result has to carry that second pass.
  Audio audio = create_offset_chord_audio(120.0f, 4, 1, 22050, 8.0f);

  MusicAnalyzerConfig config;
  config.start_bpm = 120.0f;

  MusicAnalyzer staged(audio, config);
  const std::vector<int> preliminary = staged.beat_analyzer().downbeat_indices();
  (void)staged.chord_analyzer().count();
  const std::vector<int> refined = staged.beat_analyzer().downbeat_indices();

  MusicAnalyzer full(audio, config);
  const AnalysisResult result = full.analyze();

  CAPTURE(preliminary, refined, result.downbeat_indices);
  REQUIRE_FALSE(refined.empty());
  // Without this the comparison below would hold for either read order.
  INFO("the fixture must keep the preliminary and chord-refined estimates apart");
  REQUIRE(preliminary != refined);
  REQUIRE(result.downbeat_indices == refined);
  REQUIRE(result.downbeat_phase == full.beat_analyzer().downbeat_phase());
}

TEST_CASE("MusicAnalyzer meter candidates change the detected numerator",
          "[.][slow][music_analyzer]") {
  Audio audio = create_accented_audio(120.0f, 5, 22050, 10.0f);

  MusicAnalyzerConfig odd;
  odd.start_bpm = 120.0f;
  odd.meter_candidate_numerators = {5, 7};
  MusicAnalyzer odd_analyzer(audio, odd);
  const AnalysisResult odd_result = odd_analyzer.analyze();

  MusicAnalyzerConfig default_config;
  default_config.start_bpm = 120.0f;
  MusicAnalyzer default_analyzer(audio, default_config);
  const AnalysisResult default_result = default_analyzer.analyze();

  CAPTURE(odd_result.time_signature.numerator, odd_result.rhythm.time_signature.numerator,
          default_result.time_signature.numerator);

  // Both meter estimates in the result — the beat analyzer's and the rhythm
  // analyzer's own — have to come from the requested candidate set.
  REQUIRE((odd_result.time_signature.numerator == 5 || odd_result.time_signature.numerator == 7));
  REQUIRE((odd_result.rhythm.time_signature.numerator == 5 ||
           odd_result.rhythm.time_signature.numerator == 7));

  // The defaults cannot reach those numerators, so the pattern alone does not
  // explain the result above.
  REQUIRE(default_result.time_signature.numerator != 5);
  REQUIRE(default_result.time_signature.numerator != 7);
  REQUIRE(default_result.rhythm.time_signature.numerator != 5);
  REQUIRE(default_result.rhythm.time_signature.numerator != 7);
}

TEST_CASE("MusicAnalyzer reports the requested beat unit on both meter paths",
          "[.][slow][music_analyzer]") {
  Audio audio = create_accented_audio(120.0f, 4, 22050, 6.0f);

  MusicAnalyzerConfig eighth;
  eighth.start_bpm = 120.0f;
  eighth.meter_denominator = 8;
  MusicAnalyzer eighth_analyzer(audio, eighth);
  const AnalysisResult eighth_result = eighth_analyzer.analyze();

  MusicAnalyzerConfig default_config;
  default_config.start_bpm = 120.0f;
  MusicAnalyzer default_analyzer(audio, default_config);
  const AnalysisResult default_result = default_analyzer.analyze();

  CAPTURE(eighth_result.time_signature.numerator, default_result.time_signature.numerator);
  REQUIRE(eighth_result.time_signature.denominator == 8);
  REQUIRE(eighth_result.rhythm.time_signature.denominator == 8);
  REQUIRE(default_result.time_signature.denominator == 4);
  REQUIRE(default_result.rhythm.time_signature.denominator == 4);
}

TEST_CASE("MusicAnalyzer adaptive tempo changes the tracked beats", "[.][slow][music_analyzer]") {
  // A click track that accelerates from 90 to 150 BPM: a fixed tempo prior
  // holds one spacing across the whole take, while the adaptive prior has to
  // contract with the ramp.
  Audio audio = create_tempo_ramp(90.0f, 150.0f, 22050, 8.0f);

  MusicAnalyzerConfig fixed;
  fixed.start_bpm = 100.0f;
  MusicAnalyzer fixed_analyzer(audio, fixed);
  const std::vector<int> fixed_frames = fixed_analyzer.beat_analyzer().beat_frames();

  MusicAnalyzerConfig adaptive = fixed;
  adaptive.adaptive_tempo = true;
  adaptive.tempo_update_interval_beats = 4;
  MusicAnalyzer adaptive_analyzer(audio, adaptive);
  const std::vector<int> adaptive_frames = adaptive_analyzer.beat_analyzer().beat_frames();

  REQUIRE(fixed_frames.size() >= 6);
  REQUIRE(adaptive_frames.size() >= 6);
  // A config field that never reached BeatConfig would leave these identical.
  REQUIRE(adaptive_frames != fixed_frames);

  const auto contraction = [](const std::vector<int>& frames) {
    const size_t midpoint = frames.size() / 2;
    const float early = mean_beat_interval(frames, 0, midpoint);
    const float late = mean_beat_interval(frames, midpoint, frames.size());
    return early > 0.0f ? late / early : 0.0f;
  };
  const float adaptive_contraction = contraction(adaptive_frames);
  const float fixed_contraction = contraction(fixed_frames);

  CAPTURE(adaptive_contraction, fixed_contraction);
  REQUIRE(adaptive_contraction > 0.0f);
  // The adaptive prior follows the accelerando; the fixed prior barely moves.
  REQUIRE(adaptive_contraction < 0.9f);
  REQUIRE(adaptive_contraction < fixed_contraction);
}

TEST_CASE("MusicAnalyzer tempo update interval changes the tracked beats",
          "[.][slow][music_analyzer]") {
  // The interval sets how much history the local tempo estimate sees, so two
  // very different context lengths cannot produce the same beat grid on a ramp.
  Audio audio = create_tempo_ramp(90.0f, 150.0f, 22050, 8.0f);

  MusicAnalyzerConfig shorter;
  shorter.start_bpm = 100.0f;
  shorter.adaptive_tempo = true;
  shorter.tempo_update_interval_beats = 4;
  MusicAnalyzer shorter_analyzer(audio, shorter);

  MusicAnalyzerConfig longer = shorter;
  longer.tempo_update_interval_beats = 32;
  MusicAnalyzer longer_analyzer(audio, longer);

  const std::vector<int> shorter_frames = shorter_analyzer.beat_analyzer().beat_frames();
  const std::vector<int> longer_frames = longer_analyzer.beat_analyzer().beat_frames();

  REQUIRE(shorter_frames.size() >= 6);
  REQUIRE(longer_frames != shorter_frames);
}

TEST_CASE("MusicAnalyzer publishes the beat observations behind the downbeat decision",
          "[.][slow][music_analyzer]") {
  Audio audio = create_offset_chord_audio(120.0f, 4, 1, 22050, 8.0f);

  MusicAnalyzerConfig config;
  config.start_bpm = 120.0f;

  MusicAnalyzer analyzer(audio, config);
  const AnalysisResult result = analyzer.analyze();

  const size_t beat_count = result.beats.size();
  REQUIRE(beat_count >= 8);
  REQUIRE(result.beat_observations.onset_strength.size() == beat_count);
  REQUIRE(result.beat_observations.low_frequency_energy.size() == beat_count);

  // Chord-change evidence exists only once the chord analyzer has run, and the
  // result reads the observations after it. Reading them next to the beats
  // would publish the preliminary pass's empty vector instead.
  REQUIRE_FALSE(result.chords.empty());
  const auto& chord_change = result.beat_observations.chord_change;
  REQUIRE(chord_change.size() == beat_count);
  REQUIRE(std::any_of(chord_change.begin(), chord_change.end(),
                      [](float value) { return value > 0.0f; }));

  // Without this the assertions above would hold for either read order: the
  // stream is empty right up until the chord analyzer's lazy initialization
  // re-refines the downbeats, so it is the ordering that fills it.
  MusicAnalyzer staged(audio, config);
  (void)staged.beat_analyzer().beats();
  REQUIRE(staged.beat_analyzer().beat_chord_change_observations().empty());
  (void)staged.chord_analyzer().count();
  REQUIRE(staged.beat_analyzer().beat_chord_change_observations().size() == beat_count);

  // What the result carries is what the analyzer holds after that refinement.
  REQUIRE(result.beat_observations.onset_strength ==
          analyzer.beat_analyzer().beat_onset_observations());
  REQUIRE(result.beat_observations.low_frequency_energy ==
          analyzer.beat_analyzer().beat_low_frequency_observations());
  REQUIRE(chord_change == analyzer.beat_analyzer().beat_chord_change_observations());

  // Same distinction the accessors exist for: the windowed observation is not
  // the single unwindowed envelope frame Beat::strength samples.
  bool differs = false;
  for (size_t i = 0; i < beat_count && !differs; ++i) {
    differs = result.beat_observations.onset_strength[i] != result.beats[i].strength;
  }
  REQUIRE(differs);

  for (size_t i = 0; i < beat_count; ++i) {
    CAPTURE(i);
    REQUIRE(std::isfinite(result.beat_observations.onset_strength[i]));
    REQUIRE(std::isfinite(result.beat_observations.low_frequency_energy[i]));
    REQUIRE(std::isfinite(chord_change[i]));
  }
}
