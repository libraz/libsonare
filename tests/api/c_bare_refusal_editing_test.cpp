/// @file c_bare_refusal_editing_test.cpp
/// @brief The metering and scale entries record a stated cause for every argument they refuse.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace {

std::string last_message() {
  const char* message = sonare_last_error_message();
  return message != nullptr ? message : "";
}

const float kNaN = std::numeric_limits<float>::quiet_NaN();

}  // namespace

TEST_CASE("the true-peak entry names a NULL result slot", "[c_api][editing]") {
  const std::vector<float> samples(2048, 0.1f);
  REQUIRE(sonare_metering_true_peak_dbtp(samples.data(), samples.size(), 22050, 4, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_db must not be NULL");
}

TEST_CASE("the silence ratio entries name the refused argument", "[c_api][editing]") {
  const std::vector<float> samples(2048, 0.1f);
  float ratio = 0.0f;
  const std::string frame_range = " must be an integer in [1, 2147483647]";

  REQUIRE(sonare_metering_silence_ratio(samples.data(), samples.size(), 22050, kNaN, 512, 256,
                                        &ratio) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "threshold_db must be finite and in [-3.4028235e38, 3.4028235e38]");
  REQUIRE(sonare_metering_silence_ratio(samples.data(), samples.size(), 22050, -60.0f, 0, 256,
                                        &ratio) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "frame_length" + frame_range);
  REQUIRE(sonare_metering_silence_ratio(samples.data(), samples.size(), 22050, -60.0f, 512, 0,
                                        &ratio) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "hop_length" + frame_range);
  REQUIRE(sonare_metering_silence_ratio(samples.data(), samples.size(), 22050, -60.0f, 512, 256,
                                        nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_ratio must not be NULL");

  REQUIRE(sonare_audio_silence_ratio(nullptr, -60.0f, 512, 256, &ratio) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "audio must not be NULL");

  SonareAudio* audio = nullptr;
  REQUIRE(sonare_audio_from_buffer(samples.data(), samples.size(), 22050, &audio) == SONARE_OK);
  REQUIRE(sonare_audio_silence_ratio(audio, kNaN, 512, 256, &ratio) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "threshold_db must be finite and in [-3.4028235e38, 3.4028235e38]");
  REQUIRE(sonare_audio_silence_ratio(audio, -60.0f, -1, 256, &ratio) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "frame_length" + frame_range);
  REQUIRE(sonare_audio_silence_ratio(audio, -60.0f, 512, 0, &ratio) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "hop_length" + frame_range);
  REQUIRE(sonare_audio_silence_ratio(audio, -60.0f, 512, 256, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_ratio must not be NULL");
  sonare_audio_destroy(audio);
}

TEST_CASE("the scale entries name the refused argument", "[c_api][editing]") {
  float out_f = 0.0f;
  int out_i = 0;
  uint16_t out_mask = 0;
  const uint16_t major = 0x0AB5;

  REQUIRE(sonare_scale_quantize_midi(12, major, kNaN, 60.0f, &out_f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "root must be an integer in [0, 11]");
  REQUIRE(sonare_scale_quantize_midi(0, 0, kNaN, 60.0f, &out_f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "mode_mask must be an integer in [1, 4095]");
  REQUIRE(sonare_scale_quantize_midi(0, 0x1000, kNaN, 60.0f, &out_f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "mode_mask must be an integer in [1, 4095]");
  REQUIRE(sonare_scale_quantize_midi(0, major, kNaN, kNaN, &out_f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "midi must be finite and in [-3.4028235e38, 3.4028235e38]");
  REQUIRE(sonare_scale_quantize_midi(0, major, kNaN, 60.0f, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_quantized_midi must not be NULL");

  REQUIRE(sonare_scale_correction_semitones(-1, major, kNaN, 60.0f, &out_f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "root must be an integer in [0, 11]");
  REQUIRE(sonare_scale_correction_semitones(0, major, kNaN, kNaN, &out_f) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "midi must be finite and in [-3.4028235e38, 3.4028235e38]");
  REQUIRE(sonare_scale_correction_semitones(0, major, kNaN, 60.0f, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_semitones must not be NULL");

  REQUIRE(sonare_scale_pitch_class_enabled(0, 0, 0, &out_i) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "mode_mask must be an integer in [1, 4095]");
  REQUIRE(sonare_scale_pitch_class_enabled(0, major, 12, &out_i) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "pitch_class must be an integer in [0, 11]");
  REQUIRE(sonare_scale_pitch_class_enabled(0, major, 0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_enabled must not be NULL");

  REQUIRE(sonare_scale_mask_for_mode(12, SONARE_MODE_MAJOR, &out_mask) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "root must be an integer in [0, 11]");
  REQUIRE(sonare_scale_mask_for_mode(0, static_cast<SonareMode>(7), &out_mask) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "mode must be an integer in [0, 6]");
  REQUIRE(sonare_scale_mask_for_mode(0, SONARE_MODE_MAJOR, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(last_message() == "out_mask must not be NULL");
}
