/// @file audio_io_test.cpp
/// @brief Tests for audio I/O functions.

#include "core/audio_io.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstring>
#include <vector>

using namespace sonare;

namespace {

/// @brief Creates a simple mono WAV file in memory.
std::vector<uint8_t> create_wav_buffer(const float* samples, size_t sample_count, int sample_rate) {
  // WAV header for mono float32
  struct WavHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t file_size;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmt_size = 16;
    uint16_t audio_format = 3;  // IEEE float
    uint16_t num_channels = 1;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align = 4;  // 1 channel * 4 bytes
    uint16_t bits_per_sample = 32;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t data_size;
  };

  WavHeader header;
  header.sample_rate = static_cast<uint32_t>(sample_rate);
  header.byte_rate = header.sample_rate * 4;
  header.data_size = static_cast<uint32_t>(sample_count * 4);
  header.file_size = 36 + header.data_size;

  std::vector<uint8_t> buffer(sizeof(WavHeader) + header.data_size);
  std::memcpy(buffer.data(), &header, sizeof(WavHeader));
  std::memcpy(buffer.data() + sizeof(WavHeader), samples, sample_count * 4);

  return buffer;
}

/// @brief Creates a mono 16-bit PCM WAV file in memory.
std::vector<uint8_t> create_wav_buffer_pcm16(const float* samples, size_t sample_count,
                                             int sample_rate) {
  struct WavHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t file_size;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1;  // PCM
    uint16_t num_channels = 1;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align = 2;  // 1 channel * 2 bytes
    uint16_t bits_per_sample = 16;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t data_size;
  };

  WavHeader header;
  header.sample_rate = static_cast<uint32_t>(sample_rate);
  header.byte_rate = header.sample_rate * 2;
  header.data_size = static_cast<uint32_t>(sample_count * 2);
  header.file_size = 36 + header.data_size;

  // Convert float samples to int16
  std::vector<int16_t> pcm_samples(sample_count);
  for (size_t i = 0; i < sample_count; ++i) {
    float clamped = std::max(-1.0f, std::min(1.0f, samples[i]));
    pcm_samples[i] = static_cast<int16_t>(clamped * 32767.0f);
  }

  std::vector<uint8_t> buffer(sizeof(WavHeader) + header.data_size);
  std::memcpy(buffer.data(), &header, sizeof(WavHeader));
  std::memcpy(buffer.data() + sizeof(WavHeader), pcm_samples.data(), sample_count * 2);

  return buffer;
}

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

std::vector<float> generate_sine(int samples, float freq, int sr) {
  std::vector<float> result(samples);
  for (int i = 0; i < samples; ++i) {
    result[i] = std::sin(kTwoPi * freq * i / sr);
  }
  return result;
}

}  // namespace

TEST_CASE("detect_format WAV", "[audio_io]") {
  // RIFF....WAVE header
  std::vector<uint8_t> wav_header = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
  REQUIRE(detect_format(wav_header.data(), wav_header.size()) == AudioFormat::WAV);
}

TEST_CASE("detect_format MP3 with ID3", "[audio_io]") {
  // ID3 tag header
  std::vector<uint8_t> mp3_header = {'I', 'D', '3', 0x04, 0x00, 0, 0, 0, 0, 0, 0, 0};
  REQUIRE(detect_format(mp3_header.data(), mp3_header.size()) == AudioFormat::MP3);
}

TEST_CASE("detect_format MP3 frame sync", "[audio_io]") {
  // MP3 frame sync bytes (0xFF followed by 0xFB for MPEG 1 Layer 3)
  std::vector<uint8_t> mp3_header = {0xFF, 0xFB, 0x90, 0x00, 0, 0, 0, 0, 0, 0, 0, 0};
  REQUIRE(detect_format(mp3_header.data(), mp3_header.size()) == AudioFormat::MP3);
}

TEST_CASE("detect_format unknown", "[audio_io]") {
  std::vector<uint8_t> unknown = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                  0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B};
  REQUIRE(detect_format(unknown.data(), unknown.size()) == AudioFormat::Unknown);
}

TEST_CASE("detect_format too small", "[audio_io]") {
  std::vector<uint8_t> small = {0x00, 0x01, 0x02};
  REQUIRE(detect_format(small.data(), small.size()) == AudioFormat::Unknown);
}

TEST_CASE("load_buffer_wav float32", "[audio_io]") {
  constexpr int sr = 22050;
  constexpr int samples = 1000;
  std::vector<float> original = generate_sine(samples, 440.0f, sr);
  std::vector<uint8_t> wav_data = create_wav_buffer(original.data(), original.size(), sr);

  auto [loaded, loaded_sr] = load_buffer_wav(wav_data.data(), wav_data.size());

  REQUIRE(loaded_sr == sr);
  REQUIRE(loaded.size() == samples);

  // Compare samples (should be identical for float32)
  for (size_t i = 0; i < samples; ++i) {
    REQUIRE(loaded[i] == original[i]);
  }
}

TEST_CASE("load_buffer_wav pcm16", "[audio_io]") {
  constexpr int sr = 44100;
  constexpr int samples = 2000;
  std::vector<float> original = generate_sine(samples, 1000.0f, sr);
  std::vector<uint8_t> wav_data = create_wav_buffer_pcm16(original.data(), original.size(), sr);

  auto [loaded, loaded_sr] = load_buffer_wav(wav_data.data(), wav_data.size());

  REQUIRE(loaded_sr == sr);
  REQUIRE(loaded.size() == samples);

  // Compare samples (allow for 16-bit quantization error)
  // The error can be up to 1/32768 per sample due to quantization
  using Catch::Matchers::WithinAbs;
  for (size_t i = 0; i < samples; ++i) {
    REQUIRE_THAT(loaded[i], WithinAbs(original[i], 2.0f / 32767.0f));
  }
}

TEST_CASE("load_buffer auto-detect WAV", "[audio_io]") {
  constexpr int sr = 22050;
  constexpr int samples = 500;
  std::vector<float> original = generate_sine(samples, 880.0f, sr);
  std::vector<uint8_t> wav_data = create_wav_buffer(original.data(), original.size(), sr);

  auto [loaded, loaded_sr] = load_buffer(wav_data.data(), wav_data.size());

  REQUIRE(loaded_sr == sr);
  REQUIRE(loaded.size() == samples);
}

TEST_CASE("AudioLoadOptions defaults", "[audio_io]") {
  SECTION("default max_file_size is 500MB") {
    AudioLoadOptions opts;
    REQUIRE(opts.max_file_size == 500 * 1024 * 1024);
  }

  SECTION("kDefaultLoadOptions uses defaults") {
    REQUIRE(kDefaultLoadOptions.max_file_size == 500 * 1024 * 1024);
  }
}
