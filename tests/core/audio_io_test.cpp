/// @file audio_io_test.cpp
/// @brief Tests for audio I/O functions.

#include "core/audio_io.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "support/audio_fixtures.h"
#include "util/exception.h"

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

using sonare::test::generate_sine;

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

TEST_CASE("load_buffer_wav rejects invalid channel count", "[audio_io]") {
  constexpr int sr = 22050;
  constexpr int samples = 128;
  std::vector<float> original = generate_sine(samples, 440.0f, sr);
  std::vector<uint8_t> wav_data = create_wav_buffer(original.data(), original.size(), sr);

  auto* channels = reinterpret_cast<uint16_t*>(wav_data.data() + 22);
  *channels = 0;

  REQUIRE_THROWS(load_buffer_wav(wav_data.data(), wav_data.size()));
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

TEST_CASE("load_audio with max_file_size rejects large files before allocating", "[audio_io]") {
  // Create a small WAV file on disk
  constexpr int sr = 22050;
  constexpr int samples = 100;
  std::vector<float> sine = generate_sine(samples, 440.0f, sr);
  std::vector<uint8_t> wav_data = create_wav_buffer(sine.data(), sine.size(), sr);

  // Write to a temporary file
  std::string tmp_path = "test_max_filesize.wav";
  {
    std::ofstream out(tmp_path, std::ios::binary);
    REQUIRE(out.is_open());
    out.write(reinterpret_cast<const char*>(wav_data.data()),
              static_cast<std::streamsize>(wav_data.size()));
  }

  SECTION("rejects file exceeding max_file_size") {
    AudioLoadOptions opts;
    opts.max_file_size = 10;  // 10 bytes max -- the WAV file is much larger
    REQUIRE_THROWS(load_audio(tmp_path, opts));
  }

  SECTION("accepts file within max_file_size") {
    AudioLoadOptions opts;
    opts.max_file_size = wav_data.size() + 1024;  // Generous limit
    auto [loaded, loaded_sr] = load_audio(tmp_path, opts);
    REQUIRE(loaded_sr == sr);
    REQUIRE(loaded.size() == samples);
  }

  // Clean up
  std::remove(tmp_path.c_str());
}

#ifdef SONARE_WITH_FFMPEG
TEST_CASE("load_audio decodes m4a when built with FFmpeg", "[audio_io][ffmpeg]") {
  // The test relies on the ffmpeg CLI to synthesize an m4a fixture at runtime
  // so we never commit binary audio to the repo. If the CLI is missing (e.g.
  // libavformat is linked but ffmpeg binary isn't installed) we skip cleanly.
  if (std::system("command -v ffmpeg >/dev/null 2>&1") != 0) {
    SKIP("ffmpeg CLI not found on PATH");
  }

  const std::string wav_path = "test_tone_ffmpeg.wav";
  const std::string m4a_path = "test_tone_ffmpeg.m4a";
  auto cleanup = [&]() {
    std::remove(wav_path.c_str());
    std::remove(m4a_path.c_str());
  };

  const std::string gen_wav =
      "ffmpeg -loglevel error -f lavfi -i "
      "sine=frequency=440:duration=0.5:sample_rate=22050 "
      "-ac 1 -y " +
      wav_path;
  const std::string gen_m4a =
      "ffmpeg -loglevel error -i " + wav_path + " -c:a aac -b:a 64k -y " + m4a_path;
  REQUIRE(std::system(gen_wav.c_str()) == 0);
  REQUIRE(std::system(gen_m4a.c_str()) == 0);

  AudioLoadResult loaded;
  REQUIRE_NOTHROW(loaded = load_audio(m4a_path));
  REQUIRE(std::get<0>(loaded).size() > 1000);
  REQUIRE(std::get<1>(loaded) > 0);

  cleanup();
}
#endif  // SONARE_WITH_FFMPEG

#ifndef SONARE_WITH_FFMPEG
TEST_CASE("load_buffer rejects unknown format with actionable message", "[audio_io]") {
  // 12 bytes of nothing recognisable as WAV or MP3.
  std::vector<uint8_t> garbage = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                  0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B};

  using Catch::Matchers::ContainsSubstring;
  REQUIRE_THROWS_WITH(load_buffer(garbage.data(), garbage.size()),
                      ContainsSubstring("Unsupported audio format") &&
                          ContainsSubstring("WAV, MP3") && ContainsSubstring("ffmpeg"));
}

TEST_CASE("load_audio reports extension and ffmpeg hint for unsupported file", "[audio_io]") {
  // Write a tiny non-audio file with an .m4a extension to disk.
  std::string tmp_path = "test_unsupported.m4a";
  {
    std::ofstream out(tmp_path, std::ios::binary);
    REQUIRE(out.is_open());
    const char payload[] = "not really an m4a file";
    out.write(payload, static_cast<std::streamsize>(sizeof(payload) - 1));
  }

  using Catch::Matchers::ContainsSubstring;
  REQUIRE_THROWS_WITH(load_audio(tmp_path), ContainsSubstring("'.m4a'") &&
                                                ContainsSubstring("ffmpeg -i") &&
                                                ContainsSubstring("SONARE_WITH_FFMPEG"));

  std::remove(tmp_path.c_str());
}
#endif  // !SONARE_WITH_FFMPEG

