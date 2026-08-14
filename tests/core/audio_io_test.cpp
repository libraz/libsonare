/// @file audio_io_test.cpp
/// @brief Tests for audio I/O functions.

#include "core/audio_io.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/audio.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/resource_limits.h"

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

TEST_CASE("load_buffer_mp3 rejects an oversized declared PCM stream before decode allocation",
          "[audio_io]") {
  // A Xing/Info VBR tag supplies frame count before PCM decode. Mutate a tiny
  // valid MP3 to claim billions of frames: the streaming decoder must reject
  // that declaration instead of using mp3dec_load_buf, which would allocate
  // the entire decoded int16 pool before our resource check runs.
  if (std::system("command -v ffmpeg >/dev/null 2>&1") != 0) {
    SKIP("ffmpeg CLI not found");
  }
  const std::string path = "test_mp3_declared_limit.mp3";
  const std::string command =
      "ffmpeg -loglevel error -f lavfi -i "
      "sine=frequency=440:duration=0.25:sample_rate=22050 "
      "-ac 1 -c:a libmp3lame -b:a 32k -y " +
      path;
  REQUIRE(std::system(command.c_str()) == 0);

  std::ifstream input(path, std::ios::binary);
  REQUIRE(input.is_open());
  std::vector<uint8_t> mp3((std::istreambuf_iterator<char>(input)), {});
  input.close();

  const std::array<uint8_t, 4> info_tag = {'I', 'n', 'f', 'o'};
  const auto tag = std::search(mp3.begin(), mp3.end(), info_tag.begin(), info_tag.end());
  REQUIRE(tag != mp3.end());
  const size_t tag_offset = static_cast<size_t>(tag - mp3.begin());
  REQUIRE(tag_offset + 12 <= mp3.size());
  // `Info` + big-endian flags (FRAMES_FLAG is set by libmp3lame) + frames.
  REQUIRE((mp3[tag_offset + 7] & 0x01) != 0);
  std::fill_n(mp3.begin() + static_cast<std::ptrdiff_t>(tag_offset + 8), 4, 0xFF);

  REQUIRE_THROWS_WITH(load_buffer_mp3(mp3.data(), mp3.size()),
                      Catch::Matchers::ContainsSubstring("offline decode limit"));
  std::remove(path.c_str());
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
      "-ac 2 -y " +
      wav_path;
  const std::string gen_m4a =
      "ffmpeg -loglevel error -i " + wav_path + " -c:a aac -b:a 64k -y " + m4a_path;
  REQUIRE(std::system(gen_wav.c_str()) == 0);
  REQUIRE(std::system(gen_m4a.c_str()) == 0);

  AudioLoadResult loaded;
  REQUIRE_NOTHROW(loaded = load_audio(m4a_path));
  REQUIRE(std::get<0>(loaded).size() > 1000);
  REQUIRE(std::get<1>(loaded) > 0);
  REQUIRE(audio_channel_count(m4a_path) == 2);

  cleanup();
}

TEST_CASE("load_audio adopts HE-AAC frame parameters", "[audio_io][ffmpeg][he-aac]") {
  if (std::system("ffmpeg -hide_banner -h encoder=aac_at >/dev/null 2>&1") != 0) {
    SKIP("ffmpeg AudioToolbox HE-AAC encoder is unavailable");
  }

  const std::string fixture_path = "test_tone_he_aac.m4a";
  const std::string generate =
      "ffmpeg -loglevel error -f lavfi -i "
      "'sine=frequency=440:duration=1:sample_rate=44100' "
      "-ac 2 -c:a aac_at -profile:a 4 -b:a 48k -y " +
      fixture_path;
  REQUIRE(std::system(generate.c_str()) == 0);

  AudioLoadResult loaded;
  REQUIRE_NOTHROW(loaded = load_audio(fixture_path));
  const std::vector<float>& audio = std::get<0>(loaded);
  const int sample_rate = std::get<1>(loaded);
  REQUIRE(sample_rate == 44100);
  REQUIRE(audio_channel_count(fixture_path) == 2);
  const double duration = static_cast<double>(audio.size()) / sample_rate;
  REQUIRE(duration > 0.9);
  REQUIRE(duration < 1.3);

  std::remove(fixture_path.c_str());
}

