/// @file sonare_c_mastering_repair_test.cpp
/// @brief Mastering repair and dynamics C API tests.

#include <algorithm>
#include <string>

#include "c_api/sonare_c_last_refusal.h"
#include "sonare_c_test_helpers.h"
#include "util/parameter_checks.h"

#ifdef SONARE_WITH_MASTERING
TEST_CASE("sonare_mastering_repair_declick", "[c_api][mastering]") {
  const int sr = 48000;
  auto samples = generate_sine(440.0f, sr, 0.5f);
  for (auto& s : samples) s *= 0.3f;
  // Inject a few impulsive clicks (sample-wide spikes).
  for (size_t i = 0; i < 5; ++i) {
    samples[2000 + i * 1500] = 1.0f;
  }

  SECTION("returns same-length cleaned buffer with NULL config (defaults)") {
    SonareDeclickResult result{};
    REQUIRE(sonare_mastering_repair_declick(samples.data(), samples.size(), sr, nullptr, &result) ==
            SONARE_OK);
    REQUIRE(result.samples != nullptr);
    REQUIRE(result.length == samples.size());
    // Output should be finite and not silent (the wrapper preserves the signal).
    REQUIRE(std::isfinite(max_abs(result.samples, result.length)));
    REQUIRE(max_abs(result.samples, result.length) > 0.1f);
    sonare_free_declick_result(&result);
  }

  SECTION("accepts explicit config") {
    SonareDeclickConfig config;
    sonare_declick_config_init(&config);
    config.threshold = 0.8f;
    config.neighbor_ratio = 4.0f;
    config.max_click_samples = 8;
    config.lpc_order = 20;
    config.residual_ratio = 8.0f;

    SonareDeclickResult result{};
    REQUIRE(sonare_mastering_repair_declick(samples.data(), samples.size(), sr, &config, &result) ==
            SONARE_OK);
    REQUIRE(result.length == samples.size());
    sonare_free_declick_result(&result);
  }

  SECTION("rejects null result / bad inputs") {
    REQUIRE(sonare_mastering_repair_declick(samples.data(), samples.size(), sr, nullptr, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    SonareDeclickResult result{};
    result.samples = non_null_sentinel_float_ptr();
    result.length = 123;
    REQUIRE(sonare_mastering_repair_declick(nullptr, 0, sr, nullptr, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.samples == nullptr);
    REQUIRE(result.length == 0);
  }

  SECTION("invalid config returns invalid parameter and clears output") {
    SonareDeclickConfig config;
    sonare_declick_config_init(&config);
    config.threshold = 0.0f;
    config.neighbor_ratio = 4.0f;
    config.max_click_samples = 8;
    config.lpc_order = 20;
    config.residual_ratio = 8.0f;
    SonareDeclickResult result{};
    result.samples = non_null_sentinel_float_ptr();
    result.length = 123;
    REQUIRE(sonare_mastering_repair_declick(samples.data(), samples.size(), sr, &config, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.samples == nullptr);
    REQUIRE(result.length == 0);
  }
}

TEST_CASE("sonare_mastering_repair_declick_stereo", "[c_api][mastering]") {
  const int sr = 48000;
  // The two channels carry different tones and their clicks sit at disjoint
  // positions. Identical channels would let an implementation that declicks one
  // and copies it satisfy every assertion below.
  auto left = generate_sine(440.0f, sr, 0.5f);
  auto right = generate_sine(660.0f, sr, 0.5f);
  // 0.2, not 0.3: the detector wants a run to stand neighbor_ratio (4.0) above
  // its neighbours, so a 1.0 spike landing near a tone's own peak clears the
  // ratio at 0.2 and misses it at 0.3, making detection depend on the phase the
  // click happens to land on.
  for (auto& s : left) s *= 0.2f;
  for (auto& s : right) s *= 0.2f;
  const size_t kLeftClick = 4000;
  const size_t kRightClick = 12000;
  left[kLeftClick] = 1.0f;
  right[kRightClick] = 1.0f;

  SECTION("repairs the union of both channels' runs") {
    SonareDeclickStereoResult out{};
    REQUIRE(sonare_mastering_repair_declick_stereo(left.data(), right.data(), left.size(), sr,
                                                   nullptr, &out) == SONARE_OK);
    REQUIRE(out.left != nullptr);
    REQUIRE(out.right != nullptr);
    REQUIRE(out.length == left.size());

    // The fixture only witnesses linking while each channel detects its own
    // click and nothing else. A channel that detects neither still satisfies
    // "repaired more runs than it detected" and reads as linked.
    CHECK(out.left_report.detected.count == 1);
    CHECK(out.right_report.detected.count == 1);

    // Each channel's own click is gone.
    CHECK(std::abs(out.left[kLeftClick]) < 0.5f);
    CHECK(std::abs(out.right[kRightClick]) < 0.5f);

    // The discriminating property: a run only one channel detected is repaired
    // in both, so each report attributes runs to the other channel's detection.
    // A per-channel implementation reports zero here and still passes the two
    // checks above.
    CHECK(out.left_report.linked_runs > 0);
    CHECK(out.right_report.linked_runs > 0);
    CHECK(out.left_report.repaired_runs > out.left_report.detected.count);
    CHECK(out.right_report.repaired_runs > out.right_report.detected.count);

    // The two outputs are not the same buffer restated.
    CHECK(out.left[kRightClick] != out.right[kRightClick]);

    sonare_free_declick_stereo_result(&out);
  }

  SECTION("linking changes the output against the mono entry point") {
    SonareDeclickStereoResult stereo{};
    REQUIRE(sonare_mastering_repair_declick_stereo(left.data(), right.data(), left.size(), sr,
                                                   nullptr, &stereo) == SONARE_OK);
    SonareDeclickResult mono{};
    REQUIRE(sonare_mastering_repair_declick(left.data(), left.size(), sr, nullptr, &mono) ==
            SONARE_OK);
    REQUIRE(mono.length == stereo.length);

    // The mono pass never sees the right channel's click, so it leaves the left
    // channel untouched there while the stereo pass repairs it. Without this the
    // stereo entry could be the mono one run twice.
    CHECK(stereo.left[kRightClick] != mono.samples[kRightClick]);
    CHECK(stereo.left[kLeftClick] == Catch::Approx(mono.samples[kLeftClick]).margin(1e-6));

    sonare_free_declick_result(&mono);
    sonare_free_declick_stereo_result(&stereo);
  }

  SECTION("the releaser empties the result, and a second release is a no-op") {
    SonareDeclickStereoResult out{};
    REQUIRE(sonare_mastering_repair_declick_stereo(left.data(), right.data(), left.size(), sr,
                                                   nullptr, &out) == SONARE_OK);
    sonare_free_declick_stereo_result(&out);
    CHECK(out.left == nullptr);
    CHECK(out.right == nullptr);
    CHECK(out.length == 0);
    sonare_free_declick_stereo_result(&out);
    sonare_free_declick_stereo_result(nullptr);
  }

  SECTION("clears the result before refusing") {
    SonareDeclickStereoResult out{};
    out.left = non_null_sentinel_float_ptr();
    out.length = 123;
    out.left_report.repaired_runs = 99;
    REQUIRE(sonare_mastering_repair_declick_stereo(nullptr, right.data(), left.size(), sr, nullptr,
                                                   &out) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(out.left == nullptr);
    CHECK(out.right == nullptr);
    CHECK(out.length == 0);
    CHECK(out.left_report.repaired_runs == 0);
  }

  SECTION("refuses a null result") {
    REQUIRE(sonare_mastering_repair_declick_stereo(left.data(), right.data(), left.size(), sr,
                                                   nullptr,
                                                   nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_mastering_repair_denoise_classical", "[c_api][mastering]") {
  const int sr = 22050;
  auto signal = generate_sine(440.0f, sr, 1.0f);
  // Add white noise.
  std::vector<float> noisy(signal.size());
  uint32_t state = 1u;
  for (size_t i = 0; i < signal.size(); ++i) {
    state = state * 1664525u + 1013904223u;
    float u = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);  // [0,1)
    float n = (u - 0.5f) * 0.4f;
    noisy[i] = 0.5f * signal[i] + n;
  }

  SECTION("LogMMSE default config reduces noise floor") {
    SonareDenoiseResult result{};
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, nullptr,
                                                      &result) == SONARE_OK);
    REQUIRE(result.samples != nullptr);
    REQUIRE(result.length == noisy.size());
    sonare_free_denoise_result(&result);
  }

  SECTION("Berouti SpectralSubtraction config runs") {
    SonareDenoiseClassicalConfig config;
    sonare_denoise_classical_config_init(&config);
    config.mode = SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION;
    config.noise_estimator = SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE;
    config.n_fft = 1024;
    config.hop_length = 256;
    config.dd_alpha = 0.98f;
    config.reduction_db = 26.0f;
    config.over_subtraction = 2.0f;
    config.spectral_floor = 0.05f;
    config.noise_estimation_quantile = 0.1f;
    config.speech_presence_gain = 0;
    config.gain_smoothing = 1;

    SonareDenoiseResult result{};
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, &config,
                                                      &result) == SONARE_OK);
    REQUIRE(result.length == noisy.size());
    sonare_free_denoise_result(&result);
  }

  SECTION("rejects non-power-of-two n_fft and bad hop") {
    SonareDenoiseClassicalConfig config;
    sonare_denoise_classical_config_init(&config);
    config.mode = SONARE_DENOISE_MODE_LOG_MMSE;
    config.noise_estimator = SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE;
    config.n_fft = 1500;  // not a power of two
    config.hop_length = 256;
    SonareDenoiseResult result{};
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, &config,
                                                      &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.samples == nullptr);
    REQUIRE(result.length == 0);

    config.n_fft = 1024;
    config.hop_length = 0;
    result = {};
    result.samples = non_null_sentinel_float_ptr();
    result.length = 123;
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, &config,
                                                      &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.samples == nullptr);
    REQUIRE(result.length == 0);
  }

  SECTION("rejects unknown mode and noise estimator enums") {
    SonareDenoiseClassicalConfig config;
    sonare_denoise_classical_config_init(&config);
    config.mode = 999;
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
    SonareDenoiseResult result{};
    result.samples = non_null_sentinel_float_ptr();
    result.length = 123;
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, &config,
                                                      &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.samples == nullptr);
    REQUIRE(result.length == 0);

    config.mode = SONARE_DENOISE_MODE_LOG_MMSE;
    config.noise_estimator = 999;
    result = {};
    result.samples = non_null_sentinel_float_ptr();
    result.length = 123;
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, &config,
                                                      &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.samples == nullptr);
    REQUIRE(result.length == 0);
  }
}

