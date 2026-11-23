/// @file c_bare_refusal_compat_test.cpp
/// @brief The compat and transform entries record a stated cause for every argument they refuse.

#include <sonare/sonare_c.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "core/audio.h"

namespace {

std::string last_message() {
  const char* message = sonare_last_error_message();
  return message != nullptr ? message : "";
}

const float kNaN = std::numeric_limits<float>::quiet_NaN();
const std::string kBufferLimit = std::to_string(sonare::kMaxAudioBufferSize);

}  // namespace

TEST_CASE("the dB and level transforms name the refused buffer and output", "[c_api][compat]") {
  const std::vector<float> values(4, 0.5f);
  float* out = nullptr;
  size_t out_length = 0;

  REQUIRE(sonare_power_to_db(nullptr, 4, 1.0f, 1e-10f, 80.0f, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "values must not be NULL when length is not 0");
  REQUIRE(sonare_amplitude_to_db(nullptr, 4, 1.0f, 1e-5f, 80.0f, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "values must not be NULL when length is not 0");
  REQUIRE(sonare_db_to_power(nullptr, 4, 1.0f, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "values must not be NULL when length is not 0");
  REQUIRE(sonare_db_to_amplitude(nullptr, 4, 1.0f, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "values must not be NULL when length is not 0");

  std::vector<float> bad = values;
  bad[2] = kNaN;
  REQUIRE(sonare_db_to_power(bad.data(), 4, 1.0f, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "values contains a non-finite sample at index 2");

  REQUIRE(sonare_db_to_power(values.data(), 4, 1.0f, nullptr, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out must not be NULL");
  REQUIRE(sonare_db_to_power(values.data(), 4, 1.0f, &out, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_length must not be NULL");
  REQUIRE(out == nullptr);
}

TEST_CASE("the length and peak transforms name the refused buffer", "[c_api][compat]") {
  float* out = nullptr;
  int* out_ints = nullptr;
  size_t out_length = 0;

  REQUIRE(sonare_pad_center(nullptr, 3, 8, 0.0f, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "values must not be NULL when length is not 0");
  REQUIRE(sonare_fix_length(nullptr, 3, 8, 0.0f, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "values must not be NULL when length is not 0");
  REQUIRE(sonare_peak_pick(nullptr, 3, 1, 1, 1, 1, 0.1f, 1, &out_ints, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "values must not be NULL when length is not 0");

  REQUIRE(sonare_fix_frames(nullptr, 3, 0, 10, 1, &out_ints, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "frames must not be NULL when length is not 0");
  const std::array<int, 2> frames{1, 2};
  REQUIRE(sonare_fix_frames(frames.data(), 2, 0, 10, 1, nullptr, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out must not be NULL");
  REQUIRE(sonare_fix_frames(frames.data(), 2, 0, 10, 1, &out_ints, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_length must not be NULL");
}

TEST_CASE("the emphasis filters and split_silence name the refused buffer", "[c_api][compat]") {
  float* out = nullptr;
  int* out_ints = nullptr;
  size_t out_length = 0;

  REQUIRE(sonare_preemphasis(nullptr, 3, 0.97f, 0.0f, 0, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "samples must not be NULL when length is not 0");
  REQUIRE(sonare_deemphasis(nullptr, 3, 0.97f, 0.0f, 0, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "samples must not be NULL when length is not 0");
  REQUIRE(sonare_split_silence(nullptr, 3, 60.0f, 2048, 512, &out_ints, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "samples must not be NULL when length is not 0");

  const std::vector<float> samples(8, 0.1f);
  REQUIRE(sonare_preemphasis(samples.data(), 8, 0.97f, 0.0f, 0, nullptr, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out must not be NULL");
  REQUIRE(sonare_split_silence(samples.data(), 8, 60.0f, 2048, 512, &out_ints, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_length must not be NULL");
}

TEST_CASE("sonare_split_silence_common refuses its arguments in header order", "[c_api][compat]") {
  const std::vector<float> take(16, 0.1f);
  std::vector<float> nan_take = take;
  nan_take[2] = kNaN;
  const float* signals[2] = {take.data(), take.data()};
  const size_t lengths[2] = {take.size(), take.size()};
  int* intervals = nullptr;
  size_t interval_count = 0;

  REQUIRE(sonare_split_silence_common(nullptr, 2, lengths, 60.0f, 2048, 512, &intervals,
                                      &interval_count, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "signals must not be NULL");
  REQUIRE(sonare_split_silence_common(signals, 0, lengths, 60.0f, 2048, 512, &intervals,
                                      &interval_count, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "signal_count must be an integer of at least 1");
  REQUIRE(sonare_split_silence_common(signals, 2, nullptr, 60.0f, 2048, 512, &intervals,
                                      &interval_count, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "lengths must not be NULL");

  const float* null_second[2] = {take.data(), nullptr};
  REQUIRE(sonare_split_silence_common(null_second, 2, lengths, 60.0f, 2048, 512, &intervals,
                                      &interval_count, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "signals[1] must not be NULL when lengths[1] is not 0");
  const float* nan_second[2] = {take.data(), nan_take.data()};
  REQUIRE(sonare_split_silence_common(nan_second, 2, lengths, 60.0f, 2048, 512, &intervals,
                                      &interval_count, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "signals[1] contains a non-finite sample at index 2");

  SonareSilenceCommonReport report{};
  REQUIRE(sonare_split_silence_common(signals, 2, lengths, 60.0f, 2048, 512, nullptr,
                                      &interval_count, &report) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_intervals must not be NULL");
  REQUIRE(sonare_split_silence_common(signals, 2, lengths, 60.0f, 2048, 512, &intervals, nullptr,
                                      &report) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_interval_count must not be NULL");
}

TEST_CASE("sonare_vector_normalize names the norm it refuses", "[c_api][compat]") {
  const std::vector<float> values(4, 0.5f);
  float* out = nullptr;
  size_t out_length = 0;

  REQUIRE(sonare_vector_normalize(nullptr, 4, SONARE_NORM_L2, 0.0f, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "values must not be NULL when length is not 0");
  REQUIRE(sonare_vector_normalize(values.data(), 4, static_cast<SonareNormType>(7), 0.0f, &out,
                                  &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "norm must be one of 0, 1, 2, 3");
  REQUIRE(sonare_vector_normalize(values.data(), 4, SONARE_NORM_L2, 0.0f, nullptr, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out must not be NULL");
  REQUIRE(sonare_vector_normalize(values.data(), 4, SONARE_NORM_L2, 0.0f, &out, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_length must not be NULL");
}

TEST_CASE("sonare_pcen and sonare_tonnetz name the refused matrix argument", "[c_api][compat]") {
  const std::vector<float> values(12, 0.5f);
  float* out = nullptr;
  size_t out_length = 0;
  const int max_int = std::numeric_limits<int>::max();

  REQUIRE(sonare_pcen(values.data(), -1, 3, 22050, 512, 0.4f, 0.98f, 2.0f, 0.5f, 1e-6f, &out,
                      &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "n_bins must be an integer in [0, 2147483647]");
  REQUIRE(sonare_pcen(values.data(), 4, -1, 22050, 512, 0.4f, 0.98f, 2.0f, 0.5f, 1e-6f, &out,
                      &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "n_frames must be an integer in [0, 2147483647]");
  REQUIRE(sonare_pcen(values.data(), max_int, max_int, 22050, 512, 0.4f, 0.98f, 2.0f, 0.5f, 1e-6f,
                      &out, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "n_bins times n_frames must be at most " + kBufferLimit);
  REQUIRE(sonare_pcen(nullptr, 4, 3, 22050, 512, 0.4f, 0.98f, 2.0f, 0.5f, 1e-6f, &out,
                      &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "values must not be NULL when n_bins and n_frames are not 0");
  REQUIRE(sonare_pcen(values.data(), 4, 3, 22050, 512, 0.4f, 0.98f, 2.0f, 0.5f, 1e-6f, nullptr,
                      &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out must not be NULL");
  REQUIRE(sonare_pcen(values.data(), 4, 3, 22050, 512, 0.4f, 0.98f, 2.0f, 0.5f, 1e-6f, &out,
                      nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_length must not be NULL");

  REQUIRE(sonare_tonnetz(values.data(), -1, 2, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "n_chroma must be an integer in [0, 2147483647]");
  REQUIRE(sonare_tonnetz(values.data(), 6, -1, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "n_frames must be an integer in [0, 2147483647]");
  REQUIRE(sonare_tonnetz(values.data(), max_int, max_int, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "n_chroma times n_frames must be at most " + kBufferLimit);
  REQUIRE(sonare_tonnetz(nullptr, 6, 2, &out, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "chromagram must not be NULL when n_chroma and n_frames are not 0");
  REQUIRE(sonare_tonnetz(values.data(), 6, 2, nullptr, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out must not be NULL");
  REQUIRE(sonare_tonnetz(values.data(), 6, 2, &out, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_length must not be NULL");
}

TEST_CASE("the tempogram family names the refused onset envelope and mode", "[c_api][compat]") {
  const std::vector<float> envelope(64, 0.5f);
  std::vector<float> nan_envelope = envelope;
  nan_envelope[3] = kNaN;
  float* out = nullptr;
  size_t out_length = 0;
  int n_frames = 0;

  REQUIRE(sonare_tempogram(nullptr, 64, 22050, 512, 16, 1, 0, SONARE_TEMPOGRAM_AUTOCORRELATION,
                           &out, &out_length, &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "onset_envelope must not be NULL when length is not 0");
  REQUIRE(sonare_tempogram(nan_envelope.data(), 64, 22050, 512, 16, 1, 0,
                           SONARE_TEMPOGRAM_AUTOCORRELATION, &out, &out_length,
                           &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "onset_envelope contains a non-finite sample at index 3");
  REQUIRE(sonare_tempogram(envelope.data(), 64, 22050, 512, 16, 1, 0, 9, &out, &out_length,
                           &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "mode must be one of " +
                                std::to_string(SONARE_TEMPOGRAM_AUTOCORRELATION) + ", " +
                                std::to_string(SONARE_TEMPOGRAM_COSINE));
  REQUIRE(sonare_tempogram(envelope.data(), 64, 22050, 512, 16, 1, 0,
                           SONARE_TEMPOGRAM_AUTOCORRELATION, nullptr, &out_length,
                           &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out must not be NULL");

  REQUIRE(sonare_cyclic_tempogram(nullptr, 64, 22050, 512, 16, 30.0f, 12, &out, &out_length,
                                  &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "onset_envelope must not be NULL when length is not 0");
  REQUIRE(sonare_cyclic_tempogram(nan_envelope.data(), 64, 22050, 512, 16, 30.0f, 12, &out,
                                  &out_length, &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "onset_envelope contains a non-finite sample at index 3");

  REQUIRE(sonare_plp(nullptr, 64, 22050, 512, 30.0f, 300.0f, 16, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "onset_envelope must not be NULL when length is not 0");
  REQUIRE(sonare_plp(nan_envelope.data(), 64, 22050, 512, 30.0f, 300.0f, 16, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "onset_envelope contains a non-finite sample at index 3");
  REQUIRE(sonare_plp(envelope.data(), 64, 22050, 512, 30.0f, 300.0f, 16, nullptr, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out must not be NULL");
  REQUIRE(sonare_plp(envelope.data(), 64, 22050, 512, 30.0f, 300.0f, 16, &out, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_length must not be NULL");

  REQUIRE(sonare_fourier_tempogram(nullptr, 64, 22050, 512, 16, 1, 0, &out, &out_length,
                                   &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "onset_envelope must not be NULL when length is not 0");

  REQUIRE(sonare_tempogram_ratio(nullptr, 64, 16, 22050, 512, nullptr, 0, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "tempogram must not be NULL when length is not 0");
  REQUIRE(sonare_tempogram_ratio(envelope.data(), 64, 16, 22050, 512, nullptr, 2, &out,
                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "factors must not be NULL when n_factors is not 0");
  REQUIRE(sonare_tempogram_ratio(envelope.data(), 64, 16, 22050, 512, nullptr, 0, nullptr,
                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out must not be NULL");
  REQUIRE(sonare_tempogram_ratio(envelope.data(), 64, 16, 22050, 512, nullptr, 0, &out, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_length must not be NULL");
}

TEST_CASE("the interleaved LUFS entries name the refused argument", "[c_api][compat]") {
  const std::vector<float> stereo(2 * 4800, 0.1f);
  SonareLufsResult result{};

  REQUIRE(sonare_lufs_interleaved(stereo.data(), 4800, 0, 48000, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "num_channels must be an integer in [1, 2147483647]");
  REQUIRE(sonare_lufs_interleaved(stereo.data(), sonare::kMaxAudioBufferSize, 2, 48000, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "frames times num_channels must be at most " + kBufferLimit);

  float* momentary = nullptr;
  size_t momentary_length = 0;
  float* short_term = nullptr;
  size_t short_term_length = 0;
  REQUIRE(sonare_lufs_series_interleaved(stereo.data(), 4800, 2, 48000, &momentary, nullptr,
                                         nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() ==
          "out_momentary_lufs_length must not be NULL when out_momentary_lufs is not NULL");
  REQUIRE(sonare_lufs_series_interleaved(stereo.data(), 4800, 2, 48000, &momentary,
                                         &momentary_length, &short_term,
                                         nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() ==
          "out_short_term_lufs_length must not be NULL when out_short_term_lufs is not NULL");
  REQUIRE(sonare_lufs_series_interleaved(stereo.data(), 4800, -1, 48000, &momentary,
                                         &momentary_length, &short_term,
                                         &short_term_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "num_channels must be an integer in [1, 2147483647]");
}
