/// @file sonare_c_features_metering_test.cpp
/// @brief Feature and metering C API tests.

#include "sonare_c_test_helpers.h"

TEST_CASE("sonare_onset_strength", "[c_api]") {
  SECTION("returns a finite onset envelope") {
    auto samples = generate_clicks(120.0f, 22050, 4.0f);
    float* env = nullptr;
    size_t count = 0;

    SonareError err =
        sonare_onset_strength(samples.data(), samples.size(), 22050, 2048, 512, 128, &env, &count);

    REQUIRE(err == SONARE_OK);
    REQUIRE(count > 0);
    REQUIRE(env != nullptr);
    for (size_t i = 0; i < count; ++i) {
      REQUIRE(std::isfinite(env[i]));
    }
    sonare_free_floats(env);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_clicks(120.0f, 22050, 1.0f);
    float* env = nullptr;
    size_t count = 0;

    REQUIRE(sonare_onset_strength(nullptr, samples.size(), 22050, 2048, 512, 128, &env, &count) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_onset_strength(samples.data(), samples.size(), 22050, 2048, 512, 128, nullptr,
                                  &count) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_onset_strength(samples.data(), samples.size(), 22050, 2048, 512, 128, &env,
                                  nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_onset_strength_multi", "[c_api]") {
  auto samples = generate_clicks(120.0f, 22050, 2.0f);
  float* env = nullptr;
  size_t count = 0;
  int frames = 0;

  REQUIRE(sonare_onset_strength_multi(samples.data(), samples.size(), 22050, 1024, 256, 64, 4, &env,
                                      &count, &frames) == SONARE_OK);
  REQUIRE(frames > 0);
  REQUIRE(count == static_cast<size_t>(frames) * 4u);
  REQUIRE(env != nullptr);
  for (size_t i = 0; i < count; ++i) REQUIRE(std::isfinite(env[i]));
  sonare_free_floats(env);

  REQUIRE(sonare_onset_strength_multi(samples.data(), samples.size(), 22050, 1024, 256, 64, 0, &env,
                                      &count, &frames) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare pseudo/hybrid CQT C API", "[c_api]") {
  auto samples = generate_sine(440.0f, 22050, 1.0f);
  SonareCqtResult result{};

  REQUIRE(sonare_pseudo_cqt(samples.data(), samples.size(), 22050, 256, 55.0f, 36, 12, &result) ==
          SONARE_OK);
  REQUIRE(result.n_bins == 36);
  REQUIRE(result.n_frames > 0);
  REQUIRE(result.magnitude != nullptr);
  REQUIRE(result.frequencies != nullptr);
  sonare_free_cqt_result(&result);
  REQUIRE(result.magnitude == nullptr);

  REQUIRE(sonare_hybrid_cqt(samples.data(), samples.size(), 22050, 256, 55.0f, 36, 12, &result) ==
          SONARE_OK);
  REQUIRE(result.n_bins == 36);
  REQUIRE(result.n_frames > 0);
  REQUIRE(result.magnitude != nullptr);
  sonare_free_cqt_result(&result);

  REQUIRE(sonare_pseudo_cqt(samples.data(), samples.size(), 22050, 0, 55.0f, 36, 12, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare chroma_cens and bass_chroma C API", "[c_api]") {
  auto samples = generate_sine(110.0f, 22050, 1.0f);
  SonareChromaResult result{};

  REQUIRE(sonare_chroma_cens(samples.data(), samples.size(), 22050, 512, 12, &result) == SONARE_OK);
  REQUIRE(result.n_chroma == 12);
  REQUIRE(result.n_frames > 0);
  REQUIRE(result.features != nullptr);
  REQUIRE(result.mean_energy != nullptr);
  sonare_free_chroma_result(&result);
  REQUIRE(result.features == nullptr);

  REQUIRE(sonare_bass_chroma(samples.data(), samples.size(), 22050, 512, 12, &result) == SONARE_OK);
  REQUIRE(result.n_chroma == 12);
  REQUIRE(result.n_frames > 0);
  REQUIRE(result.features != nullptr);
  sonare_free_chroma_result(&result);

  REQUIRE(sonare_chroma_cens(samples.data(), samples.size(), 22050, 0, 12, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare_fourier_tempogram", "[c_api]") {
  SECTION("returns an [n_bins x n_frames] magnitude matrix") {
    auto samples = generate_clicks(120.0f, 22050, 4.0f);
    float* env = nullptr;
    size_t env_count = 0;
    REQUIRE(sonare_onset_strength(samples.data(), samples.size(), 22050, 2048, 512, 128, &env,
                                  &env_count) == SONARE_OK);

    const int win_length = 384;
    float* data = nullptr;
    size_t out_length = 0;
    int n_frames = 0;
    SonareError err = sonare_fourier_tempogram(env, env_count, 22050, 512, win_length, 1, 1, &data,
                                               &out_length, &n_frames);

    REQUIRE(err == SONARE_OK);
    REQUIRE(data != nullptr);
    REQUIRE(n_frames == static_cast<int>(env_count));
    const size_t n_bins = static_cast<size_t>(win_length) / 2 + 1;
    REQUIRE(out_length == n_bins * static_cast<size_t>(n_frames));
    for (size_t i = 0; i < out_length; ++i) {
      REQUIRE(std::isfinite(data[i]));
    }

    sonare_free_floats(data);
    sonare_free_floats(env);
  }

  SECTION("rejects invalid parameters") {
    std::vector<float> env(256, 0.1f);
    float* data = nullptr;
    size_t out_length = 0;
    int n_frames = 0;

    REQUIRE(sonare_fourier_tempogram(nullptr, env.size(), 22050, 512, 384, 1, 1, &data, &out_length,
                                     &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_fourier_tempogram(env.data(), env.size(), 22050, 512, 384, 1, 1, &data,
                                     &out_length, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_tempogram_with_mode", "[c_api]") {
  const std::vector<float> env{0.2f, 1.0f, 0.4f, 0.0f, 0.8f, 0.1f, 0.5f, 0.3f,
                               0.6f, 0.0f, 0.9f, 0.2f, 0.4f, 0.7f, 0.1f, 0.5f};
  float* data = nullptr;
  size_t out_length = 0;
  int n_frames = 0;
  SonareError err =
      sonare_tempogram_with_mode(env.data(), env.size(), 8, 1, 8, 0, 0, SONARE_TEMPOGRAM_COSINE,
                                 &data, &out_length, &n_frames);

  REQUIRE(err == SONARE_OK);
  REQUIRE(data != nullptr);
  REQUIRE(n_frames == static_cast<int>(env.size()));
  REQUIRE(out_length == env.size() * 8);
  for (size_t i = 0; i < out_length; ++i) {
    REQUIRE(std::isfinite(data[i]));
    REQUIRE(data[i] >= -1.0f - 1.0e-6f);
    REQUIRE(data[i] <= 1.0f + 1.0e-6f);
  }
  sonare_free_floats(data);

  REQUIRE(sonare_tempogram_with_mode(env.data(), env.size(), 8, 1, 8, 0, 0, 99, &data, &out_length,
                                     &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare_tempogram_ratio", "[c_api]") {
  SECTION("returns one finite value per factor") {
    auto samples = generate_clicks(120.0f, 22050, 4.0f);
    float* env = nullptr;
    size_t env_count = 0;
    REQUIRE(sonare_onset_strength(samples.data(), samples.size(), 22050, 2048, 512, 128, &env,
                                  &env_count) == SONARE_OK);

    const int win_length = 384;
    float* tg = nullptr;
    size_t tg_length = 0;
    int tg_frames = 0;
    REQUIRE(sonare_tempogram(env, env_count, 22050, 512, win_length, 1, 1, &tg, &tg_length,
                             &tg_frames) == SONARE_OK);

    SECTION("default factors") {
      float* ratio = nullptr;
      size_t ratio_count = 0;
      SonareError err = sonare_tempogram_ratio(tg, tg_length, win_length, 22050, 512, nullptr, 0,
                                               &ratio, &ratio_count);
      REQUIRE(err == SONARE_OK);
      REQUIRE(ratio != nullptr);
      REQUIRE(ratio_count == 5);  // {0.5, 1, 2, 3, 4}
      for (size_t i = 0; i < ratio_count; ++i) {
        REQUIRE(std::isfinite(ratio[i]));
      }
      sonare_free_floats(ratio);
    }

    SECTION("explicit factors") {
      const float factors[] = {1.0f, 2.0f, 3.0f};
      float* ratio = nullptr;
      size_t ratio_count = 0;
      SonareError err = sonare_tempogram_ratio(tg, tg_length, win_length, 22050, 512, factors, 3,
                                               &ratio, &ratio_count);
      REQUIRE(err == SONARE_OK);
      REQUIRE(ratio_count == 3);
      for (size_t i = 0; i < ratio_count; ++i) {
        REQUIRE(std::isfinite(ratio[i]));
      }
      sonare_free_floats(ratio);
    }

    sonare_free_floats(tg);
    sonare_free_floats(env);
  }

  SECTION("rejects invalid parameters") {
    std::vector<float> tg(384 * 4, 0.1f);
    const float factors[] = {1.0f};
    float* ratio = nullptr;
    size_t ratio_count = 0;

    REQUIRE(sonare_tempogram_ratio(nullptr, tg.size(), 384, 22050, 512, factors, 1, &ratio,
                                   &ratio_count) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_tempogram_ratio(tg.data(), tg.size(), 384, 22050, 512, nullptr, 2, &ratio,
                                   &ratio_count) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_nnls_chroma", "[c_api]") {
  SECTION("returns a 12 x n_frames chroma matrix") {
    auto samples = generate_chord({261.63f, 329.63f, 392.00f}, 22050, 2.0f);
    float* data = nullptr;
    size_t out_length = 0;
    int n_frames = 0;

    SonareError err =
        sonare_nnls_chroma(samples.data(), samples.size(), 22050, &data, &out_length, &n_frames);

    REQUIRE(err == SONARE_OK);
    REQUIRE(data != nullptr);
    REQUIRE(n_frames > 0);
    REQUIRE(out_length == 12u * static_cast<size_t>(n_frames));
    for (size_t i = 0; i < out_length; ++i) {
      REQUIRE(std::isfinite(data[i]));
      REQUIRE(data[i] >= 0.0f);  // NNLS output is non-negative
    }
    sonare_free_floats(data);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_sine(440.0f, 22050, 1.0f);
    float* data = nullptr;
    size_t out_length = 0;
    int n_frames = 0;

    REQUIRE(sonare_nnls_chroma(nullptr, samples.size(), 22050, &data, &out_length, &n_frames) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_nnls_chroma(samples.data(), samples.size(), 22050, nullptr, &out_length,
                               &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_nnls_chroma(samples.data(), samples.size(), 22050, &data, nullptr, &n_frames) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_nnls_chroma(samples.data(), samples.size(), 22050, &data, &out_length,
                               nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_lufs", "[c_api]") {
  SECTION("returns finite loudness measures") {
    auto samples = generate_sine(440.0f, 48000, 3.0f);
    SonareLufsResult result = {};

    SonareError err = sonare_lufs(samples.data(), samples.size(), 48000, &result);

    REQUIRE(err == SONARE_OK);
    REQUIRE(std::isfinite(result.integrated_lufs));
    REQUIRE(std::isfinite(result.momentary_lufs));
    REQUIRE(std::isfinite(result.short_term_lufs));
    REQUIRE(std::isfinite(result.loudness_range));
    REQUIRE(result.loudness_range >= 0.0f);
  }

  SECTION("louder signal measures higher integrated LUFS") {
    auto loud = generate_sine(440.0f, 48000, 3.0f);
    auto quiet = loud;
    for (auto& sample : quiet) sample *= 0.1f;

    SonareLufsResult loud_result = {};
    SonareLufsResult quiet_result = {};
    REQUIRE(sonare_lufs(loud.data(), loud.size(), 48000, &loud_result) == SONARE_OK);
    REQUIRE(sonare_lufs(quiet.data(), quiet.size(), 48000, &quiet_result) == SONARE_OK);

    REQUIRE(loud_result.integrated_lufs > quiet_result.integrated_lufs);
  }

  SECTION("rejects invalid parameters") {
    auto samples = generate_sine(440.0f, 48000, 1.0f);
    SonareLufsResult result = {};

    REQUIRE(sonare_lufs(nullptr, samples.size(), 48000, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_lufs(samples.data(), samples.size(), 48000, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_momentary_lufs and sonare_short_term_lufs", "[c_api]") {
  SECTION("return finite time series") {
    auto samples = generate_sine(440.0f, 48000, 3.0f);

    float* momentary = nullptr;
    size_t momentary_count = 0;
    REQUIRE(sonare_momentary_lufs(samples.data(), samples.size(), 48000, &momentary,
                                  &momentary_count) == SONARE_OK);
    REQUIRE(momentary != nullptr);
    REQUIRE(momentary_count > 0);
    for (size_t i = 0; i < momentary_count; ++i) {
      REQUIRE(std::isfinite(momentary[i]));
    }
    sonare_free_floats(momentary);

    float* short_term = nullptr;
    size_t short_term_count = 0;
    REQUIRE(sonare_short_term_lufs(samples.data(), samples.size(), 48000, &short_term,
                                   &short_term_count) == SONARE_OK);
    REQUIRE(short_term != nullptr);
    REQUIRE(short_term_count > 0);
    for (size_t i = 0; i < short_term_count; ++i) {
      REQUIRE(std::isfinite(short_term[i]));
    }
    sonare_free_floats(short_term);
  }

  SECTION("reject invalid parameters") {
    auto samples = generate_sine(440.0f, 48000, 1.0f);
    float* out = nullptr;
    size_t count = 0;

    REQUIRE(sonare_momentary_lufs(nullptr, samples.size(), 48000, &out, &count) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_short_term_lufs(nullptr, samples.size(), 48000, &out, &count) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_metering basic offline meters", "[c_api]") {
  SECTION("peak / rms / crest / dc agree with library") {
    auto samples = generate_sine(440.0f, 48000, 1.0f);
    float peak = 0.0f;
    float rms = 0.0f;
    float crest = 0.0f;
    float dc = 0.0f;
    REQUIRE(sonare_metering_peak_db(samples.data(), samples.size(), 48000, &peak) == SONARE_OK);
    REQUIRE(sonare_metering_rms_db(samples.data(), samples.size(), 48000, &rms) == SONARE_OK);
    REQUIRE(sonare_metering_crest_factor_db(samples.data(), samples.size(), 48000, &crest) ==
            SONARE_OK);
    REQUIRE(sonare_metering_dc_offset(samples.data(), samples.size(), 48000, &dc) == SONARE_OK);
    REQUIRE(std::isfinite(peak));
    REQUIRE(std::isfinite(rms));
    REQUIRE(peak >= rms);
    REQUIRE(crest == Catch::Approx(peak - rms).margin(1e-3f));
    REQUIRE(std::abs(dc) < 1e-2f);
  }

  SECTION("rejects null output and bad inputs") {
    auto samples = generate_sine(440.0f, 48000, 0.1f);
    float out = 0.0f;
    REQUIRE(sonare_metering_peak_db(samples.data(), samples.size(), 48000, nullptr) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_metering_true_peak_db(samples.data(), samples.size(), 48000, 3, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);  // 3 is not a power of two
  }
}

TEST_CASE("sonare_metering_true_peak_db", "[c_api]") {
  SECTION("matches sample peak within a small margin for low-frequency sine") {
    auto samples = generate_sine(440.0f, 48000, 1.0f);
    float peak_db_value = 0.0f;
    float tp_db = 0.0f;
    REQUIRE(sonare_metering_peak_db(samples.data(), samples.size(), 48000, &peak_db_value) ==
            SONARE_OK);
    REQUIRE(sonare_metering_true_peak_db(samples.data(), samples.size(), 48000, 0, &tp_db) ==
            SONARE_OK);
    // True peak >= sample peak (inter-sample peaks can only be higher).
    REQUIRE(tp_db >= peak_db_value - 0.1f);
  }
}

TEST_CASE("sonare_metering_detect_clipping", "[c_api]") {
  SECTION("reports clipped regions and frees them") {
    // Hard-clipped signal: ones in a run.
    std::vector<float> samples(8000, 0.1f);
    for (size_t i = 1000; i < 1064; ++i) samples[i] = 1.0f;
    SonareClippingResult result = {};
    REQUIRE(sonare_metering_detect_clipping(samples.data(), samples.size(), 48000, 0.999f, 0,
                                            &result) == SONARE_OK);
    REQUIRE(result.region_count >= 1);
    REQUIRE(result.regions != nullptr);
    REQUIRE(result.clipped_samples >= 1);
    REQUIRE(result.max_clipped_peak >= 1.0f);
    sonare_free_clipping_result(&result);
    REQUIRE(result.regions == nullptr);
    REQUIRE(result.region_count == 0);
  }

  SECTION("clean signal reports zero regions") {
    auto samples = generate_sine(440.0f, 48000, 0.5f);
    for (auto& s : samples) s *= 0.3f;
    SonareClippingResult result = {};
    REQUIRE(sonare_metering_detect_clipping(samples.data(), samples.size(), 48000, 0.999f, 0,
                                            &result) == SONARE_OK);
    REQUIRE(result.region_count == 0);
    REQUIRE(result.regions == nullptr);
    sonare_free_clipping_result(&result);
  }
}

TEST_CASE("sonare_metering_dynamic_range", "[c_api]") {
  SECTION("returns a positive DR for varying-level signal") {
    // 5 seconds: alternate 0.5s loud / 0.5s quiet.
    int sr = 48000;
    int total = sr * 5;
    std::vector<float> samples(total, 0.0f);
    auto loud = generate_sine(440.0f, sr, 0.5f);
    for (int i = 0; i < 5; ++i) {
      int offset = i * sr;
      float amp = (i % 2 == 0) ? 0.8f : 0.05f;
      for (size_t j = 0; j < loud.size() && offset + j < samples.size(); ++j) {
        samples[offset + j] = loud[j] * amp;
      }
    }
    SonareDynamicRangeResult result = {};
    // Negative percentiles select the library defaults (0.0 now means a literal
    // 0th percentile, so the default sentinel is negative).
    REQUIRE(sonare_metering_dynamic_range(samples.data(), samples.size(), sr, 0.0f, 0.0f, -1.0f,
                                          -1.0f, &result) == SONARE_OK);
    REQUIRE(result.window_count > 0);
    REQUIRE(result.window_rms_db != nullptr);
    REQUIRE(result.dynamic_range_db > 0.0f);
    sonare_free_dynamic_range_result(&result);
    REQUIRE(result.window_rms_db == nullptr);
  }

  SECTION("0th and 100th percentile select true min/max (sentinel reachable)") {
    int sr = 48000;
    int total = sr * 5;
    std::vector<float> samples(total, 0.0f);
    auto loud = generate_sine(440.0f, sr, 0.5f);
    for (int i = 0; i < 5; ++i) {
      int offset = i * sr;
      float amp = (i % 2 == 0) ? 0.8f : 0.05f;
      for (size_t j = 0; j < loud.size() && offset + j < samples.size(); ++j) {
        samples[offset + j] = loud[j] * amp;
      }
    }
    SonareDynamicRangeResult result = {};
    // low = 0.0 (true min window), high = 1.0 (true max window): both are real
    // requests now, not "default", and yield the widest range.
    REQUIRE(sonare_metering_dynamic_range(samples.data(), samples.size(), sr, 0.0f, 0.0f, 0.0f,
                                          1.0f, &result) == SONARE_OK);
    REQUIRE(result.dynamic_range_db > 0.0f);
    sonare_free_dynamic_range_result(&result);
  }

  SECTION("rejects inverted percentiles") {
    auto samples = generate_sine(440.0f, 48000, 1.0f);
    SonareDynamicRangeResult result = {};
    REQUIRE(sonare_metering_dynamic_range(samples.data(), samples.size(), 48000, 0.0f, 0.0f, 0.9f,
                                          0.1f, &result) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_metering stereo wrappers", "[c_api]") {
  const int sr = 48000;
  auto left = generate_sine(440.0f, sr, 0.5f);
  std::vector<float> right_in_phase = left;
  std::vector<float> right_inverted(left.size());
  for (size_t i = 0; i < left.size(); ++i) right_inverted[i] = -left[i];

  SECTION("correlation: in-phase ≈ +1, inverted ≈ -1") {
    float c = 0.0f;
    REQUIRE(sonare_metering_stereo_correlation(left.data(), right_in_phase.data(), left.size(), sr,
                                               &c) == SONARE_OK);
    REQUIRE(c == Catch::Approx(1.0f).margin(1e-3f));
    REQUIRE(sonare_metering_stereo_correlation(left.data(), right_inverted.data(), left.size(), sr,
                                               &c) == SONARE_OK);
    REQUIRE(c == Catch::Approx(-1.0f).margin(1e-3f));
  }

  SECTION("stereo_width: mono ≈ 0, inverted > mono") {
    float mono_width = 0.0f;
    float inv_width = 0.0f;
    REQUIRE(sonare_metering_stereo_width(left.data(), right_in_phase.data(), left.size(), sr,
                                         &mono_width) == SONARE_OK);
    REQUIRE(sonare_metering_stereo_width(left.data(), right_inverted.data(), left.size(), sr,
                                         &inv_width) == SONARE_OK);
    REQUIRE(mono_width < 1e-3f);
    REQUIRE(inv_width > mono_width);
  }

  SECTION("vectorscope returns one point per sample and frees cleanly") {
    SonareVectorscopeResult result = {};
    REQUIRE(sonare_metering_vectorscope(left.data(), right_in_phase.data(), left.size(), sr,
                                        &result) == SONARE_OK);
    REQUIRE(result.point_count == left.size());
    REQUIRE(result.points != nullptr);
    // In-phase: side ≈ 0 for every sample.
    float max_side = 0.0f;
    for (size_t i = 0; i < result.point_count; ++i) {
      max_side = std::max(max_side, std::abs(result.points[i].side));
    }
    REQUIRE(max_side < 1e-3f);
    sonare_free_vectorscope_result(&result);
    REQUIRE(result.points == nullptr);
    REQUIRE(result.point_count == 0);
  }

  SECTION("phase_scope populates summary stats") {
    SonarePhaseScopeResult result = {};
    REQUIRE(sonare_metering_phase_scope(left.data(), right_in_phase.data(), left.size(), sr,
                                        &result) == SONARE_OK);
    REQUIRE(result.point_count == left.size());
    REQUIRE(result.points != nullptr);
    REQUIRE(result.correlation == Catch::Approx(1.0f).margin(1e-3f));
    REQUIRE(result.max_radius > 0.0f);
    sonare_free_phase_scope_result(&result);
    REQUIRE(result.points == nullptr);
    REQUIRE(result.point_count == 0);
  }

  SECTION("rejects null pair / null out") {
    float out_v = 0.0f;
    REQUIRE(sonare_metering_stereo_correlation(nullptr, right_in_phase.data(), left.size(), sr,
                                               &out_v) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_metering_stereo_correlation(left.data(), right_in_phase.data(), left.size(), sr,
                                               nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    SonareVectorscopeResult vs = {};
    REQUIRE(sonare_metering_vectorscope(left.data(), nullptr, left.size(), sr, &vs) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_metering_spectrum", "[c_api]") {
  SECTION("returns n_fft/2+1 bins and a peak near the tone frequency") {
    const int sr = 48000;
    const int n_fft = 2048;
    auto samples = generate_sine(1000.0f, sr, 0.5f);
    SonareSpectrumResult result = {};
    REQUIRE(sonare_metering_spectrum(samples.data(), samples.size(), sr, n_fft, 0, 0, 0.0f, 0.0f,
                                     &result) == SONARE_OK);
    REQUIRE(result.bin_count == static_cast<size_t>(n_fft / 2 + 1));
    REQUIRE(result.n_fft == n_fft);
    REQUIRE(result.sample_rate == sr);
    REQUIRE(result.frequencies != nullptr);
    REQUIRE(result.magnitude != nullptr);
    REQUIRE(result.power != nullptr);
    REQUIRE(result.db != nullptr);
    // Identify the peak bin.
    size_t peak_bin = 0;
    float peak_mag = -1.0f;
    for (size_t i = 0; i < result.bin_count; ++i) {
      if (result.magnitude[i] > peak_mag) {
        peak_mag = result.magnitude[i];
        peak_bin = i;
      }
    }
    const float peak_freq = result.frequencies[peak_bin];
    REQUIRE(peak_freq == Catch::Approx(1000.0f).margin(60.0f));
    // power[i] ≈ magnitude[i]^2.
    REQUIRE(result.power[peak_bin] ==
            Catch::Approx(result.magnitude[peak_bin] * result.magnitude[peak_bin]).margin(1e-2f));
    sonare_free_spectrum_result(&result);
    REQUIRE(result.frequencies == nullptr);
    REQUIRE(result.bin_count == 0);
  }

  SECTION("rejects non-power-of-two n_fft") {
    auto samples = generate_sine(440.0f, 48000, 0.1f);
    SonareSpectrumResult result = {};
    REQUIRE(sonare_metering_spectrum(samples.data(), samples.size(), 48000, 1500, 0, 0, 0.0f, 0.0f,
                                     &result) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_scale_quantize_midi", "[c_api]") {
  // C major scale mask: C D E F G A B (bits 0,2,4,5,7,9,11).
  static constexpr uint16_t kCMajorMask = 0b101010110101;

  SECTION("snaps an off-scale note to the nearest in-scale neighbour") {
    // C#4 (61) -> nearest in C major is C4 (60) or D4 (62); either is acceptable.
    float out = 0.0f;
    REQUIRE(sonare_scale_quantize_midi(0, kCMajorMask, 0.0f, 61.0f, &out) == SONARE_OK);
    REQUIRE(
        (out == Catch::Approx(60.0f).margin(0.01f) || out == Catch::Approx(62.0f).margin(0.01f)));
  }

  SECTION("in-scale notes pass through") {
    float out = 0.0f;
    REQUIRE(sonare_scale_quantize_midi(0, kCMajorMask, 0.0f, 60.0f, &out) == SONARE_OK);
    REQUIRE(out == Catch::Approx(60.0f).margin(0.01f));
  }

  SECTION("correction_semitones agrees with quantize_midi") {
    float q = 0.0f;
    float c = 0.0f;
    REQUIRE(sonare_scale_quantize_midi(0, kCMajorMask, 0.0f, 61.4f, &q) == SONARE_OK);
    REQUIRE(sonare_scale_correction_semitones(0, kCMajorMask, 0.0f, 61.4f, &c) == SONARE_OK);
    REQUIRE(c == Catch::Approx(q - 61.4f).margin(0.01f));
  }

  SECTION("pitch_class_enabled reflects the mode mask") {
    int enabled = -1;
    REQUIRE(sonare_scale_pitch_class_enabled(0, kCMajorMask, 0, &enabled) == SONARE_OK);
    REQUIRE(enabled == 1);  // C is in C major
    REQUIRE(sonare_scale_pitch_class_enabled(0, kCMajorMask, 1, &enabled) == SONARE_OK);
    REQUIRE(enabled == 0);  // C# is not
  }

  SECTION("rejects bad roots / pitch classes / empty mask") {
    float out = 0.0f;
    REQUIRE(sonare_scale_quantize_midi(-1, kCMajorMask, 0.0f, 60.0f, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_scale_quantize_midi(0, 0, 0.0f, 60.0f, &out) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_scale_quantize_midi(0, 0x1000u, 0.0f, 60.0f, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);
    int enabled = -1;
    REQUIRE(sonare_scale_pitch_class_enabled(0, kCMajorMask, 12, &enabled) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_scale_pitch_class_enabled(0, 0xFFFFu, 0, &enabled) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }
}
