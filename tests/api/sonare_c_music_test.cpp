/// @file sonare_c_music_test.cpp
/// @brief Music and acoustic C API tests.

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
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, 2.0f, 0.5f, 0, 0, 512,
                                 0, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, 2.0f, 0.5f, 0, 2048,
                                 0, 0, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_detect_chords(samples.data(), samples.size(), 22050, 0.3f, 2.0f, 0.5f, 0, 2048,
                                 512, 0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
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
