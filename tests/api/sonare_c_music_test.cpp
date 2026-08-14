/// @file sonare_c_music_test.cpp
/// @brief Music and acoustic C API tests.

#include <limits>

#include "sonare_c_test_helpers.h"

TEST_CASE("sonare_analyze_impulse_response", "[c_api][acoustic]") {
  const int sample_rate = 48000;
  const float expected_rt60 = 1.0f;
  std::vector<float> samples(static_cast<size_t>(sample_rate) * 4);
  const float decay = std::log(1000.0f) / expected_rt60;
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[i] = std::exp(-decay * t);
  }

  SonareAcousticResult result = {};
  SonareError err =
      sonare_analyze_impulse_response(samples.data(), samples.size(), sample_rate, 6, &result);

  REQUIRE(err == SONARE_OK);
  REQUIRE(std::isfinite(result.rt60));
  REQUIRE(std::isfinite(result.edt));
  REQUIRE(result.rt60 > 0.95f);
  REQUIRE(result.rt60 < 1.05f);
  REQUIRE(result.band_count == 6);
  REQUIRE(result.rt60_bands != nullptr);
  REQUIRE(result.edt_bands != nullptr);
  REQUIRE(result.c50_bands != nullptr);
  REQUIRE(result.c80_bands != nullptr);

  sonare_free_acoustic_result(&result);
  REQUIRE(result.rt60_bands == nullptr);
  REQUIRE(result.band_count == 0);

  SECTION("explicit default preserves the legacy impulse-response result") {
    SonareAcousticResult legacy = {};
    SonareAcousticResult explicit_default = {};
    REQUIRE(sonare_analyze_impulse_response(samples.data(), samples.size(), sample_rate, 6,
                                            &legacy) == SONARE_OK);
    REQUIRE(sonare_analyze_impulse_response_ex(samples.data(), samples.size(), sample_rate, 6,
                                               30.0f, &explicit_default) == SONARE_OK);
    REQUIRE(legacy.band_count == explicit_default.band_count);
    REQUIRE(legacy.rt60 == explicit_default.rt60);
    REQUIRE(legacy.edt == explicit_default.edt);
    sonare_free_acoustic_result(&legacy);
    sonare_free_acoustic_result(&explicit_default);
  }

  SECTION("explicit decay range is accepted and invalid values reset outputs") {
    SonareAcousticResult result = {};
    REQUIRE(sonare_analyze_impulse_response_ex(samples.data(), samples.size(), sample_rate, 6,
                                               20.0f, &result) == SONARE_OK);
    REQUIRE(result.band_count == 6);
    sonare_free_acoustic_result(&result);

    std::memset(&result, 0xAA, sizeof(result));
    REQUIRE(sonare_analyze_impulse_response_ex(samples.data(), samples.size(), sample_rate, 6,
                                               std::numeric_limits<float>::infinity(),
                                               &result) == SONARE_ERROR_INVALID_PARAMETER);
    sonare_free_acoustic_result(&result);
    REQUIRE(result.rt60_bands == nullptr);
    REQUIRE(result.band_count == 0);
  }
}

TEST_CASE("sonare_detect_acoustic", "[.][slow][c_api][acoustic]") {
  const int sample_rate = 48000;
  const float expected_rt60 = 0.7f;
  std::vector<float> samples(static_cast<size_t>(sample_rate) * 4);
  const float decay = std::log(1000.0f) / expected_rt60;
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[i] = std::exp(-decay * t);
  }

  SonareAcousticResult result = {};
  SonareError err = sonare_detect_acoustic(samples.data(), samples.size(), sample_rate, 6, 24,
                                           30.0f, 10.0f, &result);

  REQUIRE(err == SONARE_OK);
  REQUIRE(result.is_blind == 1);
  REQUIRE(std::isfinite(result.rt60));
  REQUIRE(result.rt60 > 0.55f);
  REQUIRE(result.rt60 < 0.85f);
  REQUIRE(result.band_count == 6);
  REQUIRE(result.rt60_bands != nullptr);
  // Blind mode does not compute clarity bands; they are exposed as null so that
  // "not computed" is distinguishable from "computed-but-invalid".
  REQUIRE(result.c50_bands == nullptr);
  REQUIRE(result.c80_bands == nullptr);

  sonare_free_acoustic_result(&result);
}

