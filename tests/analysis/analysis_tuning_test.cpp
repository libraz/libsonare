/// @file analysis_tuning_test.cpp
/// @brief Tuning offset on batch analysis, the options-taking progress entry point, and the
///        Roman numerals analyze attaches to its chords.

#include <sonare/sonare_c.h>

#include <catch2/catch_approx.hpp>
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
#include "core/resample.h"
#include "feature/chroma.h"
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
    SonareChordAnalysisResult detected{};
    SonareStringArray roman{};
    SonareStringArray functions{};
    CHECK(sonare_chord_functional_analysis(audio.data(), audio.size(), kSampleRate, &chord,
                                           SONARE_PITCH_C, SONARE_MODE_MAJOR, &detected, &roman,
                                           &functions) == SONARE_ERROR_INVALID_PARAMETER);
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

namespace {

/// A4 = 446 Hz, expressed as a semitone fraction.
constexpr float kA446Tuning = 0.2349f;

}  // namespace

TEST_CASE("tuning converters round-trip and name the reference pitch", "[feature][tuning]") {
  CHECK(tuning_to_reference_hz(0.0f) == Catch::Approx(440.0f));
  CHECK(tuning_to_reference_hz(1.0f) == Catch::Approx(466.164f).margin(0.01));
  CHECK(tuning_to_reference_hz(-0.5f, 442.0f) == Catch::Approx(429.5f).margin(0.1));
  CHECK(reference_hz_to_tuning(446.0f) == Catch::Approx(kA446Tuning).margin(0.001));
  for (float tuning : {-0.5f, -0.2f, 0.0f, 0.2349f, 0.49f, 7.0f}) {
    CHECK(reference_hz_to_tuning(tuning_to_reference_hz(tuning, 432.0f), 432.0f) ==
          Catch::Approx(tuning).margin(1e-4));
  }
}

TEST_CASE("measure_tuning reports a semitone fraction at every input rate", "[feature][tuning]") {
  const Audio audio = detuned_pop_loop(kA446Tuning, 1);
  const float at_22k = measure_tuning(audio);
  CHECK(at_22k == Catch::Approx(kA446Tuning).margin(0.03));
  CHECK(is_valid_chroma_tuning(at_22k));

  // The same recording at 44.1 kHz reads the same value: the measurement runs at the analysis rate.
  const Audio upsampled = resample(audio, 44100);
  CHECK(measure_tuning(upsampled) == Catch::Approx(at_22k).margin(0.03));

  CHECK(measure_tuning(Audio()) == 0.0f);
}

TEST_CASE("auto tuning is measured by the key and chord analyzers and reported back",
          "[analysis][tuning]") {
  const Audio audio = detuned_pop_loop(kDetuneSemitones, 1);
  const float measured = measure_tuning(audio);
  REQUIRE(measured < -0.40f);

  KeyConfig key_config;
  key_config.auto_tuning = true;
  const KeyAnalyzer key(audio, key_config);
  CHECK(key.tuning() == measured);
  KeyConfig explicit_key;
  explicit_key.tuning = measured;
  CHECK(key.key().root == KeyAnalyzer(audio, explicit_key).key().root);
  CHECK(key.key().mode == KeyAnalyzer(audio, explicit_key).key().mode);

  // A given tuning is reported as given when auto is off.
  explicit_key.tuning = 0.1f;
  CHECK(KeyAnalyzer(audio, explicit_key).tuning() == 0.1f);

  ChordConfig chord_config;
  chord_config.auto_tuning = true;
  chord_config.use_beat_sync = false;
  const ChordAnalyzer chords(audio, chord_config);
  CHECK(chords.tuning() == measured);
  ChordConfig explicit_chord = chord_config;
  explicit_chord.auto_tuning = false;
  explicit_chord.tuning = measured;
  const ChordAnalyzer expected(audio, explicit_chord);
  REQUIRE(chords.chords().size() == expected.chords().size());
  for (size_t i = 0; i < chords.chords().size(); ++i) {
    CHECK(chords.chords()[i].to_string() == expected.chords()[i].to_string());
  }
}

TEST_CASE("MusicAnalyzer auto tuning reports the value it used", "[analysis][tuning]") {
  const Audio audio = detuned_pop_loop(kDetuneSemitones, 1);
  MusicAnalyzerConfig config;
  config.auto_tuning = true;
  const MusicAnalyzer analyzer(audio, config);
  CHECK(analyzer.tuning() == measure_tuning(audio));

  MusicAnalyzerConfig given;
  given.tuning = -0.3f;
  CHECK(MusicAnalyzer(audio, given).tuning() == -0.3f);
}