TEST_CASE("sonare_mastering_repair_denoise_classical_stereo", "[c_api][mastering]") {
  const int sr = 22050;
  auto signal = generate_sine(440.0f, sr, 1.0f);
  std::vector<float> noisy(signal.size());
  std::vector<float> silence(signal.size(), 0.0f);
  uint32_t state = 1u;
  for (size_t i = 0; i < signal.size(); ++i) {
    state = state * 1664525u + 1013904223u;
    const float u = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    noisy[i] = 0.5f * signal[i] + (u - 0.5f) * 0.4f;
  }

  SECTION("one linked mask drives both channels") {
    SonareDenoiseStereoResult out{};
    REQUIRE(sonare_mastering_repair_denoise_classical_stereo(
                noisy.data(), noisy.data(), noisy.size(), sr, nullptr, &out) == SONARE_OK);
    REQUIRE(out.length == noisy.size());

    // The mask is one real number per cell scaling both channels, so identical
    // inputs must come back bit-identical. A per-channel mask -- the shape the
    // four sibling entries use -- fails this without needing a crafted fixture.
    for (size_t i = 0; i < out.length; ++i) {
      REQUIRE(out.left[i] == out.right[i]);
    }
    // Pinned so the equality above cannot be satisfied by two silent buffers.
    REQUIRE(max_abs(out.left, out.length) > 0.01f);

    sonare_free_denoise_stereo_result(&out);
  }

  SECTION("the reported floor is the pair's, not a channel's") {
    // The estimator runs on the channel-summed power, so duplicating a channel
    // doubles it. Measured against a silent partner, whose summed power is the
    // single channel's, that is 10*log10(2) -- the power ratio is exactly two,
    // the dB conversion is float32.
    SonareDenoiseStereoResult doubled{};
    SonareDenoiseStereoResult single{};
    REQUIRE(sonare_mastering_repair_denoise_classical_stereo(
                noisy.data(), noisy.data(), noisy.size(), sr, nullptr, &doubled) == SONARE_OK);
    REQUIRE(sonare_mastering_repair_denoise_classical_stereo(
                noisy.data(), silence.data(), noisy.size(), sr, nullptr, &single) == SONARE_OK);

    // Pinned before the difference, because two floors agreeing on a sentinel
    // would satisfy any relation between them while measuring nothing.
    REQUIRE(std::isfinite(single.report.detected.floor_dbfs));
    REQUIRE(single.report.detected.floor_dbfs < 0.0f);
    REQUIRE(single.report.detected.floor_dbfs > sonare::constants::kFloorDb);
    // Measured 3.010299683 against the 3.010300159 that 10*log10(2) is, so the
    // margin is two hundred times the observed error and thirty thousand times
    // tighter than the shift it is there to catch.
    REQUIRE(doubled.report.detected.floor_dbfs - single.report.detected.floor_dbfs ==
            Catch::Approx(10.0f * sonare::constants::kLog10Of2).margin(1e-4));

    sonare_free_denoise_stereo_result(&doubled);
    sonare_free_denoise_stereo_result(&single);
  }

  SECTION("every band slot is measured, not just the first") {
    SonareDenoiseStereoResult out{};
    REQUIRE(sonare_mastering_repair_denoise_classical_stereo(
                noisy.data(), noisy.data(), noisy.size(), sr, nullptr, &out) == SONARE_OK);

    // A mirror carrying only band_floor_dbfs[0] passes any check written against
    // the first entry alone, so the whole array has to say something. A band no
    // bin reaches is NaN; every other band is a finite level.
    SonareDenoiseClassicalConfig defaults;
    sonare_denoise_classical_config_init(&defaults);
    int bins[SONARE_REPAIR_NOISE_BAND_EDGE_COUNT] = {};
    REQUIRE(sonare_mastering_repair_noise_band_bins(defaults.n_fft, sr, bins) == SONARE_OK);
    int measured = 0;
    float first_measured = 0.0f;
    bool any_differs = false;
    for (int b = 0; b < SONARE_REPAIR_NOISE_BAND_COUNT; ++b) {
      const float band = out.report.detected.band_floor_dbfs[b];
      if (bins[b] == bins[b + 1]) {
        REQUIRE(std::isnan(band));
        continue;
      }
      REQUIRE(std::isfinite(band));
      if (measured++ == 0) first_measured = band;
      if (band != first_measured) any_differs = true;
    }
    REQUIRE(measured > 1);
    REQUIRE(any_differs);

    sonare_free_denoise_stereo_result(&out);
  }

  SECTION("the gain floor is observable in the modes that have one") {
    SonareDenoiseClassicalConfig config;
    sonare_denoise_classical_config_init(&config);
    config.mode = SONARE_DENOISE_MODE_LOG_MMSE;
    config.noise_estimator = SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE;
    config.n_fft = 1024;
    config.hop_length = 256;
    config.dd_alpha = 0.98f;
    config.reduction_db = 12.0f;
    config.over_subtraction = 2.0f;
    config.spectral_floor = 0.05f;
    config.noise_estimation_quantile = 0.1f;
    config.speech_presence_gain = 1;
    config.gain_smoothing = 1;

    SonareDenoiseStereoResult out{};
    REQUIRE(sonare_mastering_repair_denoise_classical_stereo(
                noisy.data(), noisy.data(), noisy.size(), sr, &config, &out) == SONARE_OK);
    REQUIRE(out.report.mean_reduction_db > 0.0f);
    REQUIRE(out.report.max_reduction_db >= out.report.mean_reduction_db);
    REQUIRE(out.report.max_reduction_db <= config.reduction_db + 0.01f);
    REQUIRE(out.report.floor_limited_fraction > 0.0f);
    sonare_free_denoise_stereo_result(&out);

    // SpectralSubtraction floors on spectral_floor instead, so its zero here is
    // the mode answering rather than a measurement. Asserting it alone would
    // pass against a build that never populated the field at all, which is why
    // the nonzero above is checked first on the same fixture.
    config.mode = SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION;
    SonareDenoiseStereoResult berouti{};
    REQUIRE(sonare_mastering_repair_denoise_classical_stereo(
                noisy.data(), noisy.data(), noisy.size(), sr, &config, &berouti) == SONARE_OK);
    REQUIRE(berouti.report.floor_limited_fraction == 0.0f);
    REQUIRE(berouti.report.mean_reduction_db > 0.0f);
    sonare_free_denoise_stereo_result(&berouti);
  }

  SECTION("an input shorter than n_fft is padded rather than refused") {
    // As the dereverb pair pads one.
    std::vector<float> tiny(512, 0.25f);
    SonareDenoiseStereoResult out{};
    REQUIRE(sonare_mastering_repair_denoise_classical_stereo(tiny.data(), tiny.data(), tiny.size(),
                                                             sr, nullptr, &out) == SONARE_OK);
    REQUIRE(out.length == tiny.size());
    for (size_t i = 0; i < out.length; ++i) REQUIRE(std::isfinite(out.left[i]));
    sonare_free_denoise_stereo_result(&out);
  }

  SECTION("clears the result before refusing") {
    SonareDenoiseStereoResult out{};
    out.left = non_null_sentinel_float_ptr();
    out.length = 123;
    out.report.mean_reduction_db = 99.0f;
    REQUIRE(sonare_mastering_repair_denoise_classical_stereo(nullptr, noisy.data(), noisy.size(),
                                                             sr, nullptr, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(out.left == nullptr);
    CHECK(out.right == nullptr);
    CHECK(out.length == 0);
    CHECK(out.report.mean_reduction_db == 0.0f);

    REQUIRE(sonare_mastering_repair_denoise_classical_stereo(noisy.data(), noisy.data(),
                                                             noisy.size(), 0, nullptr, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_mastering_repair_denoise_classical_stereo(noisy.data(), noisy.data(),
                                                             noisy.size(), sr, nullptr, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);

    SonareDenoiseClassicalConfig bad;

    sonare_denoise_classical_config_init(&bad);
    bad.n_fft = 1000;  // not a power of two
    bad.hop_length = 256;
    REQUIRE(sonare_mastering_repair_denoise_classical_stereo(noisy.data(), noisy.data(),
                                                             noisy.size(), sr, &bad, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);
    bad.n_fft = 1024;
    bad.hop_length = 0;
    REQUIRE(sonare_mastering_repair_denoise_classical_stereo(noisy.data(), noisy.data(),
                                                             noisy.size(), sr, &bad, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

namespace {

// Runs the channel-linked entry and copies its channels into caller planes, so a
// case can compare against the mono and stereo entries plane for plane. The
// report is always copied, which is zeroed by a refusal; planes only on success.
template <typename LinkedResult, typename Config, typename Report, typename Fn, typename Free>
SonareError linked_into(Fn run, Free release, const float* const* channels, size_t num_channels,
                        size_t length, int sample_rate, const Config* config,
                        float* const* out_channels, Report* out_report) {
  LinkedResult result{};
  const SonareError error = run(channels, num_channels, length, sample_rate, config, &result);
  if (error == SONARE_OK) {
    REQUIRE(result.num_channels == num_channels);
    REQUIRE(result.length == length);
    for (size_t c = 0; c < num_channels; ++c) {
      std::copy(result.channels[c], result.channels[c] + length, out_channels[c]);
    }
  }
  *out_report = result.report;
  release(&result);
  return error;
}

SonareError denoise_linked_into(const float* const* channels, size_t num_channels, size_t length,
                                int sample_rate, const SonareDenoiseClassicalConfig* config,
                                float* const* out_channels, SonareDenoiseReport* out_report) {
  return linked_into<SonareDenoiseLinkedResult>(
      sonare_mastering_repair_denoise_classical_linked, sonare_free_denoise_linked_result, channels,
      num_channels, length, sample_rate, config, out_channels, out_report);
}

SonareError dereverb_linked_into(const float* const* channels, size_t num_channels, size_t length,
                                 int sample_rate, const SonareDereverbClassicalConfig* config,
                                 float* const* out_channels, SonareDereverbReport* out_report) {
  return linked_into<SonareDereverbLinkedResult>(
      sonare_mastering_repair_dereverb_classical_linked, sonare_free_dereverb_linked_result,
      channels, num_channels, length, sample_rate, config, out_channels, out_report);
}

}  // namespace

TEST_CASE("sonare_mastering_repair_linked_result owns its planes and clears on refusal",
          "[c_api][mastering]") {
  const std::vector<float> tone = generate_sine(440.0f, 22050, 0.5f);
  const float* in[2] = {tone.data(), tone.data()};

  SonareDenoiseLinkedResult denoise{};
  REQUIRE(sonare_mastering_repair_denoise_classical_linked(in, 2, tone.size(), 22050, nullptr,
                                                           &denoise) == SONARE_OK);
  REQUIRE(denoise.channels != nullptr);
  CHECK(denoise.num_channels == 2);
  CHECK(denoise.length == tone.size());
  CHECK(denoise.channels[0] != denoise.channels[1]);
  sonare_free_denoise_linked_result(&denoise);
  CHECK(denoise.channels == nullptr);
  CHECK(denoise.num_channels == 0);
  sonare_free_denoise_linked_result(&denoise);
  sonare_free_denoise_linked_result(nullptr);

  SonareDereverbLinkedResult dereverb{};
  REQUIRE(sonare_mastering_repair_dereverb_classical_linked(in, 2, tone.size(), 22050, nullptr,
                                                            &dereverb) == SONARE_OK);
  REQUIRE(dereverb.channels != nullptr);
  CHECK(dereverb.num_channels == 2);
  sonare_free_dereverb_linked_result(&dereverb);
  CHECK(dereverb.channels == nullptr);

  SonareDenoiseLinkedResult refused{};
  refused.num_channels = 9;
  CHECK(sonare_mastering_repair_denoise_classical_linked(
            in, 0, tone.size(), 22050, nullptr, &refused) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(refused.channels == nullptr);
  CHECK(refused.num_channels == 0);
  CHECK(sonare_mastering_repair_denoise_classical_linked(
            in, 2, tone.size(), 22050, nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_mastering_repair_dereverb_classical_linked(
            in, 2, tone.size(), 22050, nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare_mastering_repair_denoise_classical_linked", "[c_api][mastering]") {
  const int sr = 22050;
  const auto tone = generate_sine(440.0f, sr, 1.0f);
  const auto other_tone = generate_sine(660.0f, sr, 1.0f);
  std::vector<float> noisy(tone.size());
  std::vector<float> other(tone.size());
  const std::vector<float> silence(tone.size(), 0.0f);
  uint32_t state = 1u;
  for (size_t i = 0; i < tone.size(); ++i) {
    state = state * 1664525u + 1013904223u;
    const float u = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    noisy[i] = 0.5f * tone[i] + (u - 0.5f) * 0.4f;
    state = state * 1664525u + 1013904223u;
    const float v = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    other[i] = 0.4f * other_tone[i] + (v - 0.5f) * 0.3f;
  }
  const size_t length = noisy.size();

  SECTION("one channel reproduces the mono entry bit for bit") {
    // The core promises the sum of one channel is that channel, so this is not a
    // smoke test: a linked path that normalized by the channel count, or summed
    // into a differently-ordered accumulator, would land near the mono result
    // without matching it.
    std::vector<float> plane(length, 0.0f);
    const float* in[1] = {noisy.data()};
    float* outs[1] = {plane.data()};
    SonareDenoiseReport report{};
    REQUIRE(denoise_linked_into(in, 1, length, sr, nullptr, outs, &report) == SONARE_OK);

    SonareDenoiseResult mono{};
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), length, sr, nullptr, &mono) ==
            SONARE_OK);
    REQUIRE(mono.length == length);
    for (size_t i = 0; i < length; ++i) {
      REQUIRE(plane[i] == mono.samples[i]);
    }
    // Pinned so the equality above cannot be satisfied by two silent buffers.
    REQUIRE(max_abs(plane.data(), length) > 0.01f);
    sonare_free_denoise_result(&mono);
  }

  SECTION("two channels reproduce the stereo entry, plane for plane") {
    std::vector<float> first(length, 0.0f);
    std::vector<float> second(length, 0.0f);
    const float* in[2] = {noisy.data(), other.data()};
    float* outs[2] = {first.data(), second.data()};
    SonareDenoiseReport report{};
    REQUIRE(denoise_linked_into(in, 2, length, sr, nullptr, outs, &report) == SONARE_OK);

    SonareDenoiseStereoResult pair{};
    REQUIRE(sonare_mastering_repair_denoise_classical_stereo(noisy.data(), other.data(), length, sr,
                                                             nullptr, &pair) == SONARE_OK);
    // Distinct material in the two channels, so a plane pair written in the wrong
    // order fails here rather than comparing equal to itself.
    REQUIRE(pair.length == length);
    for (size_t i = 0; i < length; ++i) {
      REQUIRE(first[i] == pair.left[i]);
      REQUIRE(second[i] == pair.right[i]);
    }
    REQUIRE(report.detected.floor_dbfs == pair.report.detected.floor_dbfs);
    REQUIRE(max_abs(first.data(), length) > 0.01f);
    REQUIRE(max_abs(second.data(), length) > 0.01f);
    sonare_free_denoise_stereo_result(&pair);
  }

  SECTION("the reported floor is the whole set's, not a pair's") {
    // The stereo entry can only ever witness the doubling. Three identical
    // channels triple the summed power, which is 10*log10(3) rather than the
    // 10*log10(2) a pair moves by -- the one figure an implementation that
    // delegated to the stereo path for everything above two channels cannot
    // produce.
    std::vector<float> a(length, 0.0f);
    std::vector<float> b(length, 0.0f);
    std::vector<float> c(length, 0.0f);
    const float* three_in[3] = {noisy.data(), noisy.data(), noisy.data()};
    float* three_out[3] = {a.data(), b.data(), c.data()};
    SonareDenoiseReport tripled{};
    REQUIRE(denoise_linked_into(three_in, 3, length, sr, nullptr, three_out, &tripled) ==
            SONARE_OK);

    std::vector<float> single_plane(length, 0.0f);
    const float* one_in[1] = {noisy.data()};
    float* one_out[1] = {single_plane.data()};
    SonareDenoiseReport single{};
    REQUIRE(denoise_linked_into(one_in, 1, length, sr, nullptr, one_out, &single) == SONARE_OK);

    // Pinned before the difference, because two floors agreeing on a sentinel
    // would satisfy any relation between them while measuring nothing.
    REQUIRE(std::isfinite(single.detected.floor_dbfs));
    REQUIRE(single.detected.floor_dbfs < 0.0f);
    REQUIRE(single.detected.floor_dbfs > sonare::constants::kFloorDb);
    REQUIRE(tripled.detected.floor_dbfs - single.detected.floor_dbfs ==
            Catch::Approx(10.0f * std::log10(3.0f)).margin(1e-4));

    // One mask over the set, so three copies of one channel come back identical.
    for (size_t i = 0; i < length; ++i) {
      REQUIRE(a[i] == b[i]);
      REQUIRE(b[i] == c[i]);
    }
    REQUIRE(max_abs(a.data(), length) > 0.01f);
  }

  SECTION("each output plane carries its own input's channel") {
    // A silent channel contributes exactly zero to the summed power and float
    // addition leaves the rest untouched, so moving the material between planes
    // leaves the mask bit-identical and the outputs must simply follow it. A
    // plane written from the wrong index survives every check above, where the
    // channels are either identical or compared against a sibling entry that
    // could be wrong in the same direction.
    std::vector<float> a0(length, 0.0f);
    std::vector<float> a1(length, 0.0f);
    std::vector<float> a2(length, 0.0f);
    const float* first_in[3] = {noisy.data(), silence.data(), silence.data()};
    float* first_out[3] = {a0.data(), a1.data(), a2.data()};
    SonareDenoiseReport first_report{};
    REQUIRE(denoise_linked_into(first_in, 3, length, sr, nullptr, first_out, &first_report) ==
            SONARE_OK);

    std::vector<float> b0(length, 0.0f);
    std::vector<float> b1(length, 0.0f);
    std::vector<float> b2(length, 0.0f);
    const float* middle_in[3] = {silence.data(), noisy.data(), silence.data()};
    float* middle_out[3] = {b0.data(), b1.data(), b2.data()};
    SonareDenoiseReport middle_report{};
    REQUIRE(denoise_linked_into(middle_in, 3, length, sr, nullptr, middle_out, &middle_report) ==
            SONARE_OK);

    REQUIRE(first_report.detected.floor_dbfs == middle_report.detected.floor_dbfs);
    REQUIRE(max_abs(a0.data(), length) > 0.01f);
    for (size_t i = 0; i < length; ++i) {
      REQUIRE(a0[i] == b1[i]);
      REQUIRE(a1[i] == 0.0f);
      REQUIRE(a2[i] == 0.0f);
      REQUIRE(b0[i] == 0.0f);
      REQUIRE(b2[i] == 0.0f);
    }
  }

  SECTION("pads an input shorter than n_fft and fills every caller plane") {
    // As the dereverb entry pads one.
    const std::vector<float> tiny(512, 0.25f);
    constexpr float kSentinel = 7.5f;
    std::vector<float> plane(tiny.size(), kSentinel);
    const float* in[1] = {tiny.data()};
    float* outs[1] = {plane.data()};
    SonareDenoiseReport report{};
    REQUIRE(denoise_linked_into(in, 1, tiny.size(), sr, nullptr, outs, &report) == SONARE_OK);
    for (float sample : plane) {
      REQUIRE(std::isfinite(sample));
      REQUIRE(sample != kSentinel);
    }
  }

  SECTION("refuses a null channel anywhere in the set, not just the first") {
    std::vector<float> a(length, 0.0f);
    std::vector<float> b(length, 0.0f);
    std::vector<float> c(length, 0.0f);
    float* outs[3] = {a.data(), b.data(), c.data()};
    SonareDenoiseReport report{};

    const float* second_null[3] = {noisy.data(), nullptr, noisy.data()};
    REQUIRE(denoise_linked_into(second_null, 3, length, sr, nullptr, outs, &report) ==
            SONARE_ERROR_INVALID_PARAMETER);
    const float* last_null[3] = {noisy.data(), noisy.data(), nullptr};
    REQUIRE(denoise_linked_into(last_null, 3, length, sr, nullptr, outs, &report) ==
            SONARE_ERROR_INVALID_PARAMETER);
    // A non-finite sample in a channel past the first, likewise: every channel
    // goes through the same validation rather than only the one the length and
    // rate are read from.
    std::vector<float> tainted = noisy;
    tainted[10] = std::numeric_limits<float>::quiet_NaN();
    const float* second_nan[3] = {noisy.data(), tainted.data(), noisy.data()};
    REQUIRE(denoise_linked_into(second_nan, 3, length, sr, nullptr, outs, &report) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("refuses an empty set, a missing set and a bad rate") {
    std::vector<float> plane(length, 0.0f);
    const float* in[2] = {noisy.data(), noisy.data()};
    SonareDenoiseReport report{};
    float* outs[2] = {plane.data(), plane.data()};
    REQUIRE(denoise_linked_into(in, 0, length, sr, nullptr, outs, &report) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(denoise_linked_into(nullptr, 2, length, sr, nullptr, outs, &report) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(denoise_linked_into(in, 2, length, 0, nullptr, outs, &report) ==
            SONARE_ERROR_INVALID_PARAMETER);

    // The same config pre-check the mono and stereo entries carry, so the three
    // agree on which configs they reject before the core ever sees them.
    SonareDenoiseClassicalConfig bad;
    sonare_denoise_classical_config_init(&bad);
    bad.n_fft = 1000;  // not a power of two
    bad.hop_length = 256;
    REQUIRE(denoise_linked_into(in, 2, length, sr, &bad, outs, &report) ==
            SONARE_ERROR_INVALID_PARAMETER);
    bad.n_fft = 1024;
    bad.hop_length = 0;
    REQUIRE(denoise_linked_into(in, 2, length, sr, &bad, outs, &report) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_mastering_repair_declip_stereo", "[c_api][mastering]") {
  const int sr = 22050;
  // Different tones, so returning one channel twice fails on content alone.
  auto left = generate_sine(440.0f, sr, 0.3f);
  auto right = generate_sine(880.0f, sr, 0.3f);
  for (auto& s : left) s *= 0.5f;
  for (auto& s : right) s *= 0.5f;

  // Declip does not link the way declick does: a channel with no clipped sample
  // of its own in a run is left alone there rather than reconstructed to match.
  // Linking therefore needs the SAME region clipped in both channels to
  // DIFFERENT extents -- [2000,2010) in both, carried on to 2040 in the right
  // only, so the union run is [2000,2040) and the left is dragged past its own
  // 10 clipped samples. The isolated right-only run at 5000 is the control for
  // the other half of the rule.
  const size_t kSharedBegin = 2000, kSharedEnd = 2010;
  const size_t kRightWideEnd = 2040;
  const size_t kRightOnlyBegin = 5000, kRightOnlyEnd = 5010;
  for (size_t i = kSharedBegin; i < kSharedEnd; ++i) left[i] = 1.0f;
  for (size_t i = kSharedBegin; i < kRightWideEnd; ++i) right[i] = 1.0f;
  for (size_t i = kRightOnlyBegin; i < kRightOnlyEnd; ++i) right[i] = 1.0f;
  const std::vector<float> left_input = left;

  SECTION("a wider run on one side extends the other side's reconstruction") {
    SonareDeclipStereoResult out{};
    REQUIRE(sonare_mastering_repair_declip_stereo(left.data(), right.data(), left.size(), sr,
                                                  nullptr, &out) == SONARE_OK);
    REQUIRE(out.left != nullptr);
    REQUIRE(out.right != nullptr);
    REQUIRE(out.length == left.size());

    // The fixture only witnesses the rule while each channel clips on its own.
    CHECK(out.left_report.detected.run_count == 1);
    CHECK(out.left_report.detected.clipped_samples == kSharedEnd - kSharedBegin);
    CHECK(out.right_report.detected.run_count == 2);

    // The asymmetry is the whole point: the left is carried past its own
    // clipped samples by the right's wider run, while the right's own detection
    // already spans that run and so borrows nothing.
    CHECK(out.left_report.linked_runs > 0);
    CHECK(out.right_report.linked_runs == 0);

    // A run only the right channel clipped leaves the left untouched -- this is
    // where declick would have repaired both.
    for (size_t i = kRightOnlyBegin; i < kRightOnlyEnd; ++i) {
      CHECK(out.left[i] == Catch::Approx(left_input[i]).margin(1e-6));
    }

    sonare_free_declip_stereo_result(&out);
  }

  SECTION("the extended region differs from the mono entry point") {
    SonareDeclipStereoResult stereo{};
    REQUIRE(sonare_mastering_repair_declip_stereo(left.data(), right.data(), left.size(), sr,
                                                  nullptr, &stereo) == SONARE_OK);
    SonareDeclipResult mono{};
    REQUIRE(sonare_mastering_repair_declip(left.data(), left.size(), sr, nullptr, &mono) ==
            SONARE_OK);
    REQUIRE(mono.length == stereo.length);

    // The mono pass never sees the right channel's run, so it stops at 2010.
    bool differs = false;
    for (size_t i = kSharedEnd; i < kRightWideEnd; ++i) {
      if (stereo.left[i] != mono.samples[i]) differs = true;
    }
    CHECK(differs);

    sonare_free_declip_result(&mono);
    sonare_free_declip_stereo_result(&stereo);
  }

  SECTION("clears the result before refusing") {
    SonareDeclipStereoResult out{};
    out.left = non_null_sentinel_float_ptr();
    out.length = 123;
    out.left_report.repaired_samples = 99;
    REQUIRE(sonare_mastering_repair_declip_stereo(left.data(), right.data(), left.size(), 0,
                                                  nullptr, &out) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(out.left == nullptr);
    CHECK(out.right == nullptr);
    CHECK(out.length == 0);
    CHECK(out.left_report.repaired_samples == 0);
  }

  SECTION("refuses a null result") {
    REQUIRE(sonare_mastering_repair_declip_stereo(left.data(), right.data(), left.size(), sr,
                                                  nullptr,
                                                  nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_mastering_repair_declip", "[.][slow][c_api][mastering]") {
  const int sr = 48000;
  auto samples = generate_sine(440.0f, sr, 0.5f);
  // Hard-clip the signal at +/- 0.9.
  for (auto& s : samples) {
    s = std::max(-0.9f, std::min(0.9f, s * 2.0f));
  }

  SECTION("default config restores a length-matching buffer") {
    SonareDeclipResult result{};
    REQUIRE(sonare_mastering_repair_declip(samples.data(), samples.size(), sr, nullptr, &result) ==
            SONARE_OK);
    REQUIRE(result.samples != nullptr);
    REQUIRE(result.length == samples.size());
    REQUIRE(std::isfinite(max_abs(result.samples, result.length)));
    sonare_free_declip_result(&result);
  }

  SECTION("explicit config") {
    SonareDeclipConfig config;
    sonare_declip_config_init(&config);
    config.clip_threshold_db = 20.0f * std::log10(0.85f);
    config.lpc_order = 24;
    config.iterations = 1;
    config.lpc_blend = 0.5f;
    SonareDeclipResult result{};
    REQUIRE(sonare_mastering_repair_declip(samples.data(), samples.size(), sr, &config, &result) ==
            SONARE_OK);
    sonare_free_declip_result(&result);
  }

  SECTION("invalid config returns invalid parameter and clears output") {
    SonareDeclipConfig config;
    sonare_declip_config_init(&config);
    config.clip_threshold_db = 6.0f;  // above full scale
    config.lpc_order = 24;
    config.iterations = 1;
    config.lpc_blend = 0.5f;
    SonareDeclipResult result{};
    result.samples = non_null_sentinel_float_ptr();
    result.length = 123;
    REQUIRE(sonare_mastering_repair_declip(samples.data(), samples.size(), sr, &config, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.samples == nullptr);
    REQUIRE(result.length == 0);
  }
}

TEST_CASE("sonare_mastering_repair_decrackle", "[c_api][mastering]") {
  const int sr = 48000;
  auto samples = generate_sine(440.0f, sr, 0.5f);
  for (auto& s : samples) s *= 0.4f;
  // Inject crackle impulses.
  for (size_t i = 500; i < samples.size(); i += 1700) {
    samples[i] = (i % 2 == 0) ? 0.95f : -0.95f;
  }

  SECTION("median mode (default)") {
    SonareDecrackleResult result{};
    REQUIRE(sonare_mastering_repair_decrackle(samples.data(), samples.size(), sr, nullptr,
                                              &result) == SONARE_OK);
    REQUIRE(result.length == samples.size());
    sonare_free_decrackle_result(&result);
  }

  SECTION("wavelet shrinkage mode") {
    SonareDecrackleConfig config;
    sonare_decrackle_config_init(&config);
    config.threshold = 0.4f;
    config.mode = SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE;
    config.levels = 4;
    SonareDecrackleResult result{};
    REQUIRE(sonare_mastering_repair_decrackle(samples.data(), samples.size(), sr, &config,
                                              &result) == SONARE_OK);
    REQUIRE(result.length == samples.size());
    sonare_free_decrackle_result(&result);
  }

  SECTION("invalid config returns invalid parameter and clears output") {
    SonareDecrackleConfig config;
    sonare_decrackle_config_init(&config);
    config.threshold = 0.0f;
    config.mode = SONARE_DECRACKLE_MODE_MEDIAN;
    config.levels = 4;
    SonareDecrackleResult result{};
    result.samples = non_null_sentinel_float_ptr();
    result.length = 123;
    REQUIRE(sonare_mastering_repair_decrackle(samples.data(), samples.size(), sr, &config,
                                              &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.samples == nullptr);
    REQUIRE(result.length == 0);
  }

  SECTION("rejects unknown mode enum") {
    SonareDecrackleConfig config;
    sonare_decrackle_config_init(&config);
    config.threshold = 0.4f;
    config.mode = 999;
    config.levels = 4;
    SonareDecrackleResult result{};
    result.samples = non_null_sentinel_float_ptr();
    result.length = 123;
    REQUIRE(sonare_mastering_repair_decrackle(samples.data(), samples.size(), sr, &config,
                                              &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.samples == nullptr);
    REQUIRE(result.length == 0);
  }
}

TEST_CASE("sonare_mastering_repair_decrackle_stereo", "[c_api][mastering]") {
  const int sr = 48000;
  // Different tones and different crackle spacings, so a result that restated
  // one channel twice fails on content, and a pair of crossed reports fails on
  // the counts rather than needing an equality that happens to hold.
  auto left = generate_sine(440.0f, sr, 0.5f);
  auto right = generate_sine(880.0f, sr, 0.5f);
  for (auto& s : left) s *= 0.4f;
  for (auto& s : right) s *= 0.4f;
  for (size_t i = 500; i < left.size(); i += 1700) {
    left[i] = (i % 2 == 0) ? 0.95f : -0.95f;
  }
  for (size_t i = 900; i < right.size(); i += 2300) {
    right[i] = (i % 2 == 0) ? -0.95f : 0.95f;
  }

  SECTION("median mode processes each channel independently") {
    SonareDecrackleStereoResult out{};
    REQUIRE(sonare_mastering_repair_decrackle_stereo(left.data(), right.data(), left.size(), sr,
                                                     nullptr, &out) == SONARE_OK);
    REQUIRE(out.length == left.size());

    // Each channel must witness its own crackle. A channel detecting nothing
    // satisfies every loose claim below while reporting nothing at all.
    REQUIRE(out.left_report.detected.sample_count > 0);
    REQUIRE(out.right_report.detected.sample_count > 0);
    REQUIRE(out.left_report.detected.sample_count != out.right_report.detected.sample_count);

    // The detector and the median repair share a criterion, so this is an
    // equality rather than a bound.
    REQUIRE(out.left_report.replaced_samples == out.left_report.detected.sample_count);
    REQUIRE(out.right_report.replaced_samples == out.right_report.detected.sample_count);

    // Wavelet's fields belong to the mode that did not run.
    REQUIRE(out.left_report.detail_coefficients == 0);
    REQUIRE(out.left_report.shrunk_coefficients == 0);
    REQUIRE(out.left_report.noise_sigma == 0.0f);

    bool channels_differ = false;
    for (size_t i = 0; i < out.length; ++i) {
      if (out.left[i] != out.right[i]) channels_differ = true;
    }
    REQUIRE(channels_differ);

    sonare_free_decrackle_stereo_result(&out);
  }

  SECTION("each channel matches the mono pass sample for sample") {
    SonareDecrackleStereoResult stereo{};
    REQUIRE(sonare_mastering_repair_decrackle_stereo(left.data(), right.data(), left.size(), sr,
                                                     nullptr, &stereo) == SONARE_OK);

    // Nothing here widens a repair to match the other side, so unlike the
    // declicker and the declipper the stereo pass owes the mono pass bit
    // equality. An implementation that grew any cross-channel decision breaks
    // this without needing a fixture built to catch that decision.
    SonareDecrackleResult mono{};
    REQUIRE(sonare_mastering_repair_decrackle(left.data(), left.size(), sr, nullptr, &mono) ==
            SONARE_OK);
    REQUIRE(mono.length == stereo.length);
    for (size_t i = 0; i < mono.length; ++i) {
      REQUIRE(stereo.left[i] == mono.samples[i]);
    }
    sonare_free_decrackle_result(&mono);
    REQUIRE(sonare_mastering_repair_decrackle(right.data(), right.size(), sr, nullptr, &mono) ==
            SONARE_OK);
    REQUIRE(mono.length == stereo.length);
    for (size_t i = 0; i < mono.length; ++i) {
      REQUIRE(stereo.right[i] == mono.samples[i]);
    }
    sonare_free_decrackle_result(&mono);

    sonare_free_decrackle_stereo_result(&stereo);
  }

  SECTION("swapping the inputs swaps the reports") {
    SonareDecrackleStereoResult forward{};
    SonareDecrackleStereoResult reversed{};
    REQUIRE(sonare_mastering_repair_decrackle_stereo(left.data(), right.data(), left.size(), sr,
                                                     nullptr, &forward) == SONARE_OK);
    REQUIRE(sonare_mastering_repair_decrackle_stereo(right.data(), left.data(), left.size(), sr,
                                                     nullptr, &reversed) == SONARE_OK);

    REQUIRE(reversed.left_report.detected.sample_count ==
            forward.right_report.detected.sample_count);
    REQUIRE(reversed.right_report.detected.sample_count ==
            forward.left_report.detected.sample_count);

    sonare_free_decrackle_stereo_result(&forward);
    sonare_free_decrackle_stereo_result(&reversed);
  }

  SECTION("wavelet mode reports through its own fields") {
    SonareDecrackleConfig config;
    sonare_decrackle_config_init(&config);
    config.threshold = 0.4f;
    config.mode = SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE;
    config.levels = 4;

    SonareDecrackleStereoResult out{};
    REQUIRE(sonare_mastering_repair_decrackle_stereo(left.data(), right.data(), left.size(), sr,
                                                     &config, &out) == SONARE_OK);

    REQUIRE(out.left_report.detail_coefficients > 0);
    REQUIRE(out.left_report.shrunk_coefficients > 0);
    REQUIRE(out.left_report.noise_sigma > 0.0f);
    REQUIRE(out.right_report.detail_coefficients > 0);
    REQUIRE(out.right_report.noise_sigma > 0.0f);
    REQUIRE(out.left_report.replaced_samples == 0);
    REQUIRE(out.right_report.replaced_samples == 0);

    // Detection is the median criterion whatever mode ran, so it is populated
    // here while the median repair counter is not.
    REQUIRE(out.left_report.detected.sample_count > 0);

    sonare_free_decrackle_stereo_result(&out);
  }

  SECTION("threshold reaches the wavelet shrinkage as a cap") {
    // The level threshold is min(configured, BayesShrink's own estimate), so
    // the configured value changes the result only below that estimate. A
    // value above it is passed through faithfully and changes nothing, which
    // would let this assertion pass without the value ever arriving.
    SonareDecrackleConfig loose;
    sonare_decrackle_config_init(&loose);
    loose.threshold = 10.0f;
    loose.mode = SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE;
    loose.levels = 4;
    SonareDecrackleConfig capped = loose;
    capped.threshold = 1e-4f;

    SonareDecrackleStereoResult loose_out{};
    SonareDecrackleStereoResult capped_out{};
    REQUIRE(sonare_mastering_repair_decrackle_stereo(left.data(), right.data(), left.size(), sr,
                                                     &loose, &loose_out) == SONARE_OK);
    REQUIRE(sonare_mastering_repair_decrackle_stereo(left.data(), right.data(), left.size(), sr,
                                                     &capped, &capped_out) == SONARE_OK);

    // The same coefficients are examined either way; only how many survive moves.
    REQUIRE(capped_out.left_report.detail_coefficients ==
            loose_out.left_report.detail_coefficients);
    REQUIRE(capped_out.left_report.shrunk_coefficients < loose_out.left_report.shrunk_coefficients);

    sonare_free_decrackle_stereo_result(&loose_out);
    sonare_free_decrackle_stereo_result(&capped_out);
  }

  SECTION("rejects a mismatched pair and clears the result") {
    SonareDecrackleStereoResult out{};
    out.left = non_null_sentinel_float_ptr();
    out.length = 123;
    REQUIRE(sonare_mastering_repair_decrackle_stereo(left.data(), nullptr, left.size(), sr, nullptr,
                                                     &out) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out.left == nullptr);
    REQUIRE(out.length == 0);

    REQUIRE(sonare_mastering_repair_decrackle_stereo(left.data(), right.data(), left.size(), 0,
                                                     nullptr,
                                                     &out) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_mastering_repair_decrackle_stereo(left.data(), right.data(), 0, sr, nullptr,
                                                     &out) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("rejects unknown mode enum") {
    SonareDecrackleConfig config;
    sonare_decrackle_config_init(&config);
    config.threshold = 0.4f;
    config.mode = 999;
    config.levels = 4;
    SonareDecrackleStereoResult out{};
    REQUIRE(sonare_mastering_repair_decrackle_stereo(left.data(), right.data(), left.size(), sr,
                                                     &config,
                                                     &out) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_mastering_repair_dehum", "[c_api][mastering]") {
  const int sr = 48000;
  auto signal = generate_sine(440.0f, sr, 1.0f);
  auto hum = generate_sine(50.0f, sr, 1.0f);
  std::vector<float> samples(signal.size());
  for (size_t i = 0; i < signal.size(); ++i) samples[i] = 0.5f * signal[i] + 0.2f * hum[i];

  SECTION("static notch (default)") {
    SonareDehumResult result{};
    REQUIRE(sonare_mastering_repair_dehum(samples.data(), samples.size(), sr, nullptr, &result) ==
            SONARE_OK);
    REQUIRE(result.length == samples.size());
    sonare_free_dehum_result(&result);
  }

  SECTION("adaptive tracking with explicit config") {
    SonareDehumConfig config;
    sonare_dehum_config_init(&config);
    config.fundamental_hz = 50.0f;
    config.harmonics = 4;
    config.q = 20.0f;
    config.adaptive = 1;
    config.search_range_hz = 2.0f;
    config.adaptation = 0.25f;
    config.frame_size = 2048;
    config.pll_bandwidth = 0.01f;
    SonareDehumResult result{};
    REQUIRE(sonare_mastering_repair_dehum(samples.data(), samples.size(), sr, &config, &result) ==
            SONARE_OK);
    REQUIRE(result.length == samples.size());
    sonare_free_dehum_result(&result);
  }

  SECTION("invalid config returns invalid parameter and clears output") {
    SonareDehumConfig config;
    sonare_dehum_config_init(&config);
    config.fundamental_hz = 0.0f;
    config.harmonics = 4;
    config.q = 20.0f;
    config.frame_size = 2048;
    SonareDehumResult result{};
    result.samples = non_null_sentinel_float_ptr();
    result.length = 123;
    REQUIRE(sonare_mastering_repair_dehum(samples.data(), samples.size(), sr, &config, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.samples == nullptr);
    REQUIRE(result.length == 0);
  }
}

TEST_CASE("sonare_mastering_repair_dehum_stereo", "[c_api][mastering]") {
  const int sr = 48000;
  // Different hum frequencies in the two channels. That is what makes a shared
  // tracker visible: tracking each channel alone lands on its own frequency,
  // while the shared tracker reads the channel mean and lands between them.
  const float kLeftHumHz = 50.0f;
  const float kRightHumHz = 60.0f;
  auto left = generate_sine(220.0f, sr, 0.5f);
  auto right = generate_sine(330.0f, sr, 0.5f);
  for (size_t i = 0; i < left.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    left[i] = 0.3f * left[i] + 0.2f * std::sin(2.0f * 3.14159265f * kLeftHumHz * t);
    right[i] = 0.3f * right[i] + 0.2f * std::sin(2.0f * 3.14159265f * kRightHumHz * t);
  }

  SonareDehumConfig fixed;

  sonare_dehum_config_init(&fixed);
  fixed.fundamental_hz = kLeftHumHz;
  fixed.harmonics = 4;
  fixed.q = 20.0f;
  fixed.adaptive = 0;
  fixed.search_range_hz = 8.0f;
  fixed.adaptation = 0.25f;
  fixed.frame_size = 2048;
  fixed.pll_bandwidth = 0.01f;

  SECTION("without tracking each channel is filtered on its own") {
    SonareDehumStereoResult out{};
    REQUIRE(sonare_mastering_repair_dehum_stereo(left.data(), right.data(), left.size(), sr, &fixed,
                                                 &out) == SONARE_OK);
    REQUIRE(out.length == left.size());

    // The header calls this the measurement rather than an unset field: with
    // tracking off the notch never moves, so the drift is exactly zero.
    REQUIRE(out.left_report.fundamental_drift_hz == 0.0f);
    REQUIRE(out.right_report.fundamental_drift_hz == 0.0f);
    REQUIRE(out.left_report.applied_fundamental_hz == Catch::Approx(kLeftHumHz));

    // Nothing is shared here, so each channel owes the mono pass bit equality.
    // An implementation that grew a cross-channel decision on this path breaks
    // this without needing a fixture built to catch that decision.
    SonareDehumResult mono{};
    REQUIRE(sonare_mastering_repair_dehum(left.data(), left.size(), sr, &fixed, &mono) ==
            SONARE_OK);
    REQUIRE(mono.length == out.length);
    for (size_t i = 0; i < mono.length; ++i) {
      REQUIRE(out.left[i] == mono.samples[i]);
    }
    sonare_free_dehum_result(&mono);

    sonare_free_dehum_stereo_result(&out);
  }

  SECTION("tracking shares one fundamental while each channel measures its own") {
    SonareDehumConfig tracked = fixed;
    tracked.adaptive = 1;

    SonareDehumStereoResult out{};
    REQUIRE(sonare_mastering_repair_dehum_stereo(left.data(), right.data(), left.size(), sr,
                                                 &tracked, &out) == SONARE_OK);

    // The discriminating property, and it is an asymmetry rather than a single
    // equality: one tracker runs for the pair, so the applied frequency is the
    // same in both reports, while detection reads each channel's own input and
    // therefore need not agree. Asserting only the first half would pass for an
    // implementation that ran one channel and copied its whole report.
    // Pinned before the equality, because two reports agreeing on zero would
    // satisfy it while measuring nothing.
    REQUIRE(out.left_report.applied_fundamental_hz > 0.0f);
    REQUIRE(std::abs(out.left_report.applied_fundamental_hz - tracked.fundamental_hz) <=
            tracked.search_range_hz);
    REQUIRE(out.left_report.applied_fundamental_hz == out.right_report.applied_fundamental_hz);
    REQUIRE(out.left_report.detected.fundamental_hz > 0.0f);
    REQUIRE(out.right_report.detected.fundamental_hz > 0.0f);
    REQUIRE(out.left_report.detected.fundamental_hz != out.right_report.detected.fundamental_hz);

    // And the shared tracker must actually be reached: the mono pass tracks the
    // left channel alone, so it lands somewhere the pair's mean does not.
    SonareDehumResult mono{};
    REQUIRE(sonare_mastering_repair_dehum(left.data(), left.size(), sr, &tracked, &mono) ==
            SONARE_OK);
    REQUIRE(mono.length == out.length);
    bool differs_from_mono = false;
    for (size_t i = 0; i < mono.length; ++i) {
      if (out.left[i] != mono.samples[i]) differs_from_mono = true;
    }
    REQUIRE(differs_from_mono);
    sonare_free_dehum_result(&mono);

    sonare_free_dehum_stereo_result(&out);
  }

  SECTION("reports every harmonic slot, not just the first") {
    SonareDehumStereoResult out{};
    REQUIRE(sonare_mastering_repair_dehum_stereo(left.data(), right.data(), left.size(), sr, &fixed,
                                                 &out) == SONARE_OK);

    // A mirror that carried only harmonic_dbfs[0] passes any check written
    // against the first entry alone, so the whole array has to say something.
    bool all_finite = true;
    bool any_differs_from_first = false;
    for (int k = 0; k < SONARE_DEHUM_MAX_HARMONICS; ++k) {
      if (!std::isfinite(out.left_report.detected.harmonic_dbfs[k])) all_finite = false;
      if (out.left_report.detected.harmonic_dbfs[k] != out.left_report.detected.harmonic_dbfs[0]) {
        any_differs_from_first = true;
      }
    }
    REQUIRE(all_finite);
    REQUIRE(any_differs_from_first);

    // The notched count stops at Nyquist rather than at the configured count.
    REQUIRE(out.left_report.notched_harmonics > 0);
    REQUIRE(out.left_report.notched_harmonics <= fixed.harmonics);

    sonare_free_dehum_stereo_result(&out);
  }

  SECTION("a harmonic past Nyquist reads as not measured rather than an unset value") {
    const int low_sr = 8000;
    auto l = generate_sine(220.0f, low_sr, 0.5f);
    auto r = generate_sine(330.0f, low_sr, 0.5f);
    SonareDehumConfig high = fixed;
    high.fundamental_hz = 3000.0f;  // 2*f0 is already past the 4 kHz Nyquist

    SonareDehumStereoResult out{};
    REQUIRE(sonare_mastering_repair_dehum_stereo(l.data(), r.data(), l.size(), low_sr, &high,
                                                 &out) == SONARE_OK);
    REQUIRE(std::isnan(out.left_report.detected.harmonic_dbfs[1]));
    sonare_free_dehum_stereo_result(&out);
  }

  SECTION("clears the result before refusing") {
    SonareDehumStereoResult out{};
    out.left = non_null_sentinel_float_ptr();
    out.length = 123;
    out.left_report.notched_harmonics = 99;
    REQUIRE(sonare_mastering_repair_dehum_stereo(nullptr, right.data(), left.size(), sr, &fixed,
                                                 &out) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(out.left == nullptr);
    CHECK(out.right == nullptr);
    CHECK(out.length == 0);
    CHECK(out.left_report.notched_harmonics == 0);

    REQUIRE(sonare_mastering_repair_dehum_stereo(left.data(), right.data(), left.size(), 0, &fixed,
                                                 &out) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_mastering_repair_dehum_stereo(left.data(), right.data(), left.size(), sr, &fixed,
                                                 nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_mastering_repair_dereverb_classical", "[c_api][mastering]") {
  const int sr = 48000;
  auto samples = generate_sine(440.0f, sr, 1.0f);
  for (auto& s : samples) s *= 0.5f;

  SECTION("default config") {
    SonareDereverbResult result{};
    REQUIRE(sonare_mastering_repair_dereverb_classical(samples.data(), samples.size(), sr, nullptr,
                                                       &result) == SONARE_OK);
    REQUIRE(result.length == samples.size());
    sonare_free_dereverb_result(&result);
  }

  SECTION("WPE-enabled config") {
    SonareDereverbClassicalConfig config;
    sonare_dereverb_classical_config_init(&config);
    config.threshold = 0.05f;
    config.attenuation = 0.5f;
    config.n_fft = 1024;
    config.hop_length = 256;
    config.t60_sec = 0.4f;
    config.late_delay_ms = 50.0f;
    config.over_subtraction = 1.0f;
    config.spectral_floor = 0.08f;
    config.wpe_enabled = 1;
    config.wpe_iterations = 2;
    config.wpe_taps = 3;
    config.wpe_strength = 0.7f;
    SonareDereverbResult result{};
    REQUIRE(sonare_mastering_repair_dereverb_classical(samples.data(), samples.size(), sr, &config,
                                                       &result) == SONARE_OK);
    REQUIRE(result.length == samples.size());
    sonare_free_dereverb_result(&result);
  }

  SECTION("rejects bad n_fft / hop_length") {
    SonareDereverbClassicalConfig config;
    sonare_dereverb_classical_config_init(&config);
    config.threshold = 0.05f;
    config.attenuation = 0.5f;
    config.n_fft = 1500;  // not a power of two
    config.hop_length = 256;
    config.t60_sec = 0.4f;
    config.late_delay_ms = 50.0f;
    config.over_subtraction = 1.0f;
    config.spectral_floor = 0.08f;
    SonareDereverbResult result{};
    REQUIRE(sonare_mastering_repair_dereverb_classical(samples.data(), samples.size(), sr, &config,
                                                       &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.samples == nullptr);
    REQUIRE(result.length == 0);

    config.n_fft = 1024;
    config.hop_length = 2048;  // larger than n_fft
    result = {};
    result.samples = non_null_sentinel_float_ptr();
    result.length = 123;
    REQUIRE(sonare_mastering_repair_dereverb_classical(samples.data(), samples.size(), sr, &config,
                                                       &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.samples == nullptr);
    REQUIRE(result.length == 0);
  }
}

TEST_CASE("sonare_mastering_repair_dereverb_classical_stereo", "[c_api][mastering]") {
  const int sr = 48000;
  // A burst through two feedback combs. The tail is what both stages read: the
  // late-lag statistic needs energy still present one late_delay_ms later, and
  // the WPE predictor needs that energy to be predictable from the lagged frame.
  // A plain tone gives the second without the first and would let a build that
  // never ran the late stage pass.
  std::vector<float> reverberant(sr, 0.0f);
  uint32_t state = 7u;
  for (size_t i = 0; i < static_cast<size_t>(sr) / 10; ++i) {
    state = state * 1664525u + 1013904223u;
    const float u = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    reverberant[i] = (u - 0.5f) * 0.6f;
  }
  const size_t taps[2] = {1729, 2400};
  const float gains[2] = {0.55f, 0.45f};
  for (size_t i = 0; i < reverberant.size(); ++i) {
    for (int k = 0; k < 2; ++k) {
      if (i >= taps[k]) reverberant[i] += gains[k] * reverberant[i - taps[k]];
    }
  }
  std::vector<float> silence(reverberant.size(), 0.0f);

  SonareDereverbClassicalConfig base;

  sonare_dereverb_classical_config_init(&base);
  base.threshold = 0.0f;
  base.attenuation = 1.0f;
  base.n_fft = 1024;
  base.hop_length = 256;
  base.t60_sec = 0.4f;
  base.late_delay_ms = 50.0f;
  base.over_subtraction = 1.0f;
  base.spectral_floor = 0.08f;
  base.wpe_enabled = 0;
  base.wpe_iterations = 2;
  base.wpe_taps = 3;
  base.wpe_strength = 0.7f;

  SECTION("one linked mask and one predictor set drive both channels") {
    SonareDereverbStereoResult out{};
    REQUIRE(sonare_mastering_repair_dereverb_classical_stereo(
                reverberant.data(), reverberant.data(), reverberant.size(), sr, &base, &out) ==
            SONARE_OK);
    REQUIRE(out.length == reverberant.size());
    for (size_t i = 0; i < out.length; ++i) {
      REQUIRE(out.left[i] == out.right[i]);
    }
    // Pinned so the equality above cannot be satisfied by two silent buffers.
    REQUIRE(max_abs(out.left, out.length) > 0.01f);
    sonare_free_dereverb_stereo_result(&out);
  }

  SECTION("the WPE fields are zero on the path that does not run WPE") {
    SonareDereverbStereoResult off{};
    REQUIRE(sonare_mastering_repair_dereverb_classical_stereo(
                reverberant.data(), reverberant.data(), reverberant.size(), sr, &base, &off) ==
            SONARE_OK);
    REQUIRE(off.report.detected.has_late_predictability == 0u);
    REQUIRE(off.report.detected.late_predictability == 0.0f);
    REQUIRE(off.report.wpe_predictor_norm == 0.0f);
    // The rest of the report still has to be alive, or the two zeros above are
    // satisfied by a call that computed nothing at all.
    REQUIRE(off.report.mean_reduction_db > 0.0f);
    sonare_free_dereverb_stereo_result(&off);

    // wpe_enabled is clear by default, so the check above is the whole of what a
    // default-config test can witness: it passes against a build with no WPE
    // stage in it. Only this second half distinguishes the two.
    SonareDereverbClassicalConfig wpe = base;
    wpe.wpe_enabled = 1;
    SonareDereverbStereoResult on{};
    REQUIRE(sonare_mastering_repair_dereverb_classical_stereo(
                reverberant.data(), reverberant.data(), reverberant.size(), sr, &wpe, &on) ==
            SONARE_OK);
    REQUIRE(on.report.detected.has_late_predictability == 1u);
    REQUIRE(on.report.detected.late_predictability > 0.0f);
    REQUIRE(on.report.wpe_predictor_norm > 0.0f);
    // The applied norm is the pre-clamp one passed through a 0.98 ceiling, so it
    // can equal it but never exceed it.
    REQUIRE(on.report.wpe_predictor_norm <= on.report.detected.late_predictability);
    sonare_free_dereverb_stereo_result(&on);
  }

  SECTION("no field carries the pair's level the way a denoise floor does") {
    // Every field here is a ratio or a fraction, so duplicating a channel leaves
    // them where they were -- the exact opposite of the denoise pair, whose
    // absolute floor moves by 10*log10(2) under this same transformation. Both
    // came back bit-identical here; the margin is left nonzero only because the
    // late-lag statistic drops cells under an absolute power floor, so on other
    // material doubling could move a few across it.
    SonareDereverbStereoResult doubled{};
    SonareDereverbStereoResult single{};
    REQUIRE(sonare_mastering_repair_dereverb_classical_stereo(
                reverberant.data(), reverberant.data(), reverberant.size(), sr, &base, &doubled) ==
            SONARE_OK);
    REQUIRE(sonare_mastering_repair_dereverb_classical_stereo(reverberant.data(), silence.data(),
                                                              reverberant.size(), sr, &base,
                                                              &single) == SONARE_OK);

    // Pinned first: an implementation reporting a constant zero satisfies every
    // equality below.
    REQUIRE(std::isfinite(single.report.detected.late_decay_ratio_db));
    REQUIRE(single.report.detected.late_decay_ratio_db != 0.0f);
    REQUIRE(single.report.mean_reduction_db > 0.0f);

    REQUIRE(doubled.report.detected.late_decay_ratio_db ==
            Catch::Approx(single.report.detected.late_decay_ratio_db).margin(1e-6));
    REQUIRE(doubled.report.mean_reduction_db ==
            Catch::Approx(single.report.mean_reduction_db).margin(1e-6));

    sonare_free_dereverb_stereo_result(&doubled);
    sonare_free_dereverb_stereo_result(&single);
  }

  SECTION("the threshold gate is observable in suppressed_fraction") {
    SonareDereverbStereoResult open{};
    REQUIRE(sonare_mastering_repair_dereverb_classical_stereo(
                reverberant.data(), reverberant.data(), reverberant.size(), sr, &base, &open) ==
            SONARE_OK);
    REQUIRE(open.report.suppressed_fraction > 0.0f);

    SonareDereverbClassicalConfig gated = base;
    // The validator bounds threshold to a CLOSED [0, 1], so 1 is the tightest
    // legal gate; anything above it is refused rather than gating harder.
    gated.threshold = 1.0f;
    SonareDereverbStereoResult shut{};
    REQUIRE(sonare_mastering_repair_dereverb_classical_stereo(
                reverberant.data(), reverberant.data(), reverberant.size(), sr, &gated, &shut) ==
            SONARE_OK);
    REQUIRE(shut.report.suppressed_fraction < open.report.suppressed_fraction);

    sonare_free_dereverb_stereo_result(&open);
    sonare_free_dereverb_stereo_result(&shut);
  }

  SECTION("an input shorter than n_fft is padded rather than refused") {
    std::vector<float> tiny(512, 0.25f);
    SonareDereverbStereoResult out{};
    REQUIRE(sonare_mastering_repair_dereverb_classical_stereo(tiny.data(), tiny.data(), tiny.size(),
                                                              sr, &base, &out) == SONARE_OK);
    REQUIRE(out.length == tiny.size());
    sonare_free_dereverb_stereo_result(&out);
  }

  SECTION("clears the result before refusing") {
    SonareDereverbStereoResult out{};
    out.left = non_null_sentinel_float_ptr();
    out.length = 123;
    out.report.mean_reduction_db = 99.0f;
    REQUIRE(sonare_mastering_repair_dereverb_classical_stereo(
                nullptr, reverberant.data(), reverberant.size(), sr, &base, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(out.left == nullptr);
    CHECK(out.right == nullptr);
    CHECK(out.length == 0);
    CHECK(out.report.mean_reduction_db == 0.0f);

    REQUIRE(sonare_mastering_repair_dereverb_classical_stereo(
                reverberant.data(), reverberant.data(), reverberant.size(), 0, &base, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_mastering_repair_dereverb_classical_stereo(
                reverberant.data(), reverberant.data(), reverberant.size(), sr, &base, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);

    SonareDereverbClassicalConfig bad = base;
    bad.n_fft = 1500;  // not a power of two
    REQUIRE(sonare_mastering_repair_dereverb_classical_stereo(
                reverberant.data(), reverberant.data(), reverberant.size(), sr, &bad, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);
    // Bounded by n_fft here, where the denoise pair only requires it positive.
    bad.n_fft = 1024;
    bad.hop_length = 2048;
    REQUIRE(sonare_mastering_repair_dereverb_classical_stereo(
                reverberant.data(), reverberant.data(), reverberant.size(), sr, &bad, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_mastering_repair_dereverb_classical_linked", "[c_api][mastering]") {
  const int sr = 48000;
  // A burst through two feedback combs, as the stereo case uses: the late-lag
  // statistic needs energy still present one late_delay_ms later and the WPE
  // predictor needs it to be predictable from the lagged frame.
  auto reverberate = [](uint32_t seed, float burst_amplitude) {
    std::vector<float> out(static_cast<size_t>(sr), 0.0f);
    uint32_t state = seed;
    for (size_t i = 0; i < static_cast<size_t>(sr) / 10; ++i) {
      state = state * 1664525u + 1013904223u;
      const float u = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
      out[i] = (u - 0.5f) * burst_amplitude;
    }
    const size_t taps[2] = {1729, 2400};
    const float gains[2] = {0.55f, 0.45f};
    for (size_t i = 0; i < out.size(); ++i) {
      for (int k = 0; k < 2; ++k) {
        if (i >= taps[k]) out[i] += gains[k] * out[i - taps[k]];
      }
    }
    return out;
  };
  const std::vector<float> reverberant = reverberate(7u, 0.6f);
  const std::vector<float> other = reverberate(99u, 0.45f);
  const size_t length = reverberant.size();

  SonareDereverbClassicalConfig base;

  sonare_dereverb_classical_config_init(&base);
  base.threshold = 0.0f;
  base.attenuation = 1.0f;
  base.n_fft = 1024;
  base.hop_length = 256;
  base.t60_sec = 0.4f;
  base.late_delay_ms = 50.0f;
  base.over_subtraction = 1.0f;
  base.spectral_floor = 0.08f;
  base.wpe_enabled = 0;
  base.wpe_iterations = 2;
  base.wpe_taps = 3;
  base.wpe_strength = 0.7f;

  SECTION("one channel reproduces the mono entry bit for bit") {
    std::vector<float> plane(length, 0.0f);
    const float* in[1] = {reverberant.data()};
    float* outs[1] = {plane.data()};
    SonareDereverbReport report{};
    REQUIRE(dereverb_linked_into(in, 1, length, sr, &base, outs, &report) == SONARE_OK);

    SonareDereverbResult mono{};
    REQUIRE(sonare_mastering_repair_dereverb_classical(reverberant.data(), length, sr, &base,
                                                       &mono) == SONARE_OK);
    REQUIRE(mono.length == length);
    for (size_t i = 0; i < length; ++i) {
      REQUIRE(plane[i] == mono.samples[i]);
    }
    REQUIRE(max_abs(plane.data(), length) > 0.01f);
    sonare_free_dereverb_result(&mono);
  }

  SECTION("two channels reproduce the stereo entry, plane for plane") {
    std::vector<float> first(length, 0.0f);
    std::vector<float> second(length, 0.0f);
    const float* in[2] = {reverberant.data(), other.data()};
    float* outs[2] = {first.data(), second.data()};
    SonareDereverbReport report{};
    REQUIRE(dereverb_linked_into(in, 2, length, sr, &base, outs, &report) == SONARE_OK);

    SonareDereverbStereoResult pair{};
    REQUIRE(sonare_mastering_repair_dereverb_classical_stereo(
                reverberant.data(), other.data(), length, sr, &base, &pair) == SONARE_OK);
    REQUIRE(pair.length == length);
    for (size_t i = 0; i < length; ++i) {
      REQUIRE(first[i] == pair.left[i]);
      REQUIRE(second[i] == pair.right[i]);
    }
    REQUIRE(report.detected.late_decay_ratio_db == pair.report.detected.late_decay_ratio_db);
    REQUIRE(max_abs(first.data(), length) > 0.01f);
    REQUIRE(max_abs(second.data(), length) > 0.01f);
    sonare_free_dereverb_stereo_result(&pair);
  }

  SECTION("no field moves with the channel count, unlike the denoise entry") {
    // Every field here is a ratio or a fraction. The denoise entry's floor moves
    // by 10*log10(N) under this same transformation, so a figure measured over a
    // set is comparable against a mono one here and is not there.
    std::vector<float> a(length, 0.0f);
    std::vector<float> b(length, 0.0f);
    std::vector<float> c(length, 0.0f);
    const float* three_in[3] = {reverberant.data(), reverberant.data(), reverberant.data()};
    float* three_out[3] = {a.data(), b.data(), c.data()};
    SonareDereverbReport tripled{};
    REQUIRE(dereverb_linked_into(three_in, 3, length, sr, &base, three_out, &tripled) == SONARE_OK);

    std::vector<float> single_plane(length, 0.0f);
    const float* one_in[1] = {reverberant.data()};
    float* one_out[1] = {single_plane.data()};
    SonareDereverbReport single{};
    REQUIRE(dereverb_linked_into(one_in, 1, length, sr, &base, one_out, &single) == SONARE_OK);

    // Pinned first: an implementation reporting a constant zero satisfies every
    // equality below.
    REQUIRE(std::isfinite(single.detected.late_decay_ratio_db));
    REQUIRE(single.detected.late_decay_ratio_db != 0.0f);
    REQUIRE(single.mean_reduction_db > 0.0f);
    REQUIRE(single.suppressed_fraction > 0.0f);

    // The margin is left nonzero only because the late-lag statistic drops cells
    // under an absolute power floor, so on other material tripling could move a
    // few across it.
    REQUIRE(tripled.detected.late_decay_ratio_db ==
            Catch::Approx(single.detected.late_decay_ratio_db).margin(1e-6));
    REQUIRE(tripled.mean_reduction_db == Catch::Approx(single.mean_reduction_db).margin(1e-6));
    REQUIRE(tripled.suppressed_fraction == Catch::Approx(single.suppressed_fraction).margin(1e-6));

    for (size_t i = 0; i < length; ++i) {
      REQUIRE(a[i] == b[i]);
      REQUIRE(b[i] == c[i]);
    }
    REQUIRE(max_abs(a.data(), length) > 0.01f);
  }

  SECTION("one predictor set is fitted over the whole channel set") {
    // wpe_enabled is clear by default, so a default-config test passes against a
    // build with no WPE stage in it at all. This is the half that distinguishes
    // them, and at three channels it is also what says the statistics are
    // accumulated over the set rather than over a pair.
    SonareDereverbClassicalConfig wpe = base;
    wpe.wpe_enabled = 1;
    std::vector<float> a(length, 0.0f);
    std::vector<float> b(length, 0.0f);
    std::vector<float> c(length, 0.0f);
    const float* in[3] = {reverberant.data(), reverberant.data(), reverberant.data()};
    float* outs[3] = {a.data(), b.data(), c.data()};
    SonareDereverbReport on{};
    REQUIRE(dereverb_linked_into(in, 3, length, sr, &wpe, outs, &on) == SONARE_OK);
    REQUIRE(on.detected.has_late_predictability == 1u);
    REQUIRE(on.detected.late_predictability > 0.0f);
    REQUIRE(on.wpe_predictor_norm > 0.0f);
    // The applied norm is the pre-clamp one passed through a ceiling, so it can
    // equal it but never exceed it.
    REQUIRE(on.wpe_predictor_norm <= on.detected.late_predictability);
    for (size_t i = 0; i < length; ++i) {
      REQUIRE(a[i] == b[i]);
      REQUIRE(b[i] == c[i]);
    }

    SonareDereverbReport off{};
    REQUIRE(dereverb_linked_into(in, 3, length, sr, &base, outs, &off) == SONARE_OK);
    REQUIRE(off.detected.has_late_predictability == 0u);
    REQUIRE(off.detected.late_predictability == 0.0f);
    REQUIRE(off.wpe_predictor_norm == 0.0f);
  }

  SECTION("pads an input shorter than n_fft, as the denoise entry does") {
    const std::vector<float> tiny(512, 0.25f);
    std::vector<float> plane(tiny.size(), 0.0f);
    const float* in[1] = {tiny.data()};
    float* outs[1] = {plane.data()};
    SonareDereverbReport report{};
    REQUIRE(dereverb_linked_into(in, 1, tiny.size(), sr, &base, outs, &report) == SONARE_OK);

    SonareDenoiseClassicalConfig denoise;

    sonare_denoise_classical_config_init(&denoise);
    denoise.mode = SONARE_DENOISE_MODE_LOG_MMSE;
    denoise.noise_estimator = SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE;
    denoise.n_fft = 1024;
    denoise.hop_length = 256;
    denoise.dd_alpha = 0.98f;
    denoise.reduction_db = 26.0f;
    denoise.over_subtraction = 2.0f;
    denoise.spectral_floor = 0.05f;
    denoise.noise_estimation_quantile = 0.1f;
    denoise.speech_presence_gain = 1;
    denoise.gain_smoothing = 1;
    SonareDenoiseReport padded{};
    REQUIRE(denoise_linked_into(in, 1, tiny.size(), sr, &denoise, outs, &padded) == SONARE_OK);
  }

  SECTION("a refusal leaves the caller's planes alone") {
    constexpr float kSentinel = -3.25f;
    std::vector<float> plane(length, kSentinel);
    const float* in[2] = {reverberant.data(), nullptr};
    float* outs[2] = {plane.data(), plane.data()};
    SonareDereverbReport report{};
    report.mean_reduction_db = 99.0f;
    REQUIRE(dereverb_linked_into(in, 2, length, sr, &base, outs, &report) ==
            SONARE_ERROR_INVALID_PARAMETER);
    // Nothing is allocated here, so a refusal has no result to clear -- the
    // planes are the caller's and must come back as they were handed over.
    for (float sample : plane) {
      REQUIRE(sample == kSentinel);
    }
    CHECK(report.mean_reduction_db == 0.0f);

    const float* good[2] = {reverberant.data(), reverberant.data()};
    REQUIRE(dereverb_linked_into(good, 0, length, sr, &base, outs, &report) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(dereverb_linked_into(nullptr, 2, length, sr, &base, outs, &report) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(dereverb_linked_into(good, 2, length, 0, &base, outs, &report) ==
            SONARE_ERROR_INVALID_PARAMETER);

    SonareDereverbClassicalConfig bad = base;
    bad.n_fft = 1500;  // not a power of two
    REQUIRE(dereverb_linked_into(good, 2, length, sr, &bad, outs, &report) ==
            SONARE_ERROR_INVALID_PARAMETER);
    // Bounded by n_fft here, where the denoise entry only requires it positive.
    bad.n_fft = 1024;
    bad.hop_length = 2048;
    REQUIRE(dereverb_linked_into(good, 2, length, sr, &bad, outs, &report) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("hop_length above n_fft is refused by the shared pre-check on all three entries") {
    SonareDereverbClassicalConfig bad = base;
    bad.hop_length = static_cast<size_t>(bad.n_fft) * 2;
    SonareDereverbResult mono_out{};
    REQUIRE(sonare_mastering_repair_dereverb_classical(
                reverberant.data(), length, sr, &bad, &mono_out) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) ==
          "config.hop_length must be an integer in [1, 512]");

    SonareDereverbStereoResult stereo{};
    REQUIRE(sonare_mastering_repair_dereverb_classical_stereo(reverberant.data(), other.data(),
                                                              length, sr, &bad, &stereo) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) ==
          "config.hop_length must be an integer in [1, 512]");

    std::vector<float> left(length);
    std::vector<float> right(length);
    const float* in[2] = {reverberant.data(), other.data()};
    float* outs[2] = {left.data(), right.data()};
    SonareDereverbReport report{};
    REQUIRE(dereverb_linked_into(in, 2, length, sr, &bad, outs, &report) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) ==
          "config.hop_length must be an integer in [1, 512]");
  }
}

TEST_CASE("sonare_mastering_repair_linked_rejects_impossible_channel_count", "[c_api][mastering]") {
  const float input = 0.25f;
  const float* channels[1] = {&input};
  constexpr float kOutputSentinel = -7.25f;
  float output = kOutputSentinel;
  float* out_channels[1] = {&output};
  const size_t impossible_channel_count = std::numeric_limits<size_t>::max();

  SECTION("denoise rejects impossible channel count without throwing") {
    SonareDenoiseReport report{};
    report.detected.floor_dbfs = 91.0f;
    report.mean_reduction_db = 92.0f;
    SonareError error = SONARE_OK;

    REQUIRE_NOTHROW(error = denoise_linked_into(channels, impossible_channel_count, 1, 22050,
                                                nullptr, out_channels, &report));
    REQUIRE(error == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(report.detected.floor_dbfs == 0.0f);
    CHECK(report.mean_reduction_db == 0.0f);
    CHECK(output == kOutputSentinel);
  }

  SECTION("dereverb rejects impossible channel count without throwing") {
    SonareDereverbReport report{};
    report.detected.late_decay_ratio_db = 91.0f;
    report.mean_reduction_db = 92.0f;
    SonareError error = SONARE_OK;

    REQUIRE_NOTHROW(error = dereverb_linked_into(channels, impossible_channel_count, 1, 48000,
                                                 nullptr, out_channels, &report));
    REQUIRE(error == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(report.detected.late_decay_ratio_db == 0.0f);
    CHECK(report.mean_reduction_db == 0.0f);
    CHECK(output == kOutputSentinel);
  }
}

TEST_CASE("sonare_mastering_repair_trim_silence", "[c_api][mastering]") {
  const int sr = 48000;
  const size_t silent_pad = 2400;  // 50 ms
  std::vector<float> samples(silent_pad, 0.0f);
  auto sig = generate_sine(440.0f, sr, 0.2f);
  for (auto& s : sig) s *= 0.5f;
  samples.insert(samples.end(), sig.begin(), sig.end());
  samples.insert(samples.end(), silent_pad, 0.0f);

  SECTION("peak mode (default) shortens the buffer") {
    SonareTrimSilenceResult result{};
    REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, nullptr,
                                                 &result) == SONARE_OK);
    REQUIRE(result.samples != nullptr);
    REQUIRE(result.length < samples.size());
    REQUIRE(result.length > 0);
    sonare_free_trim_silence_result(&result);
  }

  SECTION("LUFS-gated mode with padding") {
    SonareTrimSilenceConfig config;
    sonare_trim_silence_config_init(&config);
    config.threshold_db = -60.0f;
    config.padding_samples = 1200;
    config.mode = SONARE_TRIM_SILENCE_MODE_LUFS_GATED;
    config.gate_lufs = -40.0f;
    config.window_ms = 400.0f;
    SonareTrimSilenceResult result{};
    REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, &config,
                                                 &result) == SONARE_OK);
    REQUIRE(result.samples != nullptr);
    REQUIRE(result.length > 0);
    sonare_free_trim_silence_result(&result);
  }

  SECTION("invalid config returns invalid parameter and clears output") {
    SonareTrimSilenceConfig config;
    sonare_trim_silence_config_init(&config);
    config.threshold_db = 6.0f;  // above full scale
    config.mode = SONARE_TRIM_SILENCE_MODE_PEAK;
    config.window_ms = 400.0f;
    SonareTrimSilenceResult result{};
    result.samples = non_null_sentinel_float_ptr();
    result.length = 123;
    REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, &config,
                                                 &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.samples == nullptr);
    REQUIRE(result.length == 0);
  }

  SECTION("rejects unknown mode enum") {
    SonareTrimSilenceConfig config;
    sonare_trim_silence_config_init(&config);
    config.threshold_db = -60.0f;
    config.padding_samples = 0;
    config.mode = 999;
    config.gate_lufs = -60.0f;
    config.window_ms = 400.0f;
    SonareTrimSilenceResult result{};
    result.samples = non_null_sentinel_float_ptr();
    result.length = 123;
    REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, &config,
                                                 &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(result.samples == nullptr);
    REQUIRE(result.length == 0);
  }
}

TEST_CASE("a parameter refusal is kept beside the flat message and cleared with it",
          "[c_api][mastering]") {
  const int sr = 48000;
  auto samples = generate_sine(440.0f, sr, 0.1f);
  SonareTrimSilenceConfig config;
  sonare_trim_silence_config_init(&config);
  config.threshold_db = -60.0f;
  config.mode = SONARE_TRIM_SILENCE_MODE_PEAK;
  config.gate_lufs = -60.0f;
  config.window_ms = 0.0f;
  SonareTrimSilenceResult result{};
  REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, &config,
                                               &result) == SONARE_ERROR_INVALID_PARAMETER);
  // C reads the core's own spelling; a JS binding renders the same refusal in request keys.
  CHECK(std::string(sonare_last_error_message()) ==
        "config.window_ms must be finite and in (0, 10000]");
  const sonare::SonareParameterRefusal* refusal = sonare_c_detail::last_parameter_refusal();
  REQUIRE(refusal != nullptr);
  CHECK(refusal->subject() == "config.window_ms");
  CHECK(std::string(sonare::util::check::camel_spelled(*refusal).what()) ==
        "config.windowMs must be finite and in (0, 10000]");

  // A later failure that is no parameter refusal replaces it rather than leaving it behind.
  config.window_ms = 400.0f;
  config.mode = 999;
  REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, &config,
                                               &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_c_detail::last_parameter_refusal() == nullptr);

  config.window_ms = 0.0f;
  config.mode = SONARE_TRIM_SILENCE_MODE_PEAK;
  REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, &config,
                                               &result) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_c_detail::last_parameter_refusal() != nullptr);
  config.window_ms = 400.0f;
  REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, &config,
                                               &result) == SONARE_OK);
  sonare_free_trim_silence_result(&result);
  CHECK(sonare_c_detail::last_parameter_refusal() == nullptr);
}

namespace {

// Pre-fills a result with garbage, checks a refused call empties it, then checks
// the releaser empties a real result and tolerates a second call and NULL.
template <typename R, typename Call, typename Release, typename ReportCleared>
void check_refusal_and_release(int sample_rate, Call call, Release release,
                               ReportCleared report_cleared) {
  R refused;
  std::memset(&refused, 0xAB, sizeof(refused));
  refused.samples = non_null_sentinel_float_ptr();
  refused.length = 123;
  CHECK(call(0, &refused) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(refused.samples == nullptr);
  CHECK(refused.length == 0);
  CHECK(report_cleared(refused));

  R result{};
  REQUIRE(call(sample_rate, &result) == SONARE_OK);
  REQUIRE(result.samples != nullptr);
  release(&result);
  CHECK(result.samples == nullptr);
  CHECK(result.length == 0);
  release(&result);
  release(nullptr);
}

}  // namespace

TEST_CASE("mono repair returns its report beside the audio", "[c_api][mastering]") {
  SECTION("declick") {
    const int sr = 48000;
    auto samples = generate_sine(440.0f, sr, 0.5f);
    for (auto& s : samples) s *= 0.3f;
    for (size_t i = 0; i < 5; ++i) samples[2000 + i * 1500] = 1.0f;

    SonareDeclickResult result{};
    REQUIRE(sonare_mastering_repair_declick(samples.data(), samples.size(), sr, nullptr, &result) ==
            SONARE_OK);
    CHECK(result.length == samples.size());
    CHECK(result.report.repaired_runs > 0);
    CHECK(result.report.repaired_samples > 0);
    CHECK(result.report.linked_runs == 0);
    sonare_free_declick_result(&result);
  }

  SECTION("declip") {
    const int sr = 48000;
    auto samples = generate_sine(440.0f, sr, 0.1f);
    for (auto& s : samples) s = std::max(-1.0f, std::min(1.0f, s * 2.0f));

    SonareDeclipResult result{};
    REQUIRE(sonare_mastering_repair_declip(samples.data(), samples.size(), sr, nullptr, &result) ==
            SONARE_OK);
    CHECK(result.length == samples.size());
    CHECK(result.report.repaired_samples > 0);
    CHECK(result.report.linked_runs == 0);
    sonare_free_declip_result(&result);
  }

  SECTION("decrackle") {
    const int sr = 48000;
    auto samples = generate_sine(440.0f, sr, 0.5f);
    for (auto& s : samples) s *= 0.4f;
    for (size_t i = 500; i < samples.size(); i += 1700) {
      samples[i] = (i % 2 == 0) ? 0.95f : -0.95f;
    }

    SonareDecrackleResult result{};
    REQUIRE(sonare_mastering_repair_decrackle(samples.data(), samples.size(), sr, nullptr,
                                              &result) == SONARE_OK);
    CHECK(result.length == samples.size());
    CHECK(result.report.replaced_samples > 0);
    CHECK(result.report.replaced_samples >= result.report.detected.sample_count);
    sonare_free_decrackle_result(&result);
  }

  SECTION("dehum") {
    const int sr = 48000;
    auto signal = generate_sine(440.0f, sr, 1.0f);
    auto hum = generate_sine(50.0f, sr, 1.0f);
    std::vector<float> samples(signal.size());
    for (size_t i = 0; i < signal.size(); ++i) samples[i] = 0.5f * signal[i] + 0.2f * hum[i];

    SonareDehumResult result{};
    REQUIRE(sonare_mastering_repair_dehum(samples.data(), samples.size(), sr, nullptr, &result) ==
            SONARE_OK);
    CHECK(result.length == samples.size());
    CHECK(result.report.detected.fundamental_hz == Catch::Approx(50.0f).margin(1.0f));
    CHECK(result.report.notched_harmonics > 0);
    sonare_free_dehum_result(&result);
  }

  SECTION("denoise") {
    const int sr = 22050;
    auto signal = generate_sine(440.0f, sr, 1.0f);
    std::vector<float> noisy(signal.size());
    uint32_t state = 1u;
    for (size_t i = 0; i < signal.size(); ++i) {
      state = state * 1664525u + 1013904223u;
      const float u = static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
      noisy[i] = 0.5f * signal[i] + (u - 0.5f) * 0.4f;
    }

    SonareDenoiseResult result{};
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, nullptr,
                                                      &result) == SONARE_OK);
    CHECK(result.length == noisy.size());
    CHECK(result.report.mean_reduction_db > 0.0f);
    CHECK(result.report.max_reduction_db >= result.report.mean_reduction_db);
    sonare_free_denoise_result(&result);
  }

  SECTION("dereverb") {
    const int sr = 48000;
    auto samples = generate_sine(440.0f, sr, 1.0f);
    for (auto& s : samples) s *= 0.5f;

    SonareDereverbResult result{};
    REQUIRE(sonare_mastering_repair_dereverb_classical(samples.data(), samples.size(), sr, nullptr,
                                                       &result) == SONARE_OK);
    CHECK(result.length == samples.size());
    CHECK(std::isfinite(result.report.detected.late_decay_ratio_db));
    CHECK(std::isfinite(result.report.detected.late_predictability));
    CHECK(result.report.mean_reduction_db >= 0.0f);
    sonare_free_dereverb_result(&result);
  }

  SECTION("trim_silence") {
    const int sr = 48000;
    const size_t silent_pad = 2400;
    std::vector<float> samples(silent_pad, 0.0f);
    auto sig = generate_sine(440.0f, sr, 0.2f);
    for (auto& s : sig) s *= 0.5f;
    samples.insert(samples.end(), sig.begin(), sig.end());
    samples.insert(samples.end(), silent_pad, 0.0f);

    SonareTrimRange expected{};
    REQUIRE(sonare_mastering_repair_detect_trim_range(samples.data(), samples.size(), sr, nullptr,
                                                      &expected) == SONARE_OK);
    SonareTrimSilenceResult result{};
    REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, nullptr,
                                                 &result) == SONARE_OK);
    CHECK(result.report.range.first == expected.first);
    CHECK(result.report.range.last_exclusive == expected.last_exclusive);
    CHECK(result.length == expected.last_exclusive - expected.first);
    CHECK(result.report.removed_head_samples + result.report.removed_tail_samples + result.length ==
          samples.size());
    CHECK(result.report.removed_head_samples > 0);
    sonare_free_trim_silence_result(&result);
  }

  SECTION("a refused call empties the result and the releaser is idempotent") {
    const int sr = 48000;
    auto samples = generate_sine(440.0f, sr, 0.25f);
    for (auto& s : samples) s *= 0.3f;
    for (size_t i = 0; i < 3; ++i) samples[2000 + i * 1500] = 1.0f;
    const float* data = samples.data();
    const size_t n = samples.size();

    check_refusal_and_release<SonareDeclickResult>(
        sr,
        [&](int rate, SonareDeclickResult* out) {
          return sonare_mastering_repair_declick(data, n, rate, nullptr, out);
        },
        [](SonareDeclickResult* r) { sonare_free_declick_result(r); },
        [](const SonareDeclickResult& r) { return r.report.repaired_runs == 0; });
    check_refusal_and_release<SonareDeclipResult>(
        sr,
        [&](int rate, SonareDeclipResult* out) {
          return sonare_mastering_repair_declip(data, n, rate, nullptr, out);
        },
        [](SonareDeclipResult* r) { sonare_free_declip_result(r); },
        [](const SonareDeclipResult& r) { return r.report.repaired_samples == 0; });
    check_refusal_and_release<SonareDecrackleResult>(
        sr,
        [&](int rate, SonareDecrackleResult* out) {
          return sonare_mastering_repair_decrackle(data, n, rate, nullptr, out);
        },
        [](SonareDecrackleResult* r) { sonare_free_decrackle_result(r); },
        [](const SonareDecrackleResult& r) { return r.report.replaced_samples == 0; });
    check_refusal_and_release<SonareDehumResult>(
        sr,
        [&](int rate, SonareDehumResult* out) {
          return sonare_mastering_repair_dehum(data, n, rate, nullptr, out);
        },
        [](SonareDehumResult* r) { sonare_free_dehum_result(r); },
        [](const SonareDehumResult& r) { return r.report.notched_harmonics == 0; });
    check_refusal_and_release<SonareDenoiseResult>(
        sr,
        [&](int rate, SonareDenoiseResult* out) {
          return sonare_mastering_repair_denoise_classical(data, n, rate, nullptr, out);
        },
        [](SonareDenoiseResult* r) { sonare_free_denoise_result(r); },
        [](const SonareDenoiseResult& r) { return r.report.mean_reduction_db == 0.0f; });
    check_refusal_and_release<SonareDereverbResult>(
        sr,
        [&](int rate, SonareDereverbResult* out) {
          return sonare_mastering_repair_dereverb_classical(data, n, rate, nullptr, out);
        },
        [](SonareDereverbResult* r) { sonare_free_dereverb_result(r); },
        [](const SonareDereverbResult& r) { return r.report.mean_reduction_db == 0.0f; });
    check_refusal_and_release<SonareTrimSilenceResult>(
        sr,
        [&](int rate, SonareTrimSilenceResult* out) {
          return sonare_mastering_repair_trim_silence(data, n, rate, nullptr, out);
        },
        [](SonareTrimSilenceResult* r) { sonare_free_trim_silence_result(r); },
        [](const SonareTrimSilenceResult& r) { return r.report.removed_head_samples == 0; });
  }
}

namespace {

// A burst of constant magnitude rather than a sine: the trimmer compares |x|
// against a threshold, and a sine's zero crossings would place the detected
// edges a few samples inside the span they are meant to pin.
void fill_burst(std::vector<float>& channel, size_t first, size_t last_exclusive) {
  for (size_t i = first; i < last_exclusive; ++i) {
    channel[i] = (i % 2 == 0) ? 0.5f : -0.5f;
  }
}

}  // namespace

TEST_CASE("sonare_mastering_repair_trim_silence_stereo", "[c_api][mastering]") {
  const int sr = 48000;

  // The two channels carry signal over DIFFERENT spans, so the union is a
  // genuine union and both per-channel ranges disagree with it and with each
  // other. A fixture whose channels agreed would pass just as well against an
  // implementation that cut each channel by its own range, which is the one
  // thing this entry's contract forbids.
  constexpr size_t kLength = 24000;  // 0.5 s
  constexpr size_t kLeftFirst = 4800;
  constexpr size_t kLeftEnd = 12000;
  constexpr size_t kRightFirst = 9600;
  constexpr size_t kRightEnd = 19200;

  // Mirrors mastering::repair::kMaxTrimPaddingSamples. kSizeMax is what a -1
  // padding count becomes on the way across a language boundary.
  constexpr size_t kSizeMax = static_cast<size_t>(-1);
  constexpr size_t kMaxPadding = 960000;

  std::vector<float> left(kLength, 0.0f);
  std::vector<float> right(kLength, 0.0f);
  fill_burst(left, kLeftFirst, kLeftEnd);
  fill_burst(right, kRightFirst, kRightEnd);

  SonareTrimSilenceConfig peak;

  sonare_trim_silence_config_init(&peak);
  peak.threshold_db = -60.0f;
  peak.padding_samples = 0;
  peak.mode = SONARE_TRIM_SILENCE_MODE_PEAK;
  peak.gate_lufs = -60.0f;
  peak.window_ms = 400.0f;

  SECTION("unions the two channels' ranges and cuts both channels to it") {
    SonareTrimSilenceStereoResult out{};
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &peak, &out) == SONARE_OK);

    REQUIRE(out.left_range.first == kLeftFirst);
    REQUIRE(out.left_range.last_exclusive == kLeftEnd);
    REQUIRE(out.right_range.first == kRightFirst);
    REQUIRE(out.right_range.last_exclusive == kRightEnd);
    REQUIRE(out.report.range.first == kLeftFirst);
    REQUIRE(out.report.range.last_exclusive == kRightEnd);

    // length is the OUTPUT length. Every other repair stereo entry hands back
    // the input length, so a reader carrying that habit over reads this wrong.
    REQUIRE(out.length == kRightEnd - kLeftFirst);
    REQUIRE(out.length < kLength);
    REQUIRE(out.report.removed_head_samples == kLeftFirst);
    REQUIRE(out.report.removed_tail_samples == kLength - kRightEnd);
    REQUIRE(out.report.removed_head_samples + out.report.removed_tail_samples ==
            kLength - out.length);

    // The content is what separates a shared range from two per-channel ones:
    // each channel keeps a stretch its own scan called silence, because the
    // other channel called it signal. Cutting per channel would drop both.
    REQUIRE(out.left != nullptr);
    REQUIRE(out.right != nullptr);
    REQUIRE(out.left[0] == 0.5f);                 // left's own first active sample
    REQUIRE(out.right[0] == 0.0f);                // right is still silent there
    REQUIRE(out.left[out.length - 1] == 0.0f);    // left has been silent for a while
    REQUIRE(out.right[out.length - 1] == -0.5f);  // right's own last active sample

    sonare_free_trim_silence_stereo_result(&out);
  }

  SECTION("hands back (NULL, 0) for a pair that carries no signal at all") {
    const std::vector<float> quiet(kLength, 0.0f);
    SonareTrimSilenceStereoResult out{};
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(quiet.data(), quiet.data(), kLength, sr,
                                                        &peak, &out) == SONARE_OK);

    // A success, not an error, and no zero-length allocation is made. No other
    // repair stereo entry can produce this result.
    REQUIRE(out.length == 0);
    REQUIRE(out.left == nullptr);
    REQUIRE(out.right == nullptr);

    // The empty range is reported at the far end, so the whole buffer counts as
    // removed head and the tail stays 0. The two still sum to the input length.
    REQUIRE(out.report.range.first == kLength);
    REQUIRE(out.report.range.last_exclusive == kLength);
    REQUIRE(out.report.removed_head_samples == kLength);
    REQUIRE(out.report.removed_tail_samples == 0);

    // The documented contract that a caller need not branch before freeing.
    sonare_free_trim_silence_stereo_result(&out);
  }

  SECTION("padding widens both edges of each channel's range and clamps at the buffer") {
    SonareTrimSilenceConfig padded = peak;
    padded.padding_samples = 1200;

    SonareTrimSilenceStereoResult out{};
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &padded, &out) == SONARE_OK);
    REQUIRE(out.left_range.first == kLeftFirst - 1200);
    REQUIRE(out.left_range.last_exclusive == kLeftEnd + 1200);
    REQUIRE(out.right_range.first == kRightFirst - 1200);
    REQUIRE(out.right_range.last_exclusive == kRightEnd + 1200);
    REQUIRE(out.report.range.first == kLeftFirst - 1200);
    REQUIRE(out.report.range.last_exclusive == kRightEnd + 1200);
    sonare_free_trim_silence_stereo_result(&out);

    // Enough padding to run off both ends: the left channel's head clamps at 0
    // and the right channel's tail at the input length, so the pair comes back
    // whole rather than with a range reaching outside the buffer.
    padded.padding_samples = 6000;
    SonareTrimSilenceStereoResult wide{};
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &padded, &wide) == SONARE_OK);
    REQUIRE(wide.left_range.first == 0);
    REQUIRE(wide.right_range.last_exclusive == kLength);
    REQUIRE(wide.length == kLength);
    REQUIRE(wide.report.removed_head_samples == 0);
    REQUIRE(wide.report.removed_tail_samples == 0);
    sonare_free_trim_silence_stereo_result(&wide);
  }

  SECTION("does not pad a pass that kept nothing") {
    const std::vector<float> quiet(kLength, 0.0f);
    SonareTrimSilenceConfig padded = peak;
    padded.padding_samples = 6000;

    SonareTrimSilenceStereoResult out{};
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(quiet.data(), quiet.data(), kLength, sr,
                                                        &padded, &out) == SONARE_OK);
    // Padding is applied to a range the scan found, and an empty pass found
    // none, so the result stays empty rather than widening out of nothing.
    REQUIRE(out.length == 0);
    REQUIRE(out.report.range.first == kLength);
    REQUIRE(out.report.range.last_exclusive == kLength);
  }

  SECTION("accepts the largest padding count and refuses the next one up") {
    // The bound is CLOSED, so the largest legal count is the endpoint itself --
    // asserting only that a huge value is refused would leave the interval's
    // last step untested and could not tell `>` from `>=`.
    SonareTrimSilenceConfig at_bound = peak;
    at_bound.padding_samples = kMaxPadding;
    SonareTrimSilenceStereoResult out{};
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &at_bound, &out) == SONARE_OK);
    REQUIRE(out.length == kLength);
    sonare_free_trim_silence_stereo_result(&out);

    SonareTrimSilenceConfig over = peak;
    over.padding_samples = kMaxPadding + 1;
    SonareTrimSilenceStereoResult refused{};
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &over, &refused) ==
            SONARE_ERROR_INVALID_PARAMETER);

    // The value a -1 becomes on the way across a language boundary lands well
    // inside the refused half rather than reading as a small positive count.
    SonareTrimSilenceConfig negative = peak;
    negative.padding_samples = kSizeMax;
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &negative, &refused) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("reads threshold only in peak mode and gate_lufs only in the gated one") {
    // Nothing in the fixture reaches -1 dBFS, so in peak mode this empties the pair.
    SonareTrimSilenceConfig deaf_peak = peak;
    deaf_peak.threshold_db = -1.0f;
    SonareTrimSilenceStereoResult out{};
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &deaf_peak, &out) == SONARE_OK);
    REQUIRE(out.length == 0);

    // The same threshold in gated mode: the gate decides instead, so the pair
    // survives. Zero here would read the same whether threshold was ignored or
    // the call had failed, which is why the peak half above is the control.
    SonareTrimSilenceConfig gated = deaf_peak;
    gated.mode = SONARE_TRIM_SILENCE_MODE_LUFS_GATED;
    gated.window_ms = 10.0f;
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &gated, &out) == SONARE_OK);
    REQUIRE(out.length > 0);
    sonare_free_trim_silence_stereo_result(&out);

    // The mirror image: a gate the material cannot clear empties the pair in
    // gated mode and does nothing at all in peak mode. The burst is +-0.5, so
    // its windowed RMS sits near -6 dBFS and a gate at 0 is out of reach.
    SonareTrimSilenceConfig shut = gated;
    shut.threshold_db = -60.0f;
    shut.gate_lufs = 0.0f;
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &shut, &out) == SONARE_OK);
    REQUIRE(out.length == 0);

    SonareTrimSilenceConfig shut_but_peak = shut;
    shut_but_peak.mode = SONARE_TRIM_SILENCE_MODE_PEAK;
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &shut_but_peak, &out) == SONARE_OK);
    REQUIRE(out.length == kRightEnd - kLeftFirst);
    sonare_free_trim_silence_stereo_result(&out);
  }

  SECTION("sizes the gated window from window_ms, which peak mode ignores") {
    SonareTrimSilenceConfig narrow = peak;
    narrow.mode = SONARE_TRIM_SILENCE_MODE_LUFS_GATED;
    narrow.window_ms = 10.0f;
    SonareTrimSilenceStereoResult narrow_out{};
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &narrow, &narrow_out) == SONARE_OK);

    SonareTrimSilenceConfig wide = narrow;
    wide.window_ms = 100.0f;
    SonareTrimSilenceStereoResult wide_out{};
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &wide, &wide_out) == SONARE_OK);

    // The window is centred on the sample under test, so the gate opens half a
    // window before the burst itself and a wider window opens earlier still.
    // Both therefore start ahead of the peak-mode edge rather than on it, and
    // the lead is exactly the radius: one burst sample anywhere in the window
    // already clears -60 dBFS. Measured 4560 and 2400 against the peak edge at
    // 4800, for radii of 240 and 2400 samples.
    REQUIRE(narrow_out.report.range.first < kLeftFirst);
    REQUIRE(wide_out.report.range.first < narrow_out.report.range.first);
    sonare_free_trim_silence_stereo_result(&narrow_out);
    sonare_free_trim_silence_stereo_result(&wide_out);

    // In peak mode the same two values change nothing, which is what makes the
    // comparison above an observation of window_ms rather than of the buffer.
    SonareTrimSilenceConfig peak_narrow = peak;
    peak_narrow.window_ms = 10.0f;
    SonareTrimSilenceConfig peak_wide = peak;
    peak_wide.window_ms = 100.0f;
    SonareTrimSilenceStereoResult a{};
    SonareTrimSilenceStereoResult b{};
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &peak_narrow, &a) == SONARE_OK);
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &peak_wide, &b) == SONARE_OK);
    REQUIRE(a.report.range.first == kLeftFirst);
    REQUIRE(b.report.range.first == kLeftFirst);
    sonare_free_trim_silence_stereo_result(&a);
    sonare_free_trim_silence_stereo_result(&b);
  }

  SECTION("rejects bad inputs and clears the result it was handed") {
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &peak,
                                                        nullptr) == SONARE_ERROR_INVALID_PARAMETER);

    // Pre-filled with values a caller's stack slot could plausibly hold, so a
    // rejected call that forgot to clear would leave them visible.
    SonareTrimSilenceStereoResult out{};
    auto dirty = [&out]() {
      out.left = non_null_sentinel_float_ptr();
      out.right = non_null_sentinel_float_ptr();
      out.length = 123;
      out.report.range.first = 7;
      out.left_range.last_exclusive = 9;
    };
    auto require_cleared = [&out]() {
      REQUIRE(out.left == nullptr);
      REQUIRE(out.right == nullptr);
      REQUIRE(out.length == 0);
      REQUIRE(out.report.range.first == 0);
      REQUIRE(out.left_range.last_exclusive == 0);
    };

    dirty();
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(nullptr, right.data(), kLength, sr, &peak,
                                                        &out) == SONARE_ERROR_INVALID_PARAMETER);
    require_cleared();

    dirty();
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), nullptr, kLength, sr, &peak,
                                                        &out) == SONARE_ERROR_INVALID_PARAMETER);
    require_cleared();

    dirty();
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), 0, sr, &peak,
                                                        &out) == SONARE_ERROR_INVALID_PARAMETER);
    require_cleared();

    std::vector<float> nan_left = left;
    nan_left[10] = std::numeric_limits<float>::quiet_NaN();
    dirty();
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(nan_left.data(), right.data(), kLength, sr,
                                                        &peak,
                                                        &out) == SONARE_ERROR_INVALID_PARAMETER);
    require_cleared();

    SonareTrimSilenceConfig bad_mode = peak;
    bad_mode.mode = 999;
    dirty();
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &bad_mode,
                                                        &out) == SONARE_ERROR_INVALID_PARAMETER);
    require_cleared();

    SonareTrimSilenceConfig bad_threshold = peak;
    bad_threshold.threshold_db = 6.0f;
    dirty();
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &bad_threshold,
                                                        &out) == SONARE_ERROR_INVALID_PARAMETER);
    require_cleared();

    SonareTrimSilenceConfig bad_window = peak;
    bad_window.window_ms = 0.0f;
    dirty();
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        &bad_window,
                                                        &out) == SONARE_ERROR_INVALID_PARAMETER);
    require_cleared();
  }

  SECTION("uses library defaults for a NULL config") {
    SonareTrimSilenceStereoResult out{};
    REQUIRE(sonare_mastering_repair_trim_silence_stereo(left.data(), right.data(), kLength, sr,
                                                        nullptr, &out) == SONARE_OK);
    // The defaults are peak mode at 0.001 with no padding, so a NULL config has
    // to land on exactly the explicit peak result above.
    REQUIRE(out.report.range.first == kLeftFirst);
    REQUIRE(out.report.range.last_exclusive == kRightEnd);
    REQUIRE(out.length == kRightEnd - kLeftFirst);
    sonare_free_trim_silence_stereo_result(&out);
  }
}