TEST_CASE("load_audio_interleaved preserves FFmpeg stereo channel order",
          "[audio_io][ffmpeg][interleaved]") {
  if (std::system("command -v ffmpeg >/dev/null 2>&1") != 0) {
    SKIP("ffmpeg CLI not found on PATH");
  }

  constexpr int sample_rate = 22050;
  constexpr size_t frames = static_cast<size_t>(sample_rate);
  std::vector<float> source(frames * 2);
  for (size_t frame = 0; frame < frames; ++frame) {
    const float t = static_cast<float>(frame) / static_cast<float>(sample_rate);
    source[2 * frame] =
        0.6f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 440.0f * t);
    source[2 * frame + 1] =
        0.15f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 880.0f * t);
  }

  const std::string wav_path = "test_interleaved_ffmpeg_source.wav";
  const std::string flac_path = "test_interleaved_ffmpeg_source.flac";
  const std::string m4a_path = "test_interleaved_ffmpeg_source.m4a";
  auto cleanup = [&]() {
    std::remove(wav_path.c_str());
    std::remove(flac_path.c_str());
    std::remove(m4a_path.c_str());
  };
  save_wav_multichannel(wav_path, source.data(), frames, 2, ChannelLayout::Stereo, sample_rate);

  REQUIRE(std::system(
              ("ffmpeg -loglevel error -i " + wav_path + " -c:a flac -y " + flac_path).c_str()) ==
          0);
  REQUIRE(
      std::system(("ffmpeg -loglevel error -i " + wav_path + " -c:a aac -b:a 128k -y " + m4a_path)
                      .c_str()) == 0);

  for (const std::string& path : {flac_path, m4a_path}) {
    auto [decoded, decoded_rate, decoded_channels] = load_audio_interleaved(path);
    REQUIRE(decoded_rate == sample_rate);
    REQUIRE(decoded_channels == 2);
    REQUIRE(decoded.size() >= 2 * frames / 3);
    REQUIRE(decoded.size() % 2 == 0);

    // The left channel is deliberately four times louder than the right.
    // Checking both levels and their order catches mono downmixes as well as
    // a decoder that silently swaps the L/R planes.
    const size_t decoded_frames = decoded.size() / 2;
    const size_t begin = decoded_frames / 8;
    const size_t end = decoded_frames - begin;
    double left_energy = 0.0;
    double right_energy = 0.0;
    for (size_t frame = begin; frame < end; ++frame) {
      left_energy += static_cast<double>(decoded[2 * frame]) * decoded[2 * frame];
      right_energy += static_cast<double>(decoded[2 * frame + 1]) * decoded[2 * frame + 1];
    }
    const double count = static_cast<double>(end - begin);
    const double left_rms = std::sqrt(left_energy / count);
    const double right_rms = std::sqrt(right_energy / count);
    REQUIRE(left_rms > 0.25);
    REQUIRE(right_rms > 0.04);
    REQUIRE(left_rms > 2.5 * right_rms);
    REQUIRE(audio_channel_count(path) == 2);
  }

  cleanup();
}

TEST_CASE("load_audio_interleaved rejects FFmpeg channel renegotiation",
          "[audio_io][ffmpeg][interleaved]") {
  if (std::system("command -v ffmpeg >/dev/null 2>&1") != 0) {
    SKIP("ffmpeg CLI not found on PATH");
  }

  constexpr int sample_rate = 22050;
  const std::string mono_path = "test_interleaved_ffmpeg_renegotiate_mono.aac";
  const std::string stereo_path = "test_interleaved_ffmpeg_renegotiate_stereo.aac";
  const std::string concat_path = "test_interleaved_ffmpeg_renegotiate.ts";
  auto cleanup = [&]() {
    std::remove(mono_path.c_str());
    std::remove(stereo_path.c_str());
    std::remove(concat_path.c_str());
  };

  // ADTS carries the channel configuration in every AAC frame. Concatenating
  // these two elementary streams therefore makes FFmpeg expose a real
  // mono-to-stereo layout change to the decoder, without a private test seam.
  const std::string generate_mono =
      "ffmpeg -loglevel error -f lavfi -i "
      "'sine=frequency=440:sample_rate=22050:duration=0.3' "
      "-ac 1 -c:a aac -b:a 96k -f adts -y " +
      mono_path;
  const std::string generate_stereo =
      "ffmpeg -loglevel error -f lavfi -i "
      "'sine=frequency=880:sample_rate=22050:duration=0.3' "
      "-ac 2 -c:a aac -b:a 96k -f adts -y " +
      stereo_path;
  const std::string concatenate = "ffmpeg -loglevel error -i 'concat:" + mono_path + "|" +
                                  stereo_path + "' -c copy -f mpegts -y " + concat_path;
  REQUIRE(std::system(generate_mono.c_str()) == 0);
  REQUIRE(std::system(generate_stereo.c_str()) == 0);
  REQUIRE(std::system(concatenate.c_str()) == 0);

  REQUIRE_THROWS_WITH(load_audio_interleaved(concat_path),
                      Catch::Matchers::ContainsSubstring("channel layout change"));

  // The legacy loader still downmixes the same stream while rebuilding its
  // mono resampler, so this guard does not weaken its established behavior.
  auto [decoded, decoded_rate] = load_audio(concat_path);
  REQUIRE(decoded_rate == sample_rate);
  REQUIRE(decoded.size() > 0);

  cleanup();
}
#endif  // SONARE_WITH_FFMPEG

