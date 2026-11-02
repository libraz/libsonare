/// @file chord_beat_span_test.cpp
/// @brief Tests for beat-span chroma pooling and the default constant-Q chord front end.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "analysis/chord_analyzer.h"
#include "util/constants.h"

using namespace sonare;

namespace {

/// @brief Adds a note with four 1/k partials under @p envelope, ramped in and out over 5 ms.
template <typename Envelope>
void add_note(std::vector<float>& samples, int sr, float midi, double start, double end,
              Envelope envelope) {
  const double f0 =
      sonare::constants::kA4Hz *
      std::pow(2.0, (midi - sonare::constants::kMidiA4) / sonare::constants::kSemitonesPerOctave);
  constexpr double kRamp = 0.005;
  const size_t first = static_cast<size_t>(std::max(0.0, start) * sr);
  const size_t last = std::min(samples.size(), static_cast<size_t>(end * sr));
  for (size_t i = first; i < last; ++i) {
    const double t = static_cast<double>(i) / sr;
    const double gate = std::min({1.0, (t - start) / kRamp, (end - t) / kRamp});
    double v = 0.0;
    for (int k = 1; k <= 4; ++k) v += std::sin(sonare::constants::kTwoPiD * f0 * k * t) / k;
    samples[i] += static_cast<float>(0.08 * std::max(0.0, gate) * envelope(t - start) * v);
  }
}

const std::vector<float> kCMajor = {36.0f, 60.0f, 64.0f, 67.0f};  // C2 | C4 E4 G4
const std::vector<float> kGMajor = {43.0f, 55.0f, 59.0f, 62.0f};  // G2 | G3 B3 D4
const std::vector<float> kAMinor = {45.0f, 57.0f, 60.0f, 64.0f};  // A2 | A3 C4 E4
const std::vector<float> kFMajor = {41.0f, 53.0f, 57.0f, 60.0f};  // F2 | F3 A3 C4

/// @brief I-V-vi-IV in C, 2 s per chord, each chord struck on every 0.5 s beat.
Audio pop_progression(int sr) {
  const std::vector<std::vector<float>> voicings = {kCMajor, kGMajor, kAMinor, kFMajor};
  std::vector<float> samples(static_cast<size_t>(8.0 * sr), 0.0f);
  const auto decay = [](double t) { return std::exp(-2.0 * t); };
  for (size_t c = 0; c < voicings.size(); ++c) {
    for (int strike = 0; strike < 4; ++strike) {
      const double on = 2.0 * static_cast<double>(c) + 0.5 * strike;
      for (float midi : voicings[c]) add_note(samples, sr, midi, on, on + 0.5, decay);
    }
  }
  return Audio::from_vector(std::move(samples), sr);
}

std::vector<std::string> names_of(const std::vector<Chord>& chords) {
  std::vector<std::string> names;
  for (const Chord& chord : chords) names.push_back(chord.to_string());
  return names;
}

}  // namespace

TEST_CASE("beat-synchronised chords pool chroma over the whole beat span", "[chord][beat_span]") {
  constexpr int kSr = 22050;
  constexpr double kDuration = 12.0;
  constexpr float kPeriod = 2.0f;
  constexpr double kBefore = 0.15;
  constexpr double kAfter = 0.30;
  std::vector<float> beats;
  for (float t = 1.0f; t < kDuration; t += kPeriod) beats.push_back(t);

  // G from 0.15 s before to 0.30 s after each beat, C elsewhere: G fills 22.5 % of each span.
  std::vector<float> samples(static_cast<size_t>(kDuration * kSr), 0.0f);
  const auto sustain = [](double) { return 1.0; };
  double c_start = 0.0;
  for (float beat : beats) {
    for (float midi : kCMajor) add_note(samples, kSr, midi, c_start, beat - kBefore, sustain);
    for (float midi : kGMajor) add_note(samples, kSr, midi, beat - kBefore, beat + kAfter, sustain);
    c_start = beat + kAfter;
  }
  for (float midi : kCMajor) add_note(samples, kSr, midi, c_start, kDuration, sustain);
  const Audio audio = Audio::from_vector(std::move(samples), kSr);

  const Chroma chroma = chord_chroma(audio, ChordChromaConfig{});
  BassChromaConfig bass_config;
  bass_config.cqt.hop_length = ChordConfig{}.hop_length;
  const Chroma bass = bass_chroma(audio, bass_config);

  for (const float shift : {0.0f, 0.046f}) {
    CAPTURE(shift);
    std::vector<float> shifted = beats;
    for (float& beat : shifted) beat += shift;
    const std::vector<Chord> chords = ChordAnalyzer(chroma, shifted, bass, ChordConfig{}).chords();
    CAPTURE(names_of(chords));
    REQUIRE_FALSE(chords.empty());
    for (const Chord& chord : chords) {
      CHECK(chord.root == PitchClass::C);
      CHECK(chord.quality == ChordQuality::Major);
    }
  }
}

TEST_CASE("default chord detection reads a clean I-V-vi-IV at every input rate",
          "[chord][cqt_default]") {
  const std::vector<std::string> expected = {"C", "G", "Am", "F"};
  const std::vector<float> starts = {0.0f, 2.0f, 4.0f, 6.0f};
  constexpr float kBoundaryTolerance = 0.4f;

  for (int sr : {22050, 44100, 48000}) {
    const Audio audio = pop_progression(sr);
    for (const bool beat_sync : {true, false}) {
      CAPTURE(sr, beat_sync);
      ChordConfig config;
      config.use_beat_sync = beat_sync;
      const std::vector<Chord> chords = detect_chords(audio, config);
      CAPTURE(names_of(chords));
      REQUIRE(names_of(chords) == expected);
      for (size_t i = 0; i < chords.size(); ++i) {
        CAPTURE(i, chords[i].start);
        CHECK(std::abs(chords[i].start - starts[i]) <= kBoundaryTolerance);
      }
    }
  }
}

TEST_CASE("default chord detection accepts an input shorter than a chroma frame",
          "[chord][cqt_default]") {
  constexpr int kSr = 22050;
  const Audio full = pop_progression(kSr);
  const Audio audio = Audio::from_vector(std::vector<float>(full.data(), full.data() + 4096), kSr);
  CHECK_NOTHROW(chord_chroma(audio, ChordChromaConfig{}));
  CHECK_NOTHROW(detect_chords(audio));
  ChordConfig frame_path;
  frame_path.use_beat_sync = false;
  CHECK_NOTHROW(detect_chords(audio, frame_path));
}