TEST_CASE("sonare_detect_acoustic blind mode exposes null clarity bands",
          "[.][slow][c_api][acoustic]") {
  const int sample_rate = 48000;
  const float expected_rt60 = 0.7f;
  std::vector<float> samples(static_cast<size_t>(sample_rate) * 4);
  const float decay = std::log(1000.0f) / expected_rt60;
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sample_rate);
    samples[i] = std::exp(-decay * t);
  }

  SonareAcousticResult result = {};
  SonareError err = sonare_detect_acoustic(samples.data(), samples.size(), sample_rate, 6, 24,
                                           30.0f, 10.0f, &result);

  REQUIRE(err == SONARE_OK);
  REQUIRE(result.is_blind == 1);
  REQUIRE(result.band_count > 0);
  REQUIRE(result.rt60_bands != nullptr);
  REQUIRE(result.c50_bands == nullptr);
  REQUIRE(result.c80_bands == nullptr);

  sonare_free_acoustic_result(&result);
}

TEST_CASE("sonare_analyze_timbre", "[c_api]") {
  SECTION("returns timbre scalars and spectral curves") {
    auto samples = generate_chord({261.63f, 329.63f, 392.00f}, 22050, 2.0f);
    SonareTimbreResult result = {};

    SonareError err = sonare_analyze_timbre(samples.data(), samples.size(), 22050, 2048, 512, 128,
                                            13, 0.5f, &result);

    REQUIRE(err == SONARE_OK);
    REQUIRE(result.brightness >= 0.0f);
    REQUIRE(result.brightness <= 1.0f);
    REQUIRE(result.warmth >= 0.0f);
    REQUIRE(result.warmth <= 1.0f);
    REQUIRE(result.density >= 0.0f);
    REQUIRE(result.density <= 1.0f);
    REQUIRE(result.roughness >= 0.0f);
    REQUIRE(result.roughness <= 1.0f);
    REQUIRE(result.complexity >= 0.0f);
    REQUIRE(result.complexity <= 1.0f);
    REQUIRE(result.spectral_centroid_count > 0);
    REQUIRE(result.spectral_flatness_count == result.spectral_centroid_count);
    REQUIRE(result.spectral_rolloff_count == result.spectral_centroid_count);
    REQUIRE(result.spectral_centroid != nullptr);
    REQUIRE(result.spectral_flatness != nullptr);
    REQUIRE(result.spectral_rolloff != nullptr);

    sonare_free_timbre_result(&result);
    REQUIRE(result.spectral_centroid == nullptr);
    REQUIRE(result.spectral_flatness == nullptr);
    REQUIRE(result.spectral_rolloff == nullptr);
    REQUIRE(result.spectral_centroid_count == 0);
    REQUIRE(result.spectral_flatness_count == 0);
    REQUIRE(result.spectral_rolloff_count == 0);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_sine(440.0f, 22050, 1.0f);
    SonareTimbreResult result = {};

    REQUIRE(sonare_analyze_timbre(nullptr, samples.size(), 22050, 2048, 512, 128, 13, 0.5f,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_timbre(samples.data(), samples.size(), 22050, 0, 512, 128, 13, 0.5f,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_timbre(samples.data(), samples.size(), 22050, 2048, 0, 128, 13, 0.5f,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_timbre(samples.data(), samples.size(), 22050, 2048, 512, 0, 13, 0.5f,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_timbre(samples.data(), samples.size(), 22050, 2048, 512, 128, 0, 0.5f,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_timbre(samples.data(), samples.size(), 22050, 2048, 512, 128, 13, 0.0f,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_analyze_timbre(samples.data(), samples.size(), 22050, 2048, 512, 128, 13, 0.5f,
                                  nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("free is safe on partially initialized struct") {
    SonareTimbreResult result = {};
    result.spectral_centroid = new float[1]{1000.0f};
    result.spectral_flatness = new float[1]{0.5f};
    result.spectral_rolloff = new float[1]{3000.0f};
    result.spectral_centroid_count = 1;
    result.spectral_flatness_count = 1;
    result.spectral_rolloff_count = 1;

    sonare_free_timbre_result(&result);

    REQUIRE(result.spectral_centroid == nullptr);
    REQUIRE(result.spectral_flatness == nullptr);
    REQUIRE(result.spectral_rolloff == nullptr);
    REQUIRE(result.spectral_centroid_count == 0);
    REQUIRE(result.spectral_flatness_count == 0);
    REQUIRE(result.spectral_rolloff_count == 0);
  }
}

TEST_CASE("sonare_detect_chords", "[.][slow][c_api]") {
  SECTION("returns chord segments for a simple C major chord") {
    auto samples = generate_chord({261.63f, 329.63f, 392.00f}, 22050, 2.0f);
    SonareChordAnalysisResult result = {};

    SonareError err = sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, 2.0f, 0.5f,
                                           1, 2048, 512, 0, &result);

    REQUIRE(err == SONARE_OK);
    REQUIRE(result.chord_count > 0);
    REQUIRE(result.chords != nullptr);
    REQUIRE(result.chords[0].root >= SONARE_PITCH_C);
    REQUIRE(result.chords[0].root <= SONARE_PITCH_B);
    REQUIRE(result.chords[0].quality >= SONARE_CHORD_MAJOR);
    REQUIRE(result.chords[0].quality <= SONARE_CHORD_SUS2_ADD4);
    REQUIRE(result.chords[0].end >= result.chords[0].start);
    REQUIRE(result.chords[0].confidence >= 0.0f);

    sonare_free_chord_analysis_result(&result);
    REQUIRE(result.chords == nullptr);
    REQUIRE(result.chord_count == 0);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_chord({261.63f, 329.63f, 392.00f}, 22050, 1.0f);
    SonareChordAnalysisResult result = {};

    REQUIRE(sonare_detect_chords(nullptr, samples.size(), 22050, 0.3f, 2.0f, 0.5f, 0, 2048, 512, 0,
                                 &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, -0.1f, 2.0f, 0.5f, 0, 2048,
                                 512, 0, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, 0.0f, 0.5f, 0, 2048,
                                 512, 0, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, 2.0f, -0.1f, 0, 2048,
                                 512, 0, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, 2.0f, 1.01f, 0, 2048,
                                 512, 0, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, 2.0f, 0.5f, 0, 0, 512,
                                 0, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, 2.0f, 0.5f, 0, 2048,
                                 0, 0, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, 2.0f, 0.5f, 0, 2048,
                                 512, 0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, nan, 0.5f, 0, 2048,
                                 512, 0, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, inf, 0.5f, 0, 2048,
                                 512, 0, &result) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("threshold changes ambiguous input to an explicit N.C. segment") {
    std::vector<float> silence(4096, 0.0f);
    SonareChordAnalysisResult low{};
    SonareChordAnalysisResult high{};
    REQUIRE(sonare_detect_chords(silence.data(), silence.size(), 22050, 0.0f, 0.01f, 0.0f, 1, 2048,
                                 512, 0, &low) == SONARE_OK);
    REQUIRE(sonare_detect_chords(silence.data(), silence.size(), 22050, 0.0f, 0.01f, 0.99f, 1, 2048,
                                 512, 0, &high) == SONARE_OK);
    REQUIRE(low.chord_count == 1);
    REQUIRE(low.chords[0].quality != SONARE_CHORD_UNKNOWN);
    REQUIRE(high.chord_count == 1);
    REQUIRE(high.chords[0].quality == SONARE_CHORD_UNKNOWN);
    REQUIRE(high.chords[0].start == 0.0f);
    REQUIRE(high.chords[0].end > high.chords[0].start);
    sonare_free_chord_analysis_result(&low);
    sonare_free_chord_analysis_result(&high);
  }

  SECTION("extended options enable HMM and inversion detection without changing legacy ABI") {
    auto samples = generate_harmonic_chord({82.41f, 261.63f, 329.63f, 392.00f}, 22050, 1.0f);
    SonareChordAnalysisResult result = {};
    SonareChordDetectionOptions options{};
    options.min_duration = 0.0f;
    options.smoothing_window = 2.0f;
    options.threshold = 0.5f;
    options.use_triads_only = 1;
    options.n_fft = 2048;
    options.hop_length = 512;
    options.use_beat_sync = 0;
    options.use_hmm = 1;
    options.hmm_beam_width = 8;
    options.use_key_context = 1;
    options.key_root = SONARE_PITCH_C;
    options.key_mode = SONARE_MODE_MAJOR;
    options.detect_inversions = 1;
    options.chroma_method = 1;

    SonareError err =
        sonare_detect_chords_ex(samples.data(), samples.size(), 22050, &options, &result);

    REQUIRE(err == SONARE_OK);
    REQUIRE(result.chord_count > 0);
    REQUIRE(result.chords != nullptr);
    REQUIRE(result.chords[0].root == SONARE_PITCH_C);
    REQUIRE(result.chords[0].bass == SONARE_PITCH_E);

    sonare_free_chord_analysis_result(&result);
  }

  SECTION("free is safe on partially initialized struct") {
    SonareChordAnalysisResult result = {};
    result.chords =
        new SonareChord[1]{{SONARE_PITCH_C, SONARE_CHORD_MAJOR, 0.0f, 1.0f, 1.0f, SONARE_PITCH_C}};
    result.chord_count = 1;

    sonare_free_chord_analysis_result(&result);

    REQUIRE(result.chords == nullptr);
    REQUIRE(result.chord_count == 0);
  }
}

TEST_CASE("sonare_chord_functional_analysis", "[c_api]") {
  SonareChordDetectionOptions options{};
  options.min_duration = 0.3f;
  options.smoothing_window = 2.0f;
  options.threshold = 0.5f;
  options.use_triads_only = 1;
  options.n_fft = 2048;
  options.hop_length = 512;
  options.use_beat_sync = 0;
  options.use_hmm = 0;
  options.hmm_beam_width = 8;
  options.use_key_context = 0;
  options.key_root = SONARE_PITCH_C;
  options.key_mode = SONARE_MODE_MAJOR;
  options.detect_inversions = 0;
  options.chroma_method = 0;

  SECTION("returns one Roman-numeral label per detected chord") {
    auto samples = generate_chord({261.63f, 329.63f, 392.00f}, 22050, 2.0f);

    // Detection count for the same options, to cross-check the label count.
    SonareChordAnalysisResult chords = {};
    REQUIRE(sonare_detect_chords_ex(samples.data(), samples.size(), 22050, &options, &chords) ==
            SONARE_OK);
    const size_t expected = chords.chord_count;
    sonare_free_chord_analysis_result(&chords);
    REQUIRE(expected > 0);

    SonareStringArray labels = {};
    SonareError err =
        sonare_chord_functional_analysis(samples.data(), samples.size(), 22050, &options,
                                         SONARE_PITCH_C, SONARE_MODE_MAJOR, &labels);

    REQUIRE(err == SONARE_OK);
    REQUIRE(labels.count == expected);
    REQUIRE(labels.items != nullptr);
    for (size_t i = 0; i < labels.count; ++i) {
      REQUIRE(labels.items[i] != nullptr);
      REQUIRE(std::string(labels.items[i]).size() > 0);
    }

    sonare_free_string_array(&labels);
    REQUIRE(labels.items == nullptr);
    REQUIRE(labels.count == 0);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_chord({261.63f, 329.63f, 392.00f}, 22050, 1.0f);
    SonareStringArray labels = {};

    REQUIRE(sonare_chord_functional_analysis(nullptr, samples.size(), 22050, &options,
                                             SONARE_PITCH_C, SONARE_MODE_MAJOR,
                                             &labels) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_chord_functional_analysis(samples.data(), samples.size(), 22050, nullptr,
                                             SONARE_PITCH_C, SONARE_MODE_MAJOR,
                                             &labels) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_chord_functional_analysis(samples.data(), samples.size(), 22050, &options,
                                             SONARE_PITCH_C, SONARE_MODE_MAJOR,
                                             nullptr) == SONARE_ERROR_INVALID_PARAMETER);

    SonareChordDetectionOptions bad = options;
    bad.n_fft = 0;
    REQUIRE(sonare_chord_functional_analysis(samples.data(), samples.size(), 22050, &bad,
                                             SONARE_PITCH_C, SONARE_MODE_MAJOR,
                                             &labels) == SONARE_ERROR_INVALID_PARAMETER);

    REQUIRE(sonare_chord_functional_analysis(samples.data(), samples.size(), 22050, &options,
                                             static_cast<SonarePitchClass>(99), SONARE_MODE_MAJOR,
                                             &labels) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_chord_functional_analysis(samples.data(), samples.size(), 22050, &options,
                                             SONARE_PITCH_C, static_cast<SonareMode>(99),
                                             &labels) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("free is safe on a zero-initialized struct") {
    SonareStringArray labels = {};
    sonare_free_string_array(&labels);
    REQUIRE(labels.items == nullptr);
    REQUIRE(labels.count == 0);
  }
}

TEST_CASE(
    "analysis wrappers zero owning out-pointers before a validating early-return, so free is "
    "always safe",
    "[c_api][safety]") {
  // Idiomatic C usage: SonareXxxResult r; sonare_xxx(..., &r); sonare_free_xxx(&r);. Poison the
  // struct with non-zero stack garbage first, then reject the call with an invalid scalar
  // parameter: if the out-pointer is zeroed only after the scalar validation (or not at all on
  // that path), sonare_free_xxx would delete[] the poisoned pointer instead of a NULL one.
  auto samples = generate_sine(440.0f, 22050, 1.0f);

  SECTION("sonare_analyze_impulse_response") {
    SonareAcousticResult result;
    std::memset(&result, 0xAA, sizeof(result));
    REQUIRE(sonare_analyze_impulse_response(samples.data(), samples.size(), 22050, -1, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    sonare_free_acoustic_result(&result);
    REQUIRE(result.rt60_bands == nullptr);
    REQUIRE(result.band_count == 0);
  }

  SECTION("sonare_detect_acoustic") {
    SonareAcousticResult result;
    std::memset(&result, 0xAA, sizeof(result));
    REQUIRE(sonare_detect_acoustic(samples.data(), samples.size(), 22050, -1, 24, 30.0f, 10.0f,
                                   &result) == SONARE_ERROR_INVALID_PARAMETER);
    sonare_free_acoustic_result(&result);
    REQUIRE(result.rt60_bands == nullptr);
    REQUIRE(result.band_count == 0);
  }

  SECTION("sonare_analyze_rhythm") {
    SonareRhythmResult result;
    std::memset(&result, 0xAA, sizeof(result));
    REQUIRE(sonare_analyze_rhythm(samples.data(), samples.size(), 22050, 0.0f, 200.0f, 120.0f, 2048,
                                  512, &result) == SONARE_ERROR_INVALID_PARAMETER);
    sonare_free_rhythm_result(&result);
    REQUIRE(result.beat_intervals == nullptr);
    REQUIRE(result.beat_interval_count == 0);
  }

  SECTION("sonare_analyze_dynamics") {
    SonareDynamicsResult result;
    std::memset(&result, 0xAA, sizeof(result));
    REQUIRE(sonare_analyze_dynamics(samples.data(), samples.size(), 22050, 0.0f, 512, 0.5f,
                                    &result) == SONARE_ERROR_INVALID_PARAMETER);
    sonare_free_dynamics_result(&result);
    REQUIRE(result.loudness_times == nullptr);
    REQUIRE(result.loudness_rms_db == nullptr);
    REQUIRE(result.loudness_count == 0);
  }

  SECTION("sonare_analyze_timbre") {
    SonareTimbreResult result;
    std::memset(&result, 0xAA, sizeof(result));
    REQUIRE(sonare_analyze_timbre(samples.data(), samples.size(), 22050, 0, 512, 128, 13, 0.5f,
                                  &result) == SONARE_ERROR_INVALID_PARAMETER);
    sonare_free_timbre_result(&result);
    REQUIRE(result.spectral_centroid == nullptr);
    REQUIRE(result.spectral_flatness == nullptr);
    REQUIRE(result.spectral_rolloff == nullptr);
    REQUIRE(result.timbre_over_time == nullptr);
  }

  SECTION("sonare_detect_chords") {
    SonareChordAnalysisResult result;
    std::memset(&result, 0xAA, sizeof(result));
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, -0.1f, 2.0f, 0.5f, 0, 2048,
                                 512, 0, &result) == SONARE_ERROR_INVALID_PARAMETER);
    sonare_free_chord_analysis_result(&result);
    REQUIRE(result.chords == nullptr);
    REQUIRE(result.chord_count == 0);
  }

  SECTION("sonare_detect_chords_ex") {
    SonareChordAnalysisResult result;
    std::memset(&result, 0xAA, sizeof(result));
    SonareChordDetectionOptions options{};
    options.min_duration = 0.3f;
    options.smoothing_window = 2.0f;
    options.threshold = 0.5f;
    options.n_fft = 0;  // invalid
    options.hop_length = 512;
    REQUIRE(sonare_detect_chords_ex(samples.data(), samples.size(), 22050, &options, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    sonare_free_chord_analysis_result(&result);
    REQUIRE(result.chords == nullptr);
    REQUIRE(result.chord_count == 0);
  }

  SECTION("sonare_chord_functional_analysis") {
    SonareStringArray labels;
    std::memset(&labels, 0xAA, sizeof(labels));
    SonareChordDetectionOptions options{};
    options.min_duration = -1.0f;  // invalid
    options.smoothing_window = 2.0f;
    options.threshold = 0.5f;
    options.n_fft = 2048;
    options.hop_length = 512;
    REQUIRE(sonare_chord_functional_analysis(samples.data(), samples.size(), 22050, &options,
                                             SONARE_PITCH_C, SONARE_MODE_MAJOR,
                                             &labels) == SONARE_ERROR_INVALID_PARAMETER);
    sonare_free_string_array(&labels);
    REQUIRE(labels.items == nullptr);
    REQUIRE(labels.count == 0);
  }

  SECTION("sonare_analyze_sections") {
    SonareSectionResult result;
    std::memset(&result, 0xAA, sizeof(result));
    REQUIRE(sonare_analyze_sections(samples.data(), samples.size(), 22050, 0, 512, 0.0f, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    sonare_free_section_result(&result);
    REQUIRE(result.sections == nullptr);
    REQUIRE(result.section_count == 0);
  }

  SECTION("sonare_analyze_melody_ex") {
    SonareMelodyResult result;
    std::memset(&result, 0xAA, sizeof(result));
    REQUIRE(sonare_analyze_melody_ex(samples.data(), samples.size(), 22050, 0.0f, 800.0f, 2048, 512,
                                     0.1f, 0, 1, &result) == SONARE_ERROR_INVALID_PARAMETER);
    sonare_free_melody_result(&result);
    REQUIRE(result.points == nullptr);
    REQUIRE(result.point_count == 0);
  }
}
