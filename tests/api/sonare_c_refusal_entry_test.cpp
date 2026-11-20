/// @file sonare_c_refusal_entry_test.cpp
/// @brief sonare_last_refusal_entry names the refused call by its c-entry-name key.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

namespace {

constexpr int kRate = 22050;

std::vector<float> tone() {
  std::vector<float> samples(2048);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = 0.25f * std::sin(0.05f * static_cast<float>(i));
  }
  return samples;
}

}  // namespace

TEST_CASE("sonare_last_refusal_entry names the refused entry without its prefix",
          "[c_api][refusal_entry]") {
  const std::vector<float> samples = tone();
  float* out = nullptr;
  size_t out_length = 0;
  REQUIRE(sonare_fade(samples.data(), samples.size(), kRate, 0.0f, -1.0f, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_last_refusal_subject_count() > 0);
  REQUIRE(sonare_last_refusal_entry() != nullptr);
  CHECK(std::string(sonare_last_refusal_entry()) == "fade");
}

TEST_CASE("sonare_last_refusal_entry folds a second form into its base key",
          "[c_api][refusal_entry]") {
  const std::vector<float> mel(16 * 4, 1.0f);
  SonareInverseResult result{};
  REQUIRE(sonare_mel_to_stft_ex(mel.data(), 16, 4, 0, 512, 0.0f, 0.0f, 0, &result) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_last_refusal_subject_count() > 0);
  REQUIRE(sonare_last_refusal_entry() != nullptr);
  CHECK(std::string(sonare_last_refusal_entry()) == "mel_to_stft");
}

TEST_CASE("sonare_last_refusal_entry is NULL exactly when no refusal is recorded",
          "[c_api][refusal_entry]") {
  const std::vector<float> samples = tone();
  float* out = nullptr;
  size_t out_length = 0;
  REQUIRE(sonare_fade(samples.data(), samples.size(), kRate, 0.0f, 0.0f, &out, &out_length) ==
          SONARE_OK);
  sonare_free_floats(out);
  CHECK(sonare_last_refusal_subject_count() == 0);
  CHECK(sonare_last_refusal_entry() == nullptr);

  REQUIRE(sonare_fade(samples.data(), samples.size(), kRate, 0.0f, -1.0f, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  const std::string first = sonare_last_refusal_entry();
  CHECK(sonare_last_refusal_entry() == sonare_last_refusal_entry());
  CHECK(std::string(sonare_last_error_message()).find("fade_out_sec") != std::string::npos);
  CHECK(first == "fade");
}
