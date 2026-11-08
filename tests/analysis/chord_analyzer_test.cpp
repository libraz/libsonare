/// @file chord_analyzer_test.cpp
/// @brief Tests for chord analyzer.

#include "analysis/chord_analyzer.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

#include "analysis/beat_analyzer.h"
#include "core/resample.h"
#include "support/rate_material.h"
#include "util/constants.h"

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
      samples[i] += 0.5f * std::sin(2.0f * sonare::constants::kPiD * freq * t);
      samples[i] += 0.25f * std::sin(2.0f * sonare::constants::kPiD * freq * 2.0f * t);
      samples[i] += 0.125f * std::sin(2.0f * sonare::constants::kPiD * freq * 3.0f * t);
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

/// @brief Creates C major over E bass.
Audio create_c_major_over_e(int sr = 22050, float duration = 1.0f) {
  return create_chord({40.0f, 60.0f, 64.0f, 67.0f}, sr, duration);  // E2, C4, E4, G4
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

/// @brief Creates D minor chord (D-F-A).
Audio create_d_minor(int sr = 22050, float duration = 1.0f) {
  return create_chord({62.0f, 65.0f, 69.0f}, sr, duration);  // D4, F4, A4
}

/// @brief Creates E major chord (E-G#-B).
Audio create_e_major(int sr = 22050, float duration = 1.0f) {
  return create_chord({64.0f, 68.0f, 71.0f}, sr, duration);  // E4, G#4, B4
}

/// @brief Creates Bb major chord (Bb-D-F).
Audio create_bb_major(int sr = 22050, float duration = 1.0f) {
  return create_chord({58.0f, 62.0f, 65.0f}, sr, duration);  // Bb3, D4, F4
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

TEST_CASE("ChordAnalyzer frame-path HMM confidence is consistent and sane", "[chord_analyzer]") {
  // With HMM smoothing on the (non-beat-sync) frame path, the post-Viterbi
  // confidence must be recomputed against the same smoothed chroma the decision
  // used (mirroring the beat-sync path), not the raw per-frame chroma. The
  // resulting per-segment confidences must stay in [0, 1] and be non-trivially
  // positive for a clean sustained chord.
  Audio audio = create_c_major(22050, 2.0f);

  ChordConfig config;
  config.use_triads_only = true;
  config.use_beat_sync = false;  // exercise the frame path
  config.use_hmm = true;         // exercise the post-Viterbi confidence recompute

  ChordAnalyzer analyzer(audio, config);

  const auto& chords = analyzer.chords();
  REQUIRE(!chords.empty());

  float max_confidence = 0.0f;
  for (const auto& chord : chords) {
    REQUIRE(chord.confidence >= 0.0f);
    REQUIRE(chord.confidence <= 1.0f);
    max_confidence = std::max(max_confidence, chord.confidence);
  }
  // A sustained C major correlated against the smoothed chroma should yield a
  // clearly positive confidence; raw-chroma confidences on the noisy first/last
  // frames would have dragged this lower.
  REQUIRE(max_confidence > 0.3f);
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

TEST_CASE("ChordAnalyzer supports opt-in NNLS chroma front-end", "[.][slow][chord_analyzer]") {
  Audio audio = create_c_major(22050, 1.0f);

  ChordConfig config;
  config.use_triads_only = true;
  config.chroma_method = ChromaMethod::NNLS;
  config.use_beat_sync = false;
  config.smoothing_window = 0.5f;

  ChordAnalyzer analyzer(audio, config);

  REQUIRE(analyzer.count() >= 1);
  Chord most_common = analyzer.most_common_chord();
  REQUIRE(most_common.quality == ChordQuality::Major);
}

TEST_CASE("ChordAnalyzer NNLS front-end reads a harmonic triad as the triad", "[chord_analyzer]") {
  // Eight 1/n partials per note put the 9th and major 7th of C in the spectrum (harmonics 9
  // and 15 of C, 3 of G); the full template set must still name the plain triad.
  const int sr = 22050;
  std::vector<float> samples(static_cast<size_t>(sr), 0.0f);
  for (float note : {48.0f, 52.0f, 55.0f}) {
    const float f0 = 440.0f * std::exp2((note - 69.0f) / 12.0f);
    for (int h = 1; h <= 8; ++h) {
      for (size_t i = 0; i < samples.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sr);
        samples[i] += 0.1f / static_cast<float>(h) *
                      std::sin(2.0f * sonare::constants::kPiD * f0 * static_cast<float>(h) * t);
      }
    }
  }
  const Audio audio = Audio::from_vector(std::move(samples), sr);

  ChordConfig config;
  config.chroma_method = ChromaMethod::NNLS;
  config.use_beat_sync = false;
  config.smoothing_window = 0.5f;

  ChordAnalyzer analyzer(audio, config);
  REQUIRE(analyzer.count() >= 1);
  const Chord most_common = analyzer.most_common_chord();
  CAPTURE(most_common.to_string());
  CHECK(most_common.root == PitchClass::C);
  CHECK(most_common.quality == ChordQuality::Major);
}

TEST_CASE("ChordAnalyzer uses low-frequency bass chroma for inversion detection",
          "[.][slow][chord_analyzer]") {
  Audio audio = create_c_major_over_e(22050, 1.0f);

  ChordConfig config;
  config.use_triads_only = true;
  config.chroma_method = ChromaMethod::NNLS;
  config.use_beat_sync = false;
  config.detect_inversions = true;
  config.smoothing_window = 0.5f;
  config.min_duration = 0.0f;

  ChordAnalyzer analyzer(audio, config);
  REQUIRE(analyzer.count() >= 1);

  const auto& chords = analyzer.chords();
  REQUIRE(!chords.empty());
  const Chord most_common = analyzer.most_common_chord();
  REQUIRE(most_common.root == PitchClass::C);
  REQUIRE(most_common.quality == ChordQuality::Major);
  REQUIRE(most_common.bass == PitchClass::E);
}

TEST_CASE("ChordAnalyzer inversion bass is not C-biased for a high pitch-class bass",
          "[.][slow][chord_analyzer]") {
  // G major over a low B bass (first inversion, G/B). B is pitch class 11, the
  // index most heavily penalized by the old energy[c]*(1-0.025*c) weighting,
  // which biased the bass estimate toward low indices (C). With that bogus
  // per-index bias removed, the raw bass-band energy must recover B as the bass.
  Audio audio = create_chord({47.0f, 55.0f, 59.0f, 62.0f}, 22050, 1.0f);  // B2, G3, B3, D4

  ChordConfig config;
  config.use_triads_only = true;
  config.chroma_method = ChromaMethod::NNLS;
  config.use_beat_sync = false;
  config.detect_inversions = true;
  config.smoothing_window = 0.5f;
  config.min_duration = 0.0f;

  ChordAnalyzer analyzer(audio, config);
  REQUIRE(analyzer.count() >= 1);
  const Chord most_common = analyzer.most_common_chord();
  REQUIRE(most_common.root == PitchClass::G);
  REQUIRE(most_common.quality == ChordQuality::Major);
  // The bass must not collapse to C purely from the removed index bias.
  REQUIRE(most_common.bass != PitchClass::C);
  REQUIRE(most_common.bass == PitchClass::B);
}

namespace {

/// @brief Builds a synthetic 12-bin chromagram from a per-time pitch-class set.
/// @param pitches_before Pitch classes active before @p switch_time
/// @param pitches_after Pitch classes active from @p switch_time onwards
Chroma make_two_part_chroma(const std::vector<int>& pitches_before,
                            const std::vector<int>& pitches_after, float switch_time,
                            float duration, int sr, int hop) {
  const int n_frames = static_cast<int>(duration * sr / hop);
  std::vector<float> features(12 * static_cast<size_t>(n_frames), 0.0f);
  for (int f = 0; f < n_frames; ++f) {
    const float time = static_cast<float>(f) * hop / static_cast<float>(sr);
    const auto& active = time < switch_time ? pitches_before : pitches_after;
    for (int pitch : active) {
      features[static_cast<size_t>(pitch) * static_cast<size_t>(n_frames) +
               static_cast<size_t>(f)] = 1.0f;
    }
  }
  return Chroma(std::move(features), 12, n_frames, sr, hop);
}

}  // namespace

TEST_CASE("ChordAnalyzer bass chroma is indexed in its own frame space",
          "[chord_analyzer][inversion]") {
  // The chord segments are expressed in the harmonic chromagram's frame space.
  // A bass chromagram computed at a finer hop covers the same seconds in more
  // frames, so slicing it with harmonic frame numbers reads an earlier part of
  // the song. The inversion decision must therefore be invariant to the bass
  // chromagram's hop length: the same bass content at hop 512 and at hop 2048
  // has to yield the same slash chords.
  constexpr int kSampleRate = 22050;
  constexpr int kCoarseHop = 2048;
  constexpr int kFineHop = 512;
  constexpr float kDuration = 4.0f;
  constexpr float kSwitch = 2.0f;

  // C major for the first half, G major for the second.
  const std::vector<int> c_major = {0, 4, 7};
  const std::vector<int> g_major = {7, 11, 2};
  const Chroma harmonic =
      make_two_part_chroma(c_major, g_major, kSwitch, kDuration, kSampleRate, kCoarseHop);

  // Bass sits on E under the C chord and on D under the G chord, so the
  // expected reading is C/E followed by G/D.
  const std::vector<int> bass_first = {4};
  const std::vector<int> bass_second = {2};
  const Chroma bass_coarse =
      make_two_part_chroma(bass_first, bass_second, kSwitch, kDuration, kSampleRate, kCoarseHop);
  const Chroma bass_fine =
      make_two_part_chroma(bass_first, bass_second, kSwitch, kDuration, kSampleRate, kFineHop);
  REQUIRE(bass_fine.n_frames() == 4 * bass_coarse.n_frames());

  std::vector<float> beat_times;
  for (float t = 0.0f; t < kDuration; t += 0.5f) {
    beat_times.push_back(t);
  }

  ChordConfig config;
  config.use_triads_only = true;
  config.use_beat_sync = true;
  config.detect_inversions = true;
  config.min_duration = 0.0f;

  const ChordAnalyzer coarse(harmonic, beat_times, bass_coarse, config);
  const ChordAnalyzer fine(harmonic, beat_times, bass_fine, config);

  const auto& coarse_chords = coarse.chords();
  const auto& fine_chords = fine.chords();
  REQUIRE(coarse_chords.size() == fine_chords.size());
  REQUIRE(coarse_chords.size() >= 2);

  for (size_t i = 0; i < coarse_chords.size(); ++i) {
    CAPTURE(i, coarse_chords[i].to_string(), fine_chords[i].to_string());
    REQUIRE(coarse_chords[i].root == fine_chords[i].root);
    REQUIRE(coarse_chords[i].quality == fine_chords[i].quality);
    REQUIRE(coarse_chords[i].bass == fine_chords[i].bass);
  }

  // Pin the expected reading so an invariance that holds only because both
  // paths are wrong cannot pass.
  const Chord& first = coarse_chords.front();
  const Chord& last = coarse_chords.back();
  CAPTURE(first.to_string(), last.to_string());
  REQUIRE(first.root == PitchClass::C);
  REQUIRE(first.bass == PitchClass::E);
  REQUIRE(last.root == PitchClass::G);
  REQUIRE(last.bass == PitchClass::D);
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

TEST_CASE("ChordAnalyzer functional analysis minor mode", "[chord_analyzer]") {
  SECTION("chord in relative minor key") {
    Audio audio = create_a_minor(22050, 2.0f);

    ChordConfig config;
    config.use_triads_only = true;

    ChordAnalyzer analyzer(audio, config);

    // Analyze A minor chord in key of A minor
    auto roman = analyzer.functional_analysis(PitchClass::A, Mode::Minor);

    REQUIRE(!roman.empty());
    // A minor in key of A minor should be "i"
    REQUIRE(roman[0] == "i");
  }

  SECTION("major chord in minor key") {
    Audio audio = create_c_major(22050, 2.0f);

    ChordConfig config;
    config.use_triads_only = true;

    ChordAnalyzer analyzer(audio, config);

    // Analyze C major chord in key of A minor
    auto roman = analyzer.functional_analysis(PitchClass::A, Mode::Minor);

    REQUIRE(!roman.empty());
    // C major in key of A minor should be "III" (bIII in major, but III in minor)
    REQUIRE(roman[0] == "III");
  }

  SECTION("G major in A minor key") {
    Audio audio = create_g_major(22050, 2.0f);

    ChordConfig config;
    config.use_triads_only = true;

    ChordAnalyzer analyzer(audio, config);

    // Analyze G major chord in key of A minor
    auto roman = analyzer.functional_analysis(PitchClass::A, Mode::Minor);

    REQUIRE(!roman.empty());
    // G major in key of A minor should be "VII" (bVII in minor scale)
    REQUIRE(roman[0] == "VII");
  }
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

  chord.quality = ChordQuality::Add9;
  REQUIRE(chord.to_string() == "Cadd9");

  chord.quality = ChordQuality::HalfDim7;
  REQUIRE(chord.to_string() == "Cm7b5");

  chord.quality = ChordQuality::Dominant9;
  REQUIRE(chord.to_string() == "C9");

  chord.root = PitchClass::C;
  chord.quality = ChordQuality::Major;
  chord.bass = PitchClass::E;
  REQUIRE(chord.to_string() == "C/E");

  chord.quality = ChordQuality::Unknown;
  REQUIRE(chord.to_string() == "N.C.");
}

TEST_CASE("ChordAnalyzer frame chords", "[chord_analyzer]") {
  Audio audio = create_c_major(22050, 1.0f);

  // Per-frame chords are the frame-level path's; beat sync does not produce them.
  ChordConfig config;
  config.use_beat_sync = false;
  ChordAnalyzer analyzer(audio, config);

  const auto& frame_chords = analyzer.frame_chords();

  REQUIRE(!frame_chords.empty());

  // A tonal fixture at the default threshold should have valid template ids;
  // ambiguous/rejected frames use the documented -1 N.C. sentinel.
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

  // Full templates: 12 roots × (16 base qualities + the extended vocabulary)
  REQUIRE(analyzer1.templates().size() == 12 * (16 + extended_chord_qualities().size()));

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

TEST_CASE("ChordAnalyzer Roman numeral case by quality", "[chord_analyzer]") {
  SECTION("F major in C major key returns uppercase IV") {
    Audio audio = create_f_major(22050, 2.0f);

    ChordConfig config;
    config.use_triads_only = true;

    ChordAnalyzer analyzer(audio, config);

    auto roman = analyzer.functional_analysis(PitchClass::C, Mode::Major);

    REQUIRE(!roman.empty());
    // F major chord is scale degree IV in C major → uppercase "IV"
    REQUIRE(roman[0] == "IV");
  }

  SECTION("D minor in A minor key returns lowercase iv") {
    Audio audio = create_d_minor(22050, 2.0f);

    ChordConfig config;
    config.use_triads_only = true;

    ChordAnalyzer analyzer(audio, config);

    auto roman = analyzer.functional_analysis(PitchClass::A, Mode::Minor);

    REQUIRE(!roman.empty());
    // D minor chord is scale degree iv in A minor → lowercase "iv"
    REQUIRE(roman[0] == "iv");
  }

  SECTION("A minor in C major key returns lowercase vi") {
    Audio audio = create_a_minor(22050, 2.0f);

    ChordConfig config;
    config.use_triads_only = true;

    ChordAnalyzer analyzer(audio, config);

    auto roman = analyzer.functional_analysis(PitchClass::C, Mode::Major);

    REQUIRE(!roman.empty());
    // A minor in C major is vi (minor quality → lowercase)
    REQUIRE(roman[0] == "vi");
  }

  SECTION("G major in C major key returns uppercase V") {
    Audio audio = create_g_major(22050, 2.0f);

    ChordConfig config;
    config.use_triads_only = true;

    ChordAnalyzer analyzer(audio, config);

    auto roman = analyzer.functional_analysis(PitchClass::C, Mode::Major);

    REQUIRE(!roman.empty());
    // G major in C major → "V"
    REQUIRE(roman[0] == "V");
  }

  SECTION("E major in A minor key returns uppercase V") {
    // E major is not diatonic to natural A minor (which has Em at degree v),
    // but E natural minor 7th → interval 7 from A = E, which is degree V.
    // However E major has a raised G# making it the harmonic minor V.
    // The function only looks at root interval, so E in A minor = degree V.
    Audio audio = create_e_major(22050, 2.0f);

    ChordConfig config;
    config.use_triads_only = true;

    ChordAnalyzer analyzer(audio, config);

    auto roman = analyzer.functional_analysis(PitchClass::A, Mode::Minor);

    REQUIRE(!roman.empty());
    // E major chord, root E is degree V in A minor → uppercase "V"
    REQUIRE(roman[0] == "V");
  }
}

TEST_CASE("ChordAnalyzer borrowed chord Roman numerals", "[chord_analyzer]") {
  SECTION("Bb major in C major key is chromatic") {
    // Bb is interval 10 from C. In C major, this is not a diatonic degree.
    // Prefer the conventional lowered seventh spelling over the enharmonic #VI.
    Audio audio = create_bb_major(22050, 2.0f);

    ChordConfig config;
    config.use_triads_only = true;

    ChordAnalyzer analyzer(audio, config);

    auto roman = analyzer.functional_analysis(PitchClass::C, Mode::Major);

    REQUIRE(!roman.empty());
    REQUIRE(roman[0] == "bVII");
  }
}

namespace {

/// @brief C-Am-F-G sine triads at 120 BPM, each held three beats then one beat of rest.
/// @details The last chord holds its whole bar. Attack/decay/release follow a plain ADSR so
///          the rests carry a short release tail, as a rendered clip would.
Audio create_triad_turnaround(int sr) {
  const std::vector<std::vector<float>> voicings = {
      {60.0f, 64.0f, 67.0f}, {57.0f, 60.0f, 64.0f}, {53.0f, 57.0f, 60.0f}, {55.0f, 59.0f, 62.0f}};
  const double bar_sec = 2.0;
  const double beat_sec = 0.5;
  const double attack = 0.008, decay = 0.025, sustain = 0.78, release = 0.020;
  const size_t n = static_cast<size_t>(sr * bar_sec * voicings.size());
  std::vector<float> samples(n, 0.0f);
  for (size_t bar = 0; bar < voicings.size(); ++bar) {
    const double on = static_cast<double>(bar) * bar_sec;
    const double len = bar + 1 == voicings.size() ? bar_sec : bar_sec - beat_sec;
    for (float midi : voicings[bar]) {
      const double freq =
          sonare::constants::kA4Hz * std::pow(2.0, (midi - sonare::constants::kMidiA4) /
                                                       sonare::constants::kSemitonesPerOctave);
      const size_t first = static_cast<size_t>(on * sr);
      const size_t last = std::min(n, static_cast<size_t>((on + len + release) * sr));
      for (size_t i = first; i < last; ++i) {
        const double t = static_cast<double>(i) / sr - on;
        double env = t < attack           ? t / attack
                     : t < attack + decay ? 1.0 - (1.0 - sustain) * (t - attack) / decay
                                          : sustain;
        if (t > len) env *= std::max(0.0, 1.0 - (t - len) / release);
        samples[i] +=
            static_cast<float>(0.25 * env * std::sin(sonare::constants::kTwoPiD * freq * t));
      }
    }
  }
  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("ChordAnalyzer STFT chroma keeps a triad turnaround across sample rates",
          "[chord_analyzer]") {
  const Audio source = create_triad_turnaround(32000);
  const std::vector<std::string> expected = {"C", "Am", "F", "G"};
  const std::vector<float> starts = {0.0f, 2.0f, 4.0f, 6.0f};
  // A 2 s smoothing window straddles each 0.5 s rest, so a boundary may land inside it.
  const float boundary_tolerance = 0.4f;

  for (int sr : {32000, 44100, 48000}) {
    CAPTURE(sr);
    const Audio audio = sr == source.sample_rate() ? source : resample(source, sr);
    ChordConfig config;
    config.use_triads_only = true;
    config.min_duration = 0.3f;
    config.use_beat_sync = false;  // the frame-level STFT chroma path is the subject
    config.chroma_method = ChromaMethod::STFT;
    const auto chords = detect_chords(audio, config);

    std::vector<std::string> names;
    for (const auto& chord : chords) names.push_back(chord.to_string());
    CAPTURE(names);
    REQUIRE(names == expected);
    for (size_t i = 0; i < chords.size(); ++i) {
      CAPTURE(i, chords[i].start);
      CHECK(std::abs(chords[i].start - starts[i]) <= boundary_tolerance);
    }
  }
}

TEST_CASE("Roman numerals keep a sus2 or sus4 chord apart from the plain triad",
          "[chord_analyzer]") {
  const auto numeral = [](ChordQuality quality) {
    const Chord c{PitchClass::C, quality, 0.0f, 1.0f, 1.0f, PitchClass::C};
    return ChordAnalyzer::chord_to_roman_numeral(c, PitchClass::C, Mode::Major);
  };
  CHECK(numeral(ChordQuality::Major) == "I");
  CHECK(numeral(ChordQuality::Sus2) == "Isus2");
  CHECK(numeral(ChordQuality::Sus4) == "Isus4");
}

TEST_CASE("detect_chords from audio honours use_beat_sync", "[chord_analyzer]") {
  // C, G, Am, F under a 120 BPM click, each change a quarter of a beat past a
  // beat, so beat-synchronised boundaries and frame boundaries fall apart.
  const int sr = 22050;
  const std::vector<std::vector<float>> progression = {
      {60, 64, 67}, {55, 59, 62}, {57, 60, 64}, {53, 57, 60}};
  const float beat = 0.5f;
  const float segment = 4.0f * beat;
  std::vector<float> samples;
  for (size_t c = 0; c < progression.size(); ++c) {
    const float length = c == 0 ? segment + 0.25f * beat : segment;
    const Audio part = create_chord(progression[c], sr, length);
    samples.insert(samples.end(), part.data(), part.data() + part.size());
  }
  const int click = sr / 100;
  for (size_t start = 0; start < samples.size(); start += static_cast<size_t>(beat * sr)) {
    for (int i = 0; i < click && start + static_cast<size_t>(i) < samples.size(); ++i) {
      samples[start + static_cast<size_t>(i)] += 1.5f * (1.0f - static_cast<float>(i) / click);
    }
  }
  const Audio audio = Audio::from_vector(std::move(samples), sr);
  // Beat-synchronous chord windows start at the reported beat times.
  const std::vector<float> beats = BeatAnalyzer(audio).beat_times();
  REQUIRE(beats.size() >= 8);
  const auto on_beat_grid = [&](float t) {
    for (float b : beats) {
      if (std::abs(b - t) < 0.03f) return true;
    }
    return false;
  };

  ChordConfig config;
  config.use_beat_sync = true;
  const auto synced = detect_chords(audio, config);
  config.use_beat_sync = false;
  const auto framed = detect_chords(audio, config);
  REQUIRE(synced.size() >= 2);
  REQUIRE(framed.size() >= 2);
  for (size_t i = 1; i < synced.size(); ++i) {
    INFO("beat-synced boundary " << synced[i].start);
    CHECK(on_beat_grid(synced[i].start));
  }
  bool framed_off_grid = false;
  for (size_t i = 1; i < framed.size(); ++i) framed_off_grid |= !on_beat_grid(framed[i].start);
  CHECK(framed_off_grid);
}

TEST_CASE("beat-synchronised detection keeps the opening chord of a triad turnaround",
          "[chord_analyzer]") {
  const Audio source = create_triad_turnaround(32000);
  const std::vector<std::string> expected = {"C", "Am", "F", "G"};
  for (int sr : {32000, 44100, 48000}) {
    CAPTURE(sr);
    const Audio audio = sr == source.sample_rate() ? source : resample(source, sr);
    ChordConfig config;
    config.use_triads_only = true;
    config.min_duration = 0.3f;
    config.use_beat_sync = true;
    const auto chords = detect_chords(audio, config);
    std::vector<std::string> names;
    for (const auto& chord : chords) names.push_back(chord.to_string());
    std::vector<float> beats = BeatAnalyzer(audio).beat_times();
    CAPTURE(names, beats);
    CHECK(names == expected);
  }
}

TEST_CASE("beat-synchronised chord names do not depend on the sample rate",
          "[analysis_rate][chord_analyzer]") {
  using sonare::test::make_rate_material;
  using sonare::test::RateMaterial;
  // Leading and trailing N.C. shorter than one beat period are edge artifacts of the beat grid.
  auto names_at = [](int sr) {
    ChordConfig config;
    config.use_triads_only = true;
    config.min_duration = 0.3f;
    config.use_beat_sync = true;
    const Audio audio = make_rate_material(RateMaterial::TriadTurnaround, sr);
    const std::vector<float> beats = BeatAnalyzer(audio).beat_times();
    std::vector<float> intervals;
    for (size_t i = 1; i < beats.size(); ++i) intervals.push_back(beats[i] - beats[i - 1]);
    REQUIRE_FALSE(intervals.empty());
    std::nth_element(intervals.begin(), intervals.begin() + intervals.size() / 2, intervals.end());
    const float period = intervals[intervals.size() / 2];
    std::vector<Chord> chords = detect_chords(audio, config);
    auto is_edge_nc = [period](const Chord& c) {
      return c.quality == ChordQuality::Unknown && c.duration() < period;
    };
    while (!chords.empty() && is_edge_nc(chords.back())) chords.pop_back();
    size_t first = 0;
    while (first < chords.size() && is_edge_nc(chords[first])) ++first;
    std::vector<std::string> names;
    for (size_t i = first; i < chords.size(); ++i) names.push_back(chords[i].to_string());
    return names;
  };
  const auto reference = names_at(22050);
  CHECK_FALSE(reference.empty());
  for (int sr : {32000, 44100, 48000}) {
    CAPTURE(sr);
    CHECK(names_at(sr) == reference);
  }
}

namespace {

/// @brief Decaying piano-like I-V-vi-IV in C with a bass line, 2 s per chord.
Audio create_decaying_pop_progression(int sr) {
  constexpr double kChordSec = 2.0;
  const std::vector<std::vector<float>> voicings = {
      {36.0f, 60.0f, 64.0f, 67.0f},  // C2 | C4 E4 G4
      {43.0f, 55.0f, 59.0f, 62.0f},  // G2 | G3 B3 D4
      {45.0f, 57.0f, 60.0f, 64.0f},  // A2 | A3 C4 E4
      {41.0f, 53.0f, 57.0f, 60.0f},  // F2 | F3 A3 C4
  };
  const size_t per_chord = static_cast<size_t>(kChordSec * sr);
  std::vector<float> samples(per_chord * voicings.size(), 0.0f);
  for (size_t c = 0; c < voicings.size(); ++c) {
    for (float midi : voicings[c]) {
      const double f0 =
          sonare::constants::kA4Hz * std::pow(2.0, (midi - sonare::constants::kMidiA4) /
                                                       sonare::constants::kSemitonesPerOctave);
      for (size_t i = 0; i < per_chord; ++i) {
        const double t = static_cast<double>(i) / sr;
        const double env = std::exp(-1.5 * t);
        double v = 0.0;
        for (int k = 1; k <= 6; ++k) {
          v += std::sin(sonare::constants::kTwoPiD * f0 * k * t) / k;
        }
        samples[c * per_chord + i] += static_cast<float>(0.1 * env * v);
      }
    }
  }
  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("Chord HMM smoothing keeps a clean I-V-vi-IV progression", "[chord_analyzer][hmm]") {
  // Smoothing may only settle ambiguous transition frames: it may drop a short
  // segment the unsmoothed path reported, but every label it emits must be the
  // one that path chose over most of the same span, every stable unsmoothed
  // segment must survive under its own label, and a boundary may move by at
  // most one beat.
  constexpr int kSr = 22050;
  constexpr int kHop = 512;
  constexpr float kBeatSec = 0.5f;
  const Audio audio = create_decaying_pop_progression(kSr);

  ChromaConfig chroma_config;
  chroma_config.hop_length = kHop;
  const Chroma chroma = Chroma::compute(audio, chroma_config);
  BassChromaConfig bass_config;
  bass_config.cqt.hop_length = kHop;
  const Chroma bass = bass_chroma(audio, bass_config);
  std::vector<float> beats;
  for (float t = 0.0f; t < 8.0f; t += kBeatSec) beats.push_back(t);

  constexpr float kStableSec = 2.0f * kBeatSec;
  const float kBoundaryTolerance = kBeatSec + static_cast<float>(kHop) / kSr;
  const auto same_label_overlap = [](const Chord& segment, const std::vector<Chord>& chords) {
    float overlap = 0.0f;
    for (const Chord& other : chords) {
      if (other.to_string() != segment.to_string()) continue;
      overlap +=
          std::max(0.0f, std::min(segment.end, other.end) - std::max(segment.start, other.start));
    }
    return overlap;
  };

  const std::vector<std::string> expected = {"C", "G", "Am", "F"};
  const auto names = [](const std::vector<Chord>& chords) {
    std::vector<std::string> out;
    for (const auto& chord : chords) out.push_back(chord.to_string());
    return out;
  };

  for (const bool beat_sync : {false, true}) {
    for (const bool triads_only : {true, false}) {
      for (const bool key_context : {false, true}) {
        CAPTURE(beat_sync, triads_only, key_context);
        ChordConfig config;
        config.use_beat_sync = beat_sync;
        config.use_triads_only = triads_only;
        config.use_hmm = false;
        const ChordAnalyzer plain_analyzer(chroma, beats, bass, config);
        const std::vector<Chord>& plain = plain_analyzer.chords();

        config.use_hmm = true;
        config.use_key_context = key_context;
        config.key_root = PitchClass::C;
        config.key_mode = Mode::Major;
        const std::vector<Chord> smoothed = ChordAnalyzer(chroma, beats, bass, config).chords();

        CAPTURE(names(plain), names(smoothed));
        // Over the full vocabulary the unsmoothed argmax already reads these
        // partials as extended chords; that is the argmax's own decision, and
        // what smoothing owes it is only not to relabel it.
        if (triads_only) CHECK(names(plain) == expected);
        // The frame path's oracle is the per-frame argmax before short segments
        // are merged: a run the argmax fragments is merged away there, while the
        // decoder may report it whole.
        const std::vector<int>& argmax = plain_analyzer.frame_chords();
        const float hop_sec = static_cast<float>(kHop) / kSr;
        for (const Chord& segment : smoothed) {
          CAPTURE(segment.to_string(), segment.start, segment.end);
          if (beat_sync) {
            CHECK(same_label_overlap(segment, plain) >= 0.5f * segment.duration());
            continue;
          }
          const int first = static_cast<int>(std::lround(segment.start / hop_sec));
          const int last = std::min(static_cast<int>(argmax.size()),
                                    static_cast<int>(std::lround(segment.end / hop_sec)));
          int agreeing = 0;
          std::vector<std::string> argmax_labels;
          for (int f = first; f < last; ++f) {
            const int index = argmax[static_cast<size_t>(f)];
            std::string label = "N.C.";
            if (index >= 0) {
              const ChordTemplate& chosen = plain_analyzer.templates()[static_cast<size_t>(index)];
              label = chord_symbol(static_cast<int>(chosen.root), static_cast<int>(chosen.quality));
            }
            agreeing += label == segment.to_string() ? 1 : 0;
            argmax_labels.push_back(label);
          }
          CAPTURE(argmax_labels);
          CHECK(2 * agreeing >= last - first);
        }
        for (const Chord& segment : plain) {
          if (segment.duration() < kStableSec) continue;
          CAPTURE(segment.to_string(), segment.start, segment.end);
          CHECK(same_label_overlap(segment, smoothed) >= 0.5f * segment.duration());
          bool boundary_kept = false;
          for (const Chord& other : smoothed) {
            boundary_kept |= other.to_string() == segment.to_string() &&
                             std::abs(other.start - segment.start) <= kBoundaryTolerance;
          }
          CHECK(boundary_kept);
        }
      }
    }
  }
}

namespace merge_confidence_tests {
using namespace sonare;
struct Row {
  int root;
  bool minor, beat, hmm;
  float scale, gain, noise;
  bool reverse;
  int sr;
};
struct Piece {
  int root;
  bool minor;
  int frames;
  float noise;
};
const Row rows[] = {{5, 0, 1, 1, 1, 0.05, 0.8, 0, 44100},   {3, 1, 0, 0, 2, 1, 0.6, 1, 44100},
                    {9, 1, 0, 0, 1, 8, 0.8, 0, 10000},      {9, 0, 1, 1, 0.5, 8, 0.6, 1, 48000},
                    {2, 1, 0, 1, 0.5, 0.05, 0.6, 1, 10000}, {4, 0, 1, 1, 2, 1, 0.8, 0, 48000},
                    {11, 1, 1, 0, 1, 0.05, 0.6, 1, 48000},  {1, 0, 1, 0, 0.5, 1, 0.8, 1, 10000},
                    {0, 0, 0, 1, 1, 1, 0.6, 0, 48000},      {6, 1, 0, 0, 2, 0.05, 0.8, 1, 10000},
                    {10, 1, 1, 0, 0.5, 8, 0.6, 0, 10000},   {7, 1, 0, 1, 0.5, 0.05, 0.6, 0, 44100},
                    {8, 1, 0, 1, 2, 1, 0.8, 0, 44100},      {3, 0, 1, 1, 0.5, 8, 0.8, 0, 10000},
                    {2, 1, 1, 0, 2, 8, 0.8, 0, 44100},      {8, 1, 1, 0, 1, 0.05, 0.6, 1, 48000},
                    {5, 1, 0, 0, 2, 1, 0.8, 1, 10000},      {11, 1, 0, 1, 0.5, 1, 0.8, 0, 48000},
                    {1, 1, 0, 1, 2, 8, 0.6, 0, 44100},      {10, 0, 0, 1, 2, 1, 0.8, 1, 44100},
                    {11, 0, 0, 1, 2, 8, 0.6, 0, 44100},     {5, 1, 1, 1, 0.5, 8, 0.6, 0, 48000},
                    {0, 1, 1, 0, 2, 8, 0.8, 0, 44100},      {2, 0, 0, 1, 1, 1, 0.8, 0, 48000},
                    {6, 0, 1, 1, 1, 8, 0.6, 0, 48000},      {4, 0, 0, 0, 0.5, 0.05, 0.6, 1, 44100},
                    {7, 0, 0, 0, 2, 8, 0.8, 1, 48000},      {3, 1, 1, 1, 1, 0.05, 0.8, 0, 48000},
                    {8, 0, 0, 1, 0.5, 8, 0.8, 1, 10000},    {6, 0, 1, 1, 0.5, 1, 0.8, 0, 48000},
                    {6, 1, 1, 1, 1, 8, 0.6, 1, 44100},      {10, 0, 0, 1, 1, 0.05, 0.6, 1, 48000},
                    {1, 0, 0, 1, 1, 1, 0.8, 0, 48000},      {4, 1, 0, 1, 1, 8, 0.6, 1, 10000},
                    {0, 0, 0, 1, 0.5, 0.05, 0.6, 1, 48000}, {7, 0, 0, 1, 1, 1, 0.6, 0, 10000},
                    {11, 0, 0, 1, 1, 8, 0.8, 0, 10000},     {9, 1, 0, 0, 2, 0.05, 0.6, 0, 48000},
                    {1, 1, 0, 0, 2, 0.05, 0.8, 0, 44100},   {0, 1, 0, 1, 1, 8, 0.6, 1, 10000},
                    {9, 1, 0, 1, 1, 0.05, 0.8, 0, 44100},   {7, 0, 1, 0, 2, 8, 0.6, 1, 10000},
                    {9, 0, 1, 1, 0.5, 1, 0.8, 0, 48000}};
std::vector<Chord> analyze(const std::vector<Piece>& pieces, const Row& r, float min,
                           float threshold = 0) {
  int n = 0;
  for (auto& p : pieces) n += p.frames;
  std::vector<float> x(12 * n), beats;
  int f0 = 0;
  float dt = float(r.sr / 100) / r.sr;
  for (auto& p : pieces) {
    beats.push_back(f0 * dt);
    for (int f = f0; f < f0 + p.frames; ++f)
      for (int c = 0; c < 12; ++c) {
        float v = p.noise;
        if (p.root >= 0 &&
            (c == p.root || c == (p.root + (p.minor ? 3 : 4)) % 12 || c == (p.root + 7) % 12))
          v = 1;
        x[c * n + f] = v * r.gain;
      }
    f0 += p.frames;
  }
  beats.push_back(n * dt);
  ChordConfig cfg;
  cfg.min_duration = min;
  cfg.smoothing_window = 0;
  cfg.threshold = threshold;
  cfg.use_hmm = r.hmm;
  cfg.use_beat_sync = r.beat;
  Chroma z(std::move(x), 12, n, r.sr, r.sr / 100);
  return r.beat ? ChordAnalyzer(z, beats, cfg).chords() : ChordAnalyzer(z, cfg).chords();
}
bool eq(const Chord& a, const Chord& b) {
  return a.root == b.root && a.quality == b.quality && a.bass == b.bass &&
         std::abs(a.start - b.start) < 1e-5 && std::abs(a.end - b.end) < 1e-5 &&
         std::abs(a.confidence - b.confidence) < 2e-5;
}

TEST_CASE("same-identity chord merges preserve the confidence duration integral",
          "[chord][chord_analyzer][merge_confidence]") {
  for (size_t ri = 0; ri < std::size(rows); ++ri) {
    const auto& r = rows[ri];
    int a = int(100 * r.scale), b = int(10 * r.scale), z = int(300 * r.scale);
    float lo = r.reverse ? 0 : r.noise, hi = r.reverse ? r.noise : 0;
    int other = (r.root + 6) % 12;
    for (std::string kind : {"unequal", "short_same", "three_same", "equal_score", "distinct_final",
                             "nc_barrier", "unmerged"}) {
      std::vector<Piece> parts;
      float threshold = 0;
      if (kind == "short_same")
        parts = {{r.root, r.minor, a, lo}, {other, r.minor, b, 0}, {r.root, r.minor, b, hi}};
      else if (kind == "three_same")
        parts = {{r.root, r.minor, a, lo},
                 {other, r.minor, b, 0},
                 {r.root, r.minor, 2 * a, .7f},
                 {other, r.minor, b, 0},
                 {r.root, r.minor, 4 * a, hi}};
      else if (kind == "distinct_final")
        parts = {{r.root, r.minor, a, 0}, {other, r.minor, b, 0}};
      else if (kind == "nc_barrier") {
        parts = {{r.root, r.minor, a, 0}, {-1, r.minor, b, 1}, {r.root, r.minor, z, 0}};
        threshold = .9f;
      } else
        parts = {{r.root, r.minor, a, lo},
                 {other, r.minor, b, 0},
                 {r.root, r.minor, z, kind == "equal_score" ? lo : hi}};
      auto raw = analyze(parts, r, 0, threshold),
           got = analyze(parts, r, kind == "unmerged" ? 0 : .3f, threshold);
      REQUIRE(raw.size() == parts.size());
      for (size_t j = 0; j < parts.size(); ++j) {
        REQUIRE(raw[j].quality ==
                (parts[j].root < 0 ? ChordQuality::Unknown
                                   : (parts[j].minor ? ChordQuality::Minor : ChordQuality::Major)));
        if (j) REQUIRE(std::abs(raw[j - 1].end - raw[j].start) < 1e-5f);
      }
      bool setup = raw.size() == parts.size();
      if (setup)
        for (size_t j = 0; j < parts.size(); ++j) {
          setup = setup && (parts[j].root < 0 ? raw[j].quality == ChordQuality::Unknown
                                              : int(raw[j].root) == parts[j].root);
        }
      if (setup && (kind == "unequal" || kind == "short_same" || kind == "three_same"))
        setup = std::abs(raw[0].confidence - raw[2].confidence) > 1e-3f;
      double want = 0;
      bool pass = false;
      if (setup) {
        if (kind == "distinct_final" || kind == "nc_barrier" || kind == "unmerged") {
          pass = got.size() == raw.size();
          if (pass)
            for (size_t j = 0; j < raw.size(); ++j) pass = pass && eq(raw[j], got[j]);
          want = raw[0].confidence;
        } else {
          double d0 = double(raw[1].end) - raw[0].start, d1 = raw[2].duration();
          want = (d0 * raw[0].confidence + d1 * raw[2].confidence) / (d0 + d1);
          if (kind == "three_same") {
            double first = double(raw[3].end) - raw[0].start, last = raw[4].duration();
            want = (first * want + last * raw[4].confidence) / (first + last);
          }
          pass = got.size() == 1 && int(got[0].root) == r.root &&
                 got[0].quality == (r.minor ? ChordQuality::Minor : ChordQuality::Major) &&
                 got[0].bass == raw[0].bass && std::abs(got[0].start) < 1e-5 &&
                 std::abs(got[0].end - raw.back().end) < 1e-5 &&
                 std::abs(got[0].confidence - want) < 2e-5;
        }
      }

      CAPTURE(ri, kind, r.root, r.minor, r.beat, r.hmm, r.scale, r.gain, r.noise, r.reverse, r.sr,
              want);
      REQUIRE(setup);
      CHECK(pass);
    }
  }
}

TEST_CASE("short different chords and rejected intervals preserve confidence policy",
          "[chord][chord_analyzer][merge_confidence]") {
  for (size_t ri = 0; ri < std::size(rows); ++ri) {
    const auto& r = rows[ri];
    int a = int(100 * r.scale), b = int(10 * r.scale), other = (r.root + 6) % 12,
        next = (r.root + 5) % 12;
    for (std::string kind : {"different_absorb", "prefix_absorb", "silence"}) {
      std::vector<Piece> pieces;
      if (kind == "different_absorb")
        pieces = {{r.root, r.minor, a, r.noise}, {other, r.minor, b, 0}, {next, r.minor, 2 * a, 0}};
      else if (kind == "prefix_absorb")
        pieces = {{other, r.minor, b, 0}, {r.root, r.minor, a, r.noise}};
      else
        pieces = {{-1, false, a, 0}};
      auto raw = analyze(pieces, r, 0, kind == "silence" ? .5f : 0),
           got = analyze(pieces, r, .3f, kind == "silence" ? .5f : 0);
      bool setup = raw.size() == pieces.size(), pass = false;
      double want = -1;
      if (setup) {
        if (kind == "different_absorb") {
          want = raw[0].confidence;
          pass = got.size() == 2 && int(got[0].root) == r.root &&
                 got[0].quality == raw[0].quality && got[0].bass == raw[0].bass &&
                 std::abs(got[0].start - raw[0].start) < 1e-5 && int(got[1].root) == next &&
                 std::abs(got[0].confidence - raw[0].confidence) < 2e-5 &&
                 std::abs(got[0].end - raw[1].end) < 1e-5 && eq(got[1], raw[2]);
        } else if (kind == "prefix_absorb") {
          want = raw[1].confidence;
          pass = got.size() == 1 && int(got[0].root) == r.root &&
                 got[0].quality == raw[1].quality && got[0].bass == raw[1].bass &&
                 std::abs(got[0].confidence - want) < 2e-5 && std::abs(got[0].start) < 1e-5 &&
                 std::abs(got[0].end - raw[1].end) < 1e-5;
        } else {
          want = 0;
          pass = got.size() == 1 && got[0].quality == ChordQuality::Unknown &&
                 std::abs(got[0].confidence) < 2e-5 && eq(got[0], raw[0]);
        }
      }
      CAPTURE(ri, kind, r.root, r.minor, r.beat, r.hmm, r.scale, r.gain, r.noise, r.reverse, r.sr,
              want);
      REQUIRE(setup);
      CHECK(pass);
    }
  }
}

}  // namespace merge_confidence_tests

TEST_CASE("a zero-duration same chord does not dilute retained confidence",
          "[chord][chord_analyzer][merge_confidence]") {
  constexpr int frames = 110;
  std::vector<float> data(12 * frames);
  for (int c = 0; c < 12; ++c) {
    for (int f = 0; f < frames; ++f) {
      const int chord_root = f < 100 || f == frames - 1 ? 0 : 6;
      const float noise = f == frames - 1 ? 0.8f : 0.0f;
      data[c * frames + f] =
          c == chord_root || c == (chord_root + 4) % 12 || c == (chord_root + 7) % 12 ? 1.0f
                                                                                      : noise;
    }
  }
  const Chroma chroma(std::move(data), 12, frames, 10000, 100);
  ChordConfig config;
  config.use_hmm = false;
  config.smoothing_window = 0.0f;
  config.threshold = 0.0f;
  config.min_duration = 0.0f;
  const std::vector<float> beats{0.0f, 1.0f, chroma.duration()};
  const auto raw = ChordAnalyzer(chroma, beats, config).chords();
  REQUIRE(raw.size() == 3);
  REQUIRE(raw[0].root == PitchClass::C);
  REQUIRE(raw[1].root == PitchClass::Fs);
  REQUIRE(raw[2].root == PitchClass::C);
  REQUIRE(raw[2].quality == ChordQuality::Major);
  REQUIRE(std::abs(raw[2].end - raw[2].start) < 1e-6f);
  REQUIRE(raw[0].confidence > 0.9f);
  REQUIRE(raw[2].confidence < 0.8f);
  config.min_duration = 0.3f;
  const auto merged = ChordAnalyzer(chroma, beats, config).chords();
  REQUIRE(merged.size() == 1);
  CHECK(merged[0].root == raw[0].root);
  CHECK(merged[0].quality == raw[0].quality);
  CHECK(merged[0].bass == raw[0].bass);
  CHECK_THAT(merged[0].start, WithinAbs(raw[0].start, 1e-5f));
  CHECK_THAT(merged[0].end, WithinAbs(raw[2].end, 1e-5f));
  CHECK_THAT(merged[0].confidence, WithinAbs(raw[0].confidence, 2e-5f));
}
