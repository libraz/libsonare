/// @file chord_synthetic_matrix_test.cpp
/// @brief Synthetic matrix tests for chord detection.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
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

TEST_CASE("ChordAnalyzer threshold emits continuous N.C. for frame beat and HMM paths",
          "[chord_analyzer][threshold]") {
  constexpr int kFrames = 4;
  constexpr int kSampleRate = 1000;
  constexpr int kHopLength = 1000;
  const std::vector<float> beat_times = {0.0f, 1.0f, 2.0f, 3.0f};

  for (const bool beat_sync : {false, true}) {
    for (const bool use_hmm : {false, true}) {
      CAPTURE(beat_sync, use_hmm);
      const Chroma ambiguous(std::vector<float>(12 * kFrames, 0.0f), 12, kFrames, kSampleRate,
                             kHopLength);

      ChordConfig low;
      low.min_duration = 0.0f;
      low.smoothing_window = 0.0f;
      low.threshold = 0.0f;
      low.use_triads_only = true;
      low.use_beat_sync = beat_sync;
      low.use_hmm = use_hmm;
      low.hmm_beam_width = 4;
      const ChordAnalyzer low_analyzer =
          beat_sync ? ChordAnalyzer(ambiguous, beat_times, low) : ChordAnalyzer(ambiguous, low);
      REQUIRE(low_analyzer.count() == 1);
      REQUIRE(low_analyzer.chords().front().quality != ChordQuality::Unknown);

      ChordConfig high = low;
      high.threshold = 0.5f;
      const ChordAnalyzer high_analyzer =
          beat_sync ? ChordAnalyzer(ambiguous, beat_times, high) : ChordAnalyzer(ambiguous, high);
      REQUIRE(high_analyzer.count() == 1);
      const Chord& no_chord = high_analyzer.chords().front();
      REQUIRE(no_chord.quality == ChordQuality::Unknown);
      REQUIRE(no_chord.to_string() == "N.C.");
      REQUIRE(no_chord.start == 0.0f);
      REQUIRE(no_chord.end > no_chord.start);
      REQUIRE(high_analyzer.progression_pattern() == "N.C.");
      REQUIRE(high_analyzer.functional_analysis(PitchClass::C) == std::vector<std::string>{"N.C."});
      if (!beat_sync) {
        REQUIRE(high_analyzer.frame_chords() == std::vector<int>(kFrames, -1));
      }
    }
  }
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
  REQUIRE(chord_templates.size() == 12 * (16 + extended_chord_qualities().size()));

  for (const auto& chord_template : chord_templates) {
    CAPTURE(chord_template.to_string());

    ChordAnalyzer analyzer(repeated_chroma(chord_template), config);
    REQUIRE(analyzer.count() == 1);

    const Chord detected = analyzer.chords().front();
    // Three of the extended qualities spell the same four pitch classes as a
    // commoner quality rooted elsewhere -- a maj6 and the m7 a minor third
    // below are the same notes. A chromagram cannot distinguish them, and this
    // case feeds nothing else, so what has to hold is that the detected chord
    // spells the generated one. Which of the two names comes back is settled by
    // the bass evidence, which the case below supplies.
    const bool anagram = chord_template.quality == ChordQuality::Major6 ||
                         chord_template.quality == ChordQuality::Minor6 ||
                         chord_template.quality == ChordQuality::Dominant7Sus4;
    if (anagram) {
      REQUIRE(chord_pitch_class_mask(static_cast<int>(detected.root),
                                     static_cast<int>(detected.quality)) ==
              chord_pitch_class_mask(static_cast<int>(chord_template.root),
                                     static_cast<int>(chord_template.quality)));
    } else {
      REQUIRE(detected.root == chord_template.root);
      REQUIRE(detected.quality == chord_template.quality);
    }
    REQUIRE(detected.confidence > 0.9f);
  }
}

