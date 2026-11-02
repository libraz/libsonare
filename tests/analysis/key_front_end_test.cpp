/// @file key_front_end_test.cpp
/// @brief Tests that key detection reads the constant-Q chord chromagram, without its bass blend,
/// at
///        the measured tuning, and that the unified analysis reaches the same key.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "analysis/analysis_rate.h"
#include "analysis/chord_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/music_analyzer.h"
#include "feature/pitch.h"
#include "util/constants.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;

namespace {

constexpr int kSampleRate = 22050;
constexpr float kSharpSemitones = 0.30f;
/// Per pitch class, between a detuned fixture read at its measured tuning and the in-tune one.
constexpr float kChromaTolerance = 0.02f;

/// @brief Adds a sustained note with four 1/k partials, ramped in and out over 5 ms.
void add_note(std::vector<float>& samples, float midi, double start, double end) {
  const double f0 = constants::kA4Hz *
                    std::pow(2.0, (midi - constants::kMidiA4) / constants::kSemitonesPerOctave);
  constexpr double kRamp = 0.005;
  const size_t first = static_cast<size_t>(start * kSampleRate);
  const size_t last = std::min(samples.size(), static_cast<size_t>(end * kSampleRate));
  for (size_t i = first; i < last; ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    const double gate = std::min({1.0, (t - start) / kRamp, (end - t) / kRamp});
    double v = 0.0;
    for (int k = 1; k <= 4; ++k) v += std::sin(constants::kTwoPiD * f0 * k * t) / k;
    samples[i] += static_cast<float>(0.06 * std::max(0.0, gate) * v);
  }
}

/// @brief C F G C triads over their roots, two seconds each, every note @p detune semitones off.
Audio cfgc_progression(float detune) {
  constexpr double kChordSeconds = 2.0;
  const std::vector<std::vector<float>> voicings = {
      {36.0f, 60.0f, 64.0f, 67.0f},  // C2 | C4 E4 G4
      {41.0f, 57.0f, 60.0f, 65.0f},  // F2 | A3 C4 F4
      {43.0f, 55.0f, 59.0f, 62.0f},  // G2 | G3 B3 D4
      {36.0f, 60.0f, 64.0f, 67.0f},  // C2 | C4 E4 G4
  };
  std::vector<float> samples(static_cast<size_t>(voicings.size() * kChordSeconds * kSampleRate),
                             0.0f);
  for (size_t i = 0; i < voicings.size(); ++i) {
    const double on = static_cast<double>(i) * kChordSeconds;
    for (float midi : voicings[i]) add_note(samples, midi + detune, on, on + kChordSeconds);
  }
  return Audio::from_vector(std::move(samples), kSampleRate);
}

KeyConfig given_tuning(float tuning) {
  KeyConfig config;
  config.tuning = tuning;
  return config;
}

ChordChromaConfig key_chroma_config(float tuning) {
  ChordChromaConfig config;
  config.hop_length = 512 * chord_constants::kChordChromaHopMultiplier;
  config.tuning = tuning;
  config.bass_weighted = false;
  return config;
}

/// @brief A major I-vi-IV-V, one decaying triad per second over a bass on each chord root.
/// @details Every chord root sounds in the bass for the same time, so a chroma that leans on the
///          bass weighs A, F#, D and E alike and loses the tonic to D major.
Audio a_major_root_bass_loop() {
  constexpr int kTonic = 9;
  constexpr std::array<int, 7> kSteps = {0, 2, 4, 5, 7, 9, 11};
  constexpr std::array<int, 4> kDegrees = {0, 5, 3, 4};
  const int n = 8 * kSampleRate;
  std::vector<float> samples(static_cast<size_t>(n), 0.0f);
  auto hz = [](int midi) {
    return constants::kA4Hz *
           std::pow(2.0, (midi - constants::kMidiA4) / constants::kSemitonesPerOctave);
  };
  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    const int degree = kDegrees[static_cast<size_t>((i / kSampleRate) % 4)];
    const double env = std::exp(-2.2 * static_cast<double>(i % kSampleRate) / kSampleRate);
    double v = 0.0;
    for (int voice = 0; voice < 3; ++voice) {
      const int index = degree + voice * 2;
      const double f = hz(60 + kTonic + kSteps[static_cast<size_t>(index % 7)] + 12 * (index / 7));
      for (int k = 1; k <= 4; ++k) v += (0.30 / k) * env * std::sin(constants::kTwoPiD * f * k * t);
    }
    v += 0.45 * env *
         std::sin(constants::kTwoPiD * hz(36 + kTonic + kSteps[static_cast<size_t>(degree)]) * t);
    samples[static_cast<size_t>(i)] = static_cast<float>(0.25 * v);
  }
  return Audio::from_vector(std::move(samples), kSampleRate);
}

