/// @file audio_test.cpp
/// @brief Tests for Audio buffer class.

#include "core/audio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstring>
#include <iterator>
#include <limits>
#include <vector>

#include "support/audio_fixtures.h"
#include "util/exception.h"
#include "util/types.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
using sonare::test::generate_sine;
}  // namespace

TEST_CASE("Audio from_buffer", "[audio]") {
  std::vector<float> samples = generate_sine(1000, 440.0f, 22050);
  Audio audio = Audio::from_buffer(samples.data(), samples.size(), 22050);

  REQUIRE(audio.size() == 1000);
  REQUIRE(audio.sample_rate() == 22050);
  REQUIRE(audio.channels() == 1);
  REQUIRE_FALSE(audio.empty());
  REQUIRE_THAT(audio.duration(), WithinRel(1000.0f / 22050.0f, 0.001f));
}

TEST_CASE("default Audio exposes a valid empty iterator range", "[audio][iterator]") {
  const Audio audio;
  REQUIRE(audio.data() == nullptr);
  REQUIRE(audio.begin() != nullptr);
  REQUIRE(audio.begin() == audio.end());
  REQUIRE(std::distance(audio.begin(), audio.end()) == 0);

  size_t visited = 0;
  for ([[maybe_unused]] float sample : audio) ++visited;
  REQUIRE(visited == 0);
}

TEST_CASE("Audio from_buffer null safety", "[audio]") {
  SECTION("null pointer with zero size is valid (empty audio)") {
    Audio audio = Audio::from_buffer(nullptr, 0, 22050);
    REQUIRE(audio.empty());
    REQUIRE(audio.size() == 0);
    REQUIRE(audio.sample_rate() == 22050);

    Audio mono = audio.to_mono();
    REQUIRE(mono.empty());
    REQUIRE(mono.size() == 0);
    REQUIRE(mono.sample_rate() == 22050);
    REQUIRE(mono.data() == nullptr);
  }

  SECTION("null pointer with non-zero size throws InvalidParameter") {
    try {
      Audio audio = Audio::from_buffer(nullptr, 10, 22050);
      FAIL("Expected SonareException");
    } catch (const SonareException& e) {
      REQUIRE(e.code() == ErrorCode::InvalidParameter);
    }
  }

  SECTION("valid pointer with zero size is valid (empty audio)") {
    float dummy = 0.0f;
    Audio audio = Audio::from_buffer(&dummy, 0, 22050);
    REQUIRE(audio.empty());
    REQUIRE(audio.size() == 0);
    REQUIRE(audio.sample_rate() == 22050);
  }

  SECTION("valid pointer with non-zero size succeeds") {
    std::vector<float> samples = {0.1f, 0.2f, 0.3f, 0.4f};
    Audio audio = Audio::from_buffer(samples.data(), samples.size(), 22050);
    REQUIRE(audio.size() == 4);
    REQUIRE(audio.sample_rate() == 22050);
    REQUIRE_THAT(audio[0], WithinAbs(0.1f, 1e-6f));
    REQUIRE_THAT(audio[3], WithinAbs(0.4f, 1e-6f));
  }

  SECTION("invalid sample rate throws InvalidParameter") {
    std::vector<float> samples = {0.1f, 0.2f};
    try {
      Audio audio = Audio::from_buffer(samples.data(), samples.size(), 0);
      FAIL("Expected SonareException");
    } catch (const SonareException& e) {
      REQUIRE(e.code() == ErrorCode::InvalidParameter);
    }
  }
}

TEST_CASE("Audio from_memory null safety", "[audio]") {
  SECTION("null pointer throws InvalidParameter") {
    try {
      Audio audio = Audio::from_memory(nullptr, 100);
      FAIL("Expected SonareException");
    } catch (const SonareException& e) {
      REQUIRE(e.code() == ErrorCode::InvalidParameter);
    }
  }

  SECTION("zero size throws InvalidParameter") {
    uint8_t dummy = 0;
    try {
      Audio audio = Audio::from_memory(&dummy, 0);
      FAIL("Expected SonareException");
    } catch (const SonareException& e) {
      REQUIRE(e.code() == ErrorCode::InvalidParameter);
    }
  }
}

namespace {

// Minimal single-channel 32-bit-float RIFF/WAVE blob. IEEE-float samples are
// passed through by the decoder untouched, which is how a NaN (or a rate no
// analysis entry point would accept) reaches a decoded handle.
std::vector<uint8_t> float_wav(const std::vector<float>& samples, uint32_t sample_rate) {
  std::vector<uint8_t> out;
  const auto push_u32 = [&out](uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
    }
  };
  const auto push_u16 = [&out](uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
  };
  const auto push_tag = [&out](const char* tag) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(tag[i]));
  };

  const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * sizeof(float));
  push_tag("RIFF");
  push_u32(36u + data_bytes);
  push_tag("WAVE");
  push_tag("fmt ");
  push_u32(16u);
  push_u16(3u);  // WAVE_FORMAT_IEEE_FLOAT
  push_u16(1u);  // mono
  push_u32(sample_rate);
  push_u32(sample_rate * 4u);  // byte rate
  push_u16(4u);                // block align
  push_u16(32u);               // bits per sample
  push_tag("data");
  push_u32(data_bytes);
  for (const float sample : samples) {
    uint32_t bits = 0;
    std::memcpy(&bits, &sample, sizeof(bits));
    push_u32(bits);
  }
  return out;
}

ErrorCode from_memory_error(const std::vector<uint8_t>& blob) {
  try {
    const Audio audio = Audio::from_memory(blob.data(), blob.size());
    CAPTURE(audio.size(), audio.sample_rate());
    return ErrorCode::Ok;
  } catch (const SonareException& error) {
    return error.code();
  }
}

}  // namespace

