/// @file sonare_c_mastering_repair_detect_test.cpp
/// @brief Mastering repair detection C API tests.

#include <random>

#include "sonare_c_test_helpers.h"

#ifdef SONARE_WITH_MASTERING
namespace {

constexpr int kSr = 48000;
constexpr size_t kLength = 24000;  // 0.5 s

SonareDeclickConfig default_declick_config() {
  SonareDeclickConfig config{};
  config.threshold = 0.8f;
  config.neighbor_ratio = 4.0f;
  config.max_click_samples = 8;
  config.lpc_order = 20;
  config.residual_ratio = 8.0f;
  return config;
}

SonareDeclipConfig default_declip_config() {
  SonareDeclipConfig config{};
  config.clip_threshold = 0.98f;
  config.lpc_order = 36;
  config.iterations = 2;
  config.lpc_blend = 0.65f;
  return config;
}

SonareDecrackleConfig default_decrackle_config() {
  SonareDecrackleConfig config{};
  config.threshold = 0.4f;
  config.mode = SONARE_DECRACKLE_MODE_MEDIAN;
  config.levels = 4;
  return config;
}

SonareDehumConfig default_dehum_config() {
  SonareDehumConfig config{};
  config.fundamental_hz = 50.0f;
  config.harmonics = 4;
  config.q = 20.0f;
  config.adaptive = 0;
  config.search_range_hz = 2.0f;
  config.adaptation = 0.25f;
  config.frame_size = 2048;
  config.pll_bandwidth = 0.01f;
  return config;
}

SonareDenoiseClassicalConfig default_denoise_config() {
  SonareDenoiseClassicalConfig config{};
  config.mode = SONARE_DENOISE_MODE_LOG_MMSE;
  config.noise_estimator = SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.dd_alpha = 0.98f;
  config.reduction_db = 26.0f;
  config.over_subtraction = 2.0f;
  config.spectral_floor = 0.05f;
  config.noise_estimation_quantile = 0.1f;
  config.speech_presence_gain = 1;
  config.gain_smoothing = 1;
  return config;
}

SonareDereverbClassicalConfig default_dereverb_config() {
  SonareDereverbClassicalConfig config{};
  config.threshold = 0.0f;
  config.attenuation = 1.0f;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.t60_sec = 0.4f;
  config.late_delay_ms = 50.0f;
  config.over_subtraction = 1.0f;
  config.spectral_floor = 0.08f;
  config.wpe_enabled = 0;
  config.wpe_iterations = 2;
  config.wpe_taps = 3;
  config.wpe_strength = 0.7f;
  return config;
}

SonareTrimSilenceConfig default_trim_config() {
  SonareTrimSilenceConfig config{};
  config.threshold = 0.001f;
  config.padding_samples = 0;
  config.mode = SONARE_TRIM_SILENCE_MODE_PEAK;
  config.gate_lufs = -60.0f;
  config.window_ms = 400.0f;
  return config;
}

// A burst of constant magnitude rather than a sine: the trimmer compares |x|
// against a threshold, and a sine's zero crossings would place the detected
// edges a few samples inside the span they are meant to pin.
void fill_burst(std::vector<float>& channel, size_t first, size_t last_exclusive) {
  for (size_t i = first; i < last_exclusive; ++i) {
    channel[i] = (i % 2 == 0) ? 0.5f : -0.5f;
  }
}

std::vector<float> white_noise(size_t length, float amplitude, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> dist(-amplitude, amplitude);
  std::vector<float> samples(length);
  for (auto& sample : samples) sample = dist(rng);
  return samples;
}

}  // namespace

