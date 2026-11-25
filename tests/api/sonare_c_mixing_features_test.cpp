/// @file sonare_c_mixing_features_test.cpp
/// @brief Mixing and feature edge-case C API tests.

#include "sonare_c_test_helpers.h"

#if defined(SONARE_WITH_MIXING)
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "c_api/mixing_internal.h"
#endif

#if defined(SONARE_WITH_MIXING)
TEST_CASE("sonare_strip_schedule_send_automation error mapping", "[c_api][mixing]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 512, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "src", nullptr);
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
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 512, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "src", nullptr);
  REQUIRE(strip != nullptr);
  REQUIRE(sonare_mixer_add_bus(mixer, "sub", "submix") == SONARE_OK);

  size_t send_index = 0;
  REQUIRE(sonare_strip_add_send(strip, "send0", "sub", -6.0f, 0, &send_index) == SONARE_OK);

  REQUIRE(sonare_mixer_compile(mixer) == SONARE_OK);
  REQUIRE(std::string(sonare_last_error_message()) == "");

  sonare_mixer_destroy(mixer);
}

TEST_CASE("sonare_mixer_process_stereo leaves the caller's output untouched when compile fails",
          "[c_api][mixing]") {
  constexpr size_t kBlock = 64;
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, static_cast<int>(kBlock), &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  REQUIRE(sonare_mixer_add_strip(mixer, "src", nullptr) != nullptr);
  std::vector<float> input(kBlock, 0.25f);
  const float* in[] = {input.data()};
  std::vector<float> left(kBlock, 0.0f);
  std::vector<float> right(kBlock, 0.0f);
  REQUIRE(sonare_mixer_process_stereo(mixer, in, in, 1, left.data(), right.data(), kBlock) ==
          SONARE_OK);

  // A 5.1 render width over the implicit stereo master makes the lazy compile throw.
  sonare_c_mixing_detail::set_output_channels(mixer, 6);
  for (size_t i = 0; i < kBlock; ++i) {
    left[i] = 0.5f + static_cast<float>(i);
    right[i] = -0.5f - static_cast<float>(i);
  }
  const std::vector<float> left_before = left;
  const std::vector<float> right_before = right;
  REQUIRE(sonare_mixer_process_stereo(mixer, in, in, 1, left.data(), right.data(), kBlock) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) ==
        "num_channels must not exceed the channel count of the master bus");
  REQUIRE(std::memcmp(left.data(), left_before.data(), kBlock * sizeof(float)) == 0);
  REQUIRE(std::memcmp(right.data(), right_before.data(), kBlock * sizeof(float)) == 0);
  REQUIRE(sonare_mixer_drain_tail_stereo(mixer, left.data(), right.data(), kBlock) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(std::memcmp(left.data(), left_before.data(), kBlock * sizeof(float)) == 0);
  REQUIRE(std::memcmp(right.data(), right_before.data(), kBlock * sizeof(float)) == 0);
  sonare_mixer_destroy(mixer);
}
#endif  // defined(SONARE_WITH_MIXING)

TEST_CASE("mixer meter initializers share their published defaults", "[c_api][mixing][defaults]") {
  SonareMixerAddStripOptions strip{};
  strip.struct_size = 0x7f7f7f7f;
  strip.enable_metering = 127;
  strip.measure_lufs = 127;
  strip.measure_true_peak = 127;
  strip.true_peak_oversample = 127;
  sonare_mixer_add_strip_options_init(&strip);
  CHECK(strip.struct_size == sizeof(SonareMixerAddStripOptions));
  CHECK(strip.enable_metering == 1);
  CHECK(strip.measure_lufs == 1);
  CHECK(strip.measure_true_peak == 1);
  CHECK(strip.true_peak_oversample == 4);

  SonareMixerMasterMeterConfig master{};
  master.struct_size = 0x7f7f7f7f;
  master.enabled = 127;
  master.measure_lufs = 127;
  master.measure_true_peak = 127;
  master.true_peak_oversample = 127;
  sonare_mixer_master_meter_config_init(&master);
  CHECK(master.struct_size == sizeof(SonareMixerMasterMeterConfig));
  CHECK(master.enabled == 1);
  CHECK(master.measure_lufs == strip.measure_lufs);
  CHECK(master.measure_true_peak == strip.measure_true_peak);
  CHECK(master.true_peak_oversample == strip.true_peak_oversample);
}

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