namespace {
float max_abs_sample(const float* buf, size_t length) {
  float peak = 0.0f;
  for (size_t i = 0; i < length; ++i) {
    peak = std::max(peak, std::abs(buf[i]));
  }
  return peak;
}
}  // namespace

TEST_CASE("sonare_mastering_dynamics_compressor", "[c_api][mastering]") {
  const int sr = 48000;
  auto samples = generate_sine(440.0f, sr, 0.5f);
  for (auto& s : samples) s *= 0.9f;  // hot signal so compression engages

  SECTION("default config returns finite buffer of the same length") {
    float* out = nullptr;
    size_t out_length = 0;
    int latency = -1;
    REQUIRE(sonare_mastering_dynamics_compressor(samples.data(), samples.size(), sr, nullptr, &out,
                                                 &out_length, &latency) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_length == samples.size());
    REQUIRE(latency >= 0);
    REQUIRE(std::isfinite(max_abs_sample(out, out_length)));
    sonare_free_floats(out);
  }

  SECTION("strong threshold + 4:1 ratio reduces peak vs input") {
    SonareCompressorConfig config;
    sonare_compressor_config_init(&config);
    config.threshold_db = -24.0f;
    config.ratio = 4.0f;
    config.attack_ms = 1.0f;
    config.release_ms = 50.0f;
    config.knee_db = 0.0f;
    config.makeup_gain_db = 0.0f;
    config.auto_makeup = 0;
    config.detector = SONARE_COMPRESSOR_DETECTOR_PEAK;
    config.sidechain_high_pass_hz = 100.0f;
    config.pdr_release_scale = 1.0f;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_dynamics_compressor(samples.data(), samples.size(), sr, &config, &out,
                                                 &out_length, nullptr) == SONARE_OK);
    const float in_peak = max_abs_sample(samples.data(), samples.size());
    const float out_peak = max_abs_sample(out, out_length);
    REQUIRE(out_peak < in_peak);
    sonare_free_floats(out);
  }

  SECTION("NULL output pointer returns invalid parameter") {
    REQUIRE(sonare_mastering_dynamics_compressor(samples.data(), samples.size(), sr, nullptr,
                                                 nullptr, nullptr,
                                                 nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_mastering_dynamics_gate", "[c_api][mastering]") {
  const int sr = 48000;
  // 200 ms of loud tone followed by 200 ms of near-silence.
  std::vector<float> samples;
  auto loud = generate_sine(440.0f, sr, 0.2f);
  samples.insert(samples.end(), loud.begin(), loud.end());
  for (auto& s : loud) s *= 0.0005f;  // -66 dBFS, well below default -50 threshold
  samples.insert(samples.end(), loud.begin(), loud.end());

  SECTION("default config returns finite buffer of the same length") {
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_dynamics_gate(samples.data(), samples.size(), sr, nullptr, &out,
                                           &out_length, nullptr) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_length == samples.size());
    REQUIRE(std::isfinite(max_abs_sample(out, out_length)));
    sonare_free_floats(out);
  }

  SECTION("gate attenuates the silent tail vs input") {
    SonareGateConfig config;
    sonare_gate_config_init(&config);
    config.threshold_db = -40.0f;
    config.attack_ms = 1.0f;
    config.release_ms = 20.0f;
    config.range_db = -60.0f;
    config.close_threshold_db = -40.0f;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_dynamics_gate(samples.data(), samples.size(), sr, &config, &out,
                                           &out_length, nullptr) == SONARE_OK);
    // Last 50 ms of the input should be quieter on output (gated down).
    const size_t tail = static_cast<size_t>(0.05 * sr);
    const float in_tail_peak = max_abs_sample(samples.data() + samples.size() - tail, tail);
    const float out_tail_peak = max_abs_sample(out + out_length - tail, tail);
    REQUIRE(out_tail_peak < in_tail_peak);
    sonare_free_floats(out);
  }
}