TEST_CASE("ChordAnalyzer bass evidence separates a sixth chord from its seventh anagram",
          "[chord_analyzer][synthetic_matrix]") {
  // C6 and Am7 are the same four pitch classes. With no bass the analyzer must
  // fall back on the commoner reading; told that the low register sounds C, it
  // has to name the chord C6, and told that it sounds A, Am7. Nothing in the
  // harmonic chroma differs between the two runs -- only the bass does.
  ChordConfig config;
  config.min_duration = 0.0f;
  config.smoothing_window = 0.0f;
  config.threshold = 0.0f;
  config.use_triads_only = false;
  config.use_beat_sync = false;

  const ChordTemplate c6 = create_chord_template(PitchClass::C, ChordQuality::Major6);
  const Chroma harmonic = repeated_chroma(c6);

  auto detect_with_bass = [&](PitchClass bass_pitch) {
    std::vector<float> features(12 * harmonic.n_frames(), 0.02f);
    for (int frame = 0; frame < harmonic.n_frames(); ++frame) {
      features[static_cast<int>(bass_pitch) * harmonic.n_frames() + frame] = 1.0f;
    }
    Chroma bass(std::move(features), 12, harmonic.n_frames(), harmonic.sample_rate(),
                harmonic.hop_length());
    ChordAnalyzer analyzer(harmonic, /*beat_times=*/{}, bass, config);
    REQUIRE(analyzer.count() == 1);
    return analyzer.chords().front();
  };

  const Chord over_c = detect_with_bass(PitchClass::C);
  CAPTURE(over_c.to_string());
  REQUIRE(over_c.root == PitchClass::C);
  REQUIRE(over_c.quality == ChordQuality::Major6);

  const Chord over_a = detect_with_bass(PitchClass::A);
  CAPTURE(over_a.to_string());
  REQUIRE(over_a.root == PitchClass::A);
  REQUIRE(over_a.quality == ChordQuality::Minor7);
}

