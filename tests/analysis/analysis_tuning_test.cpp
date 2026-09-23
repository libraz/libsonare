/// @file analysis_tuning_test.cpp
/// @brief Tuning offset on batch analysis, the options-taking progress entry point, and the
///        Roman numerals analyze attaches to its chords.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "analysis/analysis_json.h"
#include "analysis/chord_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/music_analyzer.h"
#include "core/audio.h"
#include "feature/pitch.h"
#include "util/constants.h"
#include "util/exception.h"

using namespace sonare;

namespace {

constexpr int kSampleRate = 22050;
constexpr float kDetuneSemitones = -0.45f;

/// I-V-vi-IV in C major, one chord per bar at 120 BPM, re-struck on every beat so the beat
/// tracker has onsets, with every partial detuned by @p detune semitones.
Audio detuned_pop_loop(float detune, int loops = 3) {
  const std::vector<std::vector<int>> chords = {
      {48, 60, 64, 67},  // C
      {43, 59, 62, 67},  // G
      {45, 60, 64, 69},  // Am
      {41, 60, 65, 69},  // F
  };
  const float beat_sec = 0.5f;
  const int beat_samples = static_cast<int>(beat_sec * kSampleRate);
  std::vector<float> samples;
  samples.reserve(static_cast<size_t>(loops) * chords.size() * 4 * beat_samples);
  for (int loop = 0; loop < loops; ++loop) {
    for (const auto& chord : chords) {
      for (int beat = 0; beat < 4; ++beat) {
        for (int n = 0; n < beat_samples; ++n) {
          const float t = static_cast<float>(n) / kSampleRate;
          const float env = std::exp(-3.0f * t) * std::min(1.0f, t * 200.0f);
          float value = 0.0f;
          for (int midi : chord) {
            const float f0 =
                constants::kA4Hz * std::pow(2.0f, (static_cast<float>(midi - 69) + detune) /
                                                      constants::kSemitonesPerOctave);
            for (int h = 1; h <= 4; ++h) {
              value += std::sin(constants::kTwoPi * f0 * h * t) / static_cast<float>(h);
            }
          }
          samples.push_back(0.08f * env * value);
        }
      }
    }
  }
  return Audio::from_vector(std::move(samples), kSampleRate);
}

bool is_c_major(const Key& key) { return key.root == PitchClass::C && key.mode == Mode::Major; }

/// Roots of the chords that are not N.C., in order, with consecutive repeats collapsed.
std::vector<PitchClass> root_sequence(const std::vector<Chord>& chords) {
  std::vector<PitchClass> roots;
  for (const Chord& chord : chords) {
    if (chord.quality == ChordQuality::Unknown) continue;
    if (roots.empty() || roots.back() != chord.root) roots.push_back(chord.root);
  }
  return roots;
}

/// Share of the non-N.C. chord roots that name the loop's four roots in their loop order.
float in_key_root_share(const std::vector<Chord>& chords) {
  const std::set<PitchClass> expected = {PitchClass::C, PitchClass::G, PitchClass::A,
                                         PitchClass::F};
  int total = 0;
  int hits = 0;
  for (const Chord& chord : chords) {
    if (chord.quality == ChordQuality::Unknown) continue;
    ++total;
    if (expected.count(chord.root) != 0) ++hits;
  }
  return total == 0 ? 0.0f : static_cast<float>(hits) / static_cast<float>(total);
}

}  // namespace

TEST_CASE("estimate_tuning reads a flat recording as negative", "[analysis][tuning]") {
  const float estimate = estimate_tuning(detuned_pop_loop(kDetuneSemitones, 1));
  CHECK(estimate < -0.40f);
  CHECK(estimate > -0.50f);
  // Sharp material reads positive, so the sign follows the reference, not the magnitude.
  CHECK(estimate_tuning(detuned_pop_loop(0.3f, 1)) > 0.2f);
}