TEST_CASE("load_buffer rejects an oversized input before decoding", "[audio_io]") {
  // The size ceiling is checked before `data` is dereferenced, so a tiny actual
  // buffer paired with an over-limit declared size exercises the guard without a
  // half-gigabyte allocation. This mirrors the file-path loader's ceiling so the
  // buffer/memory entry (the one most exposed to untrusted input) is not weaker.
  uint8_t dummy = 0;
  using Catch::Matchers::ContainsSubstring;
  REQUIRE_THROWS_WITH(load_buffer(&dummy, resource::kMaxAudioFileBytes + 1),
                      ContainsSubstring("too large"));
  // The C-ABI/Node/Python/WASM memory entries all funnel through load_buffer via
  // Audio::from_memory, so guarding here covers every surface.
  REQUIRE_THROWS(Audio::from_memory(&dummy, resource::kMaxAudioFileBytes + 1));
}

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

TEST_CASE("load_audio_interleaved keeps FFmpeg formats unavailable without FFmpeg",
          "[audio_io][interleaved]") {
  const std::string tmp_path = "test_interleaved_unsupported.m4a";
  {
    std::ofstream out(tmp_path, std::ios::binary);
    REQUIRE(out.is_open());
    const char payload[] = "not really an m4a file";
    out.write(payload, static_cast<std::streamsize>(sizeof(payload) - 1));
  }

  REQUIRE_THROWS_WITH(load_audio_interleaved(tmp_path),
                      Catch::Matchers::ContainsSubstring("WAV and MP3 only"));
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

int32_t signed_le24(const std::vector<uint8_t>& b, size_t off) {
  uint32_t value = static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
                   (static_cast<uint32_t>(b[off + 2]) << 16);
  if ((value & 0x800000u) != 0) value |= 0xFF000000u;
  return static_cast<int32_t>(value);
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
  // WAVE mask 0x63F enumerates FL FR FC LFE BL BR SL SR. Use a distinct DC
  // value for every plane so the serialized order is verified independently of
  // the mask header.
  const std::array<float, channels> first_frame = {
      0.1f,  // L / FL
      0.2f,  // R / FR
      0.3f,  // C / FC
      0.4f,  // LFE
      0.5f,  // Ls / BL
      0.6f,  // Rs / BR
      0.7f,  // Lss / SL
      0.8f,  // Rss / SR
  };
  std::vector<float> interleaved(frames * channels);
  for (size_t frame = 0; frame < frames; ++frame) {
    std::copy(first_frame.begin(), first_frame.end(),
              interleaved.begin() + static_cast<std::ptrdiff_t>(frame * channels));
  }

  save_wav_multichannel(path, interleaved.data(), frames, channels, ChannelLayout::SevenPointOne,
                        48000, 24);
  const std::vector<uint8_t> bytes = read_file_bytes(path);
  REQUIRE(le16(bytes, 20) == 0xFFFE);
  REQUIRE(le16(bytes, 22) == 8);
  REQUIRE(le16(bytes, 34) == 24);                     // 24-bit
  REQUIRE(le32(bytes, 40) == 0x63Fu);                 // 7.1 mask
  REQUIRE(le32(bytes, 64) == frames * channels * 3);  // 24-bit = 3 bytes/sample

  const size_t data_off = 68;
  for (int plane = 0; plane < channels; ++plane) {
    const int32_t raw =
        signed_le24(bytes, data_off + static_cast<size_t>(plane) * static_cast<size_t>(3));
    const float decoded = static_cast<float>(raw) / 8388607.0f;
    REQUIRE_THAT(decoded,
                 Catch::Matchers::WithinAbs(first_frame[static_cast<size_t>(plane)], 1e-6f));
  }
  std::remove(path.c_str());

  // channel_count must match the layout.
  REQUIRE_THROWS_AS(save_wav_multichannel(path, interleaved.data(), frames, 6,
                                          ChannelLayout::SevenPointOne, 48000, 16),
                    sonare::SonareException);

  // The RIFF data-size field is uint32. This must fail before dereferencing the
  // tiny sentinel buffer or creating a multi-gigabyte temporary file.
  const float sentinel = 0.0f;
  REQUIRE_THROWS_AS(save_wav_multichannel(path, &sentinel, static_cast<size_t>(1) << 30, 8,
                                          ChannelLayout::SevenPointOne, 48000, 24),
                    sonare::SonareException);
}

TEST_CASE("save_wav quantizes 16-bit PCM by rounding to nearest, not truncating", "[audio_io]") {
  // Pick float samples whose scaled magnitude has a fractional part > 0.5 so
  // round-half-away-from-zero and toward-zero truncation disagree. The old
  // writer truncated (static_cast), losing up to a full LSB and ~3 dB SNR vs a
  // libsndfile-style nearest-neighbor encoder.
  const std::vector<float> original = {
      100.7f / 32767.0f,   // +: trunc 100, round 101
      -100.7f / 32767.0f,  // -: trunc -100, round -101 (away from zero)
      50.2f / 32767.0f,    // control: trunc == round == 50
      0.0f,
  };
  const std::vector<int16_t> expected = {101, -101, 50, 0};

  const std::string path = "test_pcm16_rounding.wav";
  save_wav(path, original, 48000, 16);
  auto [loaded, loaded_sr] = load_wav(path);
  std::remove(path.c_str());

  REQUIRE(loaded_sr == 48000);
  REQUIRE(loaded.size() == original.size());
  // dr_wav reconstructs int16 V as V / 32768, so the stored integer is
  // recoverable exactly by rounding the loaded float back.
  for (size_t i = 0; i < expected.size(); ++i) {
    const long stored = std::lround(static_cast<double>(loaded[i]) * 32768.0);
    REQUIRE(stored == expected[i]);
  }
}

TEST_CASE("save_wav writes atomically and leaves no temp file", "[audio_io]") {
  const std::vector<float> samples = {0.1f, -0.2f, 0.3f, -0.4f};

  SECTION("a successful write leaves no .sonare-tmp sibling behind") {
    const std::string path = "test_atomic_ok.wav";
    save_wav(path, samples, 48000, 16);
    // The temp file must have been renamed away, not left next to the result.
    std::ifstream tmp(path + ".sonare-tmp", std::ios::binary);
    REQUIRE_FALSE(tmp.is_open());
    auto [loaded, sr] = load_wav(path);
    REQUIRE(sr == 48000);
    REQUIRE(loaded.size() == samples.size());
    std::remove(path.c_str());
  }

  SECTION("a failed write preserves an existing destination and drops the temp") {
    // Seed a valid destination, then force the second write to fail before it can
    // finalize by targeting a directory that does not exist. The pre-existing
    // file must survive intact and no stray temp file may be left behind.
    const std::string good = "test_atomic_preserve.wav";
    save_wav(good, samples, 48000, 16);
    const auto [before_samples, before_sr] = load_wav(good);

    const std::string bad = "test_atomic_missing_dir/out.wav";
    REQUIRE_THROWS_AS(save_wav(bad, samples, 48000, 16), SonareException);
    std::ifstream bad_tmp(bad + ".sonare-tmp", std::ios::binary);
    REQUIRE_FALSE(bad_tmp.is_open());

    const auto [after_samples, after_sr] = load_wav(good);
    REQUIRE(after_samples.size() == before_samples.size());
    REQUIRE(after_sr == before_sr);
    std::remove(good.c_str());
  }
}

TEST_CASE("save_wav uses a per-writer temp path so writes to one destination stay valid",
          "[audio_io]") {
  const std::vector<float> first = {0.1f, -0.2f, 0.3f, -0.4f};
  const std::vector<float> second = {0.5f, -0.6f, 0.7f, -0.8f, 0.9f, -0.1f};
  const std::string path = "test_atomic_unique.wav";

  SECTION("two sequential writes to the same destination each produce a valid file") {
    save_wav(path, first, 48000, 16);
    {
      auto [loaded, sr] = load_wav(path);
      REQUIRE(sr == 48000);
      REQUIRE(loaded.size() == first.size());
    }
    save_wav(path, second, 48000, 16);
    {
      auto [loaded, sr] = load_wav(path);
      REQUIRE(sr == 48000);
      REQUIRE(loaded.size() == second.size());
    }
    std::remove(path.c_str());
  }

  SECTION("a leftover temp from an interrupted writer does not corrupt a fresh write") {
    // The temp path now carries a per-writer suffix, so a stale sibling left by an
    // aborted writer is never truncated into or renamed by an unrelated write.
    const std::string stale_tmp = path + ".sonare-tmp.stale";
    {
      std::ofstream junk(stale_tmp, std::ios::binary);
      junk << "not a wav";
    }
    save_wav(path, first, 48000, 16);
    auto [loaded, sr] = load_wav(path);
    REQUIRE(sr == 48000);
    REQUIRE(loaded.size() == first.size());
    std::remove(stale_tmp.c_str());
    std::remove(path.c_str());
  }
}
