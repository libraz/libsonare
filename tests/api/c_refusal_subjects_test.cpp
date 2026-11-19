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

TEST_CASE("the true-peak entries refuse an unsupported oversample factor under their own name",
          "[c_api][metering]") {
  const std::vector<float> samples(4800, 0.25f);
  float db = 0.0f;
  CHECK(sonare_metering_true_peak_dbtp(samples.data(), samples.size(), 48000, 3, &db) ==
        SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(
      last_message(),
      Catch::Matchers::ContainsSubstring(
          "metering_true_peak_dbtp: oversample_factor must be one of 1, 2, 4, 8, 16, got 3"));
  REQUIRE(sonare_metering_true_peak_dbtp(samples.data(), samples.size(), 48000, 0, &db) ==
          SONARE_OK);
  float explicit_db = 0.0f;
  REQUIRE(sonare_metering_true_peak_dbtp(samples.data(), samples.size(), 48000, 4, &explicit_db) ==
          SONARE_OK);
  CHECK(db == explicit_db);

  SonareAudio* audio = nullptr;
  REQUIRE(sonare_audio_from_buffer(samples.data(), samples.size(), 48000, &audio) == SONARE_OK);
  CHECK(sonare_audio_true_peak_dbtp(audio, -2, &db) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(last_message(),
             Catch::Matchers::ContainsSubstring(
                 "audio_true_peak_dbtp: oversample_factor must be one of 1, 2, 4, 8, 16, got -2"));
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
