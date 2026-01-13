/// @file music_analyzer_test.cpp
/// @brief Tests for unified music analyzer.

#include "analysis/music_analyzer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

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
    samples[i] += 0.3f * std::sin(2.0f * M_PI * 440.0f * t);
  }

  return Audio::from_vector(std::move(samples), sr);
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

TEST_CASE("MusicAnalyzer chords", "[music_analyzer]") {
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

TEST_CASE("MusicAnalyzer form", "[music_analyzer]") {
  Audio audio = create_test_audio();

  MusicAnalyzer analyzer(audio);

  std::string form = analyzer.form();

  // Form should be a string of section characters
  REQUIRE(!form.empty());
}

TEST_CASE("MusicAnalyzer analyzer access", "[music_analyzer]") {
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

TEST_CASE("MusicAnalyzer analyze", "[music_analyzer]") {
  Audio audio = create_test_audio();

  MusicAnalyzer analyzer(audio);

  AnalysisResult result = analyzer.analyze();

  // Check result fields
  REQUIRE(result.bpm > 0.0f);
  REQUIRE(result.bpm_confidence >= 0.0f);
  REQUIRE(result.bpm_confidence <= 1.0f);
  REQUIRE(result.key.confidence >= 0.0f);
  REQUIRE(result.time_signature.numerator > 0);
  REQUIRE(result.timbre.brightness >= 0.0f);
  REQUIRE(result.dynamics.dynamic_range_db >= 0.0f);
  REQUIRE(!result.form.empty());
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

  MusicAnalyzer analyzer(audio, config);

  REQUIRE(analyzer.config().bpm_min == 80.0f);
  REQUIRE(analyzer.config().bpm_max == 160.0f);
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
  result.key.root = PitchClass::C;
  result.key.mode = Mode::Major;
  result.key.confidence = 0.8f;
  result.time_signature.numerator = 4;
  result.time_signature.denominator = 4;
  result.form = "IABABCO";

  REQUIRE(result.bpm == 120.0f);
  REQUIRE(result.key.root == PitchClass::C);
  REQUIRE(result.form == "IABABCO");
}

TEST_CASE("MusicAnalyzer progress callback", "[music_analyzer]") {
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
  REQUIRE(stages.front() == "bpm");
  REQUIRE(stages.back() == "complete");

  // Result should still be valid
  REQUIRE(result.bpm > 0.0f);
}
