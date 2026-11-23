#include <sonare/sonare_c.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "util/constants.h"
#include "util/json.h"

namespace {

constexpr int kSampleRate = static_cast<int>(sonare::constants::kDefaultDawSampleRate);

struct AudioTagsResult {
  SonareAudioTags value{};
  ~AudioTagsResult() { sonare_free_audio_tags(&value); }
};

struct AudioBytesResult {
  uint8_t* value = nullptr;
  ~AudioBytesResult() { sonare_free_bytes(value); }
};

std::vector<std::string> json_string_array(const sonare::util::json::Value& value) {
  std::vector<std::string> result;
  for (const auto& item : value.as_array()) result.push_back(item.as_string());
  return result;
}

}  // namespace

TEST_CASE("C audio tags read and encode round-trip WAV metadata", "[c_api][codec][audio_tags]") {
  char title_key[] = "title";
  char title_value[] = "Morning";
  char artist_key[] = "artist";
  char artist_value[] = "Ada";
  char* keys[] = {title_key, artist_key};
  char* values[] = {title_value, artist_value};
  SonareAudioTags input_tags{keys, values, 2};

  SonareEncodeOptions options{};
  sonare_encode_options_init(&options);
  REQUIRE(options.struct_size == sizeof(options));
  REQUIRE(std::string(options.format) == "wav");
  REQUIRE(options.bit_depth == 0);
  REQUIRE(std::isnan(options.quality));
  options.tags = &input_tags;

  const std::vector<float> planar{0.25f, -0.25f, 0.75f, -0.5f};
  AudioBytesResult encoded;
  size_t encoded_size = 0;
  REQUIRE(sonare_encode_audio(planar.data(), 2, 2, kSampleRate, &options, &encoded.value,
                              &encoded_size) == SONARE_OK);
  REQUIRE(encoded.value != nullptr);
  REQUIRE(encoded_size > 0);

  AudioTagsResult decoded;
  REQUIRE(sonare_read_audio_tags(encoded.value, encoded_size, &decoded.value) == SONARE_OK);
  REQUIRE(decoded.value.count == 2);
  REQUIRE(decoded.value.keys != nullptr);
  REQUIRE(decoded.value.values != nullptr);
  std::map<std::string, std::string> actual;
  for (size_t i = 0; i < decoded.value.count; ++i) {
    REQUIRE(decoded.value.keys[i] != nullptr);
    REQUIRE(decoded.value.values[i] != nullptr);
    actual.emplace(decoded.value.keys[i], decoded.value.values[i]);
  }
  CHECK((actual == std::map<std::string, std::string>{{"artist", "Ada"}, {"title", "Morning"}}));
}

TEST_CASE("C audio codec capability lists agree", "[c_api][codec][capabilities]") {
  char* raw_capabilities = nullptr;
  REQUIRE(sonare_capabilities_json(&raw_capabilities) == SONARE_OK);
  REQUIRE(raw_capabilities != nullptr);
  const std::string capabilities_text(raw_capabilities);
  sonare_free_string(raw_capabilities);
  const auto capabilities = sonare::util::json::parse_strict(capabilities_text);
  const auto& encode = capabilities["encode"];
  REQUIRE(encode["builtin"].is_array());
  REQUIRE(encode["builtin"].size() == 1);
  CHECK(encode["builtin"][static_cast<size_t>(0)].as_string() == "wav");
  REQUIRE(encode["ffmpeg"].is_array());

  char* raw_catalog = nullptr;
  REQUIRE(sonare_capability_catalog_json(&raw_catalog) == SONARE_OK);
  REQUIRE(raw_catalog != nullptr);
  const std::string catalog_text(raw_catalog);
  sonare_free_string(raw_catalog);
  const auto catalog = sonare::util::json::parse_strict(catalog_text);
  const auto& catalog_encode = catalog["encode"];
  CHECK(json_string_array(catalog_encode["builtin"]) == json_string_array(encode["builtin"]));
  CHECK(json_string_array(catalog_encode["ffmpeg"]) == json_string_array(encode["ffmpeg"]));

  const std::vector<std::string> ffmpeg_formats = json_string_array(encode["ffmpeg"]);
  if (std::find(ffmpeg_formats.begin(), ffmpeg_formats.end(), "flac") == ffmpeg_formats.end()) {
    const float sample = 0.25f;
    SonareEncodeOptions options{};
    sonare_encode_options_init(&options);
    options.format = "flac";
    uint8_t* output = nullptr;
    size_t output_size = 0;
    CHECK(sonare_encode_audio(&sample, 1, 1, kSampleRate, &options, &output, &output_size) ==
          SONARE_ERROR_NOT_SUPPORTED);
    CHECK(output == nullptr);
    CHECK(output_size == 0);
    REQUIRE(sonare_last_error_reason() != nullptr);
    CHECK(std::string(sonare_last_error_reason()) == "notCompiledIn");
    REQUIRE(sonare_last_refusal_subject_count() == 1);
    CHECK(std::string(sonare_last_refusal_subject(0)) == "flac");
    sonare_free_bytes(output);
  }
}

