/// @file section_analyzer_test.cpp
/// @brief Tests for section analyzer.

#include "analysis/section_analyzer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;

namespace {

/// @brief Creates audio with distinct sections (different energy levels).
Audio create_sectioned_audio(int sr = 22050) {
  // Create 20-second audio with:
  // 0-4s: Intro (low energy)
  // 4-8s: Verse (medium energy)
  // 8-12s: Chorus (high energy)
  // 12-16s: Verse (medium energy)
  // 16-20s: Outro (low energy)

  float duration = 20.0f;
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / sr;
    float freq = 440.0f;

    // Determine amplitude based on section
    float amplitude = 0.2f;  // Default low (intro/outro)
    if (t >= 4.0f && t < 8.0f) {
      amplitude = 0.5f;  // Verse
    } else if (t >= 8.0f && t < 12.0f) {
      amplitude = 0.9f;  // Chorus
    } else if (t >= 12.0f && t < 16.0f) {
      amplitude = 0.5f;  // Verse
    }

    samples[i] = amplitude * std::sin(2.0f * M_PI * freq * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates simple sine wave.
Audio create_sine(float freq, int sr = 22050, float duration = 10.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / sr;
    samples[i] = 0.5f * std::sin(2.0f * M_PI * freq * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("SectionAnalyzer basic", "[section_analyzer]") {
  Audio audio = create_sine(440.0f);

  SectionConfig config;
  SectionAnalyzer analyzer(audio, config);

  // Should have at least one section
  REQUIRE(analyzer.count() >= 1);
}

TEST_CASE("SectionAnalyzer sections", "[section_analyzer]") {
  Audio audio = create_sectioned_audio();

  SectionConfig config;
  config.min_section_sec = 2.0f;
  config.boundary_threshold = 0.2f;

  SectionAnalyzer analyzer(audio, config);

  const auto& sections = analyzer.sections();

  // Each section should have valid timing
  for (const auto& section : sections) {
    REQUIRE(section.start >= 0.0f);
    REQUIRE(section.end > section.start);
    REQUIRE(section.duration() > 0.0f);
    REQUIRE(section.energy_level >= 0.0f);
    REQUIRE(section.energy_level <= 1.0f);
    REQUIRE(section.confidence >= 0.0f);
    REQUIRE(section.confidence <= 1.0f);
  }

  // Sections should cover the audio
  if (!sections.empty()) {
    REQUIRE_THAT(sections.front().start, WithinAbs(0.0f, 0.1f));
  }
}

TEST_CASE("SectionAnalyzer form", "[section_analyzer]") {
  Audio audio = create_sectioned_audio();

  SectionConfig config;
  SectionAnalyzer analyzer(audio, config);

  std::string form = analyzer.form();

  // Form should be a string of section characters
  REQUIRE(!form.empty());

  // Each character should be valid
  for (char c : form) {
    bool is_valid =
        (c == 'I' || c == 'A' || c == 'P' || c == 'B' || c == 'C' || c == 'S' || c == 'O');
    REQUIRE(is_valid);
  }
}

TEST_CASE("SectionAnalyzer section_at", "[section_analyzer]") {
  Audio audio = create_sectioned_audio();

  SectionAnalyzer analyzer(audio);

  // Get section at middle of audio
  Section section = analyzer.section_at(10.0f);

  REQUIRE(section.start <= 10.0f);
  REQUIRE(section.end >= 10.0f);
}

TEST_CASE("SectionAnalyzer duration", "[section_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 10.0f);

  SectionAnalyzer analyzer(audio);

  // Duration should match audio duration
  REQUIRE_THAT(analyzer.duration(), WithinAbs(10.0f, 1.0f));
}

TEST_CASE("SectionAnalyzer boundary_times", "[section_analyzer]") {
  Audio audio = create_sectioned_audio();

  SectionConfig config;
  config.boundary_threshold = 0.2f;

  SectionAnalyzer analyzer(audio, config);

  auto boundaries = analyzer.boundary_times();

  // Boundaries should be sorted
  for (size_t i = 1; i < boundaries.size(); ++i) {
    REQUIRE(boundaries[i] > boundaries[i - 1]);
  }
}

TEST_CASE("SectionAnalyzer Section type_string", "[section_analyzer]") {
  Section section;
  section.start = 0.0f;
  section.end = 1.0f;
  section.energy_level = 0.5f;
  section.confidence = 0.8f;

  section.type = SectionType::Intro;
  REQUIRE(section.type_string() == "Intro");

  section.type = SectionType::Verse;
  REQUIRE(section.type_string() == "Verse");

  section.type = SectionType::Chorus;
  REQUIRE(section.type_string() == "Chorus");

  section.type = SectionType::Bridge;
  REQUIRE(section.type_string() == "Bridge");

  section.type = SectionType::Outro;
  REQUIRE(section.type_string() == "Outro");
}

TEST_CASE("section_type_to_char", "[section_analyzer]") {
  REQUIRE(section_type_to_char(SectionType::Intro) == 'I');
  REQUIRE(section_type_to_char(SectionType::Verse) == 'A');
  REQUIRE(section_type_to_char(SectionType::Chorus) == 'B');
  REQUIRE(section_type_to_char(SectionType::Bridge) == 'C');
  REQUIRE(section_type_to_char(SectionType::Outro) == 'O');
}

TEST_CASE("section_type_to_string", "[section_analyzer]") {
  REQUIRE(section_type_to_string(SectionType::Intro) == "Intro");
  REQUIRE(section_type_to_string(SectionType::Verse) == "Verse");
  REQUIRE(section_type_to_string(SectionType::PreChorus) == "Pre-Chorus");
  REQUIRE(section_type_to_string(SectionType::Chorus) == "Chorus");
  REQUIRE(section_type_to_string(SectionType::Bridge) == "Bridge");
  REQUIRE(section_type_to_string(SectionType::Instrumental) == "Instrumental");
  REQUIRE(section_type_to_string(SectionType::Outro) == "Outro");
}

TEST_CASE("SectionAnalyzer config options", "[section_analyzer]") {
  Audio audio = create_sectioned_audio();

  SectionConfig config;
  config.min_section_sec = 1.0f;
  config.boundary_threshold = 0.1f;

  SectionAnalyzer analyzer(audio, config);

  // Should produce valid sections
  REQUIRE(analyzer.count() >= 1);
}

TEST_CASE("SectionAnalyzer short audio", "[section_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 3.0f);

  SectionConfig config;
  config.min_section_sec = 1.0f;

  SectionAnalyzer analyzer(audio, config);

  // Should still work
  REQUIRE(analyzer.count() >= 1);
}

TEST_CASE("SectionAnalyzer section_at out of range", "[section_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 5.0f);

  SectionAnalyzer analyzer(audio);

  // Time beyond audio
  Section section = analyzer.section_at(100.0f);

  REQUIRE(section.duration() == 0.0f);
  REQUIRE(section.confidence == 0.0f);
}
