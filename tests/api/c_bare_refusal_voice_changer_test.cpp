/// @file c_bare_refusal_voice_changer_test.cpp
/// @brief The offline voice changer entries record a stated cause for every argument they refuse.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <string>
#include <vector>

namespace {

std::string last_message() {
  const char* message = sonare_last_error_message();
  return message != nullptr ? message : "";
}

}  // namespace

TEST_CASE("voice_change_ex names the refused config", "[c_api][voice_changer]") {
  const std::vector<float> samples(4096, 0.1f);
  float* out = nullptr;
  size_t out_length = 0;

  REQUIRE(sonare_voice_change_ex(samples.data(), samples.size(), 22050, nullptr, &out,
                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config must not be NULL");

  SonareVoiceChangeConfig config = {0.0f, 1.0f, 2};
  REQUIRE(sonare_voice_change_ex(samples.data(), samples.size(), 22050, &config, &out,
                                 &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config.formant_mode must be one of 0, 1");
}

TEST_CASE("voice_change_realtime names the refused channel layout", "[c_api][voice_changer]") {
  const std::vector<float> samples(4097, 0.1f);
  float* out = nullptr;
  size_t out_length = 0;

  REQUIRE(sonare_voice_change_realtime(samples.data(), samples.size(), 22050, nullptr, 3, &out,
                                       &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "channels must be an integer in [1, 2]");
  REQUIRE(sonare_voice_change_realtime(samples.data(), samples.size(), 22050, nullptr, 2, &out,
                                       &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "length must be even when channels is 2");
}

TEST_CASE("the preset config entry names the refused preset", "[c_api][voice_changer]") {
  SonareRealtimeVoiceChangerConfig config{};
  REQUIRE(sonare_realtime_voice_changer_preset_config(static_cast<SonareVoiceCharacterPreset>(12),
                                                      &config) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "preset must be an integer in [0, 11]");
}