TEST_CASE("sonare_mastering_repair_detect_clicks", "[c_api][mastering]") {
  auto clean = generate_sine(440.0f, kSr, 0.5f);
  for (auto& sample : clean) sample *= 0.3f;
  auto clicked = clean;
  for (size_t i = 0; i < 5; ++i) clicked[2000 + i * 1500] = 1.0f;

  SECTION("separates a clicked buffer from the same buffer without clicks") {
    SonareClickDetection clean_detection{};
    SonareClickDetection clicked_detection{};
    REQUIRE(sonare_mastering_repair_detect_clicks(clean.data(), clean.size(), kSr, nullptr,
                                                  &clean_detection) == SONARE_OK);
    REQUIRE(sonare_mastering_repair_detect_clicks(clicked.data(), clicked.size(), kSr, nullptr,
                                                  &clicked_detection) == SONARE_OK);
    REQUIRE(clicked_detection.count > clean_detection.count);
    REQUIRE(clicked_detection.count > 0);
    REQUIRE(clicked_detection.longest_run_samples > 0);
    REQUIRE(clicked_detection.per_second ==
            Catch::Approx(static_cast<float>(clicked_detection.count) / 0.5f).margin(0.01));
  }

  SECTION("uses library defaults for a NULL config") {
    SonareClickDetection from_null{};
    SonareClickDetection from_defaults{};
    const SonareDeclickConfig config = default_declick_config();
    REQUIRE(sonare_mastering_repair_detect_clicks(clicked.data(), clicked.size(), kSr, nullptr,
                                                  &from_null) == SONARE_OK);
    REQUIRE(sonare_mastering_repair_detect_clicks(clicked.data(), clicked.size(), kSr, &config,
                                                  &from_defaults) == SONARE_OK);
    REQUIRE(from_null.count == from_defaults.count);
    REQUIRE(from_null.rejected == from_defaults.rejected);
    REQUIRE(from_null.longest_run_samples == from_defaults.longest_run_samples);
  }

  SECTION("rejects bad inputs and clears the result it was handed") {
    REQUIRE(sonare_mastering_repair_detect_clicks(clicked.data(), clicked.size(), kSr, nullptr,
                                                  nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    SonareClickDetection detection{};
    detection.count = 7;
    detection.longest_run_samples = 99;
    REQUIRE(sonare_mastering_repair_detect_clicks(nullptr, 0, kSr, nullptr, &detection) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(detection.count == 0);
    REQUIRE(detection.longest_run_samples == 0);

    auto with_nan = clicked;
    with_nan[100] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(sonare_mastering_repair_detect_clicks(with_nan.data(), with_nan.size(), kSr, nullptr,
                                                  &detection) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_mastering_repair_detect_clicks(clicked.data(), clicked.size(), 0, nullptr,
                                                  &detection) == SONARE_ERROR_INVALID_PARAMETER);

    SonareDeclickConfig config = default_declick_config();
    config.lpc_order = -1;
    REQUIRE(sonare_mastering_repair_detect_clicks(clicked.data(), clicked.size(), kSr, &config,
                                                  &detection) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("accepts an lpc_order of zero, which asks for no model rather than none being allowed") {
    SonareDeclickConfig config = default_declick_config();
    config.lpc_order = 0;
    SonareClickDetection detection{};
    REQUIRE(sonare_mastering_repair_detect_clicks(clicked.data(), clicked.size(), kSr, &config,
                                                  &detection) == SONARE_OK);
    REQUIRE(std::isfinite(detection.per_second));
  }
}

TEST_CASE("sonare_mastering_repair_detect_clipping", "[c_api][mastering]") {
  // Three plateaus of known width over a level that no threshold under test
  // reaches, so every counted sample is one the fixture put there.
  std::vector<float> samples(kLength, 0.5f);
  const std::array<size_t, 3> starts{3000, 9000, 15000};
  constexpr size_t kPlateau = 10;
  for (size_t start : starts) {
    for (size_t i = 0; i < kPlateau; ++i) samples[start + i] = 1.0f;
  }

  SECTION("counts the plateaus exactly") {
    SonareClipDetection detection{};
    REQUIRE(sonare_mastering_repair_detect_clipping(samples.data(), samples.size(), kSr, nullptr,
                                                    &detection) == SONARE_OK);
    REQUIRE(detection.sample_count == starts.size() * kPlateau);
    REQUIRE(detection.run_count == starts.size());
    REQUIRE(detection.longest_run_samples == kPlateau);
    REQUIRE(
        detection.sample_fraction ==
        Catch::Approx(static_cast<float>(starts.size() * kPlateau) / static_cast<float>(kLength))
            .epsilon(1e-6));
  }

  SECTION("reads clip_threshold, which a lower setting takes the whole buffer at") {
    SonareDeclipConfig config = default_declip_config();
    config.clip_threshold = 0.4f;
    SonareClipDetection detection{};
    REQUIRE(sonare_mastering_repair_detect_clipping(samples.data(), samples.size(), kSr, &config,
                                                    &detection) == SONARE_OK);
    REQUIRE(detection.sample_count == kLength);
    REQUIRE(detection.run_count == 1);
    REQUIRE(detection.longest_run_samples == kLength);
  }

  SECTION("does not read the sample rate, which it still validates") {
    SonareClipDetection at_44100{};
    SonareClipDetection at_48000{};
    REQUIRE(sonare_mastering_repair_detect_clipping(samples.data(), samples.size(), 44100, nullptr,
                                                    &at_44100) == SONARE_OK);
    REQUIRE(sonare_mastering_repair_detect_clipping(samples.data(), samples.size(), kSr, nullptr,
                                                    &at_48000) == SONARE_OK);
    REQUIRE(at_44100.sample_count == at_48000.sample_count);
    REQUIRE(at_44100.run_count == at_48000.run_count);
    SonareClipDetection detection{};
    REQUIRE(sonare_mastering_repair_detect_clipping(samples.data(), samples.size(), -1, nullptr,
                                                    &detection) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("rejects bad inputs and clears the result it was handed") {
    REQUIRE(sonare_mastering_repair_detect_clipping(samples.data(), samples.size(), kSr, nullptr,
                                                    nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    SonareClipDetection detection{};
    detection.run_count = 5;
    REQUIRE(sonare_mastering_repair_detect_clipping(nullptr, 0, kSr, nullptr, &detection) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(detection.run_count == 0);
  }
}

TEST_CASE("sonare_mastering_repair_detect_crackle", "[c_api][mastering]") {
  auto samples = generate_sine(440.0f, kSr, 0.5f);
  for (auto& sample : samples) sample *= 0.3f;
  for (size_t i = 500; i < kLength; i += 500) samples[i] += 0.9f;

  SECTION("finds the injected deviations and reports a consistent fraction") {
    SonareCrackleDetection detection{};
    REQUIRE(sonare_mastering_repair_detect_crackle(samples.data(), samples.size(), kSr, nullptr,
                                                   &detection) == SONARE_OK);
    REQUIRE(detection.sample_count > 0);
    REQUIRE(detection.sample_fraction ==
            Catch::Approx(static_cast<float>(detection.sample_count) / static_cast<float>(kLength))
                .epsilon(1e-6));
    REQUIRE(detection.per_second ==
            Catch::Approx(static_cast<float>(detection.sample_count) / 0.5f).margin(0.01));
  }

  SECTION("answers the same in either mode, since only the median criterion detects") {
    SonareDecrackleConfig median = default_decrackle_config();
    SonareDecrackleConfig wavelet = default_decrackle_config();
    wavelet.mode = SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE;
    SonareCrackleDetection from_median{};
    SonareCrackleDetection from_wavelet{};
    REQUIRE(sonare_mastering_repair_detect_crackle(samples.data(), samples.size(), kSr, &median,
                                                   &from_median) == SONARE_OK);
    REQUIRE(sonare_mastering_repair_detect_crackle(samples.data(), samples.size(), kSr, &wavelet,
                                                   &from_wavelet) == SONARE_OK);
    REQUIRE(from_median.sample_count == from_wavelet.sample_count);
    REQUIRE(from_median.sample_fraction == from_wavelet.sample_fraction);
    REQUIRE(from_median.per_second == from_wavelet.per_second);
  }

  SECTION("rejects bad inputs and clears the result it was handed") {
    REQUIRE(sonare_mastering_repair_detect_crackle(samples.data(), samples.size(), kSr, nullptr,
                                                   nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    SonareCrackleDetection detection{};
    detection.sample_count = 11;
    REQUIRE(sonare_mastering_repair_detect_crackle(nullptr, 0, kSr, nullptr, &detection) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(detection.sample_count == 0);

    SonareDecrackleConfig config = default_decrackle_config();
    config.levels = 0;
    REQUIRE(sonare_mastering_repair_detect_crackle(samples.data(), samples.size(), kSr, &config,
                                                   &detection) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_mastering_repair_detect_hum", "[c_api][mastering]") {
  auto samples = generate_sine(50.0f, kSr, 0.5f);
  const auto second = generate_sine(100.0f, kSr, 0.5f);
  const auto programme = generate_sine(440.0f, kSr, 0.5f);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = 0.5f * samples[i] + 0.25f * second[i] + 0.1f * programme[i];
  }

  SECTION("tracks the fundamental it was given and its harmonics") {
    SonareHumDetection detection{};
    REQUIRE(sonare_mastering_repair_detect_hum(samples.data(), samples.size(), kSr, nullptr,
                                               &detection) == SONARE_OK);
    REQUIRE(detection.fundamental_hz == Catch::Approx(50.0f).margin(2.0));
    REQUIRE(detection.harmonics >= 1);
    REQUIRE(detection.fundamental_prominence >= 1.0f);
    for (float level : detection.harmonic_dbfs) REQUIRE(std::isfinite(level));
  }

  SECTION("measures the same with adaptive tracking set or clear") {
    SonareDehumConfig off = default_dehum_config();
    SonareDehumConfig on = default_dehum_config();
    on.adaptive = 1;
    SonareHumDetection from_off{};
    SonareHumDetection from_on{};
    REQUIRE(sonare_mastering_repair_detect_hum(samples.data(), samples.size(), kSr, &off,
                                               &from_off) == SONARE_OK);
    REQUIRE(sonare_mastering_repair_detect_hum(samples.data(), samples.size(), kSr, &on,
                                               &from_on) == SONARE_OK);
    REQUIRE(from_off.fundamental_hz == from_on.fundamental_hz);
    REQUIRE(from_off.harmonics == from_on.harmonics);
    REQUIRE(from_off.fundamental_prominence == from_on.fundamental_prominence);
  }

  SECTION("rejects bad inputs and clears the result it was handed") {
    REQUIRE(sonare_mastering_repair_detect_hum(samples.data(), samples.size(), kSr, nullptr,
                                               nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    SonareHumDetection detection{};
    detection.harmonics = 3;
    detection.fundamental_hz = 60.0f;
    REQUIRE(sonare_mastering_repair_detect_hum(nullptr, 0, kSr, nullptr, &detection) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(detection.harmonics == 0);
    REQUIRE(detection.fundamental_hz == 0.0f);

    SonareDehumConfig config = default_dehum_config();
    config.frame_size = 4;
    REQUIRE(sonare_mastering_repair_detect_hum(samples.data(), samples.size(), kSr, &config,
                                               &detection) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_mastering_repair_detect_noise_floor", "[c_api][mastering]") {
  const auto quiet = white_noise(kLength, 0.001f, 1234);
  const auto loud = white_noise(kLength, 0.1f, 1234);

  SECTION("follows the noise level and fills every band") {
    SonareNoiseDetection from_quiet{};
    SonareNoiseDetection from_loud{};
    REQUIRE(sonare_mastering_repair_detect_noise_floor(quiet.data(), quiet.size(), kSr, nullptr,
                                                       &from_quiet) == SONARE_OK);
    REQUIRE(sonare_mastering_repair_detect_noise_floor(loud.data(), loud.size(), kSr, nullptr,
                                                       &from_loud) == SONARE_OK);
    REQUIRE(from_loud.floor_dbfs > from_quiet.floor_dbfs);
    for (float level : from_loud.band_floor_dbfs) REQUIRE(std::isfinite(level));
  }

  SECTION("needs n_fft samples and refuses the one below it") {
    SonareDenoiseClassicalConfig config = default_denoise_config();
    const size_t n_fft = static_cast<size_t>(config.n_fft);
    SonareNoiseDetection detection{};
    REQUIRE(sonare_mastering_repair_detect_noise_floor(loud.data(), n_fft - 1, kSr, &config,
                                                       &detection) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_mastering_repair_detect_noise_floor(loud.data(), n_fft, kSr, &config,
                                                       &detection) == SONARE_OK);
  }

  SECTION("rejects bad inputs and clears the result it was handed") {
    REQUIRE(sonare_mastering_repair_detect_noise_floor(loud.data(), loud.size(), kSr, nullptr,
                                                       nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    SonareNoiseDetection detection{};
    detection.floor_dbfs = -12.0f;
    detection.band_floor_dbfs[0] = -12.0f;
    REQUIRE(sonare_mastering_repair_detect_noise_floor(nullptr, 0, kSr, nullptr, &detection) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(detection.floor_dbfs == 0.0f);
    REQUIRE(detection.band_floor_dbfs[0] == 0.0f);

    SonareDenoiseClassicalConfig config = default_denoise_config();
    config.n_fft = 1000;  // not a power of two
    REQUIRE(sonare_mastering_repair_detect_noise_floor(loud.data(), loud.size(), kSr, &config,
                                                       &detection) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_mastering_repair_detect_reverb", "[c_api][mastering]") {
  auto samples = generate_sine(440.0f, kSr, 0.5f);
  for (auto& sample : samples) sample *= 0.5f;

  SECTION("pads a buffer shorter than n_fft, which the noise-floor detector refuses") {
    SonareDereverbClassicalConfig dereverb = default_dereverb_config();
    const size_t short_length = static_cast<size_t>(dereverb.n_fft) / 2;
    SonareReverbDetection detection{};
    REQUIRE(sonare_mastering_repair_detect_reverb(samples.data(), short_length, kSr, &dereverb,
                                                  &detection) == SONARE_OK);
    SonareDenoiseClassicalConfig denoise = default_denoise_config();
    SonareNoiseDetection refused{};
    REQUIRE(sonare_mastering_repair_detect_noise_floor(samples.data(), short_length, kSr, &denoise,
                                                       &refused) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("measures predictability only when the WPE stage is enabled") {
    SonareDereverbClassicalConfig off = default_dereverb_config();
    SonareDereverbClassicalConfig on = default_dereverb_config();
    on.wpe_enabled = 1;
    SonareReverbDetection from_off{};
    SonareReverbDetection from_on{};
    REQUIRE(sonare_mastering_repair_detect_reverb(samples.data(), samples.size(), kSr, &off,
                                                  &from_off) == SONARE_OK);
    REQUIRE(sonare_mastering_repair_detect_reverb(samples.data(), samples.size(), kSr, &on,
                                                  &from_on) == SONARE_OK);
    REQUIRE(from_off.late_predictability == 0.0f);
    REQUIRE(from_on.late_predictability > 0.0f);
    // The decay statistic is the same measurement either way: WPE adds a term,
    // it does not change what the late-lag ratio reads.
    REQUIRE(from_off.late_decay_ratio_db == from_on.late_decay_ratio_db);
    REQUIRE(std::isfinite(from_off.late_decay_ratio_db));
  }

  SECTION("rejects bad inputs and clears the result it was handed") {
    REQUIRE(sonare_mastering_repair_detect_reverb(samples.data(), samples.size(), kSr, nullptr,
                                                  nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    SonareReverbDetection detection{};
    detection.late_predictability = 0.9f;
    REQUIRE(sonare_mastering_repair_detect_reverb(nullptr, 0, kSr, nullptr, &detection) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(detection.late_predictability == 0.0f);

    SonareDereverbClassicalConfig config = default_dereverb_config();
    config.wpe_taps = 0;
    REQUIRE(sonare_mastering_repair_detect_reverb(samples.data(), samples.size(), kSr, &config,
                                                  &detection) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_mastering_repair_detect_trim_range", "[c_api][mastering]") {
  constexpr size_t kFirst = 4800;
  constexpr size_t kEnd = 12000;
  std::vector<float> samples(kLength, 0.0f);
  fill_burst(samples, kFirst, kEnd);

  SECTION("pins the burst it was given") {
    SonareTrimRange range{};
    REQUIRE(sonare_mastering_repair_detect_trim_range(samples.data(), samples.size(), kSr, nullptr,
                                                      &range) == SONARE_OK);
    REQUIRE(range.first == kFirst);
    REQUIRE(range.last_exclusive == kEnd);
  }

  SECTION("returns the range the repair would cut to, padding included") {
    SonareTrimSilenceConfig config = default_trim_config();
    config.padding_samples = 100;
    SonareTrimRange range{};
    REQUIRE(sonare_mastering_repair_detect_trim_range(samples.data(), samples.size(), kSr, &config,
                                                      &range) == SONARE_OK);
    REQUIRE(range.first == kFirst - 100);
    REQUIRE(range.last_exclusive == kEnd + 100);

    config.padding_samples = kLength;  // past both edges
    REQUIRE(sonare_mastering_repair_detect_trim_range(samples.data(), samples.size(), kSr, &config,
                                                      &range) == SONARE_OK);
    REQUIRE(range.first == 0);
    REQUIRE(range.last_exclusive == kLength);
  }

  SECTION("reports (length, length) for a buffer with no signal") {
    const std::vector<float> silence(kLength, 0.0f);
    SonareTrimRange range{};
    REQUIRE(sonare_mastering_repair_detect_trim_range(silence.data(), silence.size(), kSr, nullptr,
                                                      &range) == SONARE_OK);
    REQUIRE(range.first == kLength);
    REQUIRE(range.last_exclusive == kLength);
  }

  SECTION("rejects bad inputs and clears the result it was handed") {
    REQUIRE(sonare_mastering_repair_detect_trim_range(samples.data(), samples.size(), kSr, nullptr,
                                                      nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    SonareTrimRange range{};
    range.first = 3;
    range.last_exclusive = 9;
    REQUIRE(sonare_mastering_repair_detect_trim_range(nullptr, 0, kSr, nullptr, &range) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(range.first == 0);
    REQUIRE(range.last_exclusive == 0);

    SonareTrimSilenceConfig config = default_trim_config();
    config.padding_samples = static_cast<size_t>(-1) / 2 + 1;
    REQUIRE(sonare_mastering_repair_detect_trim_range(samples.data(), samples.size(), kSr, &config,
                                                      &range) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_mastering_repair_detect_trim_range_stereo", "[c_api][mastering]") {
  constexpr size_t kLeftFirst = 4800;
  constexpr size_t kLeftEnd = 12000;
  constexpr size_t kRightFirst = 14400;
  constexpr size_t kRightEnd = 19200;
  std::vector<float> left(kLength, 0.0f);
  std::vector<float> right(kLength, 0.0f);
  fill_burst(left, kLeftFirst, kLeftEnd);
  fill_burst(right, kRightFirst, kRightEnd);

  SECTION("unions two disjoint spans") {
    SonareTrimRange range{};
    REQUIRE(sonare_mastering_repair_detect_trim_range_stereo(left.data(), right.data(), kLength,
                                                             kSr, nullptr, &range) == SONARE_OK);
    REQUIRE(range.first == kLeftFirst);
    REQUIRE(range.last_exclusive == kRightEnd);
  }

  SECTION("takes the active channel's range when the other carries nothing") {
    const std::vector<float> silence(kLength, 0.0f);
    SonareTrimRange from_silent_left{};
    REQUIRE(sonare_mastering_repair_detect_trim_range_stereo(silence.data(), right.data(), kLength,
                                                             kSr, nullptr,
                                                             &from_silent_left) == SONARE_OK);
    // A silent channel scans to (length, length), so a union taken as
    // min/max of the two ranges would push the tail out to the buffer end.
    REQUIRE(from_silent_left.first == kRightFirst);
    REQUIRE(from_silent_left.last_exclusive == kRightEnd);

    SonareTrimRange from_silent_right{};
    REQUIRE(sonare_mastering_repair_detect_trim_range_stereo(left.data(), silence.data(), kLength,
                                                             kSr, nullptr,
                                                             &from_silent_right) == SONARE_OK);
    REQUIRE(from_silent_right.first == kLeftFirst);
    REQUIRE(from_silent_right.last_exclusive == kLeftEnd);
  }

  SECTION("reports (length, length) for a pair with no signal") {
    const std::vector<float> silence(kLength, 0.0f);
    SonareTrimRange range{};
    REQUIRE(sonare_mastering_repair_detect_trim_range_stereo(
                silence.data(), silence.data(), kLength, kSr, nullptr, &range) == SONARE_OK);
    REQUIRE(range.first == kLength);
    REQUIRE(range.last_exclusive == kLength);
  }

  SECTION("agrees with the mono detector run on each channel") {
    SonareTrimRange left_range{};
    SonareTrimRange right_range{};
    SonareTrimRange union_range{};
    REQUIRE(sonare_mastering_repair_detect_trim_range(left.data(), kLength, kSr, nullptr,
                                                      &left_range) == SONARE_OK);
    REQUIRE(sonare_mastering_repair_detect_trim_range(right.data(), kLength, kSr, nullptr,
                                                      &right_range) == SONARE_OK);
    REQUIRE(sonare_mastering_repair_detect_trim_range_stereo(
                left.data(), right.data(), kLength, kSr, nullptr, &union_range) == SONARE_OK);
    REQUIRE(union_range.first == std::min(left_range.first, right_range.first));
    REQUIRE(union_range.last_exclusive ==
            std::max(left_range.last_exclusive, right_range.last_exclusive));
  }

  SECTION("rejects bad inputs and clears the result it was handed") {
    REQUIRE(sonare_mastering_repair_detect_trim_range_stereo(left.data(), right.data(), kLength,
                                                             kSr, nullptr, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    SonareTrimRange range{};
    range.first = 5;
    REQUIRE(sonare_mastering_repair_detect_trim_range_stereo(nullptr, right.data(), kLength, kSr,
                                                             nullptr, &range) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(range.first == 0);
    REQUIRE(sonare_mastering_repair_detect_trim_range_stereo(left.data(), nullptr, kLength, kSr,
                                                             nullptr, &range) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}
#endif  // SONARE_WITH_MASTERING