TEST_CASE("MusicAnalyzer auto tuning re-centres a detuned recording and lands in the JSON",
          "[analysis][tuning][.][slow]") {
  const Audio audio = detuned_pop_loop(kDetuneSemitones);
  MusicAnalyzerConfig config;
  config.auto_tuning = true;
  const AnalysisResult result = MusicAnalyzer(audio, config).analyze();
  CHECK(result.tuning == measure_tuning(audio));
  CHECK(is_c_major(result.key));
  CHECK(analysis_result_to_json(result).find("\"tuning\":-0.4") != std::string::npos);
}

TEST_CASE("C ABI auto tuning, tuning converters and the key tuning entry",
          "[c_api][analysis][tuning]") {
  const Audio audio = detuned_pop_loop(kDetuneSemitones, 1);
  const float measured = measure_tuning(audio);

  SECTION("analysis options carry a version and an auto flag") {
    SonareMusicAnalyzeOptions options = sonare_music_analyze_options_default();
    CHECK(options.struct_version == SONARE_MUSIC_ANALYZE_OPTIONS_VERSION);
    CHECK(options.tuning_auto == 0);
    options.struct_version = 1;
    char* json = nullptr;
    CHECK(sonare_analyze_json_ex(audio.data(), audio.size(), kSampleRate, &options, &json) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(json == nullptr);
  }

  SECTION("chord detection reports the tuning it used") {
    SonareChordDetectionOptions options{};
    options.min_duration = 0.3f;
    options.smoothing_window = 2.0f;
    options.threshold = 0.5f;
    options.n_fft = 2048;
    options.hop_length = 512;
    options.hmm_beam_width = 24;
    options.use_beat_sync = 0;

    SonareChordAnalysisResult given{};
    options.tuning = 0.1f;
    REQUIRE(sonare_detect_chords_ex(audio.data(), audio.size(), kSampleRate, &options, &given) ==
            SONARE_OK);
    CHECK(given.struct_version == SONARE_CHORD_ANALYSIS_RESULT_VERSION);
    CHECK(given.tuning == 0.1f);
    sonare_free_chord_analysis_result(&given);

    SonareChordAnalysisResult measured_result{};
    options.tuning_auto = 1;
    REQUIRE(sonare_detect_chords_ex(audio.data(), audio.size(), kSampleRate, &options,
                                    &measured_result) == SONARE_OK);
    CHECK(measured_result.tuning == measured);
    sonare_free_chord_analysis_result(&measured_result);

    options.struct_version = 1;
    SonareChordAnalysisResult refused{};
    CHECK(sonare_detect_chords_ex(audio.data(), audio.size(), kSampleRate, &options, &refused) ==
          SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("key detection reports the tuning it used") {
    SonareKey key{};
    float used = 99.0f;
    REQUIRE(sonare_detect_key_with_tuning(audio.data(), audio.size(), kSampleRate, 4096, 512, 0, 0,
                                          0.0f, nullptr, 0, SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER,
                                          nullptr, 0.0f, 1, &key, &used) == SONARE_OK);
    CHECK(used == measured);
    CHECK(key.root == SONARE_PITCH_C);
    CHECK(key.mode == SONARE_MODE_MAJOR);

    REQUIRE(sonare_detect_key_with_tuning(audio.data(), audio.size(), kSampleRate, 4096, 512, 0, 0,
                                          0.0f, nullptr, 0, SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER,
                                          nullptr, 0.2f, 0, &key, &used) == SONARE_OK);
    CHECK(used == 0.2f);
    CHECK(sonare_detect_key_with_tuning(audio.data(), audio.size(), kSampleRate, 4096, 512, 0, 0,
                                        0.0f, nullptr, 0, SONARE_KEY_PROFILE_KRUMHANSL_SCHMUCKLER,
                                        nullptr, 0.7f, 0, &key,
                                        &used) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(used == 0.0f);
  }

  SECTION("converters validate and round-trip") {
    float hz = 0.0f;
    REQUIRE(sonare_tuning_to_reference_hz(kA446Tuning, 440.0f, &hz) == SONARE_OK);
    CHECK(hz == Catch::Approx(446.0f).margin(0.02));
    float tuning = 0.0f;
    REQUIRE(sonare_reference_hz_to_tuning(hz, 440.0f, &tuning) == SONARE_OK);
    CHECK(tuning == Catch::Approx(kA446Tuning).margin(1e-4));
    CHECK(sonare_tuning_to_reference_hz(std::nanf(""), 440.0f, &hz) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_tuning_to_reference_hz(0.0f, 0.0f, &hz) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_reference_hz_to_tuning(-1.0f, 440.0f, &tuning) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(sonare_reference_hz_to_tuning(440.0f, 440.0f, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}