TEST_CASE("sonare_mastering_dynamics_transient_shaper", "[c_api][mastering]") {
  const int sr = 48000;
  auto samples = generate_clicks(120.0f, sr, 2.0f);
  for (auto& s : samples) s *= 0.5f;

  SECTION("default config returns finite buffer of the same length") {
    float* out = nullptr;
    size_t out_length = 0;
    int latency = -1;
    REQUIRE(sonare_mastering_dynamics_transient_shaper(samples.data(), samples.size(), sr, nullptr,
                                                       &out, &out_length, &latency) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_length == samples.size());
    REQUIRE(latency >= 0);
    REQUIRE(std::isfinite(max_abs_sample(out, out_length)));
    sonare_free_floats(out);
  }

  SECTION("boosted attack lifts the click peaks") {
    SonareTransientShaperConfig config;
    sonare_transient_shaper_config_init(&config);
    config.attack_gain_db = 9.0f;
    config.sustain_gain_db = 0.0f;
    config.fast_attack_ms = 0.0f;
    config.fast_release_ms = 10.0f;
    config.slow_attack_ms = 30.0f;
    config.slow_release_ms = 200.0f;
    config.sensitivity = 1.0f;
    config.max_gain_db = 12.0f;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_dynamics_transient_shaper(samples.data(), samples.size(), sr, &config,
                                                       &out, &out_length, nullptr) == SONARE_OK);
    const float in_peak = max_abs_sample(samples.data(), samples.size());
    const float out_peak = max_abs_sample(out, out_length);
    REQUIRE(out_peak > in_peak);
    sonare_free_floats(out);
  }
}
TEST_CASE("sonare_mastering_repair_analyze and sonare_mastering_repair_apply",
          "[c_api][mastering]") {
  const int sr = 22050;
  auto left = generate_sine(440.0f, sr, 0.25f);
  auto right = generate_sine(660.0f, sr, 0.25f);
  for (auto& s : left) s *= 0.3f;
  for (auto& s : right) s *= 0.3f;
  for (size_t i = 1000; i < left.size(); i += 1500) left[i] = 0.95f;
  const float* channels[2] = {left.data(), right.data()};
  const size_t length = left.size();

  SECTION("analysis clears its output first and refuses a missing channel") {
    char* json = reinterpret_cast<char*>(static_cast<std::uintptr_t>(0x1));
    const float* missing[2] = {left.data(), nullptr};
    REQUIRE(sonare_mastering_repair_analyze(missing, 2, length, sr, nullptr, &json) ==
            SONARE_ERROR_INVALID_PARAMETER);
    CHECK(json == nullptr);
    REQUIRE(sonare_mastering_repair_analyze(channels, 2, length, sr, nullptr, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_mastering_repair_analyze(channels, 2, length, sr, R"({"preferStreamingSafe":1})",
                                            &json) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(json == nullptr);
  }

  SECTION("analysis recommends declick for the clicked channel") {
    char* json = nullptr;
    REQUIRE(sonare_mastering_repair_analyze(channels, 2, length, sr, nullptr, &json) == SONARE_OK);
    REQUIRE(json != nullptr);
    const std::string text(json);
    sonare_free_string(json);
    CHECK(text.find(R"("stage":"declick")") != std::string::npos);
    CHECK(text.find(R"("declipThresholdSafe")") != std::string::npos);
  }

  SECTION("apply writes the planes in place and returns the reports") {
    std::vector<float> a = left;
    std::vector<float> b = right;
    float* planes[2] = {a.data(), b.data()};
    const float* inputs[2] = {a.data(), b.data()};
    char* reports = nullptr;
    REQUIRE(sonare_mastering_repair_apply(inputs, 2, length, sr, R"([{"stage":"declick"}])",
                                          nullptr, nullptr, planes, &reports, nullptr,
                                          nullptr) == SONARE_OK);
    REQUIRE(reports != nullptr);
    CHECK(std::string(reports).find(R"("scope":"channel")") != std::string::npos);
    sonare_free_string(reports);
    CHECK(std::abs(a[1000]) < 0.5f);
  }

  SECTION("apply refuses a repeated stage and leaves the planes alone") {
    constexpr float kSentinel = 7.5f;
    std::vector<float> a(length, kSentinel);
    std::vector<float> b(length, kSentinel);
    float* planes[2] = {a.data(), b.data()};
    char* reports = reinterpret_cast<char*>(static_cast<std::uintptr_t>(0x1));
    REQUIRE(sonare_mastering_repair_apply(
                channels, 2, length, sr, R"([{"stage":"dehum"},{"stage":"dehum"}])", nullptr,
                nullptr, planes, &reports, nullptr, nullptr) == SONARE_ERROR_INVALID_FORMAT);
    CHECK(reports == nullptr);
    CHECK(std::all_of(a.begin(), a.end(), [](float s) { return s == kSentinel; }));
    float* missing[2] = {a.data(), nullptr};
    REQUIRE(sonare_mastering_repair_apply(channels, 2, length, sr, "[]", nullptr, nullptr, missing,
                                          &reports, nullptr,
                                          nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("apply reports progress and returns CANCELLED when asked to stop") {
    struct Seen {
      std::vector<std::string> stages;
    } seen;
    std::vector<float> a(length, 7.5f);
    std::vector<float> b(length, 7.5f);
    float* planes[2] = {a.data(), b.data()};
    char* reports = nullptr;
    const auto progress = [](float, const char* stage, void* user_data) {
      static_cast<Seen*>(user_data)->stages.emplace_back(stage);
    };
    const auto stop = [](void*) { return 1; };
    REQUIRE(sonare_mastering_repair_apply(
                channels, 2, length, sr, R"([{"stage":"decrackle"},{"stage":"declick"}])", progress,
                &seen, planes, &reports, stop, nullptr) == SONARE_ERROR_CANCELLED);
    CHECK(reports == nullptr);
    CHECK(seen.stages == std::vector<std::string>{"repair.declick"});
    CHECK(a[0] == 7.5f);
  }
}

TEST_CASE("repair and dynamics configs are refused until their init sets struct_size",
          "[c_api][mastering]") {
  const int sr = 48000;
  const auto samples = generate_sine(440.0f, sr, 0.25f);
  const auto refused_by = [](const char* init_name) {
    CHECK(std::string(sonare_last_error_message()) ==
          std::string("config.struct_size must be set by ") + init_name);
  };

  SECTION("repair configs") {
    {
      SonareDeclickConfig zeroed{};
      SonareDeclickResult out{};
      REQUIRE(sonare_mastering_repair_declick(samples.data(), samples.size(), sr, &zeroed, &out) ==
              SONARE_ERROR_INVALID_PARAMETER);
      refused_by("sonare_declick_config_init");
    }
    {
      SonareDenoiseClassicalConfig zeroed{};
      SonareDenoiseResult out{};
      REQUIRE(sonare_mastering_repair_denoise_classical(samples.data(), samples.size(), sr, &zeroed,
                                                        &out) == SONARE_ERROR_INVALID_PARAMETER);
      refused_by("sonare_denoise_classical_config_init");
    }
    {
      SonareDeclipConfig zeroed{};
      SonareDeclipResult out{};
      REQUIRE(sonare_mastering_repair_declip(samples.data(), samples.size(), sr, &zeroed, &out) ==
              SONARE_ERROR_INVALID_PARAMETER);
      refused_by("sonare_declip_config_init");
    }
    {
      SonareDecrackleConfig zeroed{};
      SonareDecrackleResult out{};
      REQUIRE(sonare_mastering_repair_decrackle(samples.data(), samples.size(), sr, &zeroed,
                                                &out) == SONARE_ERROR_INVALID_PARAMETER);
      refused_by("sonare_decrackle_config_init");
    }
    {
      SonareDehumConfig zeroed{};
      SonareDehumResult out{};
      REQUIRE(sonare_mastering_repair_dehum(samples.data(), samples.size(), sr, &zeroed, &out) ==
              SONARE_ERROR_INVALID_PARAMETER);
      refused_by("sonare_dehum_config_init");
    }
    {
      SonareDereverbClassicalConfig zeroed{};
      SonareDereverbResult out{};
      REQUIRE(sonare_mastering_repair_dereverb_classical(samples.data(), samples.size(), sr,
                                                         &zeroed,
                                                         &out) == SONARE_ERROR_INVALID_PARAMETER);
      refused_by("sonare_dereverb_classical_config_init");
    }
    {
      SonareTrimSilenceConfig zeroed{};
      SonareTrimSilenceResult out{};
      REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, &zeroed,
                                                   &out) == SONARE_ERROR_INVALID_PARAMETER);
      refused_by("sonare_trim_silence_config_init");
    }
  }

  SECTION("dynamics configs") {
    float* out = nullptr;
    size_t out_length = 0;
    {
      SonareCompressorConfig zeroed{};
      REQUIRE(sonare_mastering_dynamics_compressor(samples.data(), samples.size(), sr, &zeroed,
                                                   &out, &out_length,
                                                   nullptr) == SONARE_ERROR_INVALID_PARAMETER);
      refused_by("sonare_compressor_config_init");
    }
    {
      SonareGateConfig zeroed{};
      REQUIRE(sonare_mastering_dynamics_gate(samples.data(), samples.size(), sr, &zeroed, &out,
                                             &out_length,
                                             nullptr) == SONARE_ERROR_INVALID_PARAMETER);
      refused_by("sonare_gate_config_init");
    }
    {
      SonareTransientShaperConfig zeroed{};
      REQUIRE(sonare_mastering_dynamics_transient_shaper(samples.data(), samples.size(), sr,
                                                         &zeroed, &out, &out_length, nullptr) ==
              SONARE_ERROR_INVALID_PARAMETER);
      refused_by("sonare_transient_shaper_config_init");
    }
  }
}

TEST_CASE("compressor sidechain high-pass is one frequency and 0 is off", "[c_api][mastering]") {
  const int sr = 48000;
  auto samples = generate_sine(110.0f, sr, 0.5f);
  for (auto& s : samples) s *= 0.9f;

  SonareCompressorConfig config;
  sonare_compressor_config_init(&config);
  REQUIRE(config.sidechain_high_pass_hz == 0.0f);
  config.threshold_db = -24.0f;
  config.ratio = 4.0f;
  config.detector = SONARE_COMPRESSOR_DETECTOR_PEAK;

  const auto run = [&](float high_pass_hz) {
    config.sidechain_high_pass_hz = high_pass_hz;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_dynamics_compressor(samples.data(), samples.size(), sr, &config, &out,
                                                 &out_length, nullptr) == SONARE_OK);
    const float peak = max_abs(out, out_length);
    sonare_free_floats(out);
    return peak;
  };
  // A 2 kHz corner removes the 110 Hz tone from the detector, so less is compressed.
  REQUIRE(run(2000.0f) > run(0.0f));

  config.sidechain_high_pass_hz = -1.0f;
  float* out = nullptr;
  size_t out_length = 0;
  REQUIRE(sonare_mastering_dynamics_compressor(samples.data(), samples.size(), sr, &config, &out,
                                               &out_length,
                                               nullptr) == SONARE_ERROR_INVALID_PARAMETER);
}

#endif
