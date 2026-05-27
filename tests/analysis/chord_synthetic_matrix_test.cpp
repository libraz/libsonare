/// @file chord_synthetic_matrix_test.cpp
/// @brief Synthetic matrix tests for chord detection.

#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "analysis/chord_analyzer.h"
#include "analysis/chord_hmm.h"
#include "analysis/chord_templates.h"

using namespace sonare;

namespace {

Chroma repeated_chroma(const ChordTemplate& chord_template) {
  constexpr int kFrames = 8;
  constexpr int kSampleRate = 8000;
  constexpr int kHopLength = 1000;

  std::vector<float> features(12 * kFrames, 0.0f);
  const int root = static_cast<int>(chord_template.root);
  for (int chroma = 0; chroma < 12; ++chroma) {
    for (int frame = 0; frame < kFrames; ++frame) {
      features[chroma * kFrames + frame] = chord_template.pattern[chroma];
      if (chroma == root) {
        features[chroma * kFrames + frame] *= 1.2f;
      }
    }
  }

  return Chroma(std::move(features), 12, kFrames, kSampleRate, kHopLength);
}

TEST_CASE("Chord HMM smooths isolated observation outliers", "[chord_analyzer][hmm]") {
  const auto chord_templates = generate_triad_templates();
  auto find_template = [&](PitchClass root, ChordQuality quality) {
    for (size_t i = 0; i < chord_templates.size(); ++i) {
      if (chord_templates[i].root == root && chord_templates[i].quality == quality) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };

  const int c_major = find_template(PitchClass::C, ChordQuality::Major);
  const int g_major = find_template(PitchClass::G, ChordQuality::Major);
  REQUIRE(c_major >= 0);
  REQUIRE(g_major >= 0);

  std::vector<ChordHmmObservation> observations(5);
  for (size_t i = 0; i < observations.size(); ++i) {
    observations[i].candidates = {{c_major, 0.95f}, {g_major, 0.60f}};
  }
  observations[2].candidates = {{g_major, 0.95f}, {c_major, 0.60f}};

  ChordHmmConfig config;
  config.beam_width = 2;
  const auto sequence = viterbi_chord_sequence(observations, chord_templates, config);

  REQUIRE(sequence.size() == observations.size());
  for (int chord_index : sequence) {
    REQUIRE(chord_index == c_major);
  }
}

TEST_CASE("Chord HMM key context favors cadential motion", "[chord_analyzer][hmm]") {
  const auto chord_templates = generate_triad_templates();
  auto find_template = [&](PitchClass root, ChordQuality quality) {
    for (size_t i = 0; i < chord_templates.size(); ++i) {
      if (chord_templates[i].root == root && chord_templates[i].quality == quality) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };

  const int g_major = find_template(PitchClass::G, ChordQuality::Major);
  const int c_major = find_template(PitchClass::C, ChordQuality::Major);
  const int cs_major = find_template(PitchClass::Cs, ChordQuality::Major);
  const int fs_major = find_template(PitchClass::Fs, ChordQuality::Major);
  REQUIRE(g_major >= 0);
  REQUIRE(c_major >= 0);
  REQUIRE(cs_major >= 0);
  REQUIRE(fs_major >= 0);

  std::vector<ChordHmmObservation> observations(2);
  observations[0].candidates = {{cs_major, 0.82f}, {g_major, 0.80f}};
  observations[1].candidates = {{fs_major, 0.82f}, {c_major, 0.80f}};

  ChordHmmConfig config;
  config.beam_width = 2;
  config.use_key_context = true;
  config.key_root = PitchClass::C;
  config.key_mode = Mode::Major;
  const auto sequence = viterbi_chord_sequence(observations, chord_templates, config);

  REQUIRE(sequence.size() == observations.size());
  REQUIRE(sequence[0] == g_major);
  REQUIRE(sequence[1] == c_major);
}

TEST_CASE("ChordAnalyzer HMM smoothing is opt-in", "[chord_analyzer][hmm]") {
  const auto c_template = create_major_template(PitchClass::C);
  const auto g_template = create_major_template(PitchClass::G);
  constexpr int kFrames = 5;
  std::vector<float> features(12 * kFrames, 0.0f);
  for (int frame = 0; frame < kFrames; ++frame) {
    for (int chroma = 0; chroma < 12; ++chroma) {
      features[chroma * kFrames + frame] =
          frame == 2 ? 0.35f * c_template.pattern[chroma] + 0.65f * g_template.pattern[chroma]
                     : c_template.pattern[chroma];
    }
  }

  Chroma chroma(std::move(features), 12, kFrames, 8000, 1000);

  ChordConfig config;
  config.min_duration = 0.0f;
  config.smoothing_window = 0.0f;
  config.threshold = 0.0f;
  config.use_triads_only = true;
  config.use_beat_sync = false;
  config.use_hmm = true;
  config.hmm_beam_width = 4;

  ChordAnalyzer analyzer(chroma, config);
  REQUIRE(analyzer.count() >= 1);
  REQUIRE(analyzer.frame_chords().size() == kFrames);
}

TEST_CASE("ChordAnalyzer folds a short leading segment into the next segment without gaps",
          "[chord_analyzer][synthetic_matrix]") {
  constexpr int kFrames = 5;
  constexpr int kSampleRate = 1000;
  constexpr int kHopLength = 1000;
  const auto c_major = create_major_template(PitchClass::C);
  const auto g_major = create_major_template(PitchClass::G);

  std::vector<float> features(12 * kFrames, 0.0f);
  for (int frame = 0; frame < kFrames; ++frame) {
    const auto& chord_template = frame == 0 ? g_major : c_major;
    for (int chroma = 0; chroma < 12; ++chroma) {
      features[chroma * kFrames + frame] = chord_template.pattern[chroma];
    }
  }

  ChordConfig config;
  config.min_duration = 1.5f;
  config.smoothing_window = 0.0f;
  config.threshold = 0.0f;
  config.use_triads_only = true;
  config.use_beat_sync = false;

  ChordAnalyzer analyzer(Chroma(std::move(features), 12, kFrames, kSampleRate, kHopLength), config);

  REQUIRE(analyzer.count() == 1);
  const Chord chord = analyzer.chords().front();
  REQUIRE(chord.root == PitchClass::C);
  REQUIRE(chord.quality == ChordQuality::Major);
  REQUIRE(chord.start == 0.0f);
  REQUIRE(chord.end == 5.0f);
}

TEST_CASE("ChordAnalyzer estimates slash chord bass when inversion detection is enabled",
          "[chord_analyzer][synthetic_matrix]") {
  constexpr int kFrames = 8;
  std::vector<float> features(12 * kFrames, 0.0f);
  const auto c_major = create_major_template(PitchClass::C);
  for (int chroma = 0; chroma < 12; ++chroma) {
    for (int frame = 0; frame < kFrames; ++frame) {
      features[chroma * kFrames + frame] = c_major.pattern[chroma];
    }
  }
  for (int frame = 0; frame < kFrames; ++frame) {
    features[static_cast<int>(PitchClass::E) * kFrames + frame] *= 2.0f;
  }

  ChordConfig config;
  config.min_duration = 0.0f;
  config.smoothing_window = 0.0f;
  config.threshold = 0.0f;
  config.use_triads_only = true;
  config.use_beat_sync = false;
  config.detect_inversions = true;

  ChordAnalyzer analyzer(Chroma(std::move(features), 12, kFrames, 8000, 1000), config);
  REQUIRE(analyzer.count() == 1);

  const Chord chord = analyzer.chords().front();
  REQUIRE(chord.root == PitchClass::C);
  REQUIRE(chord.quality == ChordQuality::Major);
  REQUIRE(chord.bass == PitchClass::E);
  REQUIRE(chord.to_string() == "C/E");
}

}  // namespace

TEST_CASE("ChordAnalyzer synthetic chroma matrix detects every generated template",
          "[chord_analyzer][synthetic_matrix]") {
  ChordConfig config;
  config.min_duration = 0.0f;
  config.smoothing_window = 0.0f;
  config.threshold = 0.0f;
  config.use_triads_only = false;
  config.use_beat_sync = false;

  const auto chord_templates = generate_all_chord_templates();
  REQUIRE(chord_templates.size() == 192);

  for (const auto& chord_template : chord_templates) {
    CAPTURE(chord_template.to_string());

    ChordAnalyzer analyzer(repeated_chroma(chord_template), config);
    REQUIRE(analyzer.count() == 1);

    const Chord detected = analyzer.chords().front();
    REQUIRE(detected.root == chord_template.root);
    REQUIRE(detected.quality == chord_template.quality);
    REQUIRE(detected.confidence > 0.9f);
  }
}