float max_abs_difference(const std::array<float, 12>& a, const std::array<float, 12>& b) {
  float out = 0.0f;
  for (size_t i = 0; i < 12; ++i) out = std::max(out, std::abs(a[i] - b[i]));
  return out;
}

}  // namespace

TEST_CASE("KeyAnalyzer reads the chord chromagram at its tuning", "[key][front_end]") {
  const Audio audio = cfgc_progression(0.0f);
  const KeyAnalyzer analyzer(audio, KeyConfig{});
  const std::array<float, 12> expected =
      chord_chroma(audio, key_chroma_config(analyzer.tuning())).mean_energy();
  for (size_t i = 0; i < 12; ++i) {
    CAPTURE(i);
    CHECK_THAT(analyzer.mean_chroma()[i], WithinAbs(expected[i], 1e-6));
  }
}

TEST_CASE("a sharp recording read at its measured tuning matches the in-tune one",
          "[key][front_end]") {
  const Audio in_tune = cfgc_progression(0.0f);
  const Audio sharp = cfgc_progression(kSharpSemitones);
  const float measured = measure_tuning(sharp);
  REQUIRE_THAT(measured, WithinAbs(kSharpSemitones, 0.05));

  const std::array<float, 12> reference =
      chord_chroma(in_tune, key_chroma_config(0.0f)).mean_energy();

  // A shift of a third of the tuning, or none, must fail the tolerance, or it proves nothing.
  const float third_shift_error = max_abs_difference(
      chord_chroma(sharp, key_chroma_config(measured / 3.0f)).mean_energy(), reference);
  const float no_shift_error =
      max_abs_difference(chord_chroma(sharp, key_chroma_config(0.0f)).mean_energy(), reference);
  CAPTURE(third_shift_error, no_shift_error);
  REQUIRE(third_shift_error > kChromaTolerance);
  REQUIRE(no_shift_error > kChromaTolerance);

  const std::array<float, 12> direct =
      chord_chroma(sharp, key_chroma_config(measured)).mean_energy();
  const KeyAnalyzer sharp_key(sharp, KeyConfig{});
  const KeyAnalyzer in_tune_key(in_tune, given_tuning(0.0f));
  CHECK(sharp_key.tuning() == measured);
  for (size_t i = 0; i < 12; ++i) {
    CAPTURE(i);
    CHECK_THAT(direct[i], WithinAbs(reference[i], kChromaTolerance));
    CHECK_THAT(sharp_key.mean_chroma()[i],
               WithinAbs(in_tune_key.mean_chroma()[i], kChromaTolerance));
  }
}

TEST_CASE("MusicAnalyzer and KeyAnalyzer agree on tuning and key", "[key][front_end]") {
  const Audio sharp = cfgc_progression(kSharpSemitones);
  const KeyAnalyzer standalone(sharp, KeyConfig{});
  MusicAnalyzer music(sharp);

  CHECK(music.tuning() == standalone.tuning());
  CHECK_THAT(music.tuning(), WithinAbs(kSharpSemitones, 0.05));

  const Key unified = music.key_analyzer().key();
  CHECK(unified.root == standalone.key().root);
  CHECK(unified.mode == standalone.key().mode);
  CHECK_THAT(unified.confidence, WithinAbs(standalone.key().confidence, 1e-6));
}

