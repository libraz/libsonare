#include "core/audio_encode.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "core/audio.h"
#include "core/audio_io.h"
#include "core/audio_tags.h"
#include "util/constants.h"
#include "util/exception.h"

namespace {

using namespace sonare;
using Catch::Matchers::WithinAbs;

constexpr int kSampleRate = static_cast<int>(constants::kDefaultDawSampleRate);
constexpr double kPcmTolerance = 1.0 / 32768.0;

uint16_t wav_field(const std::vector<uint8_t>& bytes, std::size_t offset) {
  REQUIRE(bytes.size() >= offset + 2);
  return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8u));
}

template <class Call>
void check_refusal(Call&& call, ErrorCode code, const std::string& subject,
                   std::optional<ErrorReason> reason = std::nullopt) {
  try {
    call();
    FAIL("expected an audio codec refusal");
  } catch (const SonareParameterRefusal& error) {
    CHECK(error.code() == code);
    CHECK(error.subjects() == std::vector<std::string>{subject});
    CHECK(error.reason() == reason);
  }
}

}  // namespace

TEST_CASE("WAV encodes planar samples and INFO tags into one file", "[audio_encode]") {
  const int bits = GENERATE(16, 24, 32);
  const std::array<float, 4> planar{0.25f, -0.25f, 0.75f, -0.5f};
  const std::array<float, 4> expected{0.25f, 0.75f, -0.25f, -0.5f};
  AudioEncodeOptions options;
  options.bit_depth = bits;
  options.tags = {{"title", "Wind"},      {"artist", "演奏者"}, {"album", "Set"},
                  {"date", "2026-11-23"}, {"genre", "Jazz"},    {"track", "3/12"},
                  {"comment", "Take"},    {"IENG", "Engineer"}, {"GNR1", "Session note"}};
  const auto bytes = encode_audio(planar.data(), 2, 2, kSampleRate, options);

  CHECK(wav_field(bytes, 20) == (bits == 32 ? 3 : 1));
  CHECK(wav_field(bytes, 34) == bits);
  const auto [decoded, rate, channels] = load_buffer_interleaved(bytes.data(), bytes.size());
  CHECK(rate == kSampleRate);
  CHECK(channels == 2);
  REQUIRE(decoded.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    CHECK_THAT(decoded[i], WithinAbs(expected[i], kPcmTolerance));
  }
  CHECK(read_audio_tags(bytes.data(), bytes.size()) == options.tags);
}

TEST_CASE("WAV supports extensible layouts and clips integer samples", "[audio_encode]") {
  const int channels = GENERATE(1, 2, 9, 255);
  std::vector<float> planar(static_cast<std::size_t>(channels), 0.25f);
  const auto bytes = encode_audio(planar.data(), 1, channels, kSampleRate);
  CHECK(wav_field(bytes, 20) == (channels > 2 ? 0xfffe : 1));
  CHECK(wav_field(bytes, 22) == channels);
  CHECK(wav_field(bytes, 34) == 16);
  if (channels <= 2) {
    const float loud = 1.25f;
    const auto clipped = encode_audio(&loud, 1, 1, kSampleRate);
    const auto [decoded, rate, count] = load_buffer_interleaved(clipped.data(), clipped.size());
    REQUIRE(decoded.size() == 1);
    CHECK_THAT(decoded.front(), WithinAbs(1.0 - kPcmTolerance, 1.0e-7));
    CHECK(rate == kSampleRate);
    CHECK(count == 1);
  }
}

TEST_CASE("Audio encode refuses invalid data and incompatible options", "[audio_encode]") {
  const float input = 0.5f;
  const auto reject = [&](const AudioEncodeOptions& options, const std::string& subject) {
    check_refusal([&] { encode_audio(&input, 1, 1, kSampleRate, options); },
                  ErrorCode::InvalidParameter, subject);
  };
  check_refusal([&] { encode_audio(&input, 0, 1, kSampleRate); }, ErrorCode::InvalidParameter,
                "frames");
  check_refusal([&] { encode_audio(&input, 1, 0, kSampleRate); }, ErrorCode::InvalidParameter,
                "num_channels");
  check_refusal([&] { encode_audio(&input, 1, 1, kMinAudioSampleRate - 1); },
                ErrorCode::InvalidParameter, "sample_rate");
  check_refusal([&] { encode_audio(nullptr, 1, 1, kSampleRate); }, ErrorCode::InvalidParameter,
                "samples");
  const float bad =
      GENERATE(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
               -std::numeric_limits<float>::infinity());
  check_refusal([&] { encode_audio(&bad, 1, 1, kSampleRate); }, ErrorCode::InvalidParameter,
                "samples");

  AudioEncodeOptions options;
  options.format = "WAV";
  reject(options, "format");
  options.format = "wav";
  options.bit_depth = 0;
  reject(options, "bit_depth");
  options.bit_depth = 8;
  reject(options, "bit_depth");
  options.bit_depth.reset();
  options.quality = 0.5f;
  reject(options, "quality");
  options.quality.reset();
  options.tags = {{"albumartist", "Band"}};
  reject(options, "tags.albumartist");
  options.tags = {{"disc", "1"}};
  reject(options, "tags.disc");
  options.tags.clear();
  options.format = "mp3";
  options.bit_depth = 16;
  reject(options, "bit_depth");
  options.bit_depth.reset();
  options.quality = 1.01f;
  reject(options, "quality");
}

TEST_CASE("Audio codecs match their runtime availability", "[audio_encode]") {
  const auto format = GENERATE("flac", "mp3", "ogg", "opus", "m4a");
  CAPTURE(format);
  CHECK(audio_encode_format_available("wav"));
  CHECK_FALSE(audio_encode_format_available("unknown"));
  AudioEncodeOptions options;
  options.format = format;
  if (!audio_encode_format_available(format)) {
    const float input = 0.5f;
    check_refusal([&] { encode_audio(&input, 1, 1, kSampleRate, options); },
                  ErrorCode::NotSupported, format, ErrorReason::NotCompiledIn);
    return;
  }

  const std::size_t frames = kSampleRate / 10;
  const std::vector<float> input(frames, 0.125f);
  options.tags.emplace("title", "Codec test");
  if (options.format == "m4a") {
    options.tags.emplace("albumartist", "Ensemble");
    options.tags.emplace("SESSION", "Live");
  }
  const auto bytes = encode_audio(input.data(), frames, 1, kSampleRate, options);
  CHECK_FALSE(bytes.empty());
  const auto [decoded, rate, channels] = load_buffer_interleaved(bytes.data(), bytes.size());
  CHECK_FALSE(decoded.empty());
  CHECK(rate == kSampleRate);
  CHECK(channels == 1);
  const auto tags = read_audio_tags(bytes.data(), bytes.size());
  const auto title = tags.find("title");
  REQUIRE(title != tags.end());
  CHECK(title->second == "Codec test");
  if (options.format == "m4a") {
    CHECK(tags.at("albumartist") == "Ensemble");
    CHECK(tags.at("SESSION") == "Live");
  }
}

#if !defined(SONARE_WITH_FFMPEG)
TEST_CASE("Recognized codecs report a compiled-out decoder", "[audio_encode]") {
  const std::array<uint8_t, 12> flac{'f', 'L', 'a', 'C'};
  check_refusal([&] { load_buffer(flac.data(), flac.size()); }, ErrorCode::NotSupported, "flac",
                ErrorReason::NotCompiledIn);
  check_refusal([&] { load_buffer_interleaved(flac.data(), flac.size()); }, ErrorCode::NotSupported,
                "flac", ErrorReason::NotCompiledIn);
}
#endif
