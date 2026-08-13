/// @file sonare_c_mixing_features_test.cpp
/// @brief Mixing and feature edge-case C API tests.

#include "sonare_c_test_helpers.h"

#if defined(SONARE_WITH_MIXING)
TEST_CASE("sonare_strip_schedule_send_automation error mapping", "[c_api][mixing]") {
  SonareMixer* mixer = sonare_mixer_create(48000, 512);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "src");
  REQUIRE(strip != nullptr);

  size_t send_index = 0;
  REQUIRE(sonare_strip_add_send(strip, "send0", "bus0", -6.0f, 0, &send_index) == SONARE_OK);

  SECTION("out-of-range send_index -> INVALID_PARAMETER") {
    // A bad argument must be reported distinctly from a capacity condition.
    REQUIRE(sonare_strip_schedule_send_automation(strip, send_index + 1, 0, -3.0f, 0) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("full send lane -> OUT_OF_MEMORY") {
    // Fill the lane to its ring-buffer capacity (default 1024 usable slots).
    // Use a non-decreasing sample_pos so push() is not rejected for ordering.
    SonareError err = SONARE_OK;
    int pushed = 0;
    for (int i = 0; i < 100000; ++i) {
      err = sonare_strip_schedule_send_automation(strip, send_index, i, -3.0f, 0);
      if (err != SONARE_OK) {
        break;
      }
      ++pushed;
    }
    // The lane should accept many events, then fail with OUT_OF_MEMORY (capacity)
    // rather than INVALID_PARAMETER, mirroring the fader/pan/width schedulers.
    REQUIRE(pushed > 0);
    REQUIRE(err == SONARE_ERROR_OUT_OF_MEMORY);
  }

  sonare_mixer_destroy(mixer);
}

TEST_CASE("sonare_mixer_compile success leaves last_error empty for unpatched explicit buses",
          "[c_api][mixing]") {
  SonareMixer* mixer = sonare_mixer_create(48000, 512);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "src");
  REQUIRE(strip != nullptr);
  REQUIRE(sonare_mixer_add_bus(mixer, "sub", "submix") == SONARE_OK);

  size_t send_index = 0;
  REQUIRE(sonare_strip_add_send(strip, "send0", "sub", -6.0f, 0, &send_index) == SONARE_OK);

  REQUIRE(sonare_mixer_compile(mixer) == SONARE_OK);
  REQUIRE(std::string(sonare_last_error_message()) == "");

  sonare_mixer_destroy(mixer);
}
#endif  // defined(SONARE_WITH_MIXING)