TEST_CASE("sonare_mel_spectrogram exposes a custom Mel range from pure C", "[c_api][features]") {
  const int sr = 22050;
  const int n_fft = 1024;
  const int hop = 256;
  const int n_mels = 40;
  auto samples = generate_sine(440.0f, sr, 1.0f);

  const auto mel_options = [](float fmin, float fmax, int htk) {
    SonareMelSpectrogramOptions options;
    sonare_mel_spectrogram_options_init(&options);
    options.fmin_hz = fmin;
    options.fmax_hz = fmax;
    options.htk = htk;
    return options;
  };
  const auto to_stft_options = [](int htk) {
    SonareMelToStftOptions options;
    sonare_mel_to_stft_options_init(&options);
    options.htk = htk;
    return options;
  };

  SECTION("custom fmin/fmax forward transform round-trips with the inverse API") {
    // The forward transform and the inverse sonare_mel_to_stft share the same
    // fmin/fmax, so a custom-range round-trip is possible from pure C.
    const float fmin = 100.0f;
    const float fmax = 8000.0f;
    SonareMelResult mel = {};
    const SonareMelSpectrogramOptions options = mel_options(fmin, fmax, 0);
    REQUIRE(sonare_mel_spectrogram(samples.data(), samples.size(), sr, n_fft, hop, n_mels, &options,
                                   &mel) == SONARE_OK);
    REQUIRE(mel.power != nullptr);
    REQUIRE(mel.n_mels == n_mels);
    REQUIRE(mel.n_frames > 0);

    SonareInverseResult stft = {};
    const size_t mel_total = static_cast<size_t>(mel.n_mels) * mel.n_frames;
    REQUIRE(sonare_mel_to_stft(mel.power, mel_total, mel.n_mels, mel.n_frames, sr, n_fft, fmin,
                               fmax, nullptr, &stft) == SONARE_OK);
    REQUIRE(stft.data != nullptr);
    REQUIRE(stft.rows == n_fft / 2 + 1);
    REQUIRE(stft.n_frames == mel.n_frames);

    sonare_free_inverse_result(&stft);
    sonare_free_mel_result(&mel);
  }

  SECTION("a non-default range yields a different forward result than the default") {
    SonareMelResult def = {};
    SonareMelResult ranged = {};
    REQUIRE(sonare_mel_spectrogram(samples.data(), samples.size(), sr, n_fft, hop, n_mels, nullptr,
                                   &def) == SONARE_OK);
    const SonareMelSpectrogramOptions options = mel_options(500.0f, 4000.0f, 0);
    REQUIRE(sonare_mel_spectrogram(samples.data(), samples.size(), sr, n_fft, hop, n_mels, &options,
                                   &ranged) == SONARE_OK);
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
    const SonareMelSpectrogramOptions forward_options = mel_options(0.0f, 0.0f, 1);
    REQUIRE(sonare_mel_spectrogram(samples.data(), samples.size(), sr, n_fft, hop, n_mels,
                                   &forward_options, &mel) == SONARE_OK);

    SonareInverseResult slaney = {};
    SonareInverseResult htk = {};
    const size_t mel_total = static_cast<size_t>(mel.n_mels) * mel.n_frames;
    REQUIRE(sonare_mel_to_stft(mel.power, mel_total, mel.n_mels, mel.n_frames, sr, n_fft, 0.0f,
                               0.0f, nullptr, &slaney) == SONARE_OK);
    const SonareMelToStftOptions htk_options = to_stft_options(1);
    REQUIRE(sonare_mel_to_stft(mel.power, mel_total, mel.n_mels, mel.n_frames, sr, n_fft, 0.0f,
                               0.0f, &htk_options, &htk) == SONARE_OK);
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

  SECTION("sonare_mfcc accepts the range and a null out is rejected") {
    SonareMfccOptions options;
    sonare_mfcc_options_init(&options);
    options.fmin_hz = 100.0f;
    options.fmax_hz = 8000.0f;
    SonareMfccResult mfcc = {};
    REQUIRE(sonare_mfcc(samples.data(), samples.size(), sr, n_fft, hop, n_mels, 13, &options,
                        &mfcc) == SONARE_OK);
    REQUIRE(mfcc.coefficients != nullptr);
    REQUIRE(mfcc.n_mfcc == 13);
    sonare_free_mfcc_result(&mfcc);

    // A non-zero lifter is accepted and changes the coefficients.
    options.lifter = 22.0f;
    SonareMfccResult liftered = {};
    REQUIRE(sonare_mfcc(samples.data(), samples.size(), sr, n_fft, hop, n_mels, 13, &options,
                        &liftered) == SONARE_OK);
    REQUIRE(liftered.coefficients != nullptr);
    sonare_free_mfcc_result(&liftered);

    REQUIRE(sonare_mfcc(samples.data(), samples.size(), sr, n_fft, hop, n_mels, 13, nullptr,
                        nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("every inverse entry refuses the band edges the forward transform refuses") {
    SonareMelResult mel = {};
    REQUIRE(sonare_mel_spectrogram(samples.data(), samples.size(), sr, n_fft, hop, n_mels, nullptr,
                                   &mel) == SONARE_OK);
    SonareMfccResult mfcc = {};
    REQUIRE(sonare_mfcc(samples.data(), samples.size(), sr, n_fft, hop, n_mels, 13, nullptr,
                        &mfcc) == SONARE_OK);
    const size_t mel_total = static_cast<size_t>(mel.n_mels) * mel.n_frames;
    const size_t mfcc_total = static_cast<size_t>(mfcc.n_mfcc) * mfcc.n_frames;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<std::pair<float, float>> bad = {
        {-1.0f, 0.0f}, {0.0f, -1.0f}, {nan, 0.0f}, {0.0f, nan}};
    for (const auto& [fmin, fmax] : bad) {
      INFO("fmin " << fmin << " fmax " << fmax);
      SonareMelResult forward = {};
      const SonareMelSpectrogramOptions options = mel_options(fmin, fmax, 0);
      REQUIRE(sonare_mel_spectrogram(samples.data(), samples.size(), sr, n_fft, hop, n_mels,
                                     &options, &forward) == SONARE_ERROR_INVALID_PARAMETER);
      SonareInverseResult stft = {};
      REQUIRE(sonare_mel_to_stft(mel.power, mel_total, mel.n_mels, mel.n_frames, sr, n_fft, fmin,
                                 fmax, nullptr, &stft) == SONARE_ERROR_INVALID_PARAMETER);
      float* audio = nullptr;
      size_t audio_length = 0;
      REQUIRE(sonare_mel_to_audio(mel.power, mel_total, mel.n_mels, mel.n_frames, sr, n_fft, hop,
                                  fmin, fmax, 1, nullptr, &audio,
                                  &audio_length) == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(audio == nullptr);
      REQUIRE(sonare_mfcc_to_audio(mfcc.coefficients, mfcc_total, mfcc.n_mfcc, mfcc.n_frames,
                                   n_mels, sr, n_fft, hop, fmin, fmax, 1, nullptr, &audio,
                                   &audio_length) == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(audio == nullptr);
    }
    SonareInverseResult stft = {};
    REQUIRE(sonare_mel_to_stft(mel.power, mel_total, mel.n_mels, mel.n_frames, sr, n_fft, 0.0f,
                               0.0f, nullptr, &stft) == SONARE_OK);
    sonare_free_inverse_result(&stft);
    sonare_free_mfcc_result(&mfcc);
    sonare_free_mel_result(&mel);
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

TEST_CASE("sonare_mel_delta rejects a non-finite feature instead of differentiating it",
          "[c_api][features]") {
  const std::vector<float> features = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f};
  float* delta = nullptr;
  // Positive control: identical arguments on the clean row succeed, so the
  // rejections below can only come from the non-finite element.
  REQUIRE(sonare_mel_delta(features.data(), 2, 5, 5, &delta) == SONARE_OK);
  sonare_free_floats(delta);

  for (float bad :
       {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    std::vector<float> poisoned = features;
    poisoned[7] = bad;
    delta = nullptr;
    REQUIRE(sonare_mel_delta(poisoned.data(), 2, 5, 5, &delta) == SONARE_ERROR_INVALID_PARAMETER);
    // Without the guard the call returns SONARE_OK and a delta whose whole
    // window around index 7 is NaN.
    REQUIRE(delta == nullptr);
  }
}

TEST_CASE("sonare_piptrack returns equally shaped pitch and magnitude matrices",
          "[c_api][features]") {
  constexpr int sr = 22050;
  auto samples = generate_sine(440.0f, sr, 1.0f);
  size_t length = 0;
  int n_bins = 0;
  int n_frames = 0;
  float* pitches = nullptr;
  float* magnitudes = nullptr;
  REQUIRE(sonare_piptrack(samples.data(), samples.size(), sr, 1024, 256, 100.0f, 1000.0f, 0.1f,
                          &pitches, &magnitudes, &length, &n_bins, &n_frames) == SONARE_OK);
  REQUIRE(n_bins == 513);
  REQUIRE(n_frames > 0);
  REQUIRE(length == static_cast<size_t>(n_bins) * static_cast<size_t>(n_frames));
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

TEST_CASE("sonare_stft zeroes the result before a validate_audio_params rejection",
          "[c_api][features]") {
  // The result must be defined on every exit path rather than left as caller
  // stack garbage when validate_audio_params rejects the (empty) input before
  // the STFT body ever runs.
  SonareStftOptions options;
  sonare_stft_options_init(&options);
  options.db = 1;
  SonareStftResult result;
  result.n_bins = -1;
  result.n_frames = -1;
  result.db = nullptr;
  const std::vector<float> empty;
  REQUIRE(sonare_stft(empty.data(), 0, 22050, 1024, 256, &options, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(result.n_bins == 0);
  REQUIRE(result.n_frames == 0);
  REQUIRE(result.db == nullptr);
  sonare_free_stft_result(&result);
}

TEST_CASE("sonare_stft fills the dB plane only when asked and sizes options", "[c_api][features]") {
  std::vector<float> samples(4096);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = std::sin(0.05f * static_cast<float>(i));
  }
  SonareStftResult plain{};
  REQUIRE(sonare_stft(samples.data(), samples.size(), 22050, 512, 128, nullptr, &plain) ==
          SONARE_OK);
  REQUIRE(plain.magnitude != nullptr);
  REQUIRE(plain.db == nullptr);

  SonareStftOptions options;
  sonare_stft_options_init(&options);
  REQUIRE(options.struct_size == sizeof(SonareStftOptions));
  REQUIRE(options.db == 0);
  SonareStftResult off{};
  REQUIRE(sonare_stft(samples.data(), samples.size(), 22050, 512, 128, &options, &off) ==
          SONARE_OK);
  REQUIRE(off.db == nullptr);

  options.db = 1;
  SonareStftResult with_db{};
  REQUIRE(sonare_stft(samples.data(), samples.size(), 22050, 512, 128, &options, &with_db) ==
          SONARE_OK);
  REQUIRE(with_db.db != nullptr);
  REQUIRE(with_db.n_bins == plain.n_bins);
  REQUIRE(with_db.n_frames == plain.n_frames);
  const size_t total = static_cast<size_t>(plain.n_bins) * static_cast<size_t>(plain.n_frames);
  for (size_t i = 0; i < total; ++i) {
    REQUIRE(with_db.magnitude[i] == plain.magnitude[i]);
    REQUIRE(std::isfinite(with_db.db[i]));
  }

  options.struct_size = sizeof(uint32_t);
  SonareStftResult refused{};
  REQUIRE(sonare_stft(samples.data(), samples.size(), 22050, 512, 128, &options, &refused) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare_free_stft_result(&plain);
  sonare_free_stft_result(&off);
  sonare_free_stft_result(&with_db);
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
  // Lag-by-time, padded: 2n lag rows by n frames.
  REQUIRE(lag.rows == 6);
  REQUIRE(lag.cols == 3);

  SonareSegmentMatrix restored{};
  REQUIRE(sonare_segment_lag_to_recurrence(lag.values, lag.cols, lag.rows, &restored) == SONARE_OK);
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

TEST_CASE("the segment C APIs refuse a feature matrix they cannot order",
          "[c_api][features][numeric]") {
  // These entries take a feature matrix rather than audio, so they never pass
  // through the audio validator every other analysis entry shares. 64 columns,
  // because the neighbour selection has to leave its small-input paths.
  const int rows = 4;
  const int cols = 64;
  std::vector<float> features(static_cast<size_t>(rows) * cols);
  uint32_t state = 0xc0ffeeu;
  for (auto& value : features) {
    state = state * 1664525u + 1013904223u;
    value = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
  }
  const std::vector<float> clean = features;
  features[30] = std::numeric_limits<float>::quiet_NaN();

  SonareSegmentMatrix matrix{};
  SonareSegmentIndices indices{};

  // One poisoned cell used to leave here as 16 non-finite affinity cells, or 126
  // connectivity cells, behind a SONARE_OK.
  CHECK(sonare_segment_recurrence_matrix(features.data(), rows, cols, 0, 1, 0, "cosine", "affinity",
                                         &matrix) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_segment_cross_similarity(features.data(), rows, cols, features.data(), rows, cols, 0,
                                        "cosine", "connectivity",
                                        &matrix) == SONARE_ERROR_INVALID_PARAMETER);
  const std::vector<int> parents = {0, 32, 64};
  CHECK(sonare_segment_subsegment(features.data(), rows, cols, parents.data(), parents.size(), 4,
                                  &indices) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_segment_agglomerative(features.data(), rows, cols, 4, "average", &indices) ==
        SONARE_ERROR_INVALID_PARAMETER);

  // The same four calls with that one cell finite, so the case cannot pass for a
  // guard that refused everything.
  REQUIRE(sonare_segment_recurrence_matrix(clean.data(), rows, cols, 0, 1, 0, "cosine", "affinity",
                                           &matrix) == SONARE_OK);
  REQUIRE(matrix.rows == cols);
  for (int i = 0; i < matrix.rows * matrix.cols; ++i) REQUIRE(std::isfinite(matrix.values[i]));
  sonare_free_segment_matrix(&matrix);
  REQUIRE(sonare_segment_cross_similarity(clean.data(), rows, cols, clean.data(), rows, cols, 0,
                                          "cosine", "connectivity", &matrix) == SONARE_OK);
  sonare_free_segment_matrix(&matrix);
  REQUIRE(sonare_segment_subsegment(clean.data(), rows, cols, parents.data(), parents.size(), 4,
                                    &indices) == SONARE_OK);
  sonare_free_segment_indices(&indices);
  REQUIRE(sonare_segment_agglomerative(clean.data(), rows, cols, 4, "average", &indices) ==
          SONARE_OK);
  sonare_free_segment_indices(&indices);
}

TEST_CASE("sonare_spectral_bandwidth accepts the Minkowski exponent", "[c_api][features]") {
  const auto samples = generate_sine(440.0f, 22050, 1.0f);
  float* p1 = nullptr;
  float* p2 = nullptr;
  size_t count1 = 0;
  size_t count2 = 0;
  SonareSpectralBandwidthOptions options;
  sonare_spectral_bandwidth_options_init(&options);
  REQUIRE(options.p == 2.0f);
  options.p = 1.0f;
  REQUIRE(sonare_spectral_bandwidth(samples.data(), samples.size(), 22050, 1024, 256, &options, &p1,
                                    &count1) == SONARE_OK);
  REQUIRE(sonare_spectral_bandwidth(samples.data(), samples.size(), 22050, 1024, 256, nullptr, &p2,
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

TEST_CASE("sonare_onset_backtrack rejects a non-finite energy envelope", "[c_api][features]") {
  const std::vector<int> events = {6};
  const std::vector<float> energy = {1.0f, 0.8f, 0.5f, 0.2f, 0.6f, 0.9f, 1.2f};
  int* result = nullptr;
  size_t count = 0;

  // Positive control: the clean envelope backtracks to the minimum at index 3.
  REQUIRE(sonare_onset_backtrack(events.data(), events.size(), energy.data(), energy.size(),
                                 &result, &count) == SONARE_OK);
  REQUIRE(count == 1);
  REQUIRE(result[0] == 3);
  sonare_free_ints(result);

  // An empty envelope stays legitimate: nothing to walk back over, no error.
  result = nullptr;
  count = 99;
  REQUIRE(sonare_onset_backtrack(events.data(), events.size(), nullptr, 0, &result, &count) ==
          SONARE_OK);
  REQUIRE(count == 0);

  for (float bad :
       {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    std::vector<float> poisoned = energy;
    poisoned[4] = bad;
    result = nullptr;
    count = 99;
    // Without the guard the descent comparison against the non-finite value is
    // false, so the walk stops one frame short of it and reports index 5 as the
    // preceding minimum with SONARE_OK.
    REQUIRE(sonare_onset_backtrack(events.data(), events.size(), poisoned.data(), poisoned.size(),
                                   &result, &count) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result == nullptr);
    REQUIRE(count == 0);
  }
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

TEST_CASE("C chirp refuses frequencies outside the selected sweep's domain", "[c_api][synthesis]") {
  for (const bool linear : {false, true}) {
    CAPTURE(linear);
    float* out = nullptr;
    size_t out_length = 9;
    const float fmin_hz = linear ? 440.0f : 0.0f;
    const float fmax_hz = linear ? 0.0f : 880.0f;
    const SonareError result =
        sonare_chirp(fmin_hz, fmax_hz, 22050, 0.1f, linear ? 1 : 0, &out, &out_length);
    CHECK(result == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) ==
          std::string(linear ? "fmax_hz" : "fmin_hz") + " must be finite and greater than 0");
    CHECK(out == nullptr);
    CHECK(out_length == 0);
    if (out != nullptr) sonare_free_floats(out);
  }
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

#if defined(SONARE_WITH_MIXING)
namespace {

std::string mixer_refusal_message() { return std::string(sonare_last_error_message()); }

}  // namespace

TEST_CASE("mixer count entries name the NULL argument and zero the out count",
          "[c_api][mixing][refusal]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 512, &mixer) == SONARE_OK);
  size_t count = 99;

  CHECK(sonare_mixer_get_strip_count(nullptr, &count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(count == 0);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_get_strip_count(mixer, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "out_count must not be NULL");
  CHECK(sonare_mixer_get_strip_count(nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");

  count = 99;
  CHECK(sonare_mixer_bus_count(nullptr, &count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(count == 0);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_bus_count(mixer, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "out_count must not be NULL");

  count = 99;
  CHECK(sonare_mixer_vca_group_count(nullptr, &count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(count == 0);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_vca_group_count(mixer, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "out_count must not be NULL");
  sonare_mixer_destroy(mixer);
}

TEST_CASE("mixer bus entries name the refused argument", "[c_api][mixing][refusal]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 512, &mixer) == SONARE_OK);

  CHECK(sonare_mixer_add_bus(nullptr, nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_add_bus(mixer, nullptr, "aux") == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "id must not be NULL");
  REQUIRE(sonare_mixer_add_bus(mixer, "sub", "submix") == SONARE_OK);
  CHECK(sonare_mixer_add_bus(mixer, "sub", "submix") == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "id must not be the id of a bus already in this mixer");
  // The role set is closed; NULL stays the aux default and a misspelling is not one.
  CHECK(sonare_mixer_add_bus(mixer, "group", "subgroup") == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "role must be one of master, aux, submix");
  CHECK(sonare_mixer_add_bus(mixer, "fx", "Aux") == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "role must be one of master, aux, submix");
  REQUIRE(sonare_mixer_add_bus(mixer, "fx", nullptr) == SONARE_OK);
  REQUIRE(sonare_mixer_add_bus(mixer, "group", "aux") == SONARE_OK);

  CHECK(sonare_mixer_remove_bus(nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_remove_bus(mixer, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "id must not be NULL");
  CHECK(sonare_mixer_remove_bus(mixer, "ghost") == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "id must be the id of a bus in this mixer");

  CHECK(sonare_mixer_set_output_bus(nullptr, nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_set_output_bus(mixer, nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "source_id must not be NULL");
  CHECK(sonare_mixer_set_output_bus(mixer, "sub", nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "bus_id must not be NULL");
  sonare_mixer_destroy(mixer);
}

TEST_CASE("mixer VCA group entries name the refused argument", "[c_api][mixing][refusal]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 512, &mixer) == SONARE_OK);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const char* const members[] = {"a", nullptr};

  CHECK(sonare_mixer_add_vca_group(nullptr, nullptr, 0.0f, nullptr, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_add_vca_group(mixer, nullptr, 0.0f, nullptr, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "id must not be NULL");
  CHECK(sonare_mixer_add_vca_group(mixer, "g", nan, nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "gain_db must be finite and in [-3.4028235e38, 3.4028235e38]");
  CHECK(sonare_mixer_add_vca_group(mixer, "g", 0.0f, nullptr, 1) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "members must not be NULL when member_count is not 0");
  CHECK(sonare_mixer_add_vca_group(mixer, "g", 0.0f, members, 2) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "members[1] must not be NULL");
  REQUIRE(sonare_mixer_add_vca_group(mixer, "g", 0.0f, nullptr, 0) == SONARE_OK);
  CHECK(sonare_mixer_add_vca_group(mixer, "g", 0.0f, nullptr, 0) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "id must not be the id of a VCA group already in this mixer");

  CHECK(sonare_mixer_set_vca_group_gain_db(nullptr, nullptr, 0.0f) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_set_vca_group_gain_db(mixer, nullptr, 0.0f) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "id must not be NULL");
  CHECK(sonare_mixer_set_vca_group_gain_db(mixer, "g", nan) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "gain_db must be finite and in [-3.4028235e38, 3.4028235e38]");
  CHECK(sonare_mixer_set_vca_group_gain_db(mixer, "ghost", 0.0f) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "id must be the id of a VCA group in this mixer");

  CHECK(sonare_mixer_set_vca_group_members(nullptr, nullptr, nullptr, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_set_vca_group_members(mixer, nullptr, nullptr, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "id must not be NULL");
  CHECK(sonare_mixer_set_vca_group_members(mixer, "g", nullptr, 1) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "members must not be NULL when member_count is not 0");
  CHECK(sonare_mixer_set_vca_group_members(mixer, "ghost", nullptr, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "id must be the id of a VCA group in this mixer");
  CHECK(sonare_mixer_set_vca_group_members(mixer, "g", members, 2) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "members[1] must not be NULL");

  CHECK(sonare_mixer_remove_vca_group(nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_remove_vca_group(mixer, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "id must not be NULL");
  CHECK(sonare_mixer_remove_vca_group(mixer, "ghost") == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "id must be the id of a VCA group in this mixer");
  sonare_mixer_destroy(mixer);
}

TEST_CASE("mixer strip lookup, scene and query entries name the refused argument",
          "[c_api][mixing][refusal]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 512, &mixer) == SONARE_OK);
  REQUIRE(sonare_mixer_add_strip(mixer, "src", nullptr) != nullptr);

  CHECK(sonare_mixer_strip_at(nullptr, 0) == nullptr);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_strip_at(mixer, 1) == nullptr);
  CHECK(mixer_refusal_message() == "index must be an integer in [0, 0]");
  CHECK(sonare_mixer_strip_by_id(nullptr, nullptr) == nullptr);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_strip_by_id(mixer, nullptr) == nullptr);
  CHECK(mixer_refusal_message() == "id must not be NULL");
  CHECK(sonare_mixer_strip_by_id(mixer, "ghost") == nullptr);
  CHECK(mixer_refusal_message() == "id must be the id of a strip in this mixer");

  CHECK(sonare_mixer_from_scene_json(nullptr, 48000, 512) == nullptr);
  CHECK(mixer_refusal_message() == "document must not be NULL");

  char* json = reinterpret_cast<char*>(0x1);
  CHECK(sonare_mixer_to_scene_json(nullptr, &json) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(json == nullptr);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_to_scene_json(mixer, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "json_out must not be NULL");

  CHECK(sonare_mixer_compile(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_flush_meters(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");

  int value = 99;
  CHECK(sonare_mixer_latency_samples(nullptr, &value) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(value == 0);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_latency_samples(mixer, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "out_latency_samples must not be NULL");
  value = 99;
  CHECK(sonare_mixer_tail_samples(nullptr, &value) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(value == 0);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_tail_samples(mixer, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "out_tail_samples must not be NULL");
  value = 99;
  CHECK(sonare_mixer_max_block_size(nullptr, &value) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(value == 0);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_max_block_size(mixer, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "out_max_block_size must not be NULL");

  json = reinterpret_cast<char*>(0x1);
  CHECK(sonare_mixing_scene_preset_json(nullptr, &json) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(json == nullptr);
  CHECK(mixer_refusal_message() == "preset_name must not be NULL");
  CHECK(sonare_mixing_scene_preset_json("x", nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "json_out must not be NULL");

  SonareMixerMasterMeterConfig config{};
  CHECK(sonare_mixer_configure_master_meter(nullptr, &config) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_configure_master_meter(mixer, &config) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() ==
        "config.struct_size must be set by sonare_mixer_master_meter_config_init");
  sonare_mixer_destroy(mixer);
}

TEST_CASE("mixer process_stereo refuses in header order and names the channel",
          "[c_api][mixing][refusal]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 64, &mixer) == SONARE_OK);
  REQUIRE(sonare_mixer_add_strip(mixer, "src", nullptr) != nullptr);
  std::vector<float> left(64, 0.0f);
  std::vector<float> right(64, 0.0f);
  const float* in_l[] = {left.data()};
  const float* in_r[] = {right.data()};

  CHECK(sonare_mixer_process_stereo(nullptr, nullptr, nullptr, 0, nullptr, nullptr, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "mixer must not be NULL");
  CHECK(sonare_mixer_process_stereo(mixer, nullptr, nullptr, 1, nullptr, nullptr, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "input_left must not be NULL when input_count is not 0");
  CHECK(sonare_mixer_process_stereo(mixer, in_l, nullptr, 1, nullptr, nullptr, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "input_right must not be NULL when input_count is not 0");
  CHECK(sonare_mixer_process_stereo(mixer, in_l, in_r, 2, nullptr, nullptr, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() ==
        "input_count must not exceed the number of strips in this mixer");
  CHECK(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, nullptr, right.data(), 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "output_left must not be NULL");
  CHECK(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, left.data(), nullptr, 0) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "output_right must not be NULL");
  CHECK(sonare_mixer_process_stereo(mixer, in_l, in_r, 1, left.data(), right.data(), 65) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "num_samples must be an integer in [0, 64]");

  const float* null_r[] = {nullptr};
  CHECK(sonare_mixer_process_stereo(mixer, in_l, null_r, 1, left.data(), right.data(), 64) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "input_right[0] must not be NULL");

  std::vector<float> poisoned(64, 0.0f);
  poisoned[3] = std::numeric_limits<float>::quiet_NaN();
  const float* bad_l[] = {poisoned.data()};
  CHECK(sonare_mixer_process_stereo(mixer, bad_l, in_r, 1, left.data(), right.data(), 64) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "input_left[0] must contain only finite samples");
  CHECK(sonare_mixer_drain_tail_stereo(mixer, nullptr, right.data(), 64) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mixer_refusal_message() == "output_left must not be NULL");
  sonare_mixer_destroy(mixer);
}
#endif  // defined(SONARE_WITH_MIXING)
