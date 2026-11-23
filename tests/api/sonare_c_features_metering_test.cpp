/// @file sonare_c_features_metering_test.cpp
/// @brief Feature and metering C API tests.

#include <algorithm>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "sonare_c_test_helpers.h"
#include "support/c_strings.h"

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

  REQUIRE(sonare_pseudo_cqt(samples.data(), samples.size(), 22050, 256, 55.0f, 36, 12, 1.0f,
                            &result) == SONARE_OK);
  REQUIRE(result.n_bins == 36);
  REQUIRE(result.n_frames > 0);
  REQUIRE(result.magnitude != nullptr);
  REQUIRE(result.frequencies != nullptr);
  sonare_free_cqt_result(&result);
  REQUIRE(result.magnitude == nullptr);

  REQUIRE(sonare_hybrid_cqt(samples.data(), samples.size(), 22050, 256, 55.0f, 36, 12, 1.0f,
                            &result) == SONARE_OK);
  REQUIRE(result.n_bins == 36);
  REQUIRE(result.n_frames > 0);
  REQUIRE(result.magnitude != nullptr);
  sonare_free_cqt_result(&result);

  REQUIRE(sonare_pseudo_cqt(samples.data(), samples.size(), 22050, 0, 55.0f, 36, 12, 1.0f,
                            &result) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare chroma_cens and bass_chroma C API", "[.][slow][c_api]") {
  auto samples = generate_sine(110.0f, 22050, 1.0f);
  SonareChromaResult result{};

  REQUIRE(sonare_chroma_cens(samples.data(), samples.size(), 22050, 512, 12, nullptr, &result) ==
          SONARE_OK);
  REQUIRE(result.n_chroma == 12);
  REQUIRE(result.n_frames > 0);
  REQUIRE(result.features != nullptr);
  REQUIRE(result.mean_energy != nullptr);
  sonare_free_chroma_result(&result);
  REQUIRE(result.features == nullptr);

  REQUIRE(sonare_chroma_cqt(samples.data(), samples.size(), 22050, 512, 12, nullptr, &result) ==
          SONARE_OK);
  REQUIRE(result.n_chroma == 12);
  REQUIRE(result.n_frames > 0);
  REQUIRE(result.features != nullptr);
  REQUIRE(result.mean_energy != nullptr);
  sonare_free_chroma_result(&result);

  SonareChromaCqtOptions cqt_options{};
  sonare_chroma_cqt_options_init(&cqt_options);
  REQUIRE(cqt_options.bins_per_octave == 36);
  cqt_options.bins_per_octave = 12;
  REQUIRE(sonare_chroma_cqt(samples.data(), samples.size(), 22050, 512, 12, &cqt_options,
                            &result) == SONARE_OK);
  REQUIRE(result.n_chroma == 12);
  sonare_free_chroma_result(&result);
  cqt_options.bins_per_octave = 13;
  REQUIRE(sonare_chroma_cqt(samples.data(), samples.size(), 22050, 512, 12, &cqt_options,
                            &result) == SONARE_ERROR_INVALID_PARAMETER);
  cqt_options.bins_per_octave = 36;
  cqt_options.struct_size = 0;
  REQUIRE(sonare_chroma_cqt(samples.data(), samples.size(), 22050, 512, 12, &cqt_options,
                            &result) == SONARE_ERROR_INVALID_PARAMETER);

  REQUIRE(sonare_bass_chroma(samples.data(), samples.size(), 22050, 512, 12, &result) == SONARE_OK);
  REQUIRE(result.n_chroma == 12);
  REQUIRE(result.n_frames > 0);
  REQUIRE(result.features != nullptr);
  sonare_free_chroma_result(&result);

  REQUIRE(sonare_chroma_cens(samples.data(), samples.size(), 22050, 0, 12, nullptr, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_chroma_cqt(samples.data(), samples.size(), 22050, 0, 12, nullptr, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("chroma C API defines the whole out-struct on rejected input", "[c_api]") {
  // Poisoned so an untouched field reads back as the sentinel rather than as a
  // plausible zero; every chroma entry point must overwrite all six fields
  // before its validate_audio_params early return.
  const auto poison = [](SonareChromaResult& r) {
    r.n_chroma = -1;
    r.n_frames = -1;
    r.sample_rate = -1;
    r.hop_length = std::numeric_limits<size_t>::max();
    r.features = reinterpret_cast<float*>(0x1);
    r.mean_energy = reinterpret_cast<float*>(0x1);
  };
  const auto require_defined = [](const SonareChromaResult& r) {
    REQUIRE(r.n_chroma == 0);
    REQUIRE(r.n_frames == 0);
    REQUIRE(r.sample_rate == 0);
    REQUIRE(r.hop_length == 0);
    REQUIRE(r.features == nullptr);
    REQUIRE(r.mean_energy == nullptr);
  };

  SonareChromaResult result{};

  SECTION("sonare_chroma") {
    poison(result);
    REQUIRE(sonare_chroma(nullptr, 0, 22050, 2048, 512, &result) == SONARE_ERROR_INVALID_PARAMETER);
    require_defined(result);
  }

  SECTION("sonare_chroma_cens") {
    poison(result);
    REQUIRE(sonare_chroma_cens(nullptr, 0, 22050, 512, 12, nullptr, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    require_defined(result);
  }

  SECTION("sonare_chroma_cqt") {
    poison(result);
    REQUIRE(sonare_chroma_cqt(nullptr, 0, 22050, 512, 12, nullptr, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    require_defined(result);
  }

  SECTION("sonare_bass_chroma") {
    poison(result);
    REQUIRE(sonare_bass_chroma(nullptr, 0, 22050, 512, 12, &result) ==
            SONARE_ERROR_INVALID_PARAMETER);
    require_defined(result);
  }
}

TEST_CASE("sonare_hz_to_note hands each name to the caller", "[c_api]") {
  // Each call allocates its own string, so an earlier result survives a later call.
  char* a4 = nullptr;
  char* c4 = nullptr;
  REQUIRE(sonare_hz_to_note(440.0f, &a4) == SONARE_OK);
  REQUIRE(sonare_hz_to_note(261.6256f, &c4) == SONARE_OK);
  REQUIRE(a4 != c4);
  REQUIRE(std::string(a4) == "A4");
  REQUIRE(std::string(c4) == "C4");
  sonare_free_string(a4);
  sonare_free_string(c4);
  REQUIRE(sonare_hz_to_note(440.0f, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "out_string must not be NULL");
}

TEST_CASE("sonare_hz_to_note / sonare_note_to_hz report a refused argument", "[c_api]") {
  const float nan = std::numeric_limits<float>::quiet_NaN();

  for (float bad : {0.0f, -1.0f, nan, std::numeric_limits<float>::infinity()}) {
    REQUIRE(sonare_test::c_string_error(sonare_hz_to_note, bad) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
  }

  for (const char* bad : {"H4", "C#x", "", "A4junk", "C2147483647"}) {
    INFO("name: \"" << bad << "\"");
    REQUIRE(std::isnan(sonare_note_to_hz(bad)));
    REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
  }
  REQUIRE(std::isnan(sonare_note_to_hz(nullptr)));
  REQUIRE(sonare_last_error_code() == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "note must not be NULL");

  // A valid call clears the failure it follows.
  REQUIRE(sonare_note_to_hz("A4") == Catch::Approx(440.0f).margin(0.01f));
  REQUIRE(sonare_last_error_code() == SONARE_OK);
  REQUIRE(sonare_test::c_string(sonare_hz_to_note, 440.0f) == "A4");
}

TEST_CASE("frame/time conversions return a sentinel instead of throwing on invalid rate or hop",
          "[c_api]") {
  REQUIRE(std::isnan(sonare_frames_to_time(10, 0, 512)));
  REQUIRE(std::isnan(sonare_frames_to_time(10, 22050, 0)));
  REQUIRE(std::isnan(sonare_frames_to_time(10, -1, 512)));
  REQUIRE(sonare_time_to_frames(1.0f, 0, 512) == 0);
  REQUIRE(sonare_time_to_frames(1.0f, 22050, 0) == 0);
  REQUIRE(sonare_time_to_frames(1.0f, 22050, -512) == 0);

  REQUIRE(sonare_frames_to_time(43, 22050, 512) ==
          Catch::Approx(43.0f * 512 / 22050).epsilon(1e-6));
  REQUIRE(sonare_time_to_frames(1.0f, 22050, 512) == 43);
}

TEST_CASE("sonare_peak_pick delta is in the units of its input", "[c_api]") {
  // The header now says delta is an absolute offset in the units of `values`
  // and that the envelope is not normalized. Assert exactly that: scaling the
  // input while holding delta fixed must change the result, which is what makes
  // "read a starting value off your own envelope" the right advice.
  std::vector<float> values(64, 0.01f);
  for (size_t i = 16; i < 64; i += 16) values[i] = 1.0f;

  const auto pick = [](const std::vector<float>& v, float delta) {
    int* out = nullptr;
    size_t count = 0;
    REQUIRE(sonare_peak_pick(v.data(), v.size(), 3, 3, 3, 3, delta, 2, &out, &count) == SONARE_OK);
    const size_t found = count;
    sonare_free_ints(out);
    return found;
  };

  const float delta = 0.5f;
  const size_t at_unit_scale = pick(values, delta);
  REQUIRE(at_unit_scale > 0);

  std::vector<float> quiet = values;
  for (auto& v : quiet) v *= 0.01f;
  // Same peaks, same shape, 100x quieter: the fixed delta now sits above them.
  REQUIRE(pick(quiet, delta) < at_unit_scale);
  // Scaling delta with the data recovers them, which is the documented recipe.
  REQUIRE(pick(quiet, delta * 0.01f) == at_unit_scale);
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

TEST_CASE("sonare_tempogram mode selects autocorrelation or cosine", "[c_api]") {
  const std::vector<float> env{0.2f, 1.0f, 0.4f, 0.0f, 0.8f, 0.1f, 0.5f, 0.3f,
                               0.6f, 0.0f, 0.9f, 0.2f, 0.4f, 0.7f, 0.1f, 0.5f};
  float* data = nullptr;
  size_t out_length = 0;
  int n_frames = 0;
  SonareError err = sonare_tempogram(env.data(), env.size(), 8, 1, 8, 0, 0, SONARE_TEMPOGRAM_COSINE,
                                     &data, &out_length, &n_frames);

  REQUIRE(err == SONARE_OK);
  REQUIRE(data != nullptr);
  REQUIRE(n_frames == static_cast<int>(env.size()) - 8 + 1);
  REQUIRE(out_length == static_cast<size_t>(n_frames) * 8);
  for (size_t i = 0; i < out_length; ++i) {
    REQUIRE(std::isfinite(data[i]));
    REQUIRE(data[i] >= -1.0f - 1.0e-6f);
    REQUIRE(data[i] <= 1.0f + 1.0e-6f);
  }
  sonare_free_floats(data);

  float* autocorr = nullptr;
  REQUIRE(sonare_tempogram(env.data(), env.size(), 8, 1, 8, 0, 0, SONARE_TEMPOGRAM_AUTOCORRELATION,
                           &autocorr, &out_length, &n_frames) == SONARE_OK);
  REQUIRE(autocorr != nullptr);
  sonare_free_floats(autocorr);

  REQUIRE(sonare_tempogram(env.data(), env.size(), 8, 1, 8, 0, 0, 99, &data, &out_length,
                           &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare_tempogram and sonare_fourier_tempogram frame an uncentered envelope per window",
          "[c_api]") {
  const std::vector<float> env{0.2f, 1.0f, 0.4f, 0.0f, 0.8f, 0.1f, 0.5f, 0.3f,
                               0.6f, 0.0f, 0.9f, 0.2f, 0.4f, 0.7f, 0.1f, 0.5f};
  constexpr int kWin = 8;
  float* data = nullptr;
  size_t out_length = 0;
  int n_frames = 0;
  REQUIRE(sonare_tempogram(env.data(), env.size(), 8, 1, kWin, 0, 0,
                           SONARE_TEMPOGRAM_AUTOCORRELATION, &data, &out_length,
                           &n_frames) == SONARE_OK);
  REQUIRE(n_frames == static_cast<int>(env.size()) - kWin + 1);
  REQUIRE(out_length == static_cast<size_t>(n_frames) * kWin);
  sonare_free_floats(data);

  REQUIRE(sonare_fourier_tempogram(env.data(), env.size(), 8, 1, kWin, 0, 0, &data, &out_length,
                                   &n_frames) == SONARE_OK);
  REQUIRE(n_frames == static_cast<int>(env.size()) - kWin + 1);
  REQUIRE(out_length == static_cast<size_t>(n_frames) * (kWin / 2 + 1));
  sonare_free_floats(data);

  // A centered envelope keeps one frame per envelope frame.
  REQUIRE(sonare_tempogram(env.data(), env.size(), 8, 1, kWin, 1, 0,
                           SONARE_TEMPOGRAM_AUTOCORRELATION, &data, &out_length,
                           &n_frames) == SONARE_OK);
  REQUIRE(n_frames == static_cast<int>(env.size()));
  sonare_free_floats(data);

  // An envelope shorter than the window is refused when not centered.
  REQUIRE(sonare_tempogram(env.data(), 4, 8, 1, kWin, 0, 0, SONARE_TEMPOGRAM_AUTOCORRELATION, &data,
                           &out_length, &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(data == nullptr);
  REQUIRE(n_frames == 0);
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
    REQUIRE(sonare_tempogram(env, env_count, 22050, 512, win_length, 1, 1,
                             SONARE_TEMPOGRAM_AUTOCORRELATION, &tg, &tg_length,
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

  SECTION("rejects empty data and non-finite factors instead of answering") {
    const int win_length = 384;
    std::vector<float> tg(static_cast<size_t>(win_length) * 4u, 0.1f);
    float* ratio = nullptr;
    size_t ratio_count = 0;

    // Baseline: the same call with good factors succeeds, so the rejections
    // below cannot be passing for an unrelated reason.
    const float good[] = {1.0f, 2.0f};
    REQUIRE(sonare_tempogram_ratio(tg.data(), tg.size(), win_length, 22050, 512, good, 2, &ratio,
                                   &ratio_count) == SONARE_OK);
    REQUIRE(ratio_count == 2);
    REQUIRE(ratio != nullptr);
    sonare_free_floats(ratio);

    // Empty tempogram data used to answer with five zeros.
    REQUIRE(sonare_tempogram_ratio(nullptr, 0, win_length, 22050, 512, nullptr, 0, &ratio,
                                   &ratio_count) == SONARE_ERROR_INVALID_PARAMETER);

    // Fewer samples than win_length is zero frames, i.e. no data to average.
    REQUIRE(sonare_tempogram_ratio(tg.data(), static_cast<size_t>(win_length) - 1u, win_length,
                                   22050, 512, good, 2, &ratio,
                                   &ratio_count) == SONARE_ERROR_INVALID_PARAMETER);

    // A NaN factor reached round(lag / NaN); an Inf factor resolved to lag 0.
    const float nan_factor[] = {1.0f, std::numeric_limits<float>::quiet_NaN()};
    REQUIRE(sonare_tempogram_ratio(tg.data(), tg.size(), win_length, 22050, 512, nan_factor, 2,
                                   &ratio, &ratio_count) == SONARE_ERROR_INVALID_PARAMETER);

    const float inf_factor[] = {1.0f, std::numeric_limits<float>::infinity()};
    REQUIRE(sonare_tempogram_ratio(tg.data(), tg.size(), win_length, 22050, 512, inf_factor, 2,
                                   &ratio, &ratio_count) == SONARE_ERROR_INVALID_PARAMETER);

    const float zero_factor[] = {1.0f, 0.0f};
    REQUIRE(sonare_tempogram_ratio(tg.data(), tg.size(), win_length, 22050, 512, zero_factor, 2,
                                   &ratio, &ratio_count) == SONARE_ERROR_INVALID_PARAMETER);
  }
}

TEST_CASE("sonare_nnls_chroma", "[.][slow][c_api]") {
  SECTION("returns a 12 x n_frames chroma matrix") {
    auto samples = generate_chord({261.63f, 329.63f, 392.00f}, 22050, 2.0f);
    float* data = nullptr;
    size_t out_length = 0;
    int n_frames = 0;

    SonareError err = sonare_nnls_chroma(samples.data(), samples.size(), 22050, nullptr, &data,
                                         &out_length, &n_frames);

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

    REQUIRE(sonare_nnls_chroma(nullptr, samples.size(), 22050, nullptr, &data, &out_length,
                               &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_nnls_chroma(samples.data(), samples.size(), 22050, nullptr, nullptr, &out_length,
                               &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_nnls_chroma(samples.data(), samples.size(), 22050, nullptr, &data, nullptr,
                               &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_nnls_chroma(samples.data(), samples.size(), 22050, nullptr, &data, &out_length,
                               nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    SonareNnlsChromaOptions options{};
    sonare_nnls_chroma_options_init(&options);
    options.stft_blend_weight = -0.1f;
    REQUIRE(sonare_nnls_chroma(samples.data(), samples.size(), 22050, &options, &data, &out_length,
                               &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
    options.stft_blend_weight = 0.5f;
    options.stft_blend_n_fft = 4095;
    REQUIRE(sonare_nnls_chroma(samples.data(), samples.size(), 22050, &options, &data, &out_length,
                               &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
    options.stft_blend_n_fft = 4096;
    options.struct_size = 0;
    REQUIRE(sonare_nnls_chroma(samples.data(), samples.size(), 22050, &options, &data, &out_length,
                               &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("exposes the optional STFT blend") {
    auto samples = generate_chord({261.63f, 329.63f, 392.00f}, 22050, 1.0f);
    float* data = nullptr;
    size_t out_length = 0;
    int n_frames = 0;
    SonareNnlsChromaOptions options{};
    sonare_nnls_chroma_options_init(&options);
    options.enable_stft_blend = 0;
    options.stft_blend_weight = 0.0f;
    options.stft_blend_n_fft = 2048;
    REQUIRE(sonare_nnls_chroma(samples.data(), samples.size(), 22050, &options, &data, &out_length,
                               &n_frames) == SONARE_OK);
    REQUIRE(data != nullptr);
    REQUIRE(out_length == 12u * static_cast<size_t>(n_frames));
    sonare_free_floats(data);
  }

  SECTION("the defaults use a 512-sample hop and options expose another hop") {
    auto samples = generate_chord({261.63f, 329.63f, 392.00f}, 22050, 1.0f);
    float* legacy = nullptr;
    float* explicit_default = nullptr;
    float* custom = nullptr;
    size_t legacy_length = 0;
    size_t explicit_length = 0;
    size_t custom_length = 0;
    int legacy_frames = 0;
    int explicit_frames = 0;
    int custom_frames = 0;

    SonareNnlsChromaOptions options{};
    sonare_nnls_chroma_options_init(&options);
    REQUIRE(options.enable_stft_blend == 1);
    REQUIRE(options.stft_blend_weight == 0.55f);
    REQUIRE(options.stft_blend_n_fft == 4096);
    REQUIRE(options.hop_length == 512);
    REQUIRE(sonare_nnls_chroma(samples.data(), samples.size(), 22050, nullptr, &legacy,
                               &legacy_length, &legacy_frames) == SONARE_OK);
    REQUIRE(sonare_nnls_chroma(samples.data(), samples.size(), 22050, &options, &explicit_default,
                               &explicit_length, &explicit_frames) == SONARE_OK);
    REQUIRE(legacy_length == explicit_length);
    REQUIRE(legacy_frames == explicit_frames);
    REQUIRE(std::memcmp(legacy, explicit_default, legacy_length * sizeof(float)) == 0);

    options.hop_length = 256;
    REQUIRE(sonare_nnls_chroma(samples.data(), samples.size(), 22050, &options, &custom,
                               &custom_length, &custom_frames) == SONARE_OK);
    REQUIRE(custom_length == 12u * static_cast<size_t>(custom_frames));
    REQUIRE(custom_frames > legacy_frames);

    sonare_free_floats(legacy);
    sonare_free_floats(explicit_default);
    sonare_free_floats(custom);
  }

  SECTION("outputs are reset before the hop is rejected") {
    auto samples = generate_sine(440.0f, 22050, 1.0f);
    float* data = samples.data();
    size_t out_length = 123;
    int n_frames = 456;
    SonareNnlsChromaOptions options{};
    sonare_nnls_chroma_options_init(&options);
    options.hop_length = 0;
    REQUIRE(sonare_nnls_chroma(samples.data(), samples.size(), 22050, &options, &data, &out_length,
                               &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(data == nullptr);
    REQUIRE(out_length == 0);
    REQUIRE(n_frames == 0);
  }
}

TEST_CASE("sonare_metering_silence_ratio", "[c_api][meter]") {
  std::vector<float> samples(2048, 0.0f);
  std::fill(samples.begin() + 1024, samples.end(), 1.0f);
  float ratio = -1.0f;
  REQUIRE(sonare_metering_silence_ratio(samples.data(), samples.size(), 48000, -45.0f, 1024, 1024,
                                        &ratio) == SONARE_OK);
  REQUIRE(ratio == Catch::Approx(0.5f).margin(0.001f));
  REQUIRE(sonare_metering_silence_ratio(samples.data(), samples.size(), 48000, -45.0f, 0, 1024,
                                        &ratio) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_metering_silence_ratio(samples.data(), samples.size(), 48000, -45.0f, 1024, 1024,
                                        nullptr) == SONARE_ERROR_INVALID_PARAMETER);
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
    REQUIRE(std::isfinite(result.loudness_range_lu));
    REQUIRE(result.loudness_range_lu >= 0.0f);
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
    REQUIRE(sonare_metering_true_peak_dbtp(samples.data(), samples.size(), 48000, 3, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);  // 3 is not a power of two
  }
}

TEST_CASE("sonare_metering_true_peak_dbtp", "[c_api]") {
  SECTION("matches sample peak within a small margin for low-frequency sine") {
    auto samples = generate_sine(440.0f, 48000, 1.0f);
    float peak_db_value = 0.0f;
    float tp_db = 0.0f;
    REQUIRE(sonare_metering_peak_db(samples.data(), samples.size(), 48000, &peak_db_value) ==
            SONARE_OK);
    REQUIRE(sonare_metering_true_peak_dbtp(samples.data(), samples.size(), 48000, 0, &tp_db) ==
            SONARE_OK);
    // True peak >= sample peak (inter-sample peaks can only be higher).
    REQUIRE(tp_db >= peak_db_value - 0.1f);
  }
}

TEST_CASE("sonare waveform peaks bucket interleaved audio", "[c_api][meter]") {
  const std::vector<float> samples{
      -1.0f, 0.5f, 0.25f, -0.25f, 0.75f, 0.1f, -0.5f, -0.75f, 0.0f, 0.9f,
  };
  SonareWaveformPeaksResult result{};

  REQUIRE(sonare_waveform_peaks(samples.data(), 5, 2, 2, &result) == SONARE_OK);
  REQUIRE(result.num_channels == 2);
  REQUIRE(result.bucket_count == 3);
  REQUIRE(result.samples_per_bucket == 2);
  REQUIRE(result.min != nullptr);
  REQUIRE(result.max != nullptr);

  const std::vector<float> expected_min{-1.0f, -0.5f, 0.0f, -0.25f, -0.75f, 0.9f};
  const std::vector<float> expected_max{0.25f, 0.75f, 0.0f, 0.5f, 0.1f, 0.9f};
  for (size_t i = 0; i < expected_min.size(); ++i) {
    REQUIRE(result.min[i] == Catch::Approx(expected_min[i]));
    REQUIRE(result.max[i] == Catch::Approx(expected_max[i]));
  }
  sonare_free_waveform_peaks_result(&result);
  REQUIRE(result.min == nullptr);

  REQUIRE(sonare_waveform_peaks(samples.data(), 5, 2, 0, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_waveform_peaks(samples.data(), 5, 0, 2, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);

  // A silent buffer and a non-finite one used to come back identically
  // (min == max == 0), which is what let a NaN channel render as silence.
  // Assert the two apart rather than only the refusal.
  const std::vector<float> silent(6, 0.0f);
  REQUIRE(sonare_waveform_peaks(silent.data(), 3, 2, 2, &result) == SONARE_OK);
  REQUIRE(result.bucket_count == 2);
  REQUIRE(result.min[0] == Catch::Approx(0.0f));
  REQUIRE(result.max[0] == Catch::Approx(0.0f));
  sonare_free_waveform_peaks_result(&result);

  const std::vector<float> non_finite_samples{
      -1.0f,  std::numeric_limits<float>::quiet_NaN(),
      0.5f,   std::numeric_limits<float>::infinity(),
      -0.25f, 0.75f,
  };
  REQUIRE(sonare_waveform_peaks(non_finite_samples.data(), 3, 2, 2, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  const size_t pyramid_levels[] = {2, 4};
  SonareWaveformPeakPyramidResult pyramid{};
  REQUIRE(sonare_waveform_peak_pyramid(non_finite_samples.data(), 3, 2, pyramid_levels, 2,
                                       &pyramid) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("sonare waveform peak pyramid returns requested levels", "[c_api][meter]") {
  const std::vector<float> samples{-1.0f, 0.5f,  0.25f,  -0.25f, 0.75f,
                                   0.1f,  -0.5f, -0.75f, 0.0f,   0.9f};
  const size_t levels[] = {2, 4};
  SonareWaveformPeakPyramidResult result{};

  REQUIRE(sonare_waveform_peak_pyramid(samples.data(), 5, 2, levels, 2, &result) == SONARE_OK);
  REQUIRE(result.level_count == 2);
  REQUIRE(result.levels != nullptr);
  REQUIRE(result.levels[0].bucket_count == 3);
  REQUIRE(result.levels[0].samples_per_bucket == 2);
  REQUIRE(result.levels[1].bucket_count == 2);
  REQUIRE(result.levels[1].samples_per_bucket == 4);
  REQUIRE(result.levels[1].min[0] == Catch::Approx(-1.0f));
  REQUIRE(result.levels[1].max[0] == Catch::Approx(0.75f));
  sonare_free_waveform_peak_pyramid_result(&result);
  REQUIRE(result.levels == nullptr);
}

TEST_CASE("sonare waveform peak entries refuse their arguments by name", "[c_api][meter]") {
  const std::vector<float> samples(10, 0.25f);
  const size_t levels[] = {2, 4};
  SonareWaveformPeaksResult peaks{};
  SonareWaveformPeakPyramidResult pyramid{};
  const auto refused = [](SonareError code, const char* subject) {
    CAPTURE(subject);
    CHECK(code == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_last_refusal_subject_count() >= 1);
    CHECK(std::string(sonare_last_refusal_subject(0)) == subject);
  };
  refused(sonare_waveform_peaks(samples.data(), 5, 0, 2, &peaks), "num_channels");
  refused(sonare_waveform_peaks(nullptr, 5, 2, 2, &peaks), "samples");
  refused(sonare_waveform_peak_pyramid(samples.data(), 5, 0, levels, 2, &pyramid), "num_channels");
  refused(sonare_waveform_peak_pyramid(samples.data(), 5, 2, levels, 0, &pyramid), "level_count");
  refused(sonare_waveform_peak_pyramid(samples.data(), 5, 2, nullptr, 2, &pyramid),
          "samples_per_bucket_levels");
  refused(sonare_waveform_peak_pyramid(samples.data(), 5, 2, levels, 2, nullptr), "out");
}

namespace {
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
}  // namespace

TEST_CASE("sonare_metering_detect_clipping", "[c_api]") {
  SECTION("reports clipped regions and frees them") {
    // Hard-clipped signal: ones in a run.
    std::vector<float> samples(8000, 0.1f);
    for (size_t i = 1000; i < 1064; ++i) samples[i] = 1.0f;
    SonareClippingResult result = {};
    REQUIRE(sonare_metering_detect_clipping(samples.data(), samples.size(), 48000, kNaN, 0,
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
    REQUIRE(sonare_metering_detect_clipping(samples.data(), samples.size(), 48000, kNaN, 0,
                                            &result) == SONARE_OK);
    REQUIRE(result.region_count == 0);
    REQUIRE(result.regions == nullptr);
    sonare_free_clipping_result(&result);
  }

  SECTION("NaN selects the library default level, and every other level is checked") {
    std::vector<float> samples(8000, 0.1f);
    for (size_t i = 1000; i < 1064; ++i) samples[i] = 1.0f;

    // NaN reproduces the default level (the dB of a linear 0.999) exactly.
    SonareClippingResult absent = {};
    SonareClippingResult explicit_default = {};
    REQUIRE(sonare_metering_detect_clipping(samples.data(), samples.size(), 48000, kNaN, 0,
                                            &absent) == SONARE_OK);
    REQUIRE(sonare_metering_detect_clipping(samples.data(), samples.size(), 48000,
                                            20.0f * std::log10(0.999f), 0,
                                            &explicit_default) == SONARE_OK);
    REQUIRE(absent.clipped_samples == explicit_default.clipped_samples);
    REQUIRE(absent.region_count == explicit_default.region_count);
    sonare_free_clipping_result(&absent);
    sonare_free_clipping_result(&explicit_default);

    // 0 dBFS is an ordinary level, not the default.
    SonareClippingResult full_scale = {};
    REQUIRE(sonare_metering_detect_clipping(samples.data(), samples.size(), 48000, 0.0f, 0,
                                            &full_scale) == SONARE_OK);
    REQUIRE(full_scale.clipped_samples == 64);
    sonare_free_clipping_result(&full_scale);

    // A level above full scale, below the numerical floor (where every sample would read as
    // clipped), or an infinity is refused.
    for (float invalid : {0.5f, 6.0f, -120.5f, -910.0f, std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()}) {
      CAPTURE(invalid);
      SonareClippingResult rejected = {};
      REQUIRE(sonare_metering_detect_clipping(samples.data(), samples.size(), 48000, invalid, 0,
                                              &rejected) == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(rejected.regions == nullptr);
      REQUIRE(rejected.region_count == 0);
    }
  }

  SECTION("a zero minimum run length lists every run, like 1") {
    std::vector<float> samples(8000, 0.1f);
    samples[100] = 1.0f;
    for (size_t i = 1000; i < 1064; ++i) samples[i] = 1.0f;
    SonareClippingResult zero = {};
    SonareClippingResult one = {};
    REQUIRE(sonare_metering_detect_clipping(samples.data(), samples.size(), 48000, kNaN, 0,
                                            &zero) == SONARE_OK);
    REQUIRE(sonare_metering_detect_clipping(samples.data(), samples.size(), 48000, kNaN, 1, &one) ==
            SONARE_OK);
    REQUIRE(zero.region_count == 2);
    REQUIRE(zero.region_count == one.region_count);
    sonare_free_clipping_result(&zero);
    sonare_free_clipping_result(&one);
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

  SECTION("rejects non-finite parameters before applying default sentinels") {
    auto samples = generate_sine(440.0f, 48000, 0.05f);
    SonareDynamicRangeResult result = {};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    REQUIRE(sonare_metering_dynamic_range(samples.data(), samples.size(), 48000, nan, 0.0f, -1.0f,
                                          -1.0f, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_metering_dynamic_range(samples.data(), samples.size(), 48000, 0.0f, inf, -1.0f,
                                          -1.0f, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_metering_dynamic_range(samples.data(), samples.size(), 48000, 0.0f, 0.0f, nan,
                                          -1.0f, &result) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_metering_dynamic_range(samples.data(), samples.size(), 48000, -inf, 0.0f, -1.0f,
                                          -1.0f, &result) == SONARE_ERROR_INVALID_PARAMETER);
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
    REQUIRE(sonare_metering_vectorscope(left.data(), right_in_phase.data(), left.size(), sr, 0,
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
    REQUIRE(sonare_metering_phase_scope(left.data(), right_in_phase.data(), left.size(), sr, 0,
                                        &result) == SONARE_OK);
    REQUIRE(result.point_count == left.size());
    REQUIRE(result.points != nullptr);
    REQUIRE(result.correlation == Catch::Approx(1.0f).margin(1e-3f));
    REQUIRE(result.max_radius > 0.0f);
    sonare_free_phase_scope_result(&result);
    REQUIRE(result.points == nullptr);
    REQUIRE(result.point_count == 0);
  }

  SECTION("silence leaves the stereo scalars and the phase angle not measured") {
    const std::vector<float> silence(256, 0.0f);
    float corr = 0.0f;
    float width = 0.0f;
    REQUIRE(sonare_metering_stereo_correlation(silence.data(), silence.data(), silence.size(), sr,
                                               &corr) == SONARE_OK);
    REQUIRE(sonare_metering_stereo_width(silence.data(), silence.data(), silence.size(), sr,
                                         &width) == SONARE_OK);
    REQUIRE(std::isnan(corr));
    REQUIRE(std::isnan(width));
    SonarePhaseScopeResult result = {};
    REQUIRE(sonare_metering_phase_scope(silence.data(), silence.data(), silence.size(), sr, 0,
                                        &result) == SONARE_OK);
    REQUIRE(result.point_count == silence.size());
    for (size_t i = 0; i < result.point_count; ++i) REQUIRE(std::isnan(result.points[i].angle_rad));
    REQUIRE(std::isnan(result.correlation));
    REQUIRE(std::isnan(result.average_abs_angle_rad));
    REQUIRE(result.max_radius == 0.0f);
    sonare_free_phase_scope_result(&result);
  }

  SECTION("rejects null pair / null out") {
    float out_v = 0.0f;
    REQUIRE(sonare_metering_stereo_correlation(nullptr, right_in_phase.data(), left.size(), sr,
                                               &out_v) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_metering_stereo_correlation(left.data(), right_in_phase.data(), left.size(), sr,
                                               nullptr) == SONARE_ERROR_INVALID_PARAMETER);
    SonareVectorscopeResult vs = {};
    REQUIRE(sonare_metering_vectorscope(left.data(), nullptr, left.size(), sr, 0, &vs) ==
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
    CHECK(std::string(sonare_last_error_message()) ==
          "n_fft must be a power of two in [2, 4194304]");
  }

  SECTION("names a refused dB knob rather than leaving the message empty") {
    auto samples = generate_sine(440.0f, 48000, 0.1f);
    SonareSpectrumResult result = {};
    REQUIRE(sonare_metering_spectrum(samples.data(), samples.size(), 48000, 0, 0, 0, -1.0f, 0.0f,
                                     &result) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) == "db_ref must be finite and at least 0");
    REQUIRE(sonare_metering_spectrum(samples.data(), samples.size(), 48000, 0, 0, 0, 0.0f,
                                     std::numeric_limits<float>::quiet_NaN(),
                                     &result) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) == "db_amin must be finite and at least 0");
  }
}

TEST_CASE("sonare_scale_quantize_midi", "[c_api]") {
  // C major scale mask: C D E F G A B (bits 0,2,4,5,7,9,11).
  static constexpr uint16_t kCMajorMask = 0b101010110101;
  // An optional reference_midi that is not given.
  const float kNotGiven = std::numeric_limits<float>::quiet_NaN();

  SECTION("snaps an off-scale note to the nearest in-scale neighbour") {
    // C#4 (61) -> nearest in C major is C4 (60) or D4 (62); either is acceptable.
    float out = 0.0f;
    REQUIRE(sonare_scale_quantize_midi(0, kCMajorMask, kNotGiven, 61.0f, &out) == SONARE_OK);
    REQUIRE(
        (out == Catch::Approx(60.0f).margin(0.01f) || out == Catch::Approx(62.0f).margin(0.01f)));
  }

  SECTION("in-scale notes pass through") {
    float out = 0.0f;
    REQUIRE(sonare_scale_quantize_midi(0, kCMajorMask, kNotGiven, 60.0f, &out) == SONARE_OK);
    REQUIRE(out == Catch::Approx(60.0f).margin(0.01f));
  }

  SECTION("correction_semitones agrees with quantize_midi") {
    float q = 0.0f;
    float c = 0.0f;
    REQUIRE(sonare_scale_quantize_midi(0, kCMajorMask, kNotGiven, 61.4f, &q) == SONARE_OK);
    REQUIRE(sonare_scale_correction_semitones(0, kCMajorMask, kNotGiven, 61.4f, &c) == SONARE_OK);
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
    REQUIRE(sonare_scale_quantize_midi(-1, kCMajorMask, kNotGiven, 60.0f, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_scale_quantize_midi(0, 0, kNotGiven, 60.0f, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_scale_quantize_midi(0, 0x1000u, kNotGiven, 60.0f, &out) ==
            SONARE_ERROR_INVALID_PARAMETER);
    int enabled = -1;
    REQUIRE(sonare_scale_pitch_class_enabled(0, kCMajorMask, 12, &enabled) ==
            SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(sonare_scale_pitch_class_enabled(0, 0xFFFFu, 0, &enabled) ==
            SONARE_ERROR_INVALID_PARAMETER);
  }

  SECTION("rejects non-finite midi instead of reaching an undefined int cast") {
    float out = 0.0f;
    for (float bad :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()}) {
      REQUIRE(sonare_scale_quantize_midi(0, kCMajorMask, kNotGiven, bad, &out) ==
              SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(sonare_scale_correction_semitones(0, kCMajorMask, kNotGiven, bad, &out) ==
              SONARE_ERROR_INVALID_PARAMETER);
    }
  }

  SECTION("reference_midi is applied or refused rather than replaced by the default") {
    float out = 0.0f;

    // NaN (not given) keeps the default origin, and a legal origin moves the grid off it.
    // Anchoring at 60.5 shifts the whole 12-TET grid up a half semitone, which
    // puts C# at 60.5 and C at 59.5, so quantizing 60.0 lands on 59.5 instead
    // of the default grid's 60.0 -- the request is consumed, not just accepted.
    REQUIRE(sonare_scale_quantize_midi(0, kCMajorMask, kNotGiven, 60.0f, &out) == SONARE_OK);
    REQUIRE(out == Catch::Approx(60.0f).margin(0.01f));
    REQUIRE(sonare_scale_quantize_midi(0, kCMajorMask, 60.5f, 60.0f, &out) == SONARE_OK);
    REQUIRE(out == Catch::Approx(59.5f).margin(0.01f));

    // 0 is MIDI note 0, an ordinary origin rather than a request for the default.
    REQUIRE(sonare_scale_quantize_midi(0, kCMajorMask, 0.0f, 60.0f, &out) == SONARE_OK);
    REQUIRE(std::isfinite(out));

    // A negative or out-of-range origin is refused. The quantizer is noexcept and rounds the
    // origin to an int, so there is nothing downstream to refuse it.
    for (float bad : {-1.0f, -69.0f, 128.0f, 1.0e30f}) {
      REQUIRE(sonare_scale_quantize_midi(0, kCMajorMask, bad, 60.0f, &out) ==
              SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(sonare_scale_correction_semitones(0, kCMajorMask, bad, 60.0f, &out) ==
              SONARE_ERROR_INVALID_PARAMETER);
    }
    for (float bad :
         {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
      REQUIRE(sonare_scale_quantize_midi(0, kCMajorMask, bad, 60.0f, &out) ==
              SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(sonare_scale_correction_semitones(0, kCMajorMask, bad, 60.0f, &out) ==
              SONARE_ERROR_INVALID_PARAMETER);
    }
  }
}

TEST_CASE("sonare mel range is applied or refused rather than defaulted", "[c_api]") {
  const auto samples = generate_sine(440.0f, 22050, 0.5f);

  auto mel_power = [&](float fmin, float fmax, SonareError* err) {
    SonareMelResult result{};
    SonareMelSpectrogramOptions options{};
    sonare_mel_spectrogram_options_init(&options);
    options.fmin_hz = fmin;
    options.fmax_hz = fmax;
    *err = sonare_mel_spectrogram(samples.data(), samples.size(), 22050, 2048, 512, 40, &options,
                                  &result);
    std::vector<float> power;
    if (*err == SONARE_OK) {
      const size_t total = static_cast<size_t>(result.n_mels) * result.n_frames;
      power.assign(result.power, result.power + total);
      sonare_free_mel_result(&result);
    }
    return power;
  };

  SECTION("0 keeps the librosa default and a legal range is consumed") {
    SonareError err = SONARE_OK;
    const std::vector<float> defaulted = mel_power(0.0f, 0.0f, &err);
    REQUIRE(err == SONARE_OK);
    REQUIRE_FALSE(defaulted.empty());

    // fmax == 0 documents sr/2, so spelling it out must land on the same bands.
    const std::vector<float> nyquist = mel_power(0.0f, 11025.0f, &err);
    REQUIRE(err == SONARE_OK);
    REQUIRE(nyquist.size() == defaulted.size());
    for (size_t i = 0; i < nyquist.size(); ++i) {
      REQUIRE(nyquist[i] == Catch::Approx(defaulted[i]).margin(1.0e-6f));
    }

    // A legal non-sentinel range must move the filterbank off the default one,
    // or a refusal-only test would pass against a reader that ignores the range.
    const std::vector<float> ranged = mel_power(500.0f, 4000.0f, &err);
    REQUIRE(err == SONARE_OK);
    REQUIRE(ranged.size() == defaulted.size());
    bool differs = false;
    for (size_t i = 0; i < ranged.size() && !differs; ++i) {
      differs = std::abs(ranged[i] - defaulted[i]) > 1.0e-6f;
    }
    REQUIRE(differs);
  }

  SECTION("a negative or non-finite bound is refused rather than defaulted") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    SonareError err = SONARE_OK;
    for (float bad : {-1.0f, -11025.0f, nan, inf, -inf}) {
      mel_power(bad, 0.0f, &err);
      REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
      CHECK_THAT(std::string(sonare_last_error_message()),
                 Catch::Matchers::StartsWith("options.fmin_hz"));
      mel_power(0.0f, bad, &err);
      REQUIRE(err == SONARE_ERROR_INVALID_PARAMETER);
      CHECK_THAT(std::string(sonare_last_error_message()),
                 Catch::Matchers::StartsWith("options.fmax_hz"));
    }
  }

  SECTION("the MFCC entry point carries the same contract") {
    SonareMfccResult result{};
    SonareMfccOptions options{};
    sonare_mfcc_options_init(&options);
    REQUIRE(sonare_mfcc(samples.data(), samples.size(), 22050, 2048, 512, 40, 13, &options,
                        &result) == SONARE_OK);
    REQUIRE(result.n_frames > 0);
    sonare_free_mfcc_result(&result);

    options.fmin_hz = -1.0f;
    REQUIRE(sonare_mfcc(samples.data(), samples.size(), 22050, 2048, 512, 40, 13, &options,
                        &result) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(std::string(sonare_last_error_message()),
               Catch::Matchers::StartsWith("options.fmin_hz"));
    options.fmin_hz = 0.0f;
    options.fmax_hz = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(sonare_mfcc(samples.data(), samples.size(), 22050, 2048, 512, 40, 13, &options,
                        &result) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK_THAT(std::string(sonare_last_error_message()),
               Catch::Matchers::StartsWith("options.fmax_hz"));
  }
}

TEST_CASE("mel and MFCC results carry the parameters that invert them", "[c_api][features]") {
  const auto samples = generate_sine(440.0f, 22050, 0.5f);

  SECTION("the Mel result reports the transform it came from, with fmax resolved") {
    SonareMelResult mel{};
    SonareMelSpectrogramOptions mel_options{};
    sonare_mel_spectrogram_options_init(&mel_options);
    mel_options.fmin_hz = 100.0f;
    mel_options.htk = 1;
    REQUIRE(sonare_mel_spectrogram(samples.data(), samples.size(), 22050, 2048, 512, 40,
                                   &mel_options, &mel) == SONARE_OK);
    CHECK(mel.n_mels == 40);
    CHECK(mel.sample_rate == 22050);
    CHECK(mel.hop_length == 512);
    CHECK(mel.n_fft == 2048);
    CHECK(mel.fmin_hz == 100.0f);
    CHECK(mel.fmax_hz == 11025.0f);
    CHECK(mel.htk == 1);
    CHECK(mel.is_db == 0);

    // The carried values are the inverse's arguments: the inverse over them is
    // bit-identical to the same call with the values spelled out as the forward request.
    const size_t total = static_cast<size_t>(mel.n_mels) * mel.n_frames;
    SonareInverseResult from_result{};
    SonareInverseResult from_request{};
    SonareMelToStftOptions inverse_options{};
    sonare_mel_to_stft_options_init(&inverse_options);
    inverse_options.htk = mel.htk;
    REQUIRE(sonare_mel_to_stft(mel.power, total, mel.n_mels, mel.n_frames, mel.sample_rate,
                               mel.n_fft, mel.fmin_hz, mel.fmax_hz, &inverse_options,
                               &from_result) == SONARE_OK);
    inverse_options.htk = 1;
    REQUIRE(sonare_mel_to_stft(mel.power, total, 40, mel.n_frames, 22050, 2048, 100.0f, 0.0f,
                               &inverse_options, &from_request) == SONARE_OK);
    REQUIRE(from_result.rows == from_request.rows);
    for (size_t i = 0; i < static_cast<size_t>(from_result.rows) * from_result.n_frames; ++i) {
      REQUIRE(from_result.data[i] == from_request.data[i]);
    }
    sonare_free_inverse_result(&from_result);
    sonare_free_inverse_result(&from_request);
    sonare_free_mel_result(&mel);
  }

  SECTION(
      "the MFCC result reports its Mel parameters and lifter, and the lifter inverse "
      "undoes it") {
    SonareMfccResult mfcc{};
    SonareMfccOptions mfcc_options{};
    sonare_mfcc_options_init(&mfcc_options);
    mfcc_options.fmax_hz = 8000.0f;
    mfcc_options.lifter = 22.0f;
    REQUIRE(sonare_mfcc(samples.data(), samples.size(), 22050, 2048, 512, 40, 13, &mfcc_options,
                        &mfcc) == SONARE_OK);
    CHECK(mfcc.n_mfcc == 13);
    CHECK(mfcc.n_mels == 40);
    CHECK(mfcc.sample_rate == 22050);
    CHECK(mfcc.hop_length == 512);
    CHECK(mfcc.n_fft == 2048);
    CHECK(mfcc.fmin_hz == 0.0f);
    CHECK(mfcc.fmax_hz == 8000.0f);
    CHECK(mfcc.htk == 0);
    CHECK(mfcc.is_db == 0);
    CHECK(mfcc.lifter == 22.0f);

    const size_t total = static_cast<size_t>(mfcc.n_mfcc) * mfcc.n_frames;
    SonareInverseResult checked{};
    SonareMfccToMelOptions inverse_options{};
    sonare_mfcc_to_mel_options_init(&inverse_options);
    inverse_options.lifter = mfcc.lifter;
    REQUIRE(sonare_mfcc_to_mel(mfcc.coefficients, total, mfcc.n_mfcc, mfcc.n_frames, mfcc.n_mels,
                               &inverse_options, &checked) == SONARE_OK);
    // The lifter changes the answer, so an inverse that dropped it would differ.
    SonareInverseResult unlifted{};
    REQUIRE(sonare_mfcc_to_mel(mfcc.coefficients, total, mfcc.n_mfcc, mfcc.n_frames, mfcc.n_mels,
                               nullptr, &unlifted) == SONARE_OK);
    bool differs = false;
    for (size_t i = 0; i < static_cast<size_t>(checked.rows) * checked.n_frames && !differs; ++i) {
      differs = checked.data[i] != unlifted.data[i];
    }
    CHECK(differs);
    CHECK(sonare_mfcc_to_mel(mfcc.coefficients, total - 1, mfcc.n_mfcc, mfcc.n_frames, mfcc.n_mels,
                             &inverse_options, &unlifted) == SONARE_ERROR_INVALID_PARAMETER);

    float* audio = nullptr;
    size_t audio_length = 0;
    SonareMfccToAudioOptions audio_options{};
    sonare_mfcc_to_audio_options_init(&audio_options);
    audio_options.htk = mfcc.htk;
    audio_options.lifter = mfcc.lifter;
    REQUIRE(sonare_mfcc_to_audio(mfcc.coefficients, total, mfcc.n_mfcc, mfcc.n_frames, mfcc.n_mels,
                                 mfcc.sample_rate, mfcc.n_fft, mfcc.hop_length, mfcc.fmin_hz,
                                 mfcc.fmax_hz, 2, &audio_options, &audio,
                                 &audio_length) == SONARE_OK);
    CHECK(audio_length > 0);
    sonare_free_floats(audio);
    CHECK(sonare_mfcc_to_audio(mfcc.coefficients, total + 1, mfcc.n_mfcc, mfcc.n_frames,
                               mfcc.n_mels, mfcc.sample_rate, mfcc.n_fft, mfcc.hop_length,
                               mfcc.fmin_hz, mfcc.fmax_hz, 2, &audio_options, &audio,
                               &audio_length) == SONARE_ERROR_INVALID_PARAMETER);
    sonare_free_inverse_result(&checked);
    sonare_free_inverse_result(&unlifted);
    sonare_free_mfcc_result(&mfcc);
  }
}

TEST_CASE("buffer and result entry points define their outputs on a refused input",
          "[c_api][features][out_params]") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const std::vector<float> bad_values = {0.5f, nan};
  float poison = 0.0f;

  SECTION("a float buffer transform") {
    float* out = &poison;
    size_t out_length = 7;
    REQUIRE(sonare_power_to_db(bad_values.data(), bad_values.size(), 1.0f, 1e-10f, 80.0f, &out,
                               &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }

  SECTION("the interval list of split_silence") {
    int* intervals = reinterpret_cast<int*>(&poison);
    size_t interval_count = 7;
    REQUIRE(sonare_split_silence(bad_values.data(), bad_values.size(), 60.0f, 2048, 512, &intervals,
                                 &interval_count) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(intervals == nullptr);
    REQUIRE(interval_count == 0);
  }

  SECTION("a scope result refused on a null channel") {
    SonareVectorscopeResult scope;
    std::memset(&scope, 0x5a, sizeof(scope));
    REQUIRE(sonare_metering_vectorscope(bad_values.data(), nullptr, bad_values.size(), 22050, 0,
                                        &scope) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(scope.point_count == 0);
    REQUIRE(scope.points == nullptr);
  }

  SECTION("a loudness result refused on an empty buffer") {
    SonareLufsResult lufs;
    std::memset(&lufs, 0x5a, sizeof(lufs));
    REQUIRE(sonare_lufs(nullptr, 0, 22050, &lufs) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(lufs.integrated_lufs == 0.0f);
  }
}

TEST_CASE("silence C entry points validate finite positive thresholds",
          "[c_api][features][validation]") {
  const std::vector<float> samples = {0.0f, 1.0f, 0.0f};
  const float negative_zero = -0.0f;
  const float negative_neighbor = std::nextafter(0.0f, -1.0f);
  const float positive_neighbor = std::nextafter(0.0f, 1.0f);
  const std::vector<float> invalid = {
      std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity(),
      0.0f,
      negative_zero,
      negative_neighbor,
  };

  for (const float top_db : invalid) {
    CAPTURE(top_db);
    float* trimmed = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
    size_t trimmed_length = 7;
    int64_t start = 42;
    int64_t end = 43;
    REQUIRE(sonare_trim_silence(samples.data(), samples.size(), top_db, 3, 1, &trimmed,
                                &trimmed_length, &start, &end) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(trimmed == nullptr);
    REQUIRE(trimmed_length == 0);
    REQUIRE(start == 0);
    REQUIRE(end == 0);

    int* intervals = reinterpret_cast<int*>(static_cast<uintptr_t>(0x1));
    size_t interval_count = 7;
    REQUIRE(sonare_split_silence(samples.data(), samples.size(), top_db, 3, 1, &intervals,
                                 &interval_count) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(intervals == nullptr);
    REQUIRE(interval_count == 0);
  }

  for (const float top_db : {positive_neighbor, std::numeric_limits<float>::max()}) {
    CAPTURE(top_db);
    float* trimmed = nullptr;
    size_t trimmed_length = 0;
    int64_t start = 0;
    int64_t end = 0;
    REQUIRE(sonare_trim_silence(samples.data(), samples.size(), top_db, 3, 1, &trimmed,
                                &trimmed_length, &start, &end) == SONARE_OK);
    sonare_free_floats(trimmed);

    int* intervals = nullptr;
    size_t interval_count = 0;
    REQUIRE(sonare_split_silence(samples.data(), samples.size(), top_db, 3, 1, &intervals,
                                 &interval_count) == SONARE_OK);
    sonare_free_ints(intervals);
  }
}

TEST_CASE("pre and de-emphasis C entry points validate parameters and clear outputs",
          "[c_api][features][validation]") {
  const std::vector<float> samples = {1.0f, 2.0f, 3.0f};
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  for (const bool de : {false, true}) {
    for (const float coef : {nan, inf, -inf}) {
      CAPTURE(de, coef);
      float* out = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
      size_t out_length = 7;
      const SonareError error =
          de ? sonare_deemphasis(samples.data(), samples.size(), coef, 0.0f, 0, &out, &out_length)
             : sonare_preemphasis(samples.data(), samples.size(), coef, 0.0f, 0, &out, &out_length);
      REQUIRE(error == SONARE_ERROR_INVALID_PARAMETER);
      REQUIRE(out == nullptr);
      REQUIRE(out_length == 0);
    }

    float* out = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
    size_t out_length = 7;
    const SonareError error =
        de ? sonare_deemphasis(samples.data(), samples.size(), 0.5f, nan, 1, &out, &out_length)
           : sonare_preemphasis(samples.data(), samples.size(), 0.5f, nan, 1, &out, &out_length);
    REQUIRE(error == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }

  // A raw poisoned zi is deliberately ignored when use_zi is zero. This is
  // the C representation of an unengaged std::optional<float>.
  float* pre = nullptr;
  size_t pre_length = 0;
  REQUIRE(sonare_preemphasis(samples.data(), samples.size(), 0.5f, nan, 0, &pre, &pre_length) ==
          SONARE_OK);
  REQUIRE(pre_length == samples.size());
  REQUIRE(pre[0] == Catch::Approx(1.0f));
  REQUIRE(pre[1] == Catch::Approx(1.5f));
  REQUIRE(pre[2] == Catch::Approx(2.0f));
  sonare_free_floats(pre);

  float* de = nullptr;
  size_t de_length = 0;
  REQUIRE(sonare_deemphasis(samples.data(), samples.size(), 0.5f, nan, 0, &de, &de_length) ==
          SONARE_OK);
  REQUIRE(de_length == samples.size());
  REQUIRE(de[0] == Catch::Approx(1.2f));
  REQUIRE(de[1] == Catch::Approx(2.6f));
  REQUIRE(de[2] == Catch::Approx(4.3f));
  sonare_free_floats(de);

  // Parameter checks remain active even for an allowed empty transform.
  float* empty_out = reinterpret_cast<float*>(static_cast<uintptr_t>(0x1));
  size_t empty_length = 7;
  REQUIRE(sonare_preemphasis(nullptr, 0, nan, nan, 0, &empty_out, &empty_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(empty_out == nullptr);
  REQUIRE(empty_length == 0);
}

TEST_CASE("sonare_vector_normalize refuses a norm_type outside 0..3", "[c_api][features]") {
  const std::vector<float> values = {3.0f, -4.0f};
  for (int norm_type : {-1, 4, 5}) {
    CAPTURE(norm_type);
    float* out = reinterpret_cast<float*>(&norm_type);
    size_t out_length = 99;
    REQUIRE(sonare_vector_normalize(values.data(), values.size(),
                                    static_cast<SonareNormType>(norm_type), 0.0f, &out,
                                    &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    REQUIRE(out == nullptr);
    REQUIRE(out_length == 0);
  }
  // Each documented ordinal selects its own norm: max-abs 4, L1 7, L2 5, power 25.
  const float expected_first[] = {3.0f / 4.0f, 3.0f / 7.0f, 3.0f / 5.0f, 3.0f / 25.0f};
  for (int norm_type = 0; norm_type <= 3; ++norm_type) {
    CAPTURE(norm_type);
    float* out = nullptr;
    size_t out_length = 0;
    REQUIRE(sonare_vector_normalize(values.data(), values.size(),
                                    static_cast<SonareNormType>(norm_type), 0.0f, &out,
                                    &out_length) == SONARE_OK);
    REQUIRE(out_length == values.size());
    REQUIRE(std::abs(out[0] - expected_first[norm_type]) <= 1e-6f);
    sonare_free_floats(out);
  }
}

TEST_CASE("array transforms refuse a non-finite input", "[c_api]") {
  std::vector<float> input(4096, 0.25f);
  input[100] = std::numeric_limits<float>::quiet_NaN();
  const auto refused_as = [](SonareError code, const char* text) {
    CHECK(code == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) == text);
  };
  float* out = nullptr;
  size_t out_length = 0;
  int* intervals = nullptr;
  int64_t start = 0;
  int64_t end = 0;
  int n_frames = 0;
  refused_as(sonare_preemphasis(input.data(), input.size(), 0.97f, 0.0f, 0, &out, &out_length),
             "input contains a non-finite sample");
  refused_as(sonare_deemphasis(input.data(), input.size(), 0.97f, 0.0f, 0, &out, &out_length),
             "input contains a non-finite sample");
  refused_as(sonare_trim_silence(input.data(), input.size(), 60.0f, 2048, 512, &out, &out_length,
                                 &start, &end),
             "x contains a non-finite sample");
  refused_as(
      sonare_split_silence(input.data(), input.size(), 60.0f, 2048, 512, &intervals, &out_length),
      "x contains a non-finite sample");
  refused_as(
      sonare_frame_signal(input.data(), input.size(), 2048, 512, &out, &out_length, &n_frames),
      "input contains a non-finite sample");
  refused_as(sonare_fourier_tempogram(input.data(), 256, 22050, 512, 8, 1, 0, &out, &out_length,
                                      &n_frames),
             "onset_envelope contains a non-finite sample at index 100");
  refused_as(
      sonare_tempogram_ratio(input.data(), 5 * 200, 8, 22050, 512, nullptr, 0, &out, &out_length),
      "tempogram_data contains a non-finite sample at index 100");
  CHECK(out == nullptr);
  CHECK(intervals == nullptr);

  // A NULL buffer with a length is still refused before the core is reached, with a reason.
  CHECK(sonare_fourier_tempogram(nullptr, 8, 22050, 512, 8, 1, 0, &out, &out_length, &n_frames) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()).find("NULL") != std::string::npos);
}

TEST_CASE("every constant-Q entry bounds its hop at the CQT's maximum on both sides",
          "[c_api][feature]") {
  const std::vector<float> samples(22050, 0.1f);
  const std::vector<float> magnitude(84 * 4, 0.1f);
  for (const size_t hop : {size_t{0}, size_t{65537}, std::numeric_limits<size_t>::max()}) {
    INFO("hop_length " << hop);
    SonareChromaResult chroma{};
    CHECK(sonare_chroma_cqt(samples.data(), samples.size(), 22050, hop, 12, nullptr, &chroma) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) ==
          "hop_length must be an integer in [1, 65536]");
    CHECK(sonare_chroma_cens(samples.data(), samples.size(), 22050, hop, 12, nullptr, &chroma) ==
          SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) ==
          "hop_length must be an integer in [1, 65536]");
    float* out = nullptr;
    size_t out_length = 0;
    CHECK(sonare_cqt_to_audio(magnitude.data(), magnitude.size(), 84, 4, 22050, hop, 32.70f, 12, 2,
                              &out, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) ==
          "hop_length must be an integer in [1, 65536]");
    CHECK(sonare_vqt_to_audio(magnitude.data(), magnitude.size(), 84, 4, 22050, hop, 32.70f, 12,
                              0.0f, 2, &out, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(std::string(sonare_last_error_message()) ==
          "hop_length must be an integer in [1, 65536]");
  }
}

namespace {

/// The refusal subjects of the last call, in order.
std::vector<std::string> mel_refusal_subjects() {
  std::vector<std::string> subjects;
  for (size_t i = 0; i < sonare_last_refusal_subject_count(); ++i) {
    subjects.emplace_back(sonare_last_refusal_subject(i));
  }
  return subjects;
}

std::vector<float> mel_points(int n, float fmin, float fmax, int32_t htk) {
  float* out = nullptr;
  size_t out_length = 0;
  REQUIRE(sonare_mel_frequencies(n, fmin, fmax, htk, &out, &out_length) == SONARE_OK);
  std::vector<float> points(out, out + out_length);
  sonare_free_floats(out);
  return points;
}

}  // namespace

TEST_CASE("sonare_mel_filterbank shapes its bands on the mel points of n_mels + 2", "[c_api]") {
  for (const int32_t htk : {0, 1}) {
    SonareMelFilterbankOptions options;
    sonare_mel_filterbank_options_init(&options);
    options.fmin_hz = 30.0f;
    options.fmax_hz = 8000.0f;
    options.htk = htk;
    options.norm = SONARE_MEL_NORM_NONE;
    float* out = nullptr;
    size_t out_length = 0;
    int n_mels = 0;
    int n_bins = 0;
    REQUIRE(sonare_mel_filterbank(22050, 2048, 40, &options, &out, &out_length, &n_mels, &n_bins) ==
            SONARE_OK);
    REQUIRE(n_mels == 40);
    REQUIRE(n_bins == 1025);
    REQUIRE(out_length == static_cast<size_t>(n_mels) * n_bins);
    const std::vector<float> points = mel_points(n_mels + 2, 30.0f, 8000.0f, htk);
    const float bin_hz = 22050.0f / 2048.0f;
    for (int m = 0; m < n_mels; ++m) {
      // An unnormalised band is a unit triangle, so its tallest bin sits at the centre point.
      const float* row = out + static_cast<size_t>(m) * n_bins;
      const int peak = static_cast<int>(std::max_element(row, row + n_bins) - row);
      CHECK(std::fabs(peak * bin_hz - points[m + 1]) <= bin_hz);
      CHECK(row[peak] <= 1.0f + 1e-6f);
    }
    sonare_free_floats(out);
  }
}

TEST_CASE("sonare_mel_filterbank reads fmax_hz 0 as the Nyquist and scales by norm", "[c_api]") {
  auto filterbank = [](float fmax_hz, int32_t norm) {
    SonareMelFilterbankOptions options;
    sonare_mel_filterbank_options_init(&options);
    options.fmax_hz = fmax_hz;
    options.norm = norm;
    float* out = nullptr;
    size_t out_length = 0;
    int n_mels = 0;
    int n_bins = 0;
    REQUIRE(sonare_mel_filterbank(16000, 512, 24, &options, &out, &out_length, &n_mels, &n_bins) ==
            SONARE_OK);
    std::vector<float> values(out, out + out_length);
    sonare_free_floats(out);
    return values;
  };
  SonareMelFilterbankOptions defaults;
  sonare_mel_filterbank_options_init(&defaults);
  REQUIRE(defaults.norm == SONARE_MEL_NORM_SLANEY);
  REQUIRE(defaults.fmax_hz == 0.0f);
  CHECK(filterbank(0.0f, SONARE_MEL_NORM_SLANEY) == filterbank(8000.0f, SONARE_MEL_NORM_SLANEY));
  CHECK(filterbank(0.0f, SONARE_MEL_NORM_SLANEY) != filterbank(0.0f, SONARE_MEL_NORM_NONE));
}

TEST_CASE("sonare_mel_frequencies spans fmin to fmax and one point is fmin", "[c_api]") {
  const std::vector<float> points = mel_points(40, 30.0f, 8000.0f, 0);
  REQUIRE(points.size() == 40);
  CHECK(std::fabs(points.front() - 30.0f) <= 1e-3f);
  CHECK(std::fabs(points.back() - 8000.0f) <= 8000.0f * 1e-5f);
  CHECK(std::is_sorted(points.begin(), points.end()));
  const std::vector<float> one = mel_points(1, 30.0f, 8000.0f, 1);
  REQUIRE(one.size() == 1);
  CHECK(std::fabs(one[0] - 30.0f) <= 1e-3f);

  float* out = nullptr;
  size_t out_length = 0;
  int n_mels = 0;
  int n_bins = 0;
  REQUIRE(sonare_mel_filterbank(22050, 2048, 1, nullptr, &out, &out_length, &n_mels, &n_bins) ==
          SONARE_OK);
  CHECK(n_mels == 1);
  sonare_free_floats(out);
}

TEST_CASE("sonare_mel_filterbank and sonare_mel_frequencies refuse by name", "[c_api]") {
  float* out = nullptr;
  size_t out_length = 0;
  int n_mels = 0;
  int n_bins = 0;
  SonareMelFilterbankOptions options;
  sonare_mel_filterbank_options_init(&options);

  options.fmin_hz = std::numeric_limits<float>::quiet_NaN();
  REQUIRE(sonare_mel_filterbank(22050, 2048, 40, &options, &out, &out_length, &n_mels, &n_bins) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) ==
        "options.fmin_hz must be finite and at least 0");
  options.fmin_hz = 0.0f;
  options.fmax_hz = std::numeric_limits<float>::infinity();
  REQUIRE(sonare_mel_filterbank(22050, 2048, 40, &options, &out, &out_length, &n_mels, &n_bins) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) ==
        "options.fmax_hz must be finite and at least 0");
  options.fmax_hz = 0.0f;
  options.norm = 2;
  REQUIRE(sonare_mel_filterbank(22050, 2048, 40, &options, &out, &out_length, &n_mels, &n_bins) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mel_refusal_subjects() == std::vector<std::string>{"options.norm"});
  options.norm = SONARE_MEL_NORM_SLANEY;

  options.fmin_hz = 500.0f;
  options.fmax_hz = 100.0f;
  REQUIRE(sonare_mel_filterbank(22050, 2048, 40, &options, &out, &out_length, &n_mels, &n_bins) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) ==
        "options.fmax_hz must be greater than options.fmin_hz");
  CHECK(mel_refusal_subjects() == std::vector<std::string>{"options.fmax_hz", "options.fmin_hz"});
  options.fmin_hz = 12000.0f;
  options.fmax_hz = 0.0f;
  REQUIRE(sonare_mel_filterbank(22050, 2048, 40, &options, &out, &out_length, &n_mels, &n_bins) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) ==
        "options.fmax_hz must be greater than options.fmin_hz");
  CHECK(mel_refusal_subjects() == std::vector<std::string>{"options.fmax_hz", "options.fmin_hz"});
  REQUIRE(sonare_mel_filterbank(22050, 2048, 0, nullptr, &out, &out_length, &n_mels, &n_bins) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mel_refusal_subjects() == std::vector<std::string>{"n_mels"});
  CHECK(out == nullptr);

  REQUIRE(sonare_mel_frequencies(40, 0.0f, std::numeric_limits<float>::quiet_NaN(), 0, &out,
                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) ==
        "fmax_hz must be finite and greater than fmin_hz");
  REQUIRE(sonare_mel_frequencies(40, -1.0f, 8000.0f, 0, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "fmin_hz must be finite and at least 0");
  REQUIRE(sonare_mel_frequencies(0, 0.0f, 8000.0f, 0, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(mel_refusal_subjects() == std::vector<std::string>{"n_mels"});
  CHECK(out == nullptr);
}

TEST_CASE("the mel family refuses an empty band range by naming both bounds", "[c_api]") {
  const std::vector<float> samples(4096, 0.0f);
  SonareMelSpectrogramOptions spectrogram_options;
  sonare_mel_spectrogram_options_init(&spectrogram_options);
  spectrogram_options.fmin_hz = 12000.0f;
  SonareMelResult spectrogram{};
  REQUIRE(sonare_mel_spectrogram(samples.data(), samples.size(), 22050, 1024, 512, 40,
                                 &spectrogram_options,
                                 &spectrogram) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) ==
        "options.fmax_hz must be greater than options.fmin_hz");
  CHECK(mel_refusal_subjects() == std::vector<std::string>{"options.fmax_hz", "options.fmin_hz"});

  const int n_mels = 16;
  const int n_frames = 4;
  const std::vector<float> mel(static_cast<size_t>(n_mels * n_frames), 1.0f);
  SonareInverseResult stft{};
  REQUIRE(sonare_mel_to_stft(mel.data(), mel.size(), n_mels, n_frames, 22050, 1024, 12000.0f, 0.0f,
                             nullptr, &stft) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "fmax_hz must be greater than fmin_hz");
  CHECK(mel_refusal_subjects() == std::vector<std::string>{"fmax_hz", "fmin_hz"});

  float* audio = nullptr;
  size_t audio_length = 0;
  REQUIRE(sonare_mel_to_audio(mel.data(), mel.size(), n_mels, n_frames, 22050, 1024, 256, 12000.0f,
                              0.0f, 1, nullptr, &audio,
                              &audio_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "fmax_hz must be greater than fmin_hz");
  CHECK(mel_refusal_subjects() == std::vector<std::string>{"fmax_hz", "fmin_hz"});
  CHECK(audio == nullptr);
}