TEST_CASE("MusicAnalyzer tuning re-centres a detuned recording", "[analysis][tuning][.][slow]") {
  const Audio audio = detuned_pop_loop(kDetuneSemitones);
  const float estimate = estimate_tuning(audio);

  MusicAnalyzerConfig untuned;
  const AnalysisResult off = MusicAnalyzer(audio, untuned).analyze();
  CHECK_FALSE(is_c_major(off.key));

  MusicAnalyzerConfig tuned;
  tuned.tuning = estimate;
  const AnalysisResult on = MusicAnalyzer(audio, tuned).analyze();
  CHECK(is_c_major(on.key));
  CHECK(in_key_root_share(on.chords) > 0.75f);
  CHECK(in_key_root_share(off.chords) < 0.5f);

  SECTION("roman numerals follow the tuned key and match functional analysis") {
    REQUIRE(on.chord_roman_numerals.size() == on.chords.size());
    std::set<std::string> numerals;
    for (size_t i = 0; i < on.chords.size(); ++i) {
      if (on.chords[i].quality == ChordQuality::Unknown) {
        CHECK(on.chord_roman_numerals[i].empty());
      } else {
        numerals.insert(on.chord_roman_numerals[i]);
      }
    }
    for (const char* expected : {"I", "V", "vi", "IV"}) {
      CHECK(numerals.count(expected) == 1);
    }

    // Functional analysis runs its own detection; pair its labels with analyze's by name.
    ChordConfig chord_config;
    chord_config.tuning = estimate;
    ChordAnalyzer chord_analyzer(audio, chord_config);
    const std::vector<std::string> labels =
        chord_analyzer.functional_analysis(on.key.root, on.key.mode);
    std::map<std::string, std::string> label_by_name;
    for (size_t i = 0; i < chord_analyzer.chords().size(); ++i) {
      label_by_name[chord_analyzer.chords()[i].to_string()] = labels[i];
    }
    int compared = 0;
    for (size_t i = 0; i < on.chords.size(); ++i) {
      const auto found = label_by_name.find(on.chords[i].to_string());
      if (found == label_by_name.end() || on.chords[i].quality == ChordQuality::Unknown) continue;
      ++compared;
      CHECK(on.chord_roman_numerals[i] == found->second);
    }
    CHECK(compared >= 4);
  }
}

TEST_CASE("KeyAnalyzer and ChordAnalyzer take the tuning offset", "[analysis][tuning][.][slow]") {
  const Audio audio = detuned_pop_loop(kDetuneSemitones, 2);
  const float estimate = estimate_tuning(audio);

  KeyConfig key_config;
  CHECK_FALSE(is_c_major(KeyAnalyzer(audio, key_config).key()));
  key_config.tuning = estimate;
  CHECK(is_c_major(KeyAnalyzer(audio, key_config).key()));

  ChordConfig chord_config;
  const std::vector<Chord> off = detect_chords(audio, chord_config);
  chord_config.tuning = estimate;
  const std::vector<Chord> on = detect_chords(audio, chord_config);
  // At -45 cents the STFT chroma still finds each root, but the flat partials leak into the
  // semitone below and every chord reads as a major-seventh colour of itself.
  CHECK(in_key_root_share(on) > 0.9f);
  const std::set<std::string> triads = {"C", "G", "Am", "F"};
  std::set<std::string> off_names;
  std::set<std::string> on_names;
  for (const Chord& chord : off) off_names.insert(chord.to_string());
  for (const Chord& chord : on) on_names.insert(chord.to_string());
  for (const std::string& triad : triads) {
    CHECK(on_names.count(triad) == 1);
    CHECK(off_names.count(triad) == 0);
  }
  const std::vector<PitchClass> roots = root_sequence(on);
  REQUIRE(roots.size() >= 4);
  CHECK(roots[0] == PitchClass::C);
  CHECK(roots[1] == PitchClass::G);
  CHECK(roots[2] == PitchClass::A);
  CHECK(roots[3] == PitchClass::F);

  SECTION("NNLS front-end follows the same offset") {
    chord_config.chroma_method = ChromaMethod::NNLS;
    CHECK(in_key_root_share(detect_chords(audio, chord_config)) > 0.9f);
  }
}

TEST_CASE("tuning outside [-0.5, 0.5) is rejected", "[analysis][tuning]") {
  const Audio audio = detuned_pop_loop(0.0f, 1);
  for (float bad : {0.5f, -0.51f, std::nanf("")}) {
    MusicAnalyzerConfig music;
    music.tuning = bad;
    CHECK_THROWS_AS(MusicAnalyzer(audio, music), SonareException);
    KeyConfig key;
    key.tuning = bad;
    CHECK_THROWS_AS(KeyAnalyzer(audio, key), SonareException);
    ChordConfig chord;
    chord.tuning = bad;
    CHECK_THROWS_AS(ChordAnalyzer(audio, chord), SonareException);
  }
  MusicAnalyzerConfig edge;
  edge.tuning = -0.5f;
  CHECK_NOTHROW(MusicAnalyzer(audio, edge));
}