TEST_CASE("Audio decoded from memory obeys the offline-analysis policy", "[audio][numeric]") {
  // A decoded handle used to skip the checks every raw-buffer entry point runs,
  // so the same samples were accepted from a blob and rejected from a buffer,
  // and the analyses on that handle returned NaN.
  const std::vector<float> clean = generate_sine(2205, 440.0f, 22050);

  SECTION("a well-formed float WAV still decodes") {
    const Audio audio =
        Audio::from_memory(float_wav(clean, 22050).data(), float_wav(clean, 22050).size());
    REQUIRE(audio.size() == clean.size());
    REQUIRE(audio.sample_rate() == 22050);
  }

  SECTION("non-finite decoded samples are refused") {
    std::vector<float> poisoned = clean;
    poisoned[100] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(from_memory_error(float_wav(poisoned, 22050)) == ErrorCode::DecodeFailed);
    poisoned[100] = std::numeric_limits<float>::infinity();
    REQUIRE(from_memory_error(float_wav(poisoned, 22050)) == ErrorCode::DecodeFailed);

    // The buffer entry point refuses the same samples, which is the agreement
    // that was missing: one rejected, the other returned a usable handle.
    REQUIRE_THROWS_AS(validate_offline_audio_input(poisoned.data(), poisoned.size(), 22050),
                      SonareException);
  }

  SECTION("a declared sample rate outside the supported range is refused") {
    // Rates the decoder itself accepts: this guard is what refuses them, and it
    // reports the container as malformed rather than blaming a caller argument.
    for (uint32_t rate : {1u, static_cast<uint32_t>(kMinAudioSampleRate) - 1u}) {
      CAPTURE(rate);
      REQUIRE(from_memory_error(float_wav(clean, rate)) == ErrorCode::InvalidFormat);
    }
    // Above the supported ceiling the WAV parser refuses the header before the
    // guard sees it, so only the rejection itself is asserted -- the error class
    // there belongs to the decoder.
    REQUIRE(from_memory_error(float_wav(clean, static_cast<uint32_t>(kMaxAudioSampleRate) + 1u)) !=
            ErrorCode::Ok);
  }
}

TEST_CASE("Audio from_vector", "[audio]") {
  std::vector<float> samples = generate_sine(2205, 440.0f, 22050);
  Audio audio = Audio::from_vector(std::move(samples), 22050);

  REQUIRE(audio.size() == 2205);
  REQUIRE(audio.sample_rate() == 22050);
  REQUIRE_THAT(audio.duration(), WithinRel(0.1f, 0.001f));  // 100ms
}

TEST_CASE("Audio slice by time", "[audio]") {
  constexpr int sr = 22050;
  std::vector<float> samples = generate_sine(sr, 440.0f, sr);  // 1 second
  Audio audio = Audio::from_vector(std::move(samples), sr);

  SECTION("slice first half") {
    Audio slice = audio.slice(0.0f, 0.5f);
    REQUIRE(slice.size() == sr / 2);
    REQUIRE(slice.sample_rate() == sr);
    REQUIRE_THAT(slice.duration(), WithinRel(0.5f, 0.001f));
  }

  SECTION("slice second half") {
    Audio slice = audio.slice(0.5f, 1.0f);
    REQUIRE(slice.size() == sr / 2);
    // First sample should be at 0.5s mark
    REQUIRE_THAT(slice[0], WithinAbs(audio[sr / 2], 1e-6f));
  }

  SECTION("slice to end (negative end_time)") {
    Audio slice = audio.slice(0.5f);
    REQUIRE(slice.size() == sr / 2);
  }
}

TEST_CASE("Audio slice by samples", "[audio]") {
  std::vector<float> samples(1000);
  for (int i = 0; i < 1000; ++i) {
    samples[i] = static_cast<float>(i);
  }
  Audio audio = Audio::from_vector(std::move(samples), 22050);

  SECTION("slice range") {
    Audio slice = audio.slice_samples(100, 500);
    REQUIRE(slice.size() == 400);
    REQUIRE_THAT(slice[0], WithinAbs(100.0f, 1e-6f));
    REQUIRE_THAT(slice[399], WithinAbs(499.0f, 1e-6f));
  }

  SECTION("slice shares buffer") {
    Audio slice = audio.slice_samples(0, 500);
    // Data pointers should be within the original buffer
    REQUIRE(slice.data() == audio.data());
  }

  SECTION("empty slice preserves sample rate") {
    Audio slice = audio.slice_samples(500, 500);
    REQUIRE(slice.empty());
    REQUIRE(slice.sample_rate() == 22050);
  }
}

TEST_CASE("Audio to_mono creates copy", "[audio]") {
  std::vector<float> samples = generate_sine(1000, 440.0f, 22050);
  Audio audio = Audio::from_vector(std::move(samples), 22050);
  Audio mono = audio.to_mono();

  REQUIRE(mono.size() == audio.size());
  REQUIRE(mono.sample_rate() == audio.sample_rate());
  // Should be a copy, not sharing buffer
  REQUIRE(mono.data() != audio.data());
}

TEST_CASE("Audio empty", "[audio]") {
  Audio audio;
  REQUIRE(audio.empty());
  REQUIRE(audio.size() == 0);
  REQUIRE(audio.data() == nullptr);
  REQUIRE(audio.duration() == 0.0f);
}

TEST_CASE("Audio iterator", "[audio]") {
  std::vector<float> samples = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  Audio audio = Audio::from_vector(std::move(samples), 22050);

  float sum = 0.0f;
  for (float s : audio) {
    sum += s;
  }
  REQUIRE_THAT(sum, WithinAbs(15.0f, 1e-6f));
}