namespace {

std::vector<uint8_t> read_file_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
}

uint16_t le16(const std::vector<uint8_t>& b, size_t off) {
  return static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
}

uint32_t le32(const std::vector<uint8_t>& b, size_t off) {
  return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
         (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}

}  // namespace

TEST_CASE("save_wav_multichannel writes WAVE_FORMAT_EXTENSIBLE for 5.1", "[audio_io]") {
  const std::string path = "test_surround_51.wav";
  const size_t frames = 4;
  const int channels = 6;
  std::vector<float> interleaved(frames * channels);
  for (size_t i = 0; i < interleaved.size(); ++i) {
    interleaved[i] = static_cast<float>(i) / 100.0f;  // small distinct values
  }

  save_wav_multichannel(path, interleaved.data(), frames, channels, ChannelLayout::FivePointOne,
                        48000, 16);
  const std::vector<uint8_t> bytes = read_file_bytes(path);

  REQUIRE(bytes.size() >= 68);
  REQUIRE(std::memcmp(bytes.data(), "RIFF", 4) == 0);
  REQUIRE(std::memcmp(bytes.data() + 8, "WAVE", 4) == 0);
  REQUIRE(std::memcmp(bytes.data() + 12, "fmt ", 4) == 0);
  REQUIRE(le32(bytes, 16) == 40);      // fmt chunk size (EXTENSIBLE)
  REQUIRE(le16(bytes, 20) == 0xFFFE);  // WAVE_FORMAT_EXTENSIBLE
  REQUIRE(le16(bytes, 22) == 6);       // channels
  REQUIRE(le32(bytes, 24) == 48000);   // sample rate
  REQUIRE(le16(bytes, 32) == 6 * 2);   // block align (6ch * 16-bit)
  REQUIRE(le16(bytes, 34) == 16);      // bits per sample
  REQUIRE(le16(bytes, 36) == 22);      // cbSize
  REQUIRE(le32(bytes, 40) == 0x3Fu);   // dwChannelMask (FL FR FC LFE BL BR)
  REQUIRE(bytes[44] == 0x01);          // PCM sub-format GUID first byte
  REQUIRE(std::memcmp(bytes.data() + 60, "data", 4) == 0);
  REQUIRE(le32(bytes, 64) == frames * channels * 2);  // data size

  // Sample round-trip: decode the first interleaved frame from the data chunk.
  const size_t data_off = 68;
  for (int c = 0; c < channels; ++c) {
    auto raw = static_cast<int16_t>(le16(bytes, data_off + static_cast<size_t>(c) * 2));
    const float decoded = static_cast<float>(raw) / 32767.0f;
    REQUIRE_THAT(decoded, Catch::Matchers::WithinAbs(interleaved[static_cast<size_t>(c)], 1e-4f));
  }

  std::remove(path.c_str());
}

TEST_CASE("save_wav_multichannel keeps stereo/mono as plain PCM", "[audio_io]") {
  const std::string path = "test_stereo_plain.wav";
  const size_t frames = 8;
  const int channels = 2;
  std::vector<float> interleaved(frames * channels, 0.25f);

  save_wav_multichannel(path, interleaved.data(), frames, channels, ChannelLayout::Stereo, 44100,
                        16);
  const std::vector<uint8_t> bytes = read_file_bytes(path);

  REQUIRE(std::memcmp(bytes.data(), "RIFF", 4) == 0);
  REQUIRE(le16(bytes, 20) == 0x0001);  // WAVE_FORMAT_PCM, not EXTENSIBLE
  REQUIRE(le16(bytes, 22) == 2);
  std::remove(path.c_str());
}

TEST_CASE("save_wav_multichannel writes the 7.1 mask and validates arguments", "[audio_io]") {
  const std::string path = "test_surround_71.wav";
  const size_t frames = 2;
  const int channels = 8;
  std::vector<float> interleaved(frames * channels, 0.0f);

  save_wav_multichannel(path, interleaved.data(), frames, channels, ChannelLayout::SevenPointOne,
                        48000, 24);
  const std::vector<uint8_t> bytes = read_file_bytes(path);
  REQUIRE(le16(bytes, 20) == 0xFFFE);
  REQUIRE(le16(bytes, 22) == 8);
  REQUIRE(le16(bytes, 34) == 24);                     // 24-bit
  REQUIRE(le32(bytes, 40) == 0x63Fu);                 // 7.1 mask
  REQUIRE(le32(bytes, 64) == frames * channels * 3);  // 24-bit = 3 bytes/sample
  std::remove(path.c_str());

  // channel_count must match the layout.
  REQUIRE_THROWS_AS(save_wav_multichannel(path, interleaved.data(), frames, 6,
                                          ChannelLayout::SevenPointOne, 48000, 16),
                    sonare::SonareException);
}
