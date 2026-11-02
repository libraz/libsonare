/// @file chord_one_answer_test.cpp
/// @brief Tests that chord detection and the unified analysis report the same chords.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "analysis/beat_analyzer.h"
#include "analysis/chord_analyzer.h"
#include "analysis/music_analyzer.h"
#include "util/constants.h"

using namespace sonare;

namespace {

/// @brief Adds a sustained note with four 1/k partials, ramped in and out over 5 ms.
void add_note(std::vector<float>& samples, int sr, float midi, double start, double end) {
  const double f0 =
      sonare::constants::kA4Hz *
      std::pow(2.0, (midi - sonare::constants::kMidiA4) / sonare::constants::kSemitonesPerOctave);
  constexpr double kRamp = 0.005;
  const size_t first = static_cast<size_t>(start * sr);
  const size_t last = std::min(samples.size(), static_cast<size_t>(end * sr));
  for (size_t i = first; i < last; ++i) {
    const double t = static_cast<double>(i) / sr;
    const double gate = std::min({1.0, (t - start) / kRamp, (end - t) / kRamp});
    double v = 0.0;
    for (int k = 1; k <= 4; ++k) v += std::sin(sonare::constants::kTwoPiD * f0 * k * t) / k;
    samples[i] += static_cast<float>(0.06 * std::max(0.0, gate) * v);
  }
}

/// @brief C G Am F four times at 120 BPM, two beats per chord, with a click on every beat.
/// @details A quarter-beat tail keeps the extended beat grid's last beat clear of the signal end.
Audio one_answer_progression(int sr) {
  constexpr double kBeat = 0.5;
  constexpr double kChordSeconds = 2.0 * kBeat;
  constexpr int kRepeats = 4;
  const std::vector<std::vector<float>> voicings = {
      {36.0f, 60.0f, 64.0f, 67.0f},  // C2 | C4 E4 G4
      {43.0f, 55.0f, 59.0f, 62.0f},  // G2 | G3 B3 D4
      {45.0f, 57.0f, 60.0f, 64.0f},  // A2 | A3 C4 E4
      {41.0f, 53.0f, 57.0f, 60.0f},  // F2 | F3 A3 C4
  };
  const int n_chords = kRepeats * static_cast<int>(voicings.size());
  const double duration = n_chords * kChordSeconds + 0.5 * kBeat;
  std::vector<float> samples(static_cast<size_t>(duration * sr), 0.0f);

  for (int i = 0; i < n_chords; ++i) {
    const double on = i * kChordSeconds;
    const double off = i + 1 == n_chords ? duration : on + kChordSeconds;
    for (float midi : voicings[static_cast<size_t>(i) % voicings.size()]) {
      add_note(samples, sr, midi, on, off);
    }
  }

  const size_t click_length = static_cast<size_t>(sr / 100);
  for (double t = 0.0; t < duration; t += kBeat) {
    const size_t start = static_cast<size_t>(t * sr);
    for (size_t i = 0; i < click_length && start + i < samples.size(); ++i) {
      samples[start + i] +=
          0.7f * (1.0f - static_cast<float>(i) / static_cast<float>(click_length));
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

TEST_CASE("detect_chords and MusicAnalyzer report the same chords at their defaults",
          "[chord][one_answer]") {
  constexpr int kSr = 22050;
  constexpr float kTolerance = 0.001f;
  const Audio audio = one_answer_progression(kSr);

  // The two entry points track beats separately; unequal beats mean the fixture is wrong.
  MusicAnalyzer analyzer(audio);
  const std::vector<float> direct_beats = BeatAnalyzer(audio, BeatConfig{}).beat_times();
  const std::vector<float> analyzer_beats = analyzer.beat_times();
  REQUIRE(direct_beats.size() >= 2);
  REQUIRE(analyzer_beats.size() == direct_beats.size());
  for (size_t i = 0; i < direct_beats.size(); ++i) {
    CAPTURE(i, direct_beats[i], analyzer_beats[i]);
    REQUIRE(std::abs(analyzer_beats[i] - direct_beats[i]) <= kTolerance);
  }

  const std::vector<Chord> detected = detect_chords(audio, ChordConfig{});
  const std::vector<Chord> analyzed = analyzer.chords();
  CAPTURE(names_of(detected), names_of(analyzed));
  REQUIRE_FALSE(detected.empty());
  REQUIRE(analyzed.size() == detected.size());
  for (size_t i = 0; i < detected.size(); ++i) {
    CAPTURE(i, detected[i].start, analyzed[i].start);
    CHECK(analyzed[i].root == detected[i].root);
    CHECK(analyzed[i].quality == detected[i].quality);
    CHECK(std::abs(analyzed[i].start - detected[i].start) <= kTolerance);
  }
}
