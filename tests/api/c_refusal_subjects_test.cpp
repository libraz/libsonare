/// @file c_refusal_subjects_test.cpp
/// @brief C entries refuse their own arguments under the entry's name without `sonare_`.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <limits>
#include <string>
#include <vector>

namespace {

std::string last_message() {
  const char* message = sonare_last_error_message();
  return message != nullptr ? message : "";
}

}  // namespace

TEST_CASE("the interleaved LUFS entries name themselves and the refused argument",
          "[c_api][metering]") {
  std::vector<float> stereo(2 * 4800, 0.1f);
  SonareLufsResult result{};
  CHECK(sonare_lufs_interleaved(stereo.data(), 4800, 0, 48000, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(),
             Catch::Matchers::ContainsSubstring("lufs_interleaved: channels must be positive"));

  stereo[7] = std::numeric_limits<float>::quiet_NaN();
  CHECK(sonare_lufs_interleaved(stereo.data(), 4800, 2, 48000, &result) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(), Catch::Matchers::StartsWith("lufs_interleaved: samples"));

  float* momentary = nullptr;
  CHECK(sonare_lufs_series_interleaved(stereo.data(), 4800, 2, 48000, &momentary, nullptr, nullptr,
                                       nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(),
             Catch::Matchers::ContainsSubstring(
                 "lufs_series_interleaved: out_momentary and out_momentary_length"));
  float* short_term = nullptr;
  size_t momentary_length = 0;
  size_t short_term_length = 0;
  CHECK(sonare_lufs_series_interleaved(stereo.data(), 4800, 2, 48000, &momentary, &momentary_length,
                                       &short_term,
                                       &short_term_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(), Catch::Matchers::StartsWith("lufs_series_interleaved: samples"));
}

TEST_CASE("the true-peak entries refuse an unsupported oversample factor by its parameter name",
          "[c_api][metering]") {
  const std::vector<float> samples(4800, 0.25f);
  float db = 0.0f;
  CHECK(sonare_metering_true_peak_dbtp(samples.data(), samples.size(), 48000, 3, &db) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "oversample_factor must be one of 1, 2, 4, 8, 16");
  REQUIRE(sonare_metering_true_peak_dbtp(samples.data(), samples.size(), 48000, 0, &db) ==
          SONARE_OK);
  float explicit_db = 0.0f;
  REQUIRE(sonare_metering_true_peak_dbtp(samples.data(), samples.size(), 48000, 4, &explicit_db) ==
          SONARE_OK);
  CHECK(db == explicit_db);

  SonareAudio* audio = nullptr;
  REQUIRE(sonare_audio_from_buffer(samples.data(), samples.size(), 48000, &audio) == SONARE_OK);
  CHECK(sonare_audio_true_peak_dbtp(audio, -2, &db) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "oversample_factor must be one of 1, 2, 4, 8, 16");
  sonare_audio_destroy(audio);
}

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("take alignment names the buffer it refuses", "[c_api][project]") {
  std::vector<float> reference(22050, 0.1f);
  std::vector<float> take(22050, 0.1f);
  SonareProjectWarpAnchor* anchors = nullptr;
  size_t count = 0;
  SonareTakeAlignment alignment{};

  take[100] = std::numeric_limits<float>::infinity();
  CHECK(sonare_align_take_to_reference_ex(reference.data(), reference.size(), 22050, take.data(),
                                          take.size(), 22050, nullptr, &anchors, &count,
                                          &alignment) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(), Catch::Matchers::StartsWith("align_take_to_reference_ex: take"));

  reference[5] = std::numeric_limits<float>::quiet_NaN();
  CHECK(sonare_align_take_to_reference_ex(reference.data(), reference.size(), 22050, take.data(),
                                          take.size(), 22050, nullptr, &anchors, &count,
                                          &alignment) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(), Catch::Matchers::StartsWith("align_take_to_reference_ex: reference"));

  CHECK(sonare_align_take_to_reference_ex(reference.data(), reference.size(), 22050, take.data(),
                                          take.size(), 22050, nullptr, nullptr, &count,
                                          &alignment) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(), Catch::Matchers::ContainsSubstring(
                                 "align_take_to_reference_ex: out_anchors must not be NULL"));
}
#endif  // defined(SONARE_WITH_ARRANGEMENT)

TEST_CASE("the CQT-family entries refuse each member by its core name", "[c_api][feature]") {
  const std::vector<float> samples(22050, 0.1f);
  SonareCqtResult out{};
  const auto refused = [&](int hop, float fmin, int n_bins, int bpo) {
    CHECK(sonare_cqt(samples.data(), samples.size(), 22050, hop, fmin, n_bins, bpo, &out) ==
          SONARE_ERROR_INVALID_PARAMETER);
    return last_message();
  };
  CHECK(refused(0, 32.7f, 24, 12) == "hop_length must be an integer in [1, 2147483647]");
  CHECK(refused(512, 0.0f, 24, 12) == "fmin must be finite and greater than 0");
  CHECK(refused(512, 32.7f, 0, 12) == "n_bins must be an integer in [1, 2147483647]");
  CHECK(refused(512, 32.7f, 24, 0) == "bins_per_octave must be an integer in [1, 2147483647]");

  CHECK(sonare_pseudo_cqt(samples.data(), samples.size(), 22050, 512, 32.7f, 0, 12, &out) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "n_bins must be an integer in [1, 2147483647]");
  CHECK(sonare_vqt(samples.data(), samples.size(), 22050, 0, 32.7f, 24, 12, 0.0f, &out) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "hop_length must be an integer in [1, 2147483647]");
  CHECK(sonare_vqt(samples.data(), samples.size(), 22050, 512, 32.7f, 24, 12,
                   std::numeric_limits<float>::infinity(), &out) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(), Catch::Matchers::ContainsSubstring("gamma must not be infinite"));
}

TEST_CASE("the CQT inverse refuses each member by its core name", "[c_api][feature]") {
  const std::vector<float> magnitude(24 * 4, 1.0f);
  float* out = nullptr;
  size_t out_length = 0;
  const auto message = [&](int n_bins, int n_frames, int sample_rate, int hop, float fmin, int bpo,
                           int n_iter) {
    CHECK(sonare_cqt_to_audio_checked(magnitude.data(), magnitude.size(), n_bins, n_frames,
                                      sample_rate, hop, fmin, bpo, n_iter, &out,
                                      &out_length) == SONARE_ERROR_INVALID_PARAMETER);
    return last_message();
  };
  CHECK(message(0, 4, 22050, 512, 32.7f, 12, 4) == "n_bins must be an integer in [1, 2147483647]");
  CHECK(message(24, 0, 22050, 512, 32.7f, 12, 4) ==
        "n_frames must be an integer in [1, 2147483647]");
  CHECK(message(24, 4, 100, 512, 32.7f, 12, 4) ==
        "sample_rate must be an integer in [8000, 384000]");
  CHECK(message(24, 4, 22050, 0, 32.7f, 12, 4) ==
        "hop_length must be an integer in [1, 2147483647]");
  CHECK(message(24, 4, 22050, 512, 0.0f, 12, 4) == "fmin must be finite and greater than 0");
  CHECK(message(24, 4, 22050, 512, 32.7f, 0, 4) ==
        "bins_per_octave must be an integer in [1, 2147483647]");
  CHECK(message(24, 4, 22050, 512, 32.7f, 12, 0) == "n_iter must be an integer in [1, 256]");
  CHECK(message(24, 4, 22050, 512, 32.7f, 12, 257) == "n_iter must be an integer in [1, 256]");
}

TEST_CASE("the music feature entries refuse their own members", "[c_api][feature]") {
  const std::vector<float> samples(22050, 0.1f);
  SonareChromaResult chroma{};
  CHECK(sonare_bass_chroma(samples.data(), samples.size(), 22050, 0, 12, &chroma) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "hop_length must be an integer in [1, 2147483647]");
  CHECK(sonare_bass_chroma(samples.data(), samples.size(), 22050, 512, 0, &chroma) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "n_chroma must be an integer in [1, 2147483647]");
  CHECK(sonare_bass_chroma(samples.data(), samples.size(), 22050, 512, 5, &chroma) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "bins_per_octave must be a multiple of n_chroma");

  float* env = nullptr;
  size_t env_length = 0;
  int n_frames = 0;
  CHECK(sonare_onset_strength_multi(samples.data(), samples.size(), 22050, 2048, 512, 128, 0, &env,
                                    &env_length, &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "n_bands must be an integer in [1, 2147483647]");

  const std::vector<float> freqs{440.0f, 450.0f};
  float tuning = 0.0f;
  CHECK(sonare_pitch_tuning(freqs.data(), freqs.size(), 1.0f, 12, &tuning) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "resolution must be finite and in (0, 1)");
  CHECK(sonare_pitch_tuning(freqs.data(), freqs.size(), 0.01f, 0, &tuning) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "bins_per_octave must be an integer in [1, 2147483647]");
  CHECK(sonare_estimate_tuning(samples.data(), samples.size(), 22050, 2048, 512, 0.0f, 12,
                               &tuning) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "resolution must be finite and in (0, 1)");
  CHECK(sonare_estimate_tuning(samples.data(), samples.size(), 22050, 2048, 512, 0.01f, 0,
                               &tuning) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "bins_per_octave must be an integer in [1, 2147483647]");
}

TEST_CASE("the effects and metering entries refuse their own members", "[c_api][effects]") {
  const std::vector<float> samples(22050, 0.1f);
  float* out = nullptr;
  size_t out_length = 0;
  CHECK(sonare_phase_vocoder(samples.data(), samples.size(), 22050, 0.0f, 2048, 512, &out,
                             &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "rate must be finite and greater than 0");

  const std::vector<float> s(8, 1.0f);
  float *w = nullptr, *h = nullptr;
  size_t w_length = 0, h_length = 0;
  const auto decompose = [&](int n_features, int n_frames, int n_components, int n_iter) {
    CHECK(sonare_decompose_with_init(s.data(), n_features, n_frames, n_components, n_iter, 2.0f,
                                     "random", &w, &w_length, &h,
                                     &h_length) == SONARE_ERROR_INVALID_PARAMETER);
    return last_message();
  };
  CHECK(decompose(0, 4, 2, 5) == "n_features must be an integer in [1, 2147483647]");
  CHECK(decompose(2, 0, 2, 5) == "n_frames must be an integer in [1, 2147483647]");
  CHECK(decompose(2, 4, 0, 5) == "n_components must be an integer in [1, 2147483647]");
  CHECK(decompose(2, 4, 2, -1) == "n_iter must be an integer in [0, 2147483647]");

  SonareWaveformPeaksResult peaks{};
  CHECK(sonare_waveform_peaks(samples.data(), samples.size(), 1, 0, &peaks) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples_per_bucket must be an integer of at least 1");
  const size_t levels[] = {64, 0};
  SonareWaveformPeakPyramidResult pyramid{};
  CHECK(sonare_waveform_peak_pyramid(samples.data(), samples.size(), 1, levels, 2, &pyramid) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "samples_per_bucket must be an integer of at least 1");
}
