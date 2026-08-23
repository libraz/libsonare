/// @file key_analyzer_test.cpp
/// @brief Tests for key analyzer.

#include "analysis/key_analyzer.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>

#include "util/constants.h"

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
      samples[start + i] = 0.5f * std::sin(2.0f * sonare::constants::kPiD * freqs[n] * t);
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
      samples[start + i] = 0.5f * std::sin(2.0f * sonare::constants::kPiD * freqs[n] * t);
    }
  }

  return Audio::from_vector(std::move(samples), sr);
}

Audio create_low_bass_with_c_major(int sr = 22050, float duration = 2.0f) {
  const int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(static_cast<size_t>(n_samples), 0.0f);
  const std::vector<std::pair<float, float>> tones = {
      {55.0f, 1.0f},     // A1 rumble/bass component
      {261.63f, 0.35f},  // C4
      {329.63f, 0.35f},  // E4
      {392.00f, 0.35f},  // G4
  };

  for (const auto& [freq, amplitude] : tones) {
    for (int i = 0; i < n_samples; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(sr);
      samples[static_cast<size_t>(i)] +=
          amplitude * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * freq * t);
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

TEST_CASE("Key to_string all pitch classes", "[key_analyzer]") {
  // Test all 12 pitch classes with both modes
  const std::vector<std::pair<PitchClass, std::string>> pitch_classes = {
      {PitchClass::C, "C"},   {PitchClass::Cs, "C#"}, {PitchClass::D, "D"},
      {PitchClass::Ds, "D#"}, {PitchClass::E, "E"},   {PitchClass::F, "F"},
      {PitchClass::Fs, "F#"}, {PitchClass::G, "G"},   {PitchClass::Gs, "G#"},
      {PitchClass::A, "A"},   {PitchClass::As, "A#"}, {PitchClass::B, "B"}};

  for (const auto& pc : pitch_classes) {
    Key major_key;
    major_key.root = pc.first;
    major_key.mode = Mode::Major;
    major_key.confidence = 0.9f;

    REQUIRE(major_key.to_string() == pc.second + " major");
    REQUIRE(major_key.to_short_string() == pc.second);

    Key minor_key;
    minor_key.root = pc.first;
    minor_key.mode = Mode::Minor;
    minor_key.confidence = 0.9f;

    REQUIRE(minor_key.to_string() == pc.second + " minor");
    REQUIRE(minor_key.to_short_string() == pc.second + "m");
  }
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

TEST_CASE("KeyAnalyzer supports opt-in HPSS and loudness weighted chroma", "[key_analyzer]") {
  Audio audio = create_c_major_scale();

  KeyConfig config;
  config.n_fft = 2048;
  config.use_hpss = true;
  config.loudness_weighted = true;

  KeyAnalyzer analyzer(audio, config);
  Key key = analyzer.key();

  REQUIRE(key.confidence >= 0.0f);
  REQUIRE(!analyzer.all_candidates().empty());
}

TEST_CASE("KeyAnalyzer applies configured high-pass before chroma analysis", "[key_analyzer]") {
  const Audio audio = create_low_bass_with_c_major();

  KeyConfig raw_config;
  raw_config.genre_hint = "";
  raw_config.n_fft = 4096;
  raw_config.hop_length = 512;
  KeyAnalyzer raw_analyzer(audio, raw_config);

  KeyConfig highpass_config = raw_config;
  highpass_config.high_pass_hz = 120.0f;
  KeyAnalyzer highpass_analyzer(audio, highpass_config);

  const auto& raw = raw_analyzer.mean_chroma();
  const auto& highpassed = highpass_analyzer.mean_chroma();
  const int a = static_cast<int>(PitchClass::A);
  const int c = static_cast<int>(PitchClass::C);

  REQUIRE(highpassed[a] < raw[a]);
  REQUIRE(highpassed[c] >= raw[c] * 0.5f);
}

TEST_CASE("KeyAnalyzer auto genre compares audio chroma variants", "[key_analyzer]") {
  Audio audio = create_c_major_scale();

  KeyConfig raw_config;
  raw_config.genre_hint = "";
  raw_config.use_hpss = false;
  raw_config.loudness_weighted = false;

  KeyConfig auto_config;
  auto_config.genre_hint = "auto";
  auto_config.use_hpss = false;
  auto_config.loudness_weighted = false;

  KeyAnalyzer raw_analyzer(audio, raw_config);
  KeyAnalyzer auto_analyzer(audio, auto_config);

  REQUIRE(!auto_analyzer.all_candidates().empty());
  REQUIRE(auto_analyzer.confidence() >= raw_analyzer.confidence());
}

TEST_CASE("KeyAnalyzer candidates", "[key_analyzer]") {
  Audio audio = create_c_major_scale();

  KeyAnalyzer analyzer(audio);

  auto candidates = analyzer.candidates(5);

  REQUIRE(candidates.size() == 5);
  REQUIRE(analyzer.candidates(-1).empty());

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

TEST_CASE("KeyAnalyzer auto profile selection unaffected by profile normalization",
          "[key_analyzer]") {
  // profile_correlation is Pearson, which is invariant to positive scaling of the
  // profile. Dropping the redundant normalize_profile() step in candidate scoring
  // must therefore leave auto profile/key selection unchanged. Exercise the auto
  // path and confirm it still produces a coherent, confident result.
  Audio audio = create_c_major_scale();

  KeyConfig config;
  config.genre_hint = "auto";
  config.n_fft = 4096;

  KeyAnalyzer analyzer(audio, config);
  Key key = analyzer.key();

  // C major scale resolves to C major or a closely related key.
  bool is_c_major = (key.root == PitchClass::C && key.mode == Mode::Major);
  bool is_g_major = (key.root == PitchClass::G && key.mode == Mode::Major);
  bool is_a_minor = (key.root == PitchClass::A && key.mode == Mode::Minor);
  REQUIRE((is_c_major || is_g_major || is_a_minor));

  REQUIRE(analyzer.all_candidates().size() == 24);
  REQUIRE(key.confidence > 0.0f);

  // Candidate correlations must remain sorted (scoring is well-defined without
  // the redundant profile normalization).
  const auto candidates = analyzer.candidates(5);
  REQUIRE(candidates.size() == 5);
  for (size_t i = 1; i < candidates.size(); ++i) {
    REQUIRE(candidates[0].correlation >= candidates[i].correlation);
  }
}

TEST_CASE("detect_key quick function", "[key_analyzer]") {
  Audio audio = create_c_major_scale();

  Key key = detect_key(audio);

  // Should return a valid key
  REQUIRE(static_cast<int>(key.root) >= 0);
  REQUIRE(static_cast<int>(key.root) < 12);
}

TEST_CASE("Key confidence is a distribution over the candidates it scored", "[key_analyzer]") {
  // The old confidence blended a rescaled correlation with a distinctiveness
  // term that clipped at a fixed gap, so decisive material saturated at 1.0 and
  // stayed there however close the runner-up was. Reporting a share of the
  // model's belief instead makes the number move with the evidence.
  std::array<float, 12> c_major_chroma = {};
  const std::array<int, 7> c_major_scale = {0, 2, 4, 5, 7, 9, 11};
  for (int degree : c_major_scale) {
    c_major_chroma[static_cast<size_t>(degree)] = 1.0f;
  }
  c_major_chroma[0] = 2.2f;
  c_major_chroma[7] = 1.6f;
  c_major_chroma[4] = 1.4f;

  KeyConfig config;
  config.genre_hint = "";
  KeyAnalyzer analyzer(c_major_chroma, config);

  const auto& candidates = analyzer.all_candidates();
  REQUIRE(candidates.size() == 24);

  float total = 0.0f;
  for (const auto& candidate : candidates) {
    REQUIRE(candidate.key.confidence >= 0.0f);
    REQUIRE(candidate.key.confidence <= 1.0f);
    total += candidate.key.confidence;
  }
  REQUIRE_THAT(total, WithinAbs(1.0f, 1e-4f));

  // Every candidate is on one scale now, so the ranking by confidence has to
  // agree with the ranking by correlation the list is already sorted on. The
  // old field meant one thing at index 0 and another everywhere else.
  for (size_t i = 1; i < candidates.size(); ++i) {
    REQUIRE(candidates[i - 1].key.confidence >= candidates[i].key.confidence);
  }
  REQUIRE(analyzer.key().confidence == candidates[0].key.confidence);

  // A share of a distribution over 24 candidates cannot reach 1: something is
  // always left over for the rest, which is the property that stops a decisive
  // reading from being reported as certainty.
  REQUIRE(analyzer.key().confidence < 1.0f);
}

TEST_CASE("Key confidence falls when a runner-up is close", "[key_analyzer]") {
  // Two readings that split the same evidence must not both report high. This
  // is the case the previous formula could not express: a bare C-major scale is
  // equally A minor, and it used to come back near 100% regardless.
  std::array<float, 12> ambiguous = {};
  for (int degree : {0, 2, 4, 5, 7, 9, 11}) {
    ambiguous[static_cast<size_t>(degree)] = 1.0f;
  }

  std::array<float, 12> decisive = ambiguous;
  decisive[0] = 3.0f;
  decisive[7] = 2.0f;
  decisive[4] = 1.8f;

  KeyConfig config;
  config.genre_hint = "";
  const KeyAnalyzer ambiguous_analyzer(ambiguous, config);
  const KeyAnalyzer decisive_analyzer(decisive, config);

  CAPTURE(ambiguous_analyzer.key().to_string(), ambiguous_analyzer.key().confidence);
  CAPTURE(decisive_analyzer.key().to_string(), decisive_analyzer.key().confidence);
  REQUIRE(decisive_analyzer.key().confidence > ambiguous_analyzer.key().confidence);

  // The evidence score is a different quantity and must not be confused with
  // the posterior: it is what the analyzer compares when choosing between
  // chroma front-ends, and unlike a share of a distribution it does not shrink
  // just because more candidates were scored.
  REQUIRE(decisive_analyzer.evidence_score() >= ambiguous_analyzer.evidence_score());
}

TEST_CASE("Widening the candidate set divides the same belief", "[key_analyzer]") {
  // A share of a distribution is only meaningful relative to what was scored.
  // Opening the search to the modes splits the same evidence across 84
  // candidates instead of 24, so the reported confidence falls even though the
  // audio and the answer did not change. That is correct for a posterior and
  // exactly why a confidence from one analysis must not be compared against a
  // confidence from another that searched a different set -- and why the
  // analyzer's own front-end selection compares the evidence score instead.
  std::array<float, 12> chroma = {};
  for (int degree : {0, 2, 4, 5, 7, 9, 11}) {
    chroma[static_cast<size_t>(degree)] = 1.0f;
  }
  chroma[0] = 2.4f;
  chroma[7] = 1.7f;

  KeyConfig diatonic;
  diatonic.genre_hint = "";
  diatonic.modes = {Mode::Major, Mode::Minor};

  KeyConfig modal = diatonic;
  modal.modes = {Mode::Major,  Mode::Minor,      Mode::Dorian, Mode::Phrygian,
                 Mode::Lydian, Mode::Mixolydian, Mode::Locrian};

  const KeyAnalyzer narrow(chroma, diatonic);
  const KeyAnalyzer wide(chroma, modal);
  REQUIRE(narrow.all_candidates().size() == 24);
  REQUIRE(wide.all_candidates().size() == 84);
  REQUIRE(wide.key().root == narrow.key().root);
  REQUIRE(wide.key().confidence < narrow.key().confidence);

  float wide_total = 0.0f;
  for (const auto& candidate : wide.all_candidates()) {
    wide_total += candidate.key.confidence;
  }
  REQUIRE_THAT(wide_total, WithinAbs(1.0f, 1e-4f));
}

TEST_CASE("Silence spreads key confidence evenly instead of scoring mid-range", "[key_analyzer]") {
  // Every candidate correlates to 0, so no key stands out. A uniform share is
  // the honest reading of that; the old formula reported a fixed mid-range
  // score, which reads as a weak detection rather than as no detection.
  const std::array<float, 12> silence = {};
  KeyConfig config;
  config.genre_hint = "";
  KeyAnalyzer analyzer(silence, config);

  const auto& candidates = analyzer.all_candidates();
  REQUIRE(candidates.size() == 24);
  for (const auto& candidate : candidates) {
    REQUIRE_THAT(candidate.key.confidence, WithinAbs(1.0f / 24.0f, 1e-5f));
  }
}
