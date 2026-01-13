/// @file key_analyzer_test.cpp
/// @brief Tests for key analyzer.

#include "analysis/key_analyzer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

using namespace sonare;
using Catch::Matchers::WithinAbs;

namespace {

/// @brief Creates a C major scale audio.
Audio create_c_major_scale(int sr = 22050, float duration = 2.0f) {
  // C major scale frequencies: C4, D4, E4, F4, G4, A4, B4, C5
  std::vector<float> freqs = {261.63f, 293.66f, 329.63f, 349.23f,
                              392.00f, 440.00f, 493.88f, 523.25f};

  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float note_duration = duration / static_cast<float>(freqs.size());
  int note_samples = static_cast<int>(note_duration * sr);

  for (size_t n = 0; n < freqs.size(); ++n) {
    int start = static_cast<int>(n) * note_samples;
    for (int i = 0; i < note_samples && start + i < n_samples; ++i) {
      float t = static_cast<float>(i) / static_cast<float>(sr);
      samples[start + i] = 0.5f * std::sin(2.0f * M_PI * freqs[n] * t);
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates an A minor scale audio.
Audio create_a_minor_scale(int sr = 22050, float duration = 2.0f) {
  // A natural minor scale frequencies: A3, B3, C4, D4, E4, F4, G4, A4
  std::vector<float> freqs = {220.00f, 246.94f, 261.63f, 293.66f,
                              329.63f, 349.23f, 392.00f, 440.00f};

  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples, 0.0f);

  float note_duration = duration / static_cast<float>(freqs.size());
  int note_samples = static_cast<int>(note_duration * sr);

  for (size_t n = 0; n < freqs.size(); ++n) {
    int start = static_cast<int>(n) * note_samples;
    for (int i = 0; i < note_samples && start + i < n_samples; ++i) {
      float t = static_cast<float>(i) / static_cast<float>(sr);
      samples[start + i] = 0.5f * std::sin(2.0f * M_PI * freqs[n] * t);
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("Key to_string", "[key_analyzer]") {
  Key c_major;
  c_major.root = PitchClass::C;
  c_major.mode = Mode::Major;
  c_major.confidence = 0.9f;

  REQUIRE(c_major.to_string() == "C major");

  Key a_minor;
  a_minor.root = PitchClass::A;
  a_minor.mode = Mode::Minor;
  a_minor.confidence = 0.8f;

  REQUIRE(a_minor.to_string() == "A minor");
}

TEST_CASE("Key to_short_string", "[key_analyzer]") {
  Key c_major;
  c_major.root = PitchClass::C;
  c_major.mode = Mode::Major;

  REQUIRE(c_major.to_short_string() == "C");

  Key fs_minor;
  fs_minor.root = PitchClass::Fs;
  fs_minor.mode = Mode::Minor;

  REQUIRE(fs_minor.to_short_string() == "F#m");
}

TEST_CASE("KeyAnalyzer from chroma", "[key_analyzer]") {
  // C major chroma: strong C, E, G
  std::array<float, 12> c_major_chroma = {1.0f, 0.1f, 0.3f, 0.1f, 0.8f, 0.3f,
                                          0.1f, 0.9f, 0.1f, 0.3f, 0.1f, 0.3f};

  KeyConfig config;
  KeyAnalyzer analyzer(c_major_chroma, config);

  Key key = analyzer.key();

  // Should detect C major or relative A minor
  bool is_c_major = (key.root == PitchClass::C && key.mode == Mode::Major);
  bool is_a_minor = (key.root == PitchClass::A && key.mode == Mode::Minor);

  REQUIRE((is_c_major || is_a_minor));
  REQUIRE(key.confidence > 0.0f);
}

TEST_CASE("KeyAnalyzer C major scale", "[key_analyzer]") {
  Audio audio = create_c_major_scale();

  KeyConfig config;
  config.n_fft = 4096;

  KeyAnalyzer analyzer(audio, config);

  Key key = analyzer.key();

  // Should detect C major or closely related key
  bool is_c_major = (key.root == PitchClass::C && key.mode == Mode::Major);
  bool is_g_major = (key.root == PitchClass::G && key.mode == Mode::Major);
  bool is_a_minor = (key.root == PitchClass::A && key.mode == Mode::Minor);

  REQUIRE((is_c_major || is_g_major || is_a_minor));
}

TEST_CASE("KeyAnalyzer A minor scale", "[key_analyzer]") {
  Audio audio = create_a_minor_scale();

  KeyConfig config;
  config.n_fft = 4096;

  KeyAnalyzer analyzer(audio, config);

  Key key = analyzer.key();

  // Should detect A minor or closely related key
  bool is_a_minor = (key.root == PitchClass::A && key.mode == Mode::Minor);
  bool is_c_major = (key.root == PitchClass::C && key.mode == Mode::Major);

  REQUIRE((is_a_minor || is_c_major));
}

TEST_CASE("KeyAnalyzer candidates", "[key_analyzer]") {
  Audio audio = create_c_major_scale();

  KeyAnalyzer analyzer(audio);

  auto candidates = analyzer.candidates(5);

  REQUIRE(candidates.size() == 5);

  // First candidate should have highest correlation
  for (size_t i = 1; i < candidates.size(); ++i) {
    REQUIRE(candidates[0].correlation >= candidates[i].correlation);
  }
}

TEST_CASE("KeyAnalyzer all_candidates", "[key_analyzer]") {
  Audio audio = create_c_major_scale();

  KeyAnalyzer analyzer(audio);

  const auto& all = analyzer.all_candidates();

  // Should have 24 candidates (12 major + 12 minor)
  REQUIRE(all.size() == 24);
}

TEST_CASE("KeyAnalyzer mean_chroma", "[key_analyzer]") {
  Audio audio = create_c_major_scale();

  KeyAnalyzer analyzer(audio);

  const auto& chroma = analyzer.mean_chroma();

  REQUIRE(chroma.size() == 12);

  // All values should be non-negative
  for (float val : chroma) {
    REQUIRE(val >= 0.0f);
  }
}

TEST_CASE("detect_key quick function", "[key_analyzer]") {
  Audio audio = create_c_major_scale();

  Key key = detect_key(audio);

  // Should return a valid key
  REQUIRE(static_cast<int>(key.root) >= 0);
  REQUIRE(static_cast<int>(key.root) < 12);
}
