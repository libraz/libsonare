/// @file c_bare_refusal_acoustic_test.cpp
/// @brief The acoustic entries record a stated cause for every argument they refuse.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
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

SonareRirSynthConfig rir_config() {
  SonareRirSynthConfig config;
  sonare_rir_synth_config_init(&config);
  return config;
}

SonareRoomMorphConfig morph_config() {
  SonareRoomMorphConfig config;
  sonare_room_morph_config_init(&config);
  return config;
}

}  // namespace

TEST_CASE("synthesize_rir names the refused rate and config member", "[c_api][acoustic]") {
  SonareRirSynthResult out{};
  SonareRirSynthConfig config = rir_config();

  REQUIRE(sonare_synthesize_rir(&config, 1, &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "sample_rate must be an integer in [" +
                                std::to_string(sonare::kMinAudioSampleRate) + ", " +
                                std::to_string(sonare::kMaxAudioSampleRate) + "]");

  config.length_m = kNaN;
  REQUIRE(sonare_synthesize_rir(&config, 48000, &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config.length_m must be finite and in [-3.4028235e38, 3.4028235e38]");

  config = rir_config();
  config.listener = {5.0f, kNaN, 1.7f};
  REQUIRE(sonare_synthesize_rir(&config, 48000, &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() ==
          "config.listener.y must be finite and in [-3.4028235e38, 3.4028235e38]");

  config = rir_config();
  config.absorption = 1.5f;
  REQUIRE(sonare_synthesize_rir(&config, 48000, &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config.absorption must be finite and in [0, 1]");

  config = rir_config();
  config.ism_order = -1;
  REQUIRE(sonare_synthesize_rir(&config, 48000, &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config.ism_order must be an integer in [0, 2147483647]");

  config = rir_config();
  config.max_sec = 601.0f;
  REQUIRE(sonare_synthesize_rir(&config, 48000, &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config.max_sec must be finite and in [0, 600]");

  config = rir_config();
  config.mixing_time_ms = -1.0f;
  REQUIRE(sonare_synthesize_rir(&config, 48000, &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config.mixing_time_ms must be finite and in [0, 10000]");

  config = rir_config();
  config.crossfade_ms = 1001.0f;
  REQUIRE(sonare_synthesize_rir(&config, 48000, &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config.crossfade_ms must be finite and in [0, 1000]");

  config = rir_config();
  config.material_preset = 999;
  REQUIRE(sonare_synthesize_rir(&config, 48000, &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config.material_preset must be an integer in [" +
                                std::to_string(SONARE_MATERIAL_PRESET_NONE) + ", " +
                                std::to_string(SONARE_MATERIAL_PRESET_GLASS) + "]");
}

TEST_CASE("synthesize_rir names the refused band arrays", "[c_api][acoustic]") {
  SonareRirSynthResult out{};
  SonareRirSynthConfig config = rir_config();

  config.band_absorption_count = 65;
  REQUIRE(sonare_synthesize_rir(&config, 48000, &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config.band_absorption_count must be finite and at most 64");

  config = rir_config();
  config.band_scattering_count = 3;
  REQUIRE(sonare_synthesize_rir(&config, 48000, &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() ==
          "config.band_scattering must not be NULL when config.band_scattering_count is not 0");

  const float bands[3] = {0.1f, 2.0f, 0.3f};
  config = rir_config();
  config.band_absorption = bands;
  config.band_absorption_count = 3;
  REQUIRE(sonare_synthesize_rir(&config, 48000, &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config.band_absorption[1] must be finite and in [0, 1]");

  config = rir_config();
  config.band_scattering = bands;
  config.band_scattering_count = 3;
  REQUIRE(sonare_synthesize_rir(&config, 48000, &out) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config.band_scattering[1] must be finite and in [0, 1]");

  config = rir_config();
  REQUIRE(sonare_synthesize_rir(&config, 48000, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out must not be NULL");
}

TEST_CASE("estimate_room names the refused mode", "[c_api][acoustic]") {
  const std::vector<float> samples(2048, 0.1f);
  SonareRoomEstimateConfig config;
  sonare_room_estimate_config_init(&config);
  config.mode = 99;
  SonareRoomEstimate out{};
  REQUIRE(sonare_estimate_room(samples.data(), samples.size(), 22050, &config, &out) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config.mode must be an integer in [" +
                                std::to_string(SONARE_ACOUSTIC_MODE_AUTO) + ", " +
                                std::to_string(SONARE_ACOUSTIC_MODE_IMPULSE_RESPONSE) + "]");
}

TEST_CASE("room_morph names the refused config and outputs", "[c_api][acoustic]") {
  const std::vector<float> samples(2048, 0.1f);
  float* out = nullptr;
  size_t out_length = 0;
  SonareRoomMorphConfig config = morph_config();

  REQUIRE(sonare_room_morph(samples.data(), samples.size(), 22050, nullptr, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config must not be NULL");

  config.absorption = kNaN;
  REQUIRE(sonare_room_morph(samples.data(), samples.size(), 22050, &config, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config.absorption must be finite and in [0, 1]");

  config = morph_config();
  config.source_tail_suppression = 2.0f;
  REQUIRE(sonare_room_morph(samples.data(), samples.size(), 22050, &config, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config.source_tail_suppression must be finite and in [0, 1]");

  config = morph_config();
  config.wet = -0.5f;
  REQUIRE(sonare_room_morph(samples.data(), samples.size(), 22050, &config, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config.wet must be finite and in [0, 1]");

  config = morph_config();
  REQUIRE(sonare_room_morph(samples.data(), samples.size(), 22050, &config, nullptr, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out must not be NULL");
  REQUIRE(sonare_room_morph(samples.data(), samples.size(), 22050, &config, &out, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_length must not be NULL");
}

TEST_CASE("room_morph_stereo names the refused config and output", "[c_api][acoustic]") {
  const std::vector<float> left(2048, 0.1f);
  const std::vector<float> right(2048, 0.1f);
  SonareRoomMorphConfig config = morph_config();
  float* out_left = nullptr;
  float* out_right = nullptr;
  size_t out_length = 0;

  REQUIRE(sonare_room_morph_stereo(left.data(), right.data(), left.size(), 22050, nullptr,
                                   &out_left, &out_right,
                                   &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config must not be NULL");

  config.wet = 3.0f;
  REQUIRE(sonare_room_morph_stereo(left.data(), right.data(), left.size(), 22050, &config,
                                   &out_left, &out_right,
                                   &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "config.wet must be finite and in [0, 1]");

  config = morph_config();
  REQUIRE(sonare_room_morph_stereo(left.data(), right.data(), left.size(), 22050, &config, nullptr,
                                   &out_right, &out_length) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_left must not be NULL");
}