TEST_CASE("sonare_metering stereo pair validates both channels", "[c_api][mixing]") {
  const int sr = 48000;
  auto left = generate_sine(440.0f, sr, 0.25f);
  std::vector<float> right = left;

  SECTION("right channel NaN is rejected") {
    std::vector<float> bad_right = right;
    bad_right[bad_right.size() / 2] = std::numeric_limits<float>::quiet_NaN();
    float c = 0.0f;
    REQUIRE(sonare_metering_stereo_correlation(left.data(), bad_right.data(), left.size(), sr,
                                               &c) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("right channel Inf is rejected") {
    std::vector<float> bad_right = right;
    bad_right[0] = std::numeric_limits<float>::infinity();
    float c = 0.0f;
    REQUIRE(sonare_metering_stereo_correlation(left.data(), bad_right.data(), left.size(), sr,
                                               &c) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("left channel NaN is rejected (parity)") {
    std::vector<float> bad_left = left;
    bad_left[bad_left.size() / 2] = std::numeric_limits<float>::quiet_NaN();
    float c = 0.0f;
    REQUIRE(sonare_metering_stereo_correlation(bad_left.data(), right.data(), left.size(), sr,
                                               &c) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_mel_spectrogram_ex exposes a custom Mel range from pure C", "[c_api][features]") {
  const int sr = 22050;
  const int n_fft = 1024;
  const int hop = 256;
  const int n_mels = 40;
  auto samples = generate_sine(440.0f, sr, 1.0f);

  SECTION("custom fmin/fmax forward transform round-trips with the inverse API") {
    // The forward _ex transform and the inverse sonare_mel_to_stft now share the
    // same fmin/fmax, so a custom-range round-trip is possible from pure C.
    const float fmin = 100.0f;
    const float fmax = 8000.0f;
    SonareMelResult mel = {};
    REQUIRE(sonare_mel_spectrogram_ex(samples.data(), samples.size(), sr, n_fft, hop, n_mels, fmin,
                                      fmax, 0, &mel) == SONARE_OK);
    REQUIRE(mel.power != nullptr);
    REQUIRE(mel.n_mels == n_mels);
    REQUIRE(mel.n_frames > 0);

    SonareInverseResult stft = {};
    REQUIRE(sonare_mel_to_stft(mel.power, mel.n_mels, mel.n_frames, sr, n_fft, fmin, fmax, &stft) ==
            SONARE_OK);
    REQUIRE(stft.data != nullptr);
    REQUIRE(stft.rows == n_fft / 2 + 1);
    REQUIRE(stft.n_frames == mel.n_frames);

    sonare_free_inverse_result(&stft);
    sonare_free_mel_result(&mel);
  }

  SECTION("a non-default range yields a different forward result than the default") {
    SonareMelResult def = {};
    SonareMelResult ranged = {};
    REQUIRE(sonare_mel_spectrogram(samples.data(), samples.size(), sr, n_fft, hop, n_mels, &def) ==
            SONARE_OK);
    REQUIRE(sonare_mel_spectrogram_ex(samples.data(), samples.size(), sr, n_fft, hop, n_mels,
                                      500.0f, 4000.0f, 0, &ranged) == SONARE_OK);
    const size_t total = static_cast<size_t>(n_mels) * def.n_frames;
    bool differs = false;
    for (size_t i = 0; i < total && !differs; ++i) {
      differs = std::abs(def.power[i] - ranged.power[i]) > 1e-6f;
    }
    REQUIRE(differs);
    sonare_free_mel_result(&def);
    sonare_free_mel_result(&ranged);
  }

  SECTION("HTK forward transform can be inverted with an HTK-aware inverse API") {
    SonareMelResult mel = {};
    REQUIRE(sonare_mel_spectrogram_ex(samples.data(), samples.size(), sr, n_fft, hop, n_mels, 0.0f,
                                      0.0f, 1, &mel) == SONARE_OK);

    SonareInverseResult slaney = {};
    SonareInverseResult htk = {};
    REQUIRE(sonare_mel_to_stft(mel.power, mel.n_mels, mel.n_frames, sr, n_fft, 0.0f, 0.0f,
                               &slaney) == SONARE_OK);
    REQUIRE(sonare_mel_to_stft_ex(mel.power, mel.n_mels, mel.n_frames, sr, n_fft, 0.0f, 0.0f, 1,
                                  &htk) == SONARE_OK);
    REQUIRE(slaney.rows == htk.rows);
    REQUIRE(slaney.n_frames == htk.n_frames);

    const size_t total = static_cast<size_t>(htk.rows) * htk.n_frames;
    bool differs = false;
    for (size_t i = 0; i < total && !differs; ++i) {
      differs = std::abs(slaney.data[i] - htk.data[i]) > 1e-6f;
    }
    REQUIRE(differs);

    sonare_free_inverse_result(&slaney);
    sonare_free_inverse_result(&htk);
    sonare_free_mel_result(&mel);
  }

  SECTION("sonare_mfcc_ex accepts the range and a null out is rejected") {
    SonareMfccResult mfcc = {};
    REQUIRE(sonare_mfcc_ex(samples.data(), samples.size(), sr, n_fft, hop, n_mels, 13, 100.0f,
                           8000.0f, 0, 0.0f, &mfcc) == SONARE_OK);
    REQUIRE(mfcc.coefficients != nullptr);
    REQUIRE(mfcc.n_mfcc == 13);
    sonare_free_mfcc_result(&mfcc);

    // A non-zero lifter is accepted and changes the coefficients.
    SonareMfccResult liftered = {};
    REQUIRE(sonare_mfcc_ex(samples.data(), samples.size(), sr, n_fft, hop, n_mels, 13, 100.0f,
                           8000.0f, 0, 22.0f, &liftered) == SONARE_OK);
    REQUIRE(liftered.coefficients != nullptr);
    sonare_free_mfcc_result(&liftered);

    REQUIRE(sonare_mfcc_ex(samples.data(), samples.size(), sr, n_fft, hop, n_mels, 13, 0.0f, 0.0f,
                           0, 0.0f, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_mel_delta returns the slope of a linear feature row", "[c_api][features]") {
  const std::vector<float> features = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f};
  float* delta = nullptr;
  REQUIRE(sonare_mel_delta(features.data(), 2, 5, 5, &delta) == SONARE_OK);
  REQUIRE(delta != nullptr);
  for (size_t i = 0; i < features.size(); ++i) REQUIRE(delta[i] == Catch::Approx(1.0f));
  sonare_free_floats(delta);
  REQUIRE(sonare_mel_delta(features.data(), 2, 5, 4, &delta) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare_piptrack returns equally shaped pitch and magnitude matrices",
          "[c_api][features]") {
  constexpr int sr = 22050;
  auto samples = generate_sine(440.0f, sr, 1.0f);
  int n_bins = 0;
  int n_frames = 0;
  float* pitches = nullptr;
  float* magnitudes = nullptr;
  REQUIRE(sonare_piptrack(samples.data(), samples.size(), sr, 1024, 256, 100.0f, 1000.0f, 0.1f,
                          &n_bins, &n_frames, &pitches, &magnitudes) == SONARE_OK);
  REQUIRE(n_bins == 513);
  REQUIRE(n_frames > 0);
  REQUIRE(pitches != nullptr);
  REQUIRE(magnitudes != nullptr);
  sonare_free_floats(pitches);
  sonare_free_floats(magnitudes);
}

TEST_CASE("sonare_reassigned_spectrogram returns equally shaped coordinate matrices",
          "[c_api][features]") {
  const auto samples = generate_sine(440.0f, 22050, 1.0f);
  SonareReassignedSpectrogramResult result{};
  REQUIRE(sonare_reassigned_spectrogram(samples.data(), samples.size(), 22050, 1024, 256, 1e-6f, 0,
                                        &result) == SONARE_OK);
  REQUIRE(result.n_bins == 513);
  REQUIRE(result.n_frames > 0);
  REQUIRE(result.magnitude != nullptr);
  REQUIRE(result.times != nullptr);
  REQUIRE(result.frequencies != nullptr);
  sonare_free_reassigned_spectrogram_result(&result);
  REQUIRE(result.magnitude == nullptr);
}

TEST_CASE(
    "sonare_reassigned_spectrogram rejects a zero n_fft or hop_length instead of crashing, "
    "and leaves an uninitialised out safe to free",
    "[c_api][features]") {
  const auto samples = generate_sine(440.0f, 22050, 1.0f);

  SonareReassignedSpectrogramResult zero_hop;
  std::memset(&zero_hop, 0xAA, sizeof(zero_hop));
  REQUIRE(sonare_reassigned_spectrogram(samples.data(), samples.size(), 22050, 1024, 0, 1e-6f, 0,
                                        &zero_hop) == SONARE_ERROR_INVALID_PARAMETER);
  sonare_free_reassigned_spectrogram_result(&zero_hop);
  REQUIRE(zero_hop.magnitude == nullptr);

  SonareReassignedSpectrogramResult zero_fft;
  std::memset(&zero_fft, 0xAA, sizeof(zero_fft));
  REQUIRE(sonare_reassigned_spectrogram(samples.data(), samples.size(), 22050, 0, 256, 1e-6f, 0,
                                        &zero_fft) == SONARE_ERROR_INVALID_PARAMETER);
  sonare_free_reassigned_spectrogram_result(&zero_fft);
  REQUIRE(zero_fft.magnitude == nullptr);
}

TEST_CASE("sonare_stft_db zeroes every out param before a validate_audio_params rejection",
          "[c_api][features]") {
  // out_n_bins / out_n_frames are non-owning scalars (never freed), but they
  // must still be defined on every exit path rather than left as caller
  // stack garbage when validate_audio_params rejects the (empty) input before
  // the STFT body ever runs.
  int n_bins = -1;
  int n_frames = -1;
  float* db = nullptr;
  const std::vector<float> empty;
  REQUIRE(sonare_stft_db(empty.data(), 0, 22050, 1024, 256, &n_bins, &n_frames, &db) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(n_bins == 0);
  REQUIRE(n_frames == 0);
  REQUIRE(db == nullptr);
  sonare_free_floats(db);
}

TEST_CASE("segment C APIs return owned matrices and index vectors", "[c_api][features]") {
  const std::vector<float> features = {
      1.0f, 0.0f, 1.0f,  // feature row 0, three columns
      0.0f, 1.0f, 1.0f,  // feature row 1
  };
  SonareSegmentMatrix recurrence{};
  SonareSegmentMatrix cross{};
  REQUIRE(sonare_segment_cross_similarity(features.data(), 2, 3, features.data(), 2, 3, 0, "cosine",
                                          "connectivity", &cross) == SONARE_OK);
  REQUIRE(cross.rows == 3);
  REQUIRE(cross.cols == 3);
  sonare_free_segment_matrix(&cross);

  REQUIRE(sonare_segment_recurrence_matrix(features.data(), 2, 3, 0, 1, 1, "euclidean",
                                           "connectivity", &recurrence) == SONARE_OK);
  REQUIRE(recurrence.rows == 3);
  REQUIRE(recurrence.cols == 3);
  REQUIRE(recurrence.values != nullptr);

  SonareSegmentMatrix lag{};
  REQUIRE(sonare_segment_recurrence_to_lag(recurrence.values, recurrence.rows, 1, &lag) ==
          SONARE_OK);
  REQUIRE(lag.rows == 3);
  REQUIRE(lag.cols == 5);

  SonareSegmentMatrix restored{};
  REQUIRE(sonare_segment_lag_to_recurrence(lag.values, lag.rows, lag.cols, &restored) == SONARE_OK);
  REQUIRE(restored.rows == 3);
  REQUIRE(restored.cols == 3);
  sonare_free_segment_matrix(&restored);
  sonare_free_segment_matrix(&lag);

  SonareSegmentIndices boundaries{};
  const std::vector<int> parent_boundaries = {0, 3};
  REQUIRE(sonare_segment_subsegment(features.data(), 2, 3, parent_boundaries.data(),
                                    parent_boundaries.size(), 2, &boundaries) == SONARE_OK);
  REQUIRE(boundaries.count >= 2);
  REQUIRE(boundaries.values[0] == 0);
  sonare_free_segment_indices(&boundaries);

  SonareSegmentIndices clusters{};
  REQUIRE(sonare_segment_agglomerative(features.data(), 2, 3, 2, "average", &clusters) ==
          SONARE_OK);
  REQUIRE(clusters.count == 3);
  sonare_free_segment_indices(&clusters);

  SonareSegmentMatrix enhanced{};
  REQUIRE(sonare_segment_path_enhance(recurrence.values, recurrence.rows, 1, 2, 0, 7, &enhanced) ==
          SONARE_OK);
  REQUIRE(enhanced.rows == 3);
  REQUIRE(enhanced.cols == 3);
  sonare_free_segment_matrix(&enhanced);
  sonare_free_segment_matrix(&recurrence);
}

TEST_CASE("sonare_spectral_bandwidth_ex accepts the Minkowski exponent", "[c_api][features]") {
  const auto samples = generate_sine(440.0f, 22050, 1.0f);
  float* p1 = nullptr;
  float* p2 = nullptr;
  size_t count1 = 0;
  size_t count2 = 0;
  REQUIRE(sonare_spectral_bandwidth_ex(samples.data(), samples.size(), 22050, 1024, 256, 1.0f, &p1,
                                       &count1) == SONARE_OK);
  REQUIRE(sonare_spectral_bandwidth(samples.data(), samples.size(), 22050, 1024, 256, &p2,
                                    &count2) == SONARE_OK);
  REQUIRE(count1 == count2);
  REQUIRE(count1 > 0);
  bool differs = false;
  for (size_t i = 0; i < count1 && !differs; ++i) differs = std::abs(p1[i] - p2[i]) > 1e-5f;
  REQUIRE(differs);
  sonare_free_floats(p1);
  sonare_free_floats(p2);
}

TEST_CASE("sonare_spectral_flux accepts a positive frame lag", "[c_api][features]") {
  const auto samples = generate_sine(440.0f, 22050, 1.0f);
  float* flux = nullptr;
  size_t count = 0;
  REQUIRE(sonare_spectral_flux(samples.data(), samples.size(), 22050, 1024, 256, 2, &flux,
                               &count) == SONARE_OK);
  REQUIRE(flux != nullptr);
  REQUIRE(count > 0);
  sonare_free_floats(flux);
  REQUIRE(sonare_spectral_flux(samples.data(), samples.size(), 22050, 1024, 256, 0, &flux,
                               &count) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare_onset_backtrack returns the preceding energy minimum", "[c_api][features]") {
  const std::vector<int> events = {6};
  const std::vector<float> energy = {1.0f, 0.8f, 0.5f, 0.2f, 0.6f, 0.9f, 1.2f};
  int* result = nullptr;
  size_t count = 0;
  REQUIRE(sonare_onset_backtrack(events.data(), events.size(), energy.data(), energy.size(),
                                 &result, &count) == SONARE_OK);
  REQUIRE(count == 1);
  REQUIRE(result[0] == 3);
  sonare_free_ints(result);
}

TEST_CASE("sonare_griffin_lim reconstructs STFT magnitude", "[c_api][features]") {
  const std::vector<float> magnitude(15, 1.0f);  // 3 bins x 5 frames for n_fft=4.
  float* result = nullptr;
  size_t length = 0;
  REQUIRE(sonare_griffin_lim(magnitude.data(), magnitude.size(), 3, 5, 4, 1, 22050, 1, 0.0f,
                             &result, &length) == SONARE_OK);
  REQUIRE(result != nullptr);
  REQUIRE(length > 0);
  sonare_free_floats(result);
}

TEST_CASE("synthetic audio generators are available from the C API", "[c_api][features]") {
  float* out = nullptr;
  size_t out_length = 0;
  REQUIRE(sonare_tone(440.0f, 22050, 0.01f, 0.0f, 1.0f, &out, &out_length) == SONARE_OK);
  REQUIRE(out_length == 220);
  sonare_free_floats(out);
  REQUIRE(sonare_chirp(200.0f, 800.0f, 22050, 0.01f, 1, &out, &out_length) == SONARE_OK);
  REQUIRE(out_length == 220);
  sonare_free_floats(out);
  const std::vector<float> times = {0.0f};
  REQUIRE(sonare_clicks(times.data(), times.size(), 22050, 32, 1000.0f, 0.01f, &out, &out_length) ==
          SONARE_OK);
  REQUIRE(out_length == 32);
  sonare_free_floats(out);
}
