/// @file c_header_order_editing_test.cpp
/// @brief The compat, metering, acoustic, playback, sample-bank and streaming-retune C entries
/// clear
///        every out parameter first and then refuse arguments in public-header parameter order,
///        whatever else is wrong with the call.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

std::string last_message() {
  const char* message = sonare_last_error_message();
  return message != nullptr ? message : "";
}

constexpr int kRate = 22050;
constexpr size_t kLength = 64;

std::vector<float> make_samples() {
  std::vector<float> samples(kLength);
  for (size_t i = 0; i < kLength; ++i) {
    samples[i] = 0.1f * std::sin(0.05f * static_cast<float>(i));
  }
  return samples;
}

/// A pointer value no entry may dereference or free; a cleared out parameter must not keep it.
template <typename T>
T* garbage_pointer() {
  return reinterpret_cast<T*>(static_cast<std::uintptr_t>(0x10));
}

}  // namespace

TEST_CASE("compat entries refuse the input before the outputs", "[c_api][header_order]") {
  const std::vector<float> samples = make_samples();

  float* out = garbage_pointer<float>();
  size_t out_length = 77;
  REQUIRE(sonare_power_to_db(nullptr, 4, 1.0f, 1e-10f, 80.0f, &out, &out_length) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "power_to_db: values must not be NULL with a non-zero length");
  CHECK(out == nullptr);
  CHECK(out_length == 0);

  REQUIRE(sonare_power_to_db(nullptr, 4, 1.0f, 1e-10f, 80.0f, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "power_to_db: values must not be NULL with a non-zero length");

  out = garbage_pointer<float>();
  out_length = 77;
  int n_frames = 55;
  REQUIRE(sonare_cyclic_tempogram(nullptr, 4, kRate, 512, 384, 30.0f, 12, &out, &out_length,
                                  &n_frames) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "cyclic_tempogram: onset_envelope must not be NULL with a non-zero length");
  CHECK(out == nullptr);
  CHECK(out_length == 0);
  CHECK(n_frames == 0);

  REQUIRE(sonare_cyclic_tempogram(nullptr, 4, kRate, 512, 384, 30.0f, 12, nullptr, nullptr,
                                  nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() ==
        "cyclic_tempogram: onset_envelope must not be NULL with a non-zero length");

  SonareLufsResult lufs;
  lufs.integrated_lufs = 5.0f;
  REQUIRE(sonare_lufs_interleaved(samples.data(), kLength, 0, kRate, &lufs) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "lufs_interleaved: channels must be positive");
  CHECK(lufs.integrated_lufs == 0.0f);

  REQUIRE(sonare_lufs_interleaved(samples.data(), kLength, 0, kRate, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "lufs_interleaved: channels must be positive");
}

TEST_CASE("metering entries refuse the channels before the output", "[c_api][header_order]") {
  const std::vector<float> samples = make_samples();

  REQUIRE(sonare_metering_stereo_correlation(nullptr, samples.data(), kLength, kRate, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "left must not be NULL");

  float value = 9.0f;
  REQUIRE(sonare_metering_stereo_correlation(nullptr, samples.data(), kLength, kRate, &value) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "left must not be NULL");
  CHECK(value == 0.0f);

  SonareVectorscopeResult scope;
  scope.points = garbage_pointer<SonareVectorscopePoint>();
  scope.point_count = 31;
  REQUIRE(sonare_metering_vectorscope(samples.data(), nullptr, kLength, kRate, &scope) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "right must not be NULL");
  CHECK(scope.points == nullptr);
  CHECK(scope.point_count == 0);

  REQUIRE(sonare_metering_vectorscope(samples.data(), nullptr, kLength, kRate, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "right must not be NULL");
}

#if defined(SONARE_WITH_ACOUSTIC_SIM)
TEST_CASE("acoustic entries refuse the config before the output", "[c_api][header_order]") {
  const std::vector<float> samples = make_samples();

  SonareRoomEstimate estimate;
  estimate.band_count = 9;
  estimate.band_absorption = garbage_pointer<float>();
  REQUIRE(sonare_estimate_room(samples.data(), kLength, kRate, nullptr, &estimate) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config must not be NULL");
  CHECK(estimate.band_count == 0);
  CHECK(estimate.band_absorption == nullptr);

  REQUIRE(sonare_estimate_room(samples.data(), kLength, kRate, nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config must not be NULL");

  // A rejected rate is reported before the NULL result slot; the rate asked for stays readable.
  SonareRirSynthConfig config;
  sonare_rir_synth_config_init(&config);
  SonareRirSynthResult result;
  result.rir = garbage_pointer<float>();
  result.length = 12;
  REQUIRE(sonare_synthesize_rir(&config, -5, &result) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(result.rir == nullptr);
  CHECK(result.length == 0);
  CHECK(result.sample_rate == -5);

  REQUIRE(sonare_synthesize_rir(&config, -5, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().empty());
}
#endif  // defined(SONARE_WITH_ACOUSTIC_SIM)

#if defined(SONARE_WITH_PLAYBACK)
TEST_CASE("playback entries refuse the inputs before the output", "[c_api][header_order]") {
  SonareHrtfSet* set = garbage_pointer<SonareHrtfSet>();
  REQUIRE(sonare_hrtf_set_create_from_memory(nullptr, 0, &set) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "data must not be NULL");
  CHECK(set == nullptr);

  REQUIRE(sonare_hrtf_set_create_from_memory(nullptr, 0, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "data must not be NULL");

  SonarePlaybackRenderer* renderer = garbage_pointer<SonarePlaybackRenderer>();
  REQUIRE(sonare_playback_renderer_create_json(nullptr, nullptr, kRate, 0, &renderer) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config_json must not be NULL");
  CHECK(renderer == nullptr);

  REQUIRE(sonare_playback_renderer_create_json(nullptr, nullptr, kRate, 0, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config_json must not be NULL");

  int channels = 41;
  REQUIRE(sonare_playback_config_num_output_channels(nullptr, &channels) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config_json must not be NULL");
  CHECK(channels == 0);

  REQUIRE(sonare_playback_config_num_output_channels(nullptr, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "config_json must not be NULL");
}
#endif  // defined(SONARE_WITH_PLAYBACK)

#if defined(SONARE_WITH_ARRANGEMENT)
TEST_CASE("sample bank entries clear the output and refuse the bank first",
          "[c_api][header_order]") {
  size_t count = 123;
  REQUIRE(sonare_sample_bank_sample_count(nullptr, &count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "bank must not be NULL");
  CHECK(count == 0);

  count = 123;
  REQUIRE(sonare_sample_bank_set_count(nullptr, &count) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "bank must not be NULL");
  CHECK(count == 0);

  REQUIRE(sonare_sample_bank_set_count(nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "bank must not be NULL");

  REQUIRE(sonare_sample_bank_create(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message() == "out must not be NULL");
}
#endif  // defined(SONARE_WITH_ARRANGEMENT)

#if defined(SONARE_WITH_VOICE_CHANGER)
TEST_CASE("streaming retune create refuses the config before the output", "[c_api][header_order]") {
  SonareStreamingRetune* retune = garbage_pointer<SonareStreamingRetune>();
  REQUIRE(sonare_streaming_retune_create(std::nanf(""), 0.5f, 0, &retune) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().find("pitch_semitones") != std::string::npos);
  CHECK(retune == nullptr);

  REQUIRE(sonare_streaming_retune_create(std::nanf(""), 0.5f, 0, nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(last_message().find("pitch_semitones") != std::string::npos);

  int grain = 99;
  float semitones = 4.0f;
  float mix = 4.0f;
  REQUIRE(sonare_streaming_retune_config(nullptr, &semitones, &mix, &grain) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(semitones == 0.0f);
  CHECK(mix == 0.0f);
  CHECK(grain == 0);
}
#endif  // defined(SONARE_WITH_VOICE_CHANGER)