namespace {

/// @brief Index of one quality at one root in a template set.
int template_index(const std::vector<ChordTemplate>& templates, PitchClass root,
                   ChordQuality quality) {
  for (size_t i = 0; i < templates.size(); ++i) {
    if (templates[i].root == root && templates[i].quality == quality) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

}  // namespace

TEST_CASE("Chord HMM prefers the dominant that resolves over the one that does not",
          "[chord_analyzer][hmm]") {
  // Root motion alone cannot tell a cadence from a modal shuffle: V7 -> i and
  // v -> i are the same two scale degrees. Each case below offers the decoder
  // two chords on the fifth degree with *identical* emission scores, so the
  // only thing that can separate them is whether the transition model reads the
  // quality standing on that degree.
  const auto templates = generate_all_chord_templates();

  SECTION("minor key: the harmonic-minor V7 beats the natural-minor v") {
    const int e_dom7 = template_index(templates, PitchClass::E, ChordQuality::Dominant7);
    const int e_minor = template_index(templates, PitchClass::E, ChordQuality::Minor);
    const int a_minor = template_index(templates, PitchClass::A, ChordQuality::Minor);
    REQUIRE(e_dom7 >= 0);
    REQUIRE(e_minor >= 0);
    REQUIRE(a_minor >= 0);

    std::vector<ChordHmmObservation> observations(2);
    observations[0].candidates = {{e_minor, 0.9f}, {e_dom7, 0.9f}};
    observations[1].candidates = {{a_minor, 0.95f}};

    ChordHmmConfig config;
    config.beam_width = 4;
    config.use_key_context = true;
    config.key_root = PitchClass::A;
    config.key_mode = Mode::Minor;

    const auto sequence = viterbi_chord_sequence(observations, templates, config);
    REQUIRE(sequence.size() == 2);
    REQUIRE(sequence[0] == e_dom7);
    REQUIRE(sequence[1] == a_minor);
  }

  SECTION("major key: a minor chord on the fifth degree is not the dominant") {
    const int g_major = template_index(templates, PitchClass::G, ChordQuality::Major);
    const int g_minor = template_index(templates, PitchClass::G, ChordQuality::Minor);
    const int c_major = template_index(templates, PitchClass::C, ChordQuality::Major);
    REQUIRE(g_major >= 0);
    REQUIRE(g_minor >= 0);
    REQUIRE(c_major >= 0);

    std::vector<ChordHmmObservation> observations(2);
    observations[0].candidates = {{g_minor, 0.9f}, {g_major, 0.9f}};
    observations[1].candidates = {{c_major, 0.95f}};

    ChordHmmConfig config;
    config.beam_width = 4;
    config.use_key_context = true;
    config.key_root = PitchClass::C;
    config.key_mode = Mode::Major;

    const auto sequence = viterbi_chord_sequence(observations, templates, config);
    REQUIRE(sequence.size() == 2);
    REQUIRE(sequence[0] == g_major);
  }
}

TEST_CASE("Chord HMM does not penalise a cadence for its quality", "[chord_analyzer][hmm]") {
  // Grading a cadence by quality may withdraw the bonus a spelled cadence earns;
  // it must not turn the motion into a penalty. A minor v -> i is still a
  // transition progressions make, so it has to stay at least as attractive as an
  // ordinary related move -- here, the same v chord going somewhere unrelated.
  const auto templates = generate_all_chord_templates();
  const int e_minor = template_index(templates, PitchClass::E, ChordQuality::Minor);
  const int a_minor = template_index(templates, PitchClass::A, ChordQuality::Minor);
  const int b_flat_major = template_index(templates, PitchClass::As, ChordQuality::Major);
  REQUIRE(e_minor >= 0);
  REQUIRE(a_minor >= 0);
  REQUIRE(b_flat_major >= 0);

  std::vector<ChordHmmObservation> observations(2);
  observations[0].candidates = {{e_minor, 0.9f}};
  observations[1].candidates = {{b_flat_major, 0.9f}, {a_minor, 0.9f}};

  ChordHmmConfig config;
  config.beam_width = 4;
  config.use_key_context = true;
  config.key_root = PitchClass::A;
  config.key_mode = Mode::Minor;

  const auto sequence = viterbi_chord_sequence(observations, templates, config);
  REQUIRE(sequence.size() == 2);
  REQUIRE(sequence[1] == a_minor);
}

TEST_CASE("Bass evidence separates a chord from its relative",
          "[chord_analyzer][synthetic_matrix]") {
  // A major and the minor a third below share two of three tones, so a folded
  // chromagram carrying both -- which is what a real mix of either sounds like
  // once the sixth leaks in -- has almost nothing left to say which note is the
  // root. The low register does. Both runs below see the identical harmonic
  // chroma; only the bass differs.
  ChordConfig config;
  config.min_duration = 0.0f;
  config.smoothing_window = 0.0f;
  config.threshold = 0.0f;
  config.use_triads_only = true;
  config.use_beat_sync = false;

  constexpr int kFrames = 8;
  constexpr int kSampleRate = 8000;
  constexpr int kHopLength = 1000;

  // A(9), C#(1), E(4) with a leaked F#(6): reads as A major or F# minor.
  std::array<float, 12> mixture = {};
  mixture[9] = 1.0f;
  mixture[1] = 1.0f;
  mixture[4] = 0.9f;
  mixture[6] = 0.9f;

  std::vector<float> harmonic_features(12 * kFrames, 0.0f);
  for (int chroma = 0; chroma < 12; ++chroma) {
    for (int frame = 0; frame < kFrames; ++frame) {
      harmonic_features[chroma * kFrames + frame] = mixture[static_cast<size_t>(chroma)];
    }
  }
  const Chroma harmonic(std::move(harmonic_features), 12, kFrames, kSampleRate, kHopLength);

  auto detect_with_bass = [&](PitchClass bass_pitch) {
    std::vector<float> features(12 * kFrames, 0.02f);
    for (int frame = 0; frame < kFrames; ++frame) {
      features[static_cast<int>(bass_pitch) * kFrames + frame] = 1.0f;
    }
    Chroma bass(std::move(features), 12, kFrames, kSampleRate, kHopLength);
    ChordAnalyzer analyzer(harmonic, /*beat_times=*/{}, bass, config);
    REQUIRE(analyzer.count() == 1);
    return analyzer.chords().front();
  };

  const Chord over_a = detect_with_bass(PitchClass::A);
  CAPTURE(over_a.to_string());
  REQUIRE(over_a.root == PitchClass::A);
  REQUIRE(over_a.quality == ChordQuality::Major);

  const Chord over_f_sharp = detect_with_bass(PitchClass::Fs);
  CAPTURE(over_f_sharp.to_string());
  REQUIRE(over_f_sharp.root == PitchClass::Fs);
  REQUIRE(over_f_sharp.quality == ChordQuality::Minor);
}

TEST_CASE("The bass cue is inert when the low register only mirrors the chroma",
          "[chord_analyzer][synthetic_matrix]") {
  // On material with no bass part the low-register chromagram is a scaled copy
  // of the harmonic one. That is a measurement of nothing, and it must decide
  // nothing: a cue read from it would nominate whichever pitch class the
  // leakage happened to favour and drag chord boundaries with it.
  ChordConfig config;
  config.min_duration = 0.0f;
  config.smoothing_window = 0.0f;
  config.threshold = 0.0f;
  config.use_triads_only = true;
  config.use_beat_sync = false;

  const ChordTemplate a_major = create_chord_template(PitchClass::A, ChordQuality::Major);
  const Chroma harmonic = repeated_chroma(a_major);

  std::vector<float> mirrored(12 * harmonic.n_frames(), 0.0f);
  for (int chroma = 0; chroma < 12; ++chroma) {
    for (int frame = 0; frame < harmonic.n_frames(); ++frame) {
      mirrored[chroma * harmonic.n_frames() + frame] = 0.05f * harmonic.at(chroma, frame);
    }
  }
  Chroma leakage(std::move(mirrored), 12, harmonic.n_frames(), harmonic.sample_rate(),
                 harmonic.hop_length());

  const ChordAnalyzer with_leakage(harmonic, /*beat_times=*/{}, leakage, config);
  const ChordAnalyzer without_bass(harmonic, /*beat_times=*/{}, Chroma(), config);
  REQUIRE(with_leakage.count() == without_bass.count());
  REQUIRE(with_leakage.chords().front().root == without_bass.chords().front().root);
  REQUIRE(with_leakage.chords().front().quality == without_bass.chords().front().quality);
}