TEST_CASE("C audio codec refusals clear outputs and name option fields", "[c_api][codec]") {
  const float sample = 0.25f;
  SonareEncodeOptions options{};
  sonare_encode_options_init(&options);
  options.format = "WAV";
  uint8_t* encoded = reinterpret_cast<uint8_t*>(1);
  size_t encoded_size = 99;
  REQUIRE(sonare_encode_audio(&sample, 1, 1, kSampleRate, &options, &encoded, &encoded_size) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(encoded == nullptr);
  CHECK(encoded_size == 0);
  REQUIRE(sonare_last_refusal_subject_count() == 1);
  CHECK(std::string(sonare_last_refusal_subject(0)) == "options.format");

  options.format = "wav";
  SonareAudioTags incomplete_tags{nullptr, nullptr, 1};
  options.tags = &incomplete_tags;
  REQUIRE(sonare_encode_audio(&sample, 1, 1, kSampleRate, &options, &encoded, &encoded_size) ==
          SONARE_ERROR_INVALID_PARAMETER);
  CHECK(encoded == nullptr);
  CHECK(encoded_size == 0);
  REQUIRE(sonare_last_refusal_subject_count() == 1);
  CHECK(std::string(sonare_last_refusal_subject(0)) == "tags.keys");
}

TEST_CASE("C audio tag reader rejects an unknown container", "[c_api][codec][audio_tags]") {
  const uint8_t unknown[]{0x00, 0x01, 0x02, 0x03};
  AudioTagsResult tags;
  tags.value.keys = reinterpret_cast<char**>(1);
  tags.value.values = reinterpret_cast<char**>(1);
  tags.value.count = 1;
  REQUIRE(sonare_read_audio_tags(unknown, sizeof(unknown), &tags.value) ==
          SONARE_ERROR_INVALID_FORMAT);
  CHECK(tags.value.keys == nullptr);
  CHECK(tags.value.values == nullptr);
  CHECK(tags.value.count == 0);
}

#if !defined(SONARE_WITH_FFMPEG)
TEST_CASE("C audio codec explains a recognized format absent from the build",
          "[c_api][codec][error_reason]") {
  const uint8_t flac[]{'f', 'L', 'a', 'C'};
  AudioTagsResult tags;
  REQUIRE(sonare_read_audio_tags(flac, sizeof(flac), &tags.value) == SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_last_error_reason() != nullptr);
  CHECK(std::string(sonare_last_error_reason()) == "notCompiledIn");
  CHECK(tags.value.count == 0);

  float* samples = nullptr;
  size_t frames = 1;
  int channels = 1;
  int sample_rate = 1;
  REQUIRE(sonare_decode_channels(flac, sizeof(flac), &samples, &frames, &channels, &sample_rate) ==
          SONARE_ERROR_NOT_SUPPORTED);
  REQUIRE(sonare_last_error_reason() != nullptr);
  CHECK(std::string(sonare_last_error_reason()) == "notCompiledIn");
  CHECK(samples == nullptr);
  CHECK(frames == 0);
  CHECK(channels == 0);
  CHECK(sample_rate == 0);
}
#endif
