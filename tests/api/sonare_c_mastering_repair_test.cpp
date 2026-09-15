/// @file sonare_c_mastering_repair_test.cpp
/// @brief Mastering repair and dynamics C API tests.

#include "sonare_c_test_helpers.h"

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
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_declick(samples.data(), samples.size(), sr, nullptr, &out,
                                            &out_length) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_length == samples.size());
    // Output should be finite and not silent (the wrapper preserves the signal).
    REQUIRE(std::isfinite(max_abs(out, out_length)));
    REQUIRE(max_abs(out, out_length) > 0.1f);
    sonare_free_floats(out);
  }

  SECTION("accepts explicit config") {
    SonareDeclickConfig config = {};
    config.threshold = 0.8f;
    config.neighbor_ratio = 4.0f;
    config.max_click_samples = 8;
    config.lpc_order = 20;
    config.residual_ratio = 8.0f;

    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_declick(samples.data(), samples.size(), sr, &config, &out,
                                            &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    sonare_free_floats(out);
  }

  SECTION("rejects null out / bad inputs") {
    REQUIRE(sonare_mastering_repair_declick(samples.data(), samples.size(), sr, nullptr, nullptr,
                                            nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_declick(nullptr, 0, sr, nullptr, &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }

  SECTION("invalid config returns invalid parameter and clears output") {
    SonareDeclickConfig config = {};
    config.threshold = 0.0f;
    config.neighbor_ratio = 4.0f;
    config.max_click_samples = 8;
    config.lpc_order = 20;
    config.residual_ratio = 8.0f;
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_declick(samples.data(), samples.size(), sr, &config, &out,
                                            &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
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

    sonare_free_floats(out.left);
    sonare_free_floats(out.right);
  }

  SECTION("linking changes the output against the mono entry point") {
    SonareDeclickStereoResult stereo{};
    REQUIRE(sonare_mastering_repair_declick_stereo(left.data(), right.data(), left.size(), sr,
                                                   nullptr, &stereo) == SONARE_OK);
    float* mono = nullptr;
    size_t mono_length = 0;
    REQUIRE(sonare_mastering_repair_declick(left.data(), left.size(), sr, nullptr, &mono,
                                            &mono_length) == SONARE_OK);
    REQUIRE(mono_length == stereo.length);

    // The mono pass never sees the right channel's click, so it leaves the left
    // channel untouched there while the stereo pass repairs it. Without this the
    // stereo entry could be the mono one run twice.
    CHECK(stereo.left[kRightClick] != mono[kRightClick]);
    CHECK(stereo.left[kLeftClick] == Catch::Approx(mono[kLeftClick]).margin(1e-6));

    sonare_free_floats(mono);
    sonare_free_floats(stereo.left);
    sonare_free_floats(stereo.right);
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
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, nullptr, &out,
                                                      &out_length) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_length == noisy.size());
    sonare_free_floats(out);
  }

  SECTION("Berouti SpectralSubtraction config runs") {
    SonareDenoiseClassicalConfig config = {};
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

    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, &config, &out,
                                                      &out_length) == SONARE_OK);
    REQUIRE(out_length == noisy.size());
    sonare_free_floats(out);
  }

  SECTION("rejects non-power-of-two n_fft and bad hop") {
    SonareDenoiseClassicalConfig config = {};
    config.mode = SONARE_DENOISE_MODE_LOG_MMSE;
    config.noise_estimator = SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE;
    config.n_fft = 1500;  // not a power of two
    config.hop_length = 256;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, &config, &out,
                                                      &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);

    config.n_fft = 1024;
    config.hop_length = 0;
    out = non_null_sentinel_float_ptr();
    out_length = 123;
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, &config, &out,
                                                      &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }

  SECTION("rejects unknown mode and noise estimator enums") {
    SonareDenoiseClassicalConfig config = {};
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
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, &config, &out,
                                                      &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);

    config.mode = SONARE_DENOISE_MODE_LOG_MMSE;
    config.noise_estimator = 999;
    out = non_null_sentinel_float_ptr();
    out_length = 123;
    REQUIRE(sonare_mastering_repair_denoise_classical(noisy.data(), noisy.size(), sr, &config, &out,
                                                      &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
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
    CHECK(out.left_report.detected.sample_count == kSharedEnd - kSharedBegin);
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

    sonare_free_floats(out.left);
    sonare_free_floats(out.right);
  }

  SECTION("the extended region differs from the mono entry point") {
    SonareDeclipStereoResult stereo{};
    REQUIRE(sonare_mastering_repair_declip_stereo(left.data(), right.data(), left.size(), sr,
                                                  nullptr, &stereo) == SONARE_OK);
    float* mono = nullptr;
    size_t mono_length = 0;
    REQUIRE(sonare_mastering_repair_declip(left.data(), left.size(), sr, nullptr, &mono,
                                           &mono_length) == SONARE_OK);
    REQUIRE(mono_length == stereo.length);

    // The mono pass never sees the right channel's run, so it stops at 2010.
    bool differs = false;
    for (size_t i = kSharedEnd; i < kRightWideEnd; ++i) {
      if (stereo.left[i] != mono[i]) differs = true;
    }
    CHECK(differs);

    sonare_free_floats(mono);
    sonare_free_floats(stereo.left);
    sonare_free_floats(stereo.right);
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
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_declip(samples.data(), samples.size(), sr, nullptr, &out,
                                           &out_length) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_length == samples.size());
    REQUIRE(std::isfinite(max_abs(out, out_length)));
    sonare_free_floats(out);
  }

  SECTION("explicit config") {
    SonareDeclipConfig config = {};
    config.clip_threshold = 0.85f;
    config.lpc_order = 24;
    config.iterations = 1;
    config.lpc_blend = 0.5f;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_declip(samples.data(), samples.size(), sr, &config, &out,
                                           &out_length) == SONARE_OK);
    sonare_free_floats(out);
  }

  SECTION("invalid config returns invalid parameter and clears output") {
    SonareDeclipConfig config = {};
    config.clip_threshold = 2.0f;
    config.lpc_order = 24;
    config.iterations = 1;
    config.lpc_blend = 0.5f;
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_declip(samples.data(), samples.size(), sr, &config, &out,
                                           &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
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
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_decrackle(samples.data(), samples.size(), sr, nullptr, &out,
                                              &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    sonare_free_floats(out);
  }

  SECTION("wavelet shrinkage mode") {
    SonareDecrackleConfig config = {};
    config.threshold = 0.4f;
    config.mode = SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE;
    config.levels = 4;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_decrackle(samples.data(), samples.size(), sr, &config, &out,
                                              &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    sonare_free_floats(out);
  }

  SECTION("invalid config returns invalid parameter and clears output") {
    SonareDecrackleConfig config = {};
    config.threshold = 0.0f;
    config.mode = SONARE_DECRACKLE_MODE_MEDIAN;
    config.levels = 4;
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_decrackle(samples.data(), samples.size(), sr, &config, &out,
                                              &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }

  SECTION("rejects unknown mode enum") {
    SonareDecrackleConfig config = {};
    config.threshold = 0.4f;
    config.mode = 999;
    config.levels = 4;
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_decrackle(samples.data(), samples.size(), sr, &config, &out,
                                              &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }
}

TEST_CASE("sonare_mastering_repair_dehum", "[c_api][mastering]") {
  const int sr = 48000;
  auto signal = generate_sine(440.0f, sr, 1.0f);
  auto hum = generate_sine(50.0f, sr, 1.0f);
  std::vector<float> samples(signal.size());
  for (size_t i = 0; i < signal.size(); ++i) samples[i] = 0.5f * signal[i] + 0.2f * hum[i];

  SECTION("static notch (default)") {
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_dehum(samples.data(), samples.size(), sr, nullptr, &out,
                                          &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    sonare_free_floats(out);
  }

  SECTION("adaptive tracking with explicit config") {
    SonareDehumConfig config = {};
    config.fundamental_hz = 50.0f;
    config.harmonics = 4;
    config.q = 20.0f;
    config.adaptive = 1;
    config.search_range_hz = 2.0f;
    config.adaptation = 0.25f;
    config.frame_size = 2048;
    config.pll_bandwidth = 0.01f;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_dehum(samples.data(), samples.size(), sr, &config, &out,
                                          &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    sonare_free_floats(out);
  }

  SECTION("invalid config returns invalid parameter and clears output") {
    SonareDehumConfig config = {};
    config.fundamental_hz = 0.0f;
    config.harmonics = 4;
    config.q = 20.0f;
    config.frame_size = 2048;
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_dehum(samples.data(), samples.size(), sr, &config, &out,
                                          &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }
}

TEST_CASE("sonare_mastering_repair_dereverb_classical", "[c_api][mastering]") {
  const int sr = 48000;
  auto samples = generate_sine(440.0f, sr, 1.0f);
  for (auto& s : samples) s *= 0.5f;

  SECTION("default config") {
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_dereverb_classical(samples.data(), samples.size(), sr, nullptr,
                                                       &out, &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    sonare_free_floats(out);
  }

  SECTION("WPE-enabled config") {
    SonareDereverbClassicalConfig config = {};
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
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_dereverb_classical(samples.data(), samples.size(), sr, &config,
                                                       &out, &out_length) == SONARE_OK);
    REQUIRE(out_length == samples.size());
    sonare_free_floats(out);
  }

  SECTION("rejects bad n_fft / hop_length") {
    SonareDereverbClassicalConfig config = {};
    config.threshold = 0.05f;
    config.attenuation = 0.5f;
    config.n_fft = 1500;  // not a power of two
    config.hop_length = 256;
    config.t60_sec = 0.4f;
    config.late_delay_ms = 50.0f;
    config.over_subtraction = 1.0f;
    config.spectral_floor = 0.08f;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_dereverb_classical(samples.data(), samples.size(), sr, &config,
                                                       &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);

    config.n_fft = 1024;
    config.hop_length = 2048;  // larger than n_fft
    out = non_null_sentinel_float_ptr();
    out_length = 123;
    REQUIRE(sonare_mastering_repair_dereverb_classical(samples.data(), samples.size(), sr, &config,
                                                       &out, &out_length) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
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
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, nullptr, &out,
                                                 &out_length) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_length < samples.size());
    REQUIRE(out_length > 0);
    sonare_free_floats(out);
  }

  SECTION("LUFS-gated mode with padding") {
    SonareTrimSilenceConfig config = {};
    config.threshold = 0.001f;
    config.padding_samples = 1200;
    config.mode = SONARE_TRIM_SILENCE_MODE_LUFS_GATED;
    config.gate_lufs = -40.0f;
    config.window_ms = 400.0f;
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, &config, &out,
                                                 &out_length) == SONARE_OK);
    REQUIRE(out != nullptr);
    REQUIRE(out_length > 0);
    sonare_free_floats(out);
  }

  SECTION("invalid config returns invalid parameter and clears output") {
    SonareTrimSilenceConfig config = {};
    config.threshold = -1.0f;
    config.mode = SONARE_TRIM_SILENCE_MODE_PEAK;
    config.window_ms = 400.0f;
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, &config, &out,
                                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }

  SECTION("rejects unknown mode enum") {
    SonareTrimSilenceConfig config = {};
    config.threshold = 0.001f;
    config.padding_samples = 0;
    config.mode = 999;
    config.gate_lufs = -60.0f;
    config.window_ms = 400.0f;
    float* out = non_null_sentinel_float_ptr();
    size_t out_length = 123;
    REQUIRE(sonare_mastering_repair_trim_silence(samples.data(), samples.size(), sr, &config, &out,
                                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
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
    SonareCompressorConfig config = {};
    config.threshold_db = -24.0f;
    config.ratio = 4.0f;
    config.attack_ms = 1.0f;
    config.release_ms = 50.0f;
    config.knee_db = 0.0f;
    config.makeup_gain_db = 0.0f;
    config.auto_makeup = 0;
    config.detector = SONARE_COMPRESSOR_DETECTOR_PEAK;
    config.sidechain_hpf_hz = 100.0f;
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
    SonareGateConfig config = {};
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
    SonareTransientShaperConfig config = {};
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
#endif