TEST_CASE("a root-position bass does not pull the key to the subdominant", "[key][front_end]") {
  const Audio audio = a_major_root_bass_loop();
  ChordChromaConfig blended = key_chroma_config(0.0f);
  blended.bass_weighted = true;
  // The fixture only means something if the bass-blended chord chroma does lose the tonic.
  REQUIRE(KeyAnalyzer(chord_chroma(audio, blended), given_tuning(0.0f)).key().root !=
          PitchClass::A);

  const Key key = KeyAnalyzer(audio, given_tuning(0.0f)).key();
  CHECK(key.root == PitchClass::A);
  CHECK(key.mode == Mode::Major);
}

TEST_CASE("the chroma constructors report the tuning they were given", "[key][front_end]") {
  const Chroma chroma = chord_chroma(cfgc_progression(0.0f), key_chroma_config(0.0f));
  CHECK(KeyAnalyzer(chroma, KeyConfig{}).tuning() == 0.0f);
  CHECK(KeyAnalyzer(chroma, given_tuning(0.2f)).tuning() == 0.2f);
  CHECK(KeyAnalyzer(chroma.mean_energy(), KeyConfig{}).tuning() == 0.0f);
  CHECK(KeyAnalyzer(chroma.mean_energy(), given_tuning(-0.1f)).tuning() == -0.1f);
}

TEST_CASE("a 1000-sample input returns a key", "[key][front_end]") {
  std::vector<float> samples(1000);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = 0.3f * std::sin(constants::kTwoPi * 440.0f * static_cast<float>(i) / kSampleRate);
  }
  const Audio audio = Audio::from_vector(std::move(samples), kSampleRate);
  CHECK_NOTHROW(KeyAnalyzer(audio, KeyConfig{}));
}

TEST_CASE("silence measures no tuning and spreads the candidates evenly", "[key][front_end]") {
  const Audio silence = Audio::from_vector(std::vector<float>(2 * kSampleRate, 0.0f), kSampleRate);
  const KeyAnalyzer analyzer(silence, KeyConfig{});
  CHECK(analyzer.tuning() == 0.0f);
  const auto& candidates = analyzer.all_candidates();
  REQUIRE(candidates.size() == 24);
  for (const auto& candidate : candidates) {
    CHECK_THAT(candidate.key.confidence, WithinAbs(candidates.front().key.confidence, 1e-6));
  }
  CHECK(analyzer.key().root == PitchClass::C);
  CHECK(analyzer.key().mode == Mode::Major);
}

TEST_CASE("key front end dumps the mean chroma of a manifest", "[.][key][dump]") {
  const char* manifest_env = std::getenv("SONARE_KEY_DUMP_MANIFEST");
  const char* out_env = std::getenv("SONARE_KEY_DUMP_OUT");
  if (manifest_env == nullptr || out_env == nullptr) {
    SKIP("SONARE_KEY_DUMP_MANIFEST and SONARE_KEY_DUMP_OUT are not both set");
  }
  const std::filesystem::path manifest(manifest_env);
  std::ifstream in(manifest);
  REQUIRE(in.is_open());
  std::ofstream out(out_env);
  REQUIRE(out.is_open());
  out << "# file\ttuning\tmean[12]\n" << std::setprecision(9);

  size_t written = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::stringstream stream(line);
    std::string dataset;
    std::string file;
    if (!std::getline(stream, dataset, '\t') || !std::getline(stream, file, '\t')) continue;
    const std::filesystem::path path = manifest.parent_path() / file;
    if (!std::filesystem::exists(path)) {
      WARN("Missing audio: " << path.string());
      continue;
    }
    const KeyAnalyzer analyzer(analysis_rate_audio(Audio::from_file(path.string())), KeyConfig{});
    out << file << '\t' << analyzer.tuning();
    for (float value : analyzer.mean_chroma()) out << '\t' << value;
    out << '\n';
    ++written;
  }
  CHECK(written > 0);
}
