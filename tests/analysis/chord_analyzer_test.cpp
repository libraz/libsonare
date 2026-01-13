/// @file chord_analyzer_test.cpp
/// @brief Tests for chord analyzer.

#include "analysis/chord_analyzer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;

namespace {

/// @brief Generates a chord audio signal.
Audio create_chord(const std::vector<float>& midi_notes, int sr = 22050, float duration = 1.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  for (float midi : midi_notes) {
    float freq = 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);

    for (int i = 0; i < n_samples; ++i) {
      float t = static_cast<float>(i) / static_cast<float>(sr);
      // Add harmonics for richer sound
      samples[i] += 0.5f * std::sin(2.0f * M_PI * freq * t);
      samples[i] += 0.25f * std::sin(2.0f * M_PI * freq * 2.0f * t);
      samples[i] += 0.125f * std::sin(2.0f * M_PI * freq * 3.0f * t);
    }
  }

  // Normalize
  float max_val = 0.0f;
  for (float s : samples) {
    max_val = std::max(max_val, std::abs(s));
  }
  if (max_val > 0.0f) {
    for (float& s : samples) {
      s /= max_val;
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates C major chord (C-E-G).
Audio create_c_major(int sr = 22050, float duration = 1.0f) {
  return create_chord({60.0f, 64.0f, 67.0f}, sr, duration);  // C4, E4, G4
}

/// @brief Creates A minor chord (A-C-E).
Audio create_a_minor(int sr = 22050, float duration = 1.0f) {
  return create_chord({57.0f, 60.0f, 64.0f}, sr, duration);  // A3, C4, E4
}

/// @brief Creates G major chord (G-B-D).
Audio create_g_major(int sr = 22050, float duration = 1.0f) {
  return create_chord({55.0f, 59.0f, 62.0f}, sr, duration);  // G3, B3, D4
}

/// @brief Creates F major chord (F-A-C).
Audio create_f_major(int sr = 22050, float duration = 1.0f) {
  return create_chord({53.0f, 57.0f, 60.0f}, sr, duration);  // F3, A3, C4
}

/// @brief Concatenates multiple audio signals.
Audio concat_audio(const std::vector<Audio>& audios) {
  if (audios.empty()) {
    return Audio::from_vector({}, 22050);
  }

  std::vector<float> samples;
  int sr = audios[0].sample_rate();

  for (const auto& audio : audios) {
    for (size_t i = 0; i < audio.size(); ++i) {
      samples.push_back(audio.data()[i]);
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("ChordAnalyzer basic", "[chord_analyzer]") {
  Audio audio = create_c_major();

  ChordConfig config;
  ChordAnalyzer analyzer(audio, config);

  REQUIRE(analyzer.count() >= 1);

  const auto& chords = analyzer.chords();
  REQUIRE(!chords.empty());
}

TEST_CASE("ChordAnalyzer C major detection", "[chord_analyzer]") {
  Audio audio = create_c_major(22050, 2.0f);

  ChordConfig config;
  config.use_triads_only = true;

  ChordAnalyzer analyzer(audio, config);

  // Should detect C major
  const auto& chords = analyzer.chords();
  REQUIRE(!chords.empty());

  // The most common chord should be C major
  Chord most_common = analyzer.most_common_chord();
  REQUIRE(most_common.root == PitchClass::C);
  REQUIRE(most_common.quality == ChordQuality::Major);
}

TEST_CASE("ChordAnalyzer A minor detection", "[chord_analyzer]") {
  Audio audio = create_a_minor(22050, 2.0f);

  ChordConfig config;
  config.use_triads_only = true;

  ChordAnalyzer analyzer(audio, config);

  Chord most_common = analyzer.most_common_chord();
  REQUIRE(most_common.root == PitchClass::A);
  REQUIRE(most_common.quality == ChordQuality::Minor);
}

TEST_CASE("ChordAnalyzer chord progression", "[chord_analyzer]") {
  // Create C - G - Am - F progression
  std::vector<Audio> parts = {create_c_major(22050, 1.0f), create_g_major(22050, 1.0f),
                              create_a_minor(22050, 1.0f), create_f_major(22050, 1.0f)};

  Audio audio = concat_audio(parts);

  ChordConfig config;
  config.use_triads_only = true;
  config.min_duration = 0.5f;

  ChordAnalyzer analyzer(audio, config);

  // Should detect multiple chords
  REQUIRE(analyzer.count() >= 2);

  // Check progression pattern
  std::string pattern = analyzer.progression_pattern();
  REQUIRE(!pattern.empty());
}

TEST_CASE("ChordAnalyzer functional analysis", "[chord_analyzer]") {
  Audio audio = create_c_major(22050, 2.0f);

  ChordConfig config;
  config.use_triads_only = true;

  ChordAnalyzer analyzer(audio, config);

  // Analyze in C major
  auto roman = analyzer.functional_analysis(PitchClass::C, Mode::Major);

  REQUIRE(!roman.empty());
  // C major in key of C should be "I"
  REQUIRE(roman[0] == "I");
}

TEST_CASE("ChordAnalyzer chord_at", "[chord_analyzer]") {
  Audio audio = create_c_major(22050, 2.0f);

  ChordAnalyzer analyzer(audio);

  // Get chord at middle of audio
  Chord chord = analyzer.chord_at(1.0f);

  REQUIRE(chord.duration() > 0.0f);
  REQUIRE(chord.confidence >= 0.0f);
  REQUIRE(chord.confidence <= 1.0f);
}

TEST_CASE("ChordAnalyzer from chroma", "[chord_analyzer]") {
  Audio audio = create_c_major(22050, 2.0f);

  ChromaConfig chroma_config;
  Chroma chroma = Chroma::compute(audio, chroma_config);

  ChordConfig config;
  ChordAnalyzer analyzer(chroma, config);

  REQUIRE(analyzer.count() >= 1);
}

TEST_CASE("ChordAnalyzer Chord::to_string", "[chord_analyzer]") {
  Chord chord;
  chord.root = PitchClass::C;
  chord.quality = ChordQuality::Major;
  chord.start = 0.0f;
  chord.end = 1.0f;
  chord.confidence = 0.9f;

  REQUIRE(chord.to_string() == "C");

  chord.quality = ChordQuality::Minor;
  REQUIRE(chord.to_string() == "Cm");

  chord.quality = ChordQuality::Dominant7;
  REQUIRE(chord.to_string() == "C7");

  chord.quality = ChordQuality::Major7;
  REQUIRE(chord.to_string() == "Cmaj7");

  chord.quality = ChordQuality::Minor7;
  REQUIRE(chord.to_string() == "Cm7");

  chord.quality = ChordQuality::Diminished;
  REQUIRE(chord.to_string() == "Cdim");

  chord.quality = ChordQuality::Augmented;
  REQUIRE(chord.to_string() == "Caug");
}

TEST_CASE("ChordAnalyzer frame chords", "[chord_analyzer]") {
  Audio audio = create_c_major(22050, 1.0f);

  ChordAnalyzer analyzer(audio);

  const auto& frame_chords = analyzer.frame_chords();

  REQUIRE(!frame_chords.empty());

  // All frame chord indices should be valid
  for (int idx : frame_chords) {
    REQUIRE(idx >= 0);
    REQUIRE(idx < static_cast<int>(analyzer.templates().size()));
  }
}

TEST_CASE("ChordAnalyzer templates", "[chord_analyzer]") {
  Audio audio = create_c_major();

  ChordConfig config;
  config.use_triads_only = false;
  ChordAnalyzer analyzer1(audio, config);

  // Full templates: 108 (12 roots × 9 qualities)
  REQUIRE(analyzer1.templates().size() == 108);

  config.use_triads_only = true;
  ChordAnalyzer analyzer2(audio, config);

  // Triads only: 48 (12 roots × 4 qualities)
  REQUIRE(analyzer2.templates().size() == 48);
}

TEST_CASE("detect_chords quick function", "[chord_analyzer]") {
  Audio audio = create_c_major(22050, 2.0f);

  auto chords = detect_chords(audio);

  REQUIRE(!chords.empty());
}

TEST_CASE("ChordAnalyzer chord timing", "[chord_analyzer]") {
  Audio audio = create_c_major(22050, 2.0f);

  ChordAnalyzer analyzer(audio);

  const auto& chords = analyzer.chords();

  for (size_t i = 0; i < chords.size(); ++i) {
    REQUIRE(chords[i].start >= 0.0f);
    REQUIRE(chords[i].end > chords[i].start);

    // Check sequential ordering
    if (i > 0) {
      REQUIRE_THAT(chords[i].start, WithinAbs(chords[i - 1].end, 0.01f));
    }
  }
}

TEST_CASE("ChordAnalyzer empty chord_at", "[chord_analyzer]") {
  Audio audio = create_c_major(22050, 1.0f);

  ChordAnalyzer analyzer(audio);

  // Time way beyond audio duration
  Chord chord = analyzer.chord_at(100.0f);

  REQUIRE(chord.duration() == 0.0f);
  REQUIRE(chord.confidence == 0.0f);
}