TEST_CASE("C ABI maps tuning and analyzes with options and progress", "[c_api][analysis][tuning]") {
  const Audio audio = detuned_pop_loop(kDetuneSemitones, 1);

  SECTION("defaults carry a zero tuning") {
    CHECK(sonare_music_analyze_options_default().tuning == 0.0f);
  }

  SECTION("out-of-range tuning is an invalid parameter on every entry point") {
    SonareMusicAnalyzeOptions options = sonare_music_analyze_options_default();
    options.tuning = 0.5f;
    char* json = nullptr;
    CHECK(sonare_analyze_json_ex(audio.data(), audio.size(), kSampleRate, &options, &json) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(json == nullptr);
    CHECK(sonare_analyze_json_ex_with_progress(audio.data(), audio.size(), kSampleRate, &options,
                                               nullptr, nullptr, &json, nullptr,
                                               nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(json == nullptr);

    SonareChordDetectionOptions chord{};
    chord.min_duration = 0.3f;
    chord.smoothing_window = 2.0f;
    chord.threshold = 0.5f;
    chord.n_fft = 2048;
    chord.hop_length = 512;
    chord.hmm_beam_width = 24;
    chord.tuning = -0.6f;
    SonareChordAnalysisResult chords{};
    CHECK(sonare_detect_chords_ex(audio.data(), audio.size(), kSampleRate, &chord, &chords) ==
          SONARE_ERROR_INVALID_PARAMETER);
    SonareStringArray labels{};
    CHECK(sonare_chord_functional_analysis(audio.data(), audio.size(), kSampleRate, &chord,
                                           SONARE_PITCH_C, SONARE_MODE_MAJOR,
                                           &labels) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("the progress variant matches the plain one under the same options") {
    SonareMusicAnalyzeOptions options = sonare_music_analyze_options_default();
    options.tuning = -0.45f;
    options.compute_tempo_curve = 1;
    char* plain = nullptr;
    REQUIRE(sonare_analyze_json_ex(audio.data(), audio.size(), kSampleRate, &options, &plain) ==
            SONARE_OK);
    int calls = 0;
    auto on_progress = [](float, const char*, void* user_data) { ++*static_cast<int*>(user_data); };
    char* with_progress = nullptr;
    REQUIRE(sonare_analyze_json_ex_with_progress(audio.data(), audio.size(), kSampleRate, &options,
                                                 on_progress, &calls, &with_progress, nullptr,
                                                 nullptr) == SONARE_OK);
    CHECK(calls > 0);
    CHECK(std::string(plain) == std::string(with_progress));
    CHECK(std::string(plain).find("\"romanNumeral\"") != std::string::npos);
    // The options reached the core: a tempo curve is only decoded on request.
    CHECK(std::string(plain).find("\"beatLocalBpm\":[]") == std::string::npos);

    char* defaults = nullptr;
    REQUIRE(sonare_analyze_json_with_progress_ex(audio.data(), audio.size(), kSampleRate, nullptr,
                                                 nullptr, &defaults, nullptr,
                                                 nullptr) == SONARE_OK);
    CHECK(std::string(defaults) != std::string(plain));
    sonare_free_string(plain);
    sonare_free_string(with_progress);
    sonare_free_string(defaults);
  }

  SECTION("cancellation leaves no JSON") {
    SonareMusicAnalyzeOptions options = sonare_music_analyze_options_default();
    auto cancel_now = [](void*) { return 1; };
    char* json = nullptr;
    CHECK(sonare_analyze_json_ex_with_progress(audio.data(), audio.size(), kSampleRate, &options,
                                               nullptr, nullptr, &json, cancel_now,
                                               nullptr) == SONARE_ERROR_CANCELLED);
    CHECK(json == nullptr);
  }
}

TEST_CASE("analysis JSON carries romanNumeral per chord", "[analysis][json]") {
  AnalysisResult result;
  result.key = {PitchClass::C, Mode::Major, 0.8f};
  result.chords.push_back({PitchClass::G, ChordQuality::Dominant7, 0.0f, 1.0f, 0.8f});
  result.chords.push_back({PitchClass::C, ChordQuality::Unknown, 1.0f, 2.0f, 0.1f});
  result.chord_roman_numerals = {"V7", ""};
  const std::string json = analysis_result_to_json(result);
  CHECK(json.find("\"romanNumeral\":\"V7\"") != std::string::npos);
  CHECK(json.find("\"romanNumeral\":\"\"") != std::string::npos);
}
