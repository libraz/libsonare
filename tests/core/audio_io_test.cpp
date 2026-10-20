/// @file audio_io_test.cpp
/// @brief Tests for audio I/O functions.

#include "core/audio_io.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "core/audio_io_detail.h"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#endif

#include "core/audio.h"
#include "support/alloc_guard.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/resource_limits.h"

using namespace sonare;

namespace {

/// @brief A freshly created empty directory outside the working tree.
/// @details For the write cases that deliberately drive a writer up to a
///          rejection: a guard that ever moved below the temp-file creation
///          would strand the scratch file, and the working tree is the one
///          place that must not collect it. create_directories() answers false
///          for a directory that already exists, so stepping the suffix claims
///          a directory no concurrently running test binary is using and no
///          crashed earlier one left behind.
std::filesystem::path scratch_dir() {
  const std::filesystem::path base = std::filesystem::temp_directory_path();
  for (int suffix = 0; suffix < 1024; ++suffix) {
    const std::filesystem::path dir = base / ("sonare_audio_io_test_" + std::to_string(suffix));
    if (std::filesystem::create_directories(dir)) return dir;
  }
  FAIL("no free scratch directory under the system temp path");
  return {};
}

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

/// @brief Creates a mono 8-bit PCM WAV whose `data` chunk header declares
///        @p declared_data_bytes while the file carries only @p actual_data_bytes.
/// @details A chunk size is a header field with nothing behind it: dr_wav takes
///          the declared size at face value and divides it into a PCM frame
///          count, so one 8-bit sample per declared byte turns a few dozen bytes
///          of file into a frame count of any size the attacker likes.
std::vector<uint8_t> create_wav_buffer_overdeclared_data(uint32_t declared_data_bytes,
                                                         size_t actual_data_bytes) {
  std::vector<uint8_t> out;
  const auto push_u16 = [&out](uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
  };
  const auto push_u32 = [&out](uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
    }
  };
  const auto push_tag = [&out](const char* tag) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(tag[i]));
  };

  push_tag("RIFF");
  push_u32(36u + declared_data_bytes);  // as over-declared as the data chunk
  push_tag("WAVE");
  push_tag("fmt ");
  push_u32(16u);
  push_u16(1u);  // WAVE_FORMAT_PCM
  push_u16(1u);  // mono
  push_u32(44100u);
  push_u32(44100u);
  push_u16(1u);  // block align
  push_u16(8u);  // bits per sample: one frame per declared byte
  push_tag("data");
  push_u32(declared_data_bytes);
  out.resize(out.size() + actual_data_bytes, 0x80u);  // 8-bit PCM silence
  return out;
}

/// @brief Creates a mono 8-bit AIFF whose COMM chunk declares @p declared_frames
///        while the SSND chunk carries only @p actual_data_bytes.
/// @details dr_wav clamps a RIFF data chunk against the real stream length but
///          takes an AIFF frame count verbatim, so this is the declaration a
///          loader cannot lean on. Only load_wav / load_buffer_wav reach it:
///          detect_format sees no RIFF/WAVE and routes the container elsewhere.
std::vector<uint8_t> create_aiff_buffer_overdeclared_frames(uint32_t declared_frames,
                                                            size_t actual_data_bytes) {
  std::vector<uint8_t> out;
  const auto push_be16 = [&out](uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
  };
  const auto push_be32 = [&out](uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
    }
  };
  const auto push_tag = [&out](const char* tag) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(tag[i]));
  };

  push_tag("FORM");
  push_be32(static_cast<uint32_t>(4u + 8u + 18u + 8u + 8u + actual_data_bytes));
  push_tag("AIFF");
  push_tag("COMM");
  push_be32(18u);
  push_be16(1u);               // channels
  push_be32(declared_frames);  // numSampleFrames: unbacked by the SSND chunk
  push_be16(8u);               // sample size in bits
  // 44100 Hz as an 80-bit IEEE extended float.
  for (const uint8_t byte :
       {0x40u, 0x0Eu, 0xACu, 0x44u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}) {
    out.push_back(static_cast<uint8_t>(byte));
  }
  push_tag("SSND");
  push_be32(static_cast<uint32_t>(8u + actual_data_bytes));
  push_be32(0u);  // offset
  push_be32(0u);  // block size
  out.resize(out.size() + actual_data_bytes, 0u);
  return out;
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

/// @brief Creates a frame-interleaved float32 WAV file in memory with @p channels planes.
std::vector<uint8_t> create_wav_buffer_multichannel(const float* interleaved, size_t frame_count,
                                                    int channels, int sample_rate) {
  struct WavHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t file_size;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmt_size = 16;
    uint16_t audio_format = 3;  // IEEE float
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample = 32;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t data_size;
  };

  const size_t sample_count = frame_count * static_cast<size_t>(channels);
  WavHeader header;
  header.num_channels = static_cast<uint16_t>(channels);
  header.sample_rate = static_cast<uint32_t>(sample_rate);
  header.block_align = static_cast<uint16_t>(channels * 4);
  header.byte_rate = header.sample_rate * header.block_align;
  header.data_size = static_cast<uint32_t>(sample_count * 4);
  header.file_size = 36 + header.data_size;

  std::vector<uint8_t> buffer(sizeof(WavHeader) + header.data_size);
  std::memcpy(buffer.data(), &header, sizeof(WavHeader));
  std::memcpy(buffer.data() + sizeof(WavHeader), interleaved, sample_count * 4);

  return buffer;
}

/// @brief WAVE_FORMAT_EXTENSIBLE float WAV carrying speaker mask @p channel_mask.
std::vector<uint8_t> create_wav_buffer_extensible(const float* interleaved, size_t frame_count,
                                                  int channels, int sample_rate,
                                                  uint32_t channel_mask) {
  const size_t data_bytes = frame_count * static_cast<size_t>(channels) * 4;
  std::vector<uint8_t> buffer;
  const auto put16 = [&](uint32_t v) {
    for (int i = 0; i < 2; ++i) buffer.push_back(static_cast<uint8_t>(v >> (8 * i)));
  };
  const auto put32 = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) buffer.push_back(static_cast<uint8_t>(v >> (8 * i)));
  };
  const auto tag = [&](const char* text) { buffer.insert(buffer.end(), text, text + 4); };
  tag("RIFF");
  put32(static_cast<uint32_t>(4 + 8 + 40 + 8 + data_bytes));
  tag("WAVE");
  tag("fmt ");
  put32(40);
  put16(0xFFFE);
  put16(static_cast<uint32_t>(channels));
  put32(static_cast<uint32_t>(sample_rate));
  put32(static_cast<uint32_t>(sample_rate * channels * 4));
  put16(static_cast<uint32_t>(channels * 4));
  put16(32);
  put16(22);
  put16(32);
  put32(channel_mask);
  // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
  const uint8_t subformat[16] = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
  buffer.insert(buffer.end(), subformat, subformat + 16);
  tag("data");
  put32(static_cast<uint32_t>(data_bytes));
  const size_t offset = buffer.size();
  buffer.resize(offset + data_bytes);
  std::memcpy(buffer.data() + offset, interleaved, data_bytes);
  return buffer;
}

/// @brief Root-mean-square level of a signal.
double rms(const std::vector<float>& samples) {
  double energy = 0.0;
  for (float sample : samples) energy += static_cast<double>(sample) * sample;
  return samples.empty() ? 0.0 : std::sqrt(energy / static_cast<double>(samples.size()));
}

/// @brief Largest absolute sample value.
float peak(const std::vector<float>& samples) {
  float highest = 0.0f;
  for (float sample : samples) highest = std::max(highest, std::abs(sample));
  return highest;
}

/// @brief Scatters per-plane signals into one frame-interleaved buffer.
/// @param planes One vector per plane, in canonical ChannelLayout order; empty
///        vectors are written as silence.
std::vector<float> interleave(const std::vector<std::vector<float>>& planes, size_t frame_count) {
  const size_t channels = planes.size();
  std::vector<float> interleaved(frame_count * channels, 0.0f);
  for (size_t channel = 0; channel < channels; ++channel) {
    if (planes[channel].empty()) continue;
    for (size_t frame = 0; frame < frame_count; ++frame) {
      interleaved[frame * channels + channel] = planes[channel][frame];
    }
  }
  return interleaved;
}

/// @brief Bytes of one MPEG-1 Layer III frame at 128 kbps / 48 kHz without padding.
constexpr size_t kSilentMp3FrameBytes = 384;

/// @brief Samples one MPEG-1 Layer III frame decodes to per channel.
constexpr size_t kMp3FrameSamples = 1152;

/// @brief A mono MPEG-1 Layer III frame (128 kbps, 48 kHz) whose zeroed side info
///        and main data decode to silence.
std::vector<uint8_t> silent_mp3_frame() {
  std::vector<uint8_t> frame(kSilentMp3FrameBytes, 0);
  frame[0] = 0xFF;
  frame[1] = 0xFB;  // MPEG-1, Layer III, no CRC
  frame[2] = 0x94;  // bitrate index 9 (128 kbps), sample-rate index 1 (48 kHz)
  frame[3] = 0xC0;  // mono
  return frame;
}

/// @brief @p frames consecutive silent MP3 frames.
std::vector<uint8_t> silent_mp3_stream(size_t frames) {
  std::vector<uint8_t> out;
  const std::vector<uint8_t> frame = silent_mp3_frame();
  for (size_t i = 0; i < frames; ++i) out.insert(out.end(), frame.begin(), frame.end());
  return out;
}

/// @brief An ID3v2.4 tag whose syncsafe size declares @p body_bytes of body.
std::vector<uint8_t> id3_tag(size_t body_bytes) {
  std::vector<uint8_t> tag = {'I', 'D', '3', 0x04, 0x00, 0x00};
  for (int shift = 21; shift >= 0; shift -= 7) {
    tag.push_back(static_cast<uint8_t>((body_bytes >> shift) & 0x7Fu));
  }
  tag.resize(tag.size() + body_bytes, 'T');
  return tag;
}

/// @brief @p prefix followed by @p stream.
std::vector<uint8_t> prefixed(std::vector<uint8_t> prefix, const std::vector<uint8_t>& stream) {
  prefix.insert(prefix.end(), stream.begin(), stream.end());
  return prefix;
}

/// @brief Inserts an empty `JUNK` chunk between the RIFF header and the first chunk.
std::vector<uint8_t> with_empty_chunk_before_fmt(const std::vector<uint8_t>& wav) {
  constexpr size_t kRiffHeaderBytes = 12;
  std::vector<uint8_t> out(wav.begin(), wav.begin() + kRiffHeaderBytes);
  out.insert(out.end(), {'J', 'U', 'N', 'K', 0, 0, 0, 0});
  out.insert(out.end(), wav.begin() + kRiffHeaderBytes, wav.end());
  const uint32_t riff_size = static_cast<uint32_t>(out.size() - 8);
  for (int i = 0; i < 4; ++i) out[4 + i] = static_cast<uint8_t>((riff_size >> (8 * i)) & 0xFFu);
  return out;
}

/// @brief A RIFF/WAVE carrying @p frames silent MP3 frames as WAVE_FORMAT_MPEGLAYER3 (0x0055).
std::vector<uint8_t> mp3_in_wav(size_t frames) {
  const std::vector<uint8_t> stream = silent_mp3_stream(frames);
  std::vector<uint8_t> out;
  const auto push_u16 = [&out](uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
  };
  const auto push_u32 = [&out](uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
    }
  };
  const auto push_tag = [&out](const char* tag) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(tag[i]));
  };
  push_tag("RIFF");
  push_u32(static_cast<uint32_t>(4 + 8 + 30 + 8 + stream.size()));
  push_tag("WAVE");
  push_tag("fmt ");
  push_u32(30u);
  push_u16(0x0055u);  // WAVE_FORMAT_MPEGLAYER3
  push_u16(1u);       // mono
  push_u32(48000u);
  push_u32(16000u);  // 128 kbps
  push_u16(1u);      // block align
  push_u16(0u);      // bits per sample
  push_u16(12u);     // MPEGLAYER3WAVEFORMAT extension
  push_u16(1u);      // MPEGLAYER3_ID_MPEG
  push_u32(2u);      // MPEGLAYER3_FLAG_PADDING_OFF
  push_u16(static_cast<uint16_t>(kSilentMp3FrameBytes));
  push_u16(1u);  // frames per block
  push_u16(0u);  // codec delay
  push_tag("data");
  push_u32(static_cast<uint32_t>(stream.size()));
  out.insert(out.end(), stream.begin(), stream.end());
  return out;
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

TEST_CASE("detect_format leaves non-MPEG streams behind an MP3-like prefix to FFmpeg",
          "[audio_io]") {
  SECTION("raw ADTS AAC shares the 12-bit sync but carries layer 0") {
    for (const uint8_t second : {uint8_t{0xF1}, uint8_t{0xF9}}) {
      const std::vector<uint8_t> adts = {0xFF, second, 0x50, 0x80, 0x02, 0x1F, 0xFC, 0, 0, 0, 0, 0};
      CHECK(detect_format(adts.data(), adts.size()) == AudioFormat::Unknown);
    }
  }
  SECTION("an MPEG header with a reserved field is not a frame") {
    const std::vector<uint8_t> bad_bitrate = {0xFF, 0xFB, 0xF0, 0x00, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::vector<uint8_t> bad_rate = {0xFF, 0xFB, 0x9C, 0x00, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::vector<uint8_t> bad_version = {0xFF, 0xEB, 0x90, 0x00, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(detect_format(bad_bitrate.data(), bad_bitrate.size()) == AudioFormat::Unknown);
    CHECK(detect_format(bad_rate.data(), bad_rate.size()) == AudioFormat::Unknown);
    CHECK(detect_format(bad_version.data(), bad_version.size()) == AudioFormat::Unknown);
  }
  SECTION("the stream behind an ID3v2 tag decides") {
    // 10-byte header with a syncsafe size of 4, then 4 bytes of tag body.
    const std::vector<uint8_t> tag = {'I', 'D', '3', 0x04, 0x00, 0x00, 0, 0, 0, 4, 1, 2, 3, 4};
    std::vector<uint8_t> flac = tag;
    flac.insert(flac.end(), {'f', 'L', 'a', 'C', 0, 0, 0, 0x22, 0, 0, 0, 0});
    CHECK(detect_format(flac.data(), flac.size()) == AudioFormat::Unknown);
    std::vector<uint8_t> aac = tag;
    aac.insert(aac.end(), {0xFF, 0xF1, 0x50, 0x80, 0x02, 0x1F, 0xFC, 0, 0, 0, 0, 0});
    CHECK(detect_format(aac.data(), aac.size()) == AudioFormat::Unknown);
    std::vector<uint8_t> mp3 = tag;
    mp3.insert(mp3.end(), {0, 0, 0xFF, 0xFB, 0x90, 0x00, 0, 0, 0, 0, 0, 0});
    CHECK(detect_format(mp3.data(), mp3.size()) == AudioFormat::MP3);
  }
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

TEST_CASE("detect_format finds an MPEG stream behind leading junk or zero bytes", "[audio_io]") {
  // minimp3 resyncs past whatever precedes the first frame, so routing must too:
  // a short ID3 size, stray bytes or zero padding cannot strand a decodable MP3.
  const std::vector<uint8_t> clean = silent_mp3_stream(8);
  const std::vector<uint8_t> junk = {0x12, 0x34, 0x56, 0x78};
  const std::vector<std::pair<const char*, std::vector<uint8_t>>> variants = {
      {"ID3 + junk", prefixed(prefixed(id3_tag(16), junk), clean)},
      {"ID3 + zero padding", prefixed(prefixed(id3_tag(16), std::vector<uint8_t>(32, 0)), clean)},
      {"junk only", prefixed(junk, clean)},
      {"zero prefix", prefixed(std::vector<uint8_t>(64, 0), clean)},
  };

  auto [clean_samples, clean_rate] = load_buffer(clean.data(), clean.size());
  REQUIRE(clean_rate == 48000);
  REQUIRE(!clean_samples.empty());
  for (const auto& [name, bytes] : variants) {
    CAPTURE(name);
    CHECK(detect_format(bytes.data(), bytes.size()) == AudioFormat::MP3);
    // The built-in decoder, not an FFmpeg fallback, is what a default build has.
    auto [samples, sample_rate] = load_buffer_mp3(bytes.data(), bytes.size());
    CHECK(sample_rate == clean_rate);
    CHECK(samples.size() == clean_samples.size());
    auto [routed, routed_rate] = load_buffer(bytes.data(), bytes.size());
    CHECK(routed_rate == clean_rate);
    CHECK(routed.size() == clean_samples.size());
  }
}

TEST_CASE("detect_format does not take a stray sync word past junk for an MPEG stream",
          "[audio_io]") {
  SECTION("a lone header that no following frame confirms") {
    std::vector<uint8_t> bytes = {0x01, 0x02, 0x03, 0x04};
    const std::vector<uint8_t> frame = silent_mp3_frame();
    bytes.insert(bytes.end(), frame.begin(), frame.begin() + 4);
    bytes.resize(bytes.size() + 2 * kSilentMp3FrameBytes, 0x55);
    CHECK(detect_format(bytes.data(), bytes.size()) == AudioFormat::Unknown);
  }
  SECTION("raw ADTS AAC behind junk") {
    std::vector<uint8_t> bytes = {0x01, 0x02, 0x03};
    for (int i = 0; i < 8; ++i) {
      bytes.insert(bytes.end(), {0xFF, 0xF1, 0x50, 0x80, 0x02, 0x1F, 0xFC, 0, 0, 0, 0, 0});
    }
    CHECK(detect_format(bytes.data(), bytes.size()) == AudioFormat::Unknown);
  }
  SECTION("FLAC behind an ID3 tag and junk") {
    const std::vector<uint8_t> flac = {'f', 'L', 'a', 'C', 0, 0, 0, 0x22, 0xFF, 0xF8, 0x69, 0x08};
    const std::vector<uint8_t> bytes = prefixed(prefixed(id3_tag(4), {0x01, 0x02}), flac);
    CHECK(detect_format(bytes.data(), bytes.size()) == AudioFormat::Unknown);
  }
}

TEST_CASE("an empty chunk before fmt does not hide the WAV codec from routing", "[audio_io]") {
  SECTION("PCM still decodes through the built-in decoder") {
    constexpr int sr = 22050;
    const std::vector<float> tone = generate_sine(256, 440.0f, sr, 0.5f);
    const std::vector<uint8_t> plain = create_wav_buffer(tone.data(), tone.size(), sr);
    const std::vector<uint8_t> padded = with_empty_chunk_before_fmt(plain);
    REQUIRE(detect_format(padded.data(), padded.size()) == AudioFormat::WAV);
    auto [expected, expected_rate] = load_buffer(plain.data(), plain.size());
    auto [samples, sample_rate] = load_buffer(padded.data(), padded.size());
    CHECK(sample_rate == expected_rate);
    CHECK(samples == expected);
  }
  SECTION("MPEG Layer-3 in RIFF reaches the codec path") {
    const std::vector<uint8_t> padded = with_empty_chunk_before_fmt(mp3_in_wav(8));
    REQUIRE(detect_format(padded.data(), padded.size()) == AudioFormat::WAV);
#ifdef SONARE_WITH_FFMPEG
    auto [samples, sample_rate] = load_buffer(padded.data(), padded.size());
    CHECK(sample_rate == 48000);
    CHECK(!samples.empty());
    // The channel probe follows the same routing decision as the loader.
    const std::filesystem::path path = scratch_dir() / "mp3_in_wav_empty_chunk.wav";
    {
      std::ofstream out(path, std::ios::binary);
      REQUIRE(out.is_open());
      out.write(reinterpret_cast<const char*>(padded.data()),
                static_cast<std::streamsize>(padded.size()));
    }
    CHECK(audio_channel_count(path.string()) == 1);
    std::filesystem::remove_all(path.parent_path());
#else
    REQUIRE_THROWS_AS(load_buffer(padded.data(), padded.size()), SonareException);
#endif
  }
}

TEST_CASE("MP3 loaders size their buffers from the input, not a Xing frame count", "[audio_io]") {
  // A Xing tag claiming 400000 frames passes the offline limit (460.8M samples)
  // while the buffer carries two frames: reserving for the claim would commit
  // about 1.8 GB per vector for 2304 decoded samples.
  std::vector<uint8_t> xing = silent_mp3_frame();
  constexpr size_t kSideInfoBytes = 17;  // MPEG-1 mono
  const size_t tag = 4 + kSideInfoBytes;
  const uint32_t declared_frames = 400000;
  // "Xing", flags with FRAMES_FLAG set, then the big-endian frame count.
  const char kXingTag[] = "Xing";
  std::copy(kXingTag, kXingTag + 4, xing.begin() + static_cast<std::ptrdiff_t>(tag));
  xing[tag + 7] = 0x01;
  for (size_t i = 0; i < 4; ++i) {
    xing[tag + 8 + i] = static_cast<uint8_t>(declared_frames >> (24 - 8 * i));
  }
  const std::vector<uint8_t> bytes = prefixed(xing, silent_mp3_stream(2));
  REQUIRE(static_cast<uint64_t>(declared_frames) * kMp3FrameSamples <=
          resource::kMaxOfflineAudioSamples);
  // Far above anything the input can decode to, far below what the claim asks for.
  constexpr size_t kAllocationCeilingBytes = 16u * 1024u * 1024u;

  SECTION("mono") {
    std::vector<float> samples;
    int sample_rate = 0;
    {
      sonare::test::AllocationFailureGuard guard(kAllocationCeilingBytes);
      std::tie(samples, sample_rate) = load_buffer_mp3(bytes.data(), bytes.size());
    }
    CAPTURE(samples.size(), samples.capacity());
    REQUIRE(sample_rate == 48000);
    REQUIRE(!samples.empty());
    REQUIRE(samples.size() <= 2 * kMp3FrameSamples);
    REQUIRE(samples.capacity() <= 2 * samples.size());
  }
  SECTION("interleaved") {
    std::vector<float> interleaved;
    int sample_rate = 0;
    int channels = 0;
    {
      sonare::test::AllocationFailureGuard guard(kAllocationCeilingBytes);
      std::tie(interleaved, sample_rate, channels) =
          load_buffer_interleaved(bytes.data(), bytes.size());
    }
    CAPTURE(interleaved.size(), interleaved.capacity());
    REQUIRE(sample_rate == 48000);
    REQUIRE(channels == 1);
    REQUIRE(!interleaved.empty());
    REQUIRE(interleaved.capacity() <= 2 * interleaved.size());
  }
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

TEST_CASE("MS-ADPCM blocks whose step delta keeps growing decode to bounded samples",
          "[audio_io]") {
  constexpr int sr = 22050;
  constexpr uint16_t kBlockAlign = 256;
  constexpr uint16_t kSamplesPerBlock = (kBlockAlign - 7) * 2 + 2;
  constexpr int kBlocks = 4;
  std::vector<uint8_t> wav;
  auto put16 = [&wav](uint16_t v) {
    wav.push_back(static_cast<uint8_t>(v & 0xFF));
    wav.push_back(static_cast<uint8_t>(v >> 8));
  };
  auto put32 = [&put16](uint32_t v) {
    put16(static_cast<uint16_t>(v & 0xFFFF));
    put16(static_cast<uint16_t>(v >> 16));
  };
  const uint32_t data_bytes = kBlockAlign * kBlocks;
  const uint32_t fmt_bytes = 18 + 32;
  for (char c : {'R', 'I', 'F', 'F'}) wav.push_back(static_cast<uint8_t>(c));
  put32(4 + 8 + fmt_bytes + 8 + data_bytes);
  for (char c : {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}) wav.push_back(static_cast<uint8_t>(c));
  put32(fmt_bytes);
  put16(2);  // MS-ADPCM
  put16(1);
  put32(sr);
  put32(sr * kBlockAlign / kSamplesPerBlock);
  put16(kBlockAlign);
  put16(4);
  put16(32);
  put16(kSamplesPerBlock);
  put16(7);
  for (const int16_t c : {256, 0, 512, -256, 0, 0, 192, 64, 240, 0, 460, -208, 392, -232}) {
    put16(static_cast<uint16_t>(c));
  }
  for (char c : {'d', 'a', 't', 'a'}) wav.push_back(static_cast<uint8_t>(c));
  put32(data_bytes);
  for (int b = 0; b < kBlocks; ++b) {
    wav.push_back(0);  // predictor 0
    put16(0x7FFF);     // initial delta at its signed maximum
    put16(0);
    put16(0);
    // Nibble 8 scales the delta by 768/256 each step, so a few bytes overflow an unclamped int32.
    wav.insert(wav.end(), kBlockAlign - 7, 0x88);
  }

  auto [samples, rate] = load_buffer_wav(wav.data(), wav.size());
  REQUIRE(rate == sr);
  REQUIRE(!samples.empty());
  for (const float v : samples) {
    REQUIRE(std::isfinite(v));
    REQUIRE(std::abs(v) <= 1.0f);
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

TEST_CASE("load_buffer_wav folds 5.1 with the same rule as stereo", "[audio_io][downmix]") {
  // A 5.1 master whose program sits in the front pair must arrive at the same
  // level as the stereo mixdown of that pair: BS.775 averages the folded front
  // channels, it does not average every plane in the file.
  constexpr int sr = 22050;
  constexpr size_t frames = 1024;
  const std::vector<float> left = generate_sine(static_cast<int>(frames), 440.0f, sr, 0.6f);
  const std::vector<float> right = generate_sine(static_cast<int>(frames), 660.0f, sr, 0.4f);

  const std::vector<float> stereo = interleave({left, right}, frames);
  const std::vector<float> surround = interleave({left, right, {}, {}, {}, {}}, frames);
  const std::vector<uint8_t> stereo_wav =
      create_wav_buffer_multichannel(stereo.data(), frames, 2, sr);
  const std::vector<uint8_t> surround_wav =
      create_wav_buffer_multichannel(surround.data(), frames, 6, sr);

  auto [stereo_mono, stereo_sr] = load_buffer_wav(stereo_wav.data(), stereo_wav.size());
  auto [surround_mono, surround_sr] = load_buffer_wav(surround_wav.data(), surround_wav.size());
  REQUIRE(stereo_sr == sr);
  REQUIRE(surround_sr == sr);
  REQUIRE(stereo_mono.size() == frames);
  REQUIRE(surround_mono.size() == frames);

  const double stereo_level = rms(stereo_mono);
  const double surround_level = rms(surround_mono);
  REQUIRE(stereo_level > 0.1);
  INFO("stereo RMS " << stereo_level << ", 5.1 RMS " << surround_level);
  const double level_difference_db = 20.0 * std::log10(surround_level / stereo_level);
  REQUIRE_THAT(level_difference_db, Catch::Matchers::WithinAbs(0.0, 0.05));

  // Silent center/LFE/surround planes contribute nothing, so the folded signal
  // is the stereo mixdown sample for sample.
  for (size_t i = 0; i < frames; ++i) {
    REQUIRE_THAT(surround_mono[i], Catch::Matchers::WithinAbs(stereo_mono[i], 1e-6f));
  }
}

TEST_CASE("load_buffer_wav drops LFE and attenuates the center when folding 5.1",
          "[audio_io][downmix]") {
  // BS.775 omits the LFE plane: a cinema LFE feed is calibrated +10 dB and would
  // otherwise dominate the low end of every analysis. The center plane is the
  // positive control -- the same signal placed there must survive at -3 dB, so
  // the LFE assertion cannot pass by the loader returning silence.
  constexpr int sr = 22050;
  constexpr size_t frames = 1024;
  const std::vector<float> tone = generate_sine(static_cast<int>(frames), 80.0f, sr, 0.8f);

  const std::vector<float> lfe_only = interleave({{}, {}, {}, tone, {}, {}}, frames);
  const std::vector<float> center_only = interleave({{}, {}, tone, {}, {}, {}}, frames);
  const std::vector<uint8_t> lfe_wav =
      create_wav_buffer_multichannel(lfe_only.data(), frames, 6, sr);
  const std::vector<uint8_t> center_wav =
      create_wav_buffer_multichannel(center_only.data(), frames, 6, sr);

  auto [lfe_mono, lfe_sr] = load_buffer_wav(lfe_wav.data(), lfe_wav.size());
  auto [center_mono, center_sr] = load_buffer_wav(center_wav.data(), center_wav.size());
  REQUIRE(lfe_sr == sr);
  REQUIRE(center_sr == sr);

  INFO("LFE-only peak " << peak(lfe_mono) << ", center-only peak " << peak(center_mono));
  REQUIRE_THAT(peak(lfe_mono), Catch::Matchers::WithinAbs(0.0f, 1e-7f));
  REQUIRE_THAT(peak(center_mono),
               Catch::Matchers::WithinAbs(sonare::constants::kInvSqrt2 * peak(tone), 1e-5f));
}

TEST_CASE("load_buffer_wav folds by the speaker mask a six-channel file declares",
          "[audio_io][downmix]") {
  // Six planes are not always 5.1: 6.0 (mask 0x707) carries a full-range back
  // center where 5.1 carries the LFE, so that plane must survive the fold.
  constexpr int sr = 48000;
  constexpr size_t frames = 4096;
  const std::vector<float> tone = generate_sine(static_cast<int>(frames), 1000.0f, sr, 0.5f);
  const auto plane_only = [&](size_t plane) {
    std::vector<std::vector<float>> planes(6);
    planes[plane] = tone;
    return interleave(planes, frames);
  };
  const auto load = [&](const std::vector<float>& interleaved, uint32_t mask) {
    const std::vector<uint8_t> wav =
        create_wav_buffer_extensible(interleaved.data(), frames, 6, sr, mask);
    auto [mono, rate] = load_buffer_wav(wav.data(), wav.size());
    REQUIRE(rate == sr);
    REQUIRE(mono.size() == frames);
    REQUIRE(std::all_of(mono.begin(), mono.end(), [](float x) { return std::isfinite(x); }));
    return mono;
  };

  const std::vector<float> back_center = load(plane_only(3), 0x707u);
  INFO("6.0 back-center-only peak " << peak(back_center));
  REQUIRE_THAT(peak(back_center), Catch::Matchers::WithinAbs(peak(tone) / 6.0f, 1e-6f));

  // The LFE of a real 5.1 is still dropped.
  REQUIRE_THAT(peak(load(plane_only(3), 0x3Fu)), Catch::Matchers::WithinAbs(0.0f, 1e-7f));

  // 5.1(side) spells the surround pair with the side positions and folds like 5.1(back).
  for (size_t plane = 0; plane < 6; ++plane) {
    CAPTURE(plane);
    const std::vector<float> side = load(plane_only(plane), 0x60Fu);
    const std::vector<float> back = load(plane_only(plane), 0x3Fu);
    const std::vector<float> unmasked = load(plane_only(plane), 0u);
    for (size_t i = 0; i < frames; ++i) {
      REQUIRE(side[i] == back[i]);
      REQUIRE(unmasked[i] == back[i]);
    }
  }
}

TEST_CASE("load_buffer_wav keeps the mono and stereo folds bit-identical", "[audio_io][downmix]") {
  // Every golden in the repo runs through the stereo fold, so it must stay
  // exactly the arithmetic mean of the pair -- compared bitwise rather than
  // within a tolerance, and against the literal expression the loader has always
  // evaluated (a double-precision sum divided by the channel count).
  constexpr int sr = 44100;
  constexpr size_t frames = 2048;
  const std::vector<float> left = generate_sine(static_cast<int>(frames), 437.3f, sr, 0.71f);
  const std::vector<float> right = generate_sine(static_cast<int>(frames), 913.1f, sr, 0.29f);

  const std::vector<float> stereo = interleave({left, right}, frames);
  const std::vector<uint8_t> stereo_wav =
      create_wav_buffer_multichannel(stereo.data(), frames, 2, sr);
  auto [stereo_mono, stereo_sr] = load_buffer_wav(stereo_wav.data(), stereo_wav.size());
  REQUIRE(stereo_sr == sr);
  REQUIRE(stereo_mono.size() == frames);
  for (size_t i = 0; i < frames; ++i) {
    const float expected =
        static_cast<float>((static_cast<double>(left[i]) + static_cast<double>(right[i])) / 2.0);
    REQUIRE(stereo_mono[i] == expected);
  }

  // A single-channel file is passed through untouched.
  const std::vector<uint8_t> mono_wav = create_wav_buffer_multichannel(left.data(), frames, 1, sr);
  auto [mono, mono_sr] = load_buffer_wav(mono_wav.data(), mono_wav.size());
  REQUIRE(mono_sr == sr);
  REQUIRE(mono.size() == frames);
  for (size_t i = 0; i < frames; ++i) {
    REQUIRE(mono[i] == left[i]);
  }
}

TEST_CASE("private combined loader retains only the requested channel layout",
          "[audio_io][combined]") {
  constexpr int sample_rate = 24000;
  constexpr size_t frames = 37;
  const std::filesystem::path dir = scratch_dir();

  for (const int channels : {1, 2, 6, 8}) {
    std::vector<float> source(frames * static_cast<size_t>(channels));
    for (size_t frame = 0; frame < frames; ++frame) {
      for (int channel = 0; channel < channels; ++channel) {
        // Keep every plane distinguishable while staying well inside [-1, 1].
        source[frame * static_cast<size_t>(channels) + static_cast<size_t>(channel)] =
            0.01f * static_cast<float>(channel + 1) + 0.0001f * static_cast<float>(frame + 1);
      }
    }

    const std::filesystem::path path = dir / ("combined_" + std::to_string(channels) + ".wav");
    const auto wav = create_wav_buffer_multichannel(source.data(), frames, channels, sample_rate);
    {
      std::ofstream output(path, std::ios::binary);
      REQUIRE(output.is_open());
      output.write(reinterpret_cast<const char*>(wav.data()),
                   static_cast<std::streamsize>(wav.size()));
      REQUIRE(output.good());
    }

    const auto [legacy_mono, legacy_rate] = load_audio(path.string());
    const auto [legacy_interleaved, interleaved_rate, legacy_channels] =
        load_audio_interleaved(path.string());
    REQUIRE(legacy_rate == sample_rate);
    REQUIRE(interleaved_rate == sample_rate);
    REQUIRE(legacy_channels == channels);

    const auto mono_only = load_audio_input_once(path.string(), RetainChannels::None);
    REQUIRE(mono_only.sample_rate == sample_rate);
    REQUIRE(mono_only.source_channels == channels);
    REQUIRE(mono_only.mono == legacy_mono);
    REQUIRE(mono_only.interleaved.empty());

    const auto stereo_only = load_audio_input_once(path.string(), RetainChannels::StereoIfExact);
    REQUIRE(stereo_only.mono == legacy_mono);
    REQUIRE(stereo_only.source_channels == channels);
    if (channels == 2) {
      REQUIRE(stereo_only.interleaved == legacy_interleaved);
    } else {
      REQUIRE(stereo_only.interleaved.empty());
    }

    const auto all_channels = load_audio_input_once(path.string(), RetainChannels::All);
    REQUIRE(all_channels.mono == legacy_mono);
    REQUIRE(all_channels.source_channels == channels);
    REQUIRE(all_channels.interleaved == legacy_interleaved);
  }

  std::filesystem::remove_all(dir);
}

TEST_CASE("combined built-in retained allocation failure defers only the retained branch",
          "[audio_io][combined][allocation]") {
  const auto dir = scratch_dir();
  const auto path = dir / "retained_allocation.wav";
  constexpr size_t frames = 4096;
  std::vector<float> source(frames * 2);
  for (size_t frame = 0; frame < frames; ++frame) {
    source[2 * frame] = static_cast<float>(frame) / static_cast<float>(frames);
    source[2 * frame + 1] = -source[2 * frame];
  }
  save_wav_multichannel(path.string(), source.data(), frames, 2, ChannelLayout::Stereo, 22050);

  bool deferred = false;
  int source_channels = 0;
  int retained_channels = 0;
  size_t mono_size = 0;
  size_t retained_size = 0;
  // The exact allocation ordering is an implementation detail, so walk the
  // one-shot hook until it lands on retained reserve/insert rather than
  // coupling the regression to a particular std::vector growth sequence.
  for (size_t failure_point = 1; failure_point <= 128 && !deferred; ++failure_point) {
    {
      sonare::test::AllocationFailureAtGuard guard(failure_point);
      try {
        const auto decoded =
            load_audio_input_once(path.string(), RetainChannels::All, kDefaultLoadOptions);
        deferred = decoded.retained_error != nullptr;
        source_channels = decoded.source_channels;
        retained_channels = decoded.retained_channels;
        mono_size = decoded.mono.size();
        retained_size = decoded.interleaved.size();
      } catch (const std::exception&) {
        // Earlier points can hit file/mono/scratch allocations, which are
        // shared failures and must still throw immediately.
      }
    }
  }

  REQUIRE(deferred);
  REQUIRE(source_channels == 2);
  REQUIRE(retained_channels == 2);
  REQUIRE(mono_size == frames);
  REQUIRE(retained_size == 0);
  std::filesystem::remove_all(dir);
}

TEST_CASE("load_buffer_wav averages channel counts outside the speaker model",
          "[audio_io][downmix]") {
  // ChannelLayout covers 1/2/6/8 planes. A quad or LCR file carries no speaker
  // assignment the downmix matrix can reason about, so those keep the
  // unweighted mean rather than silently dropping the unmapped planes.
  constexpr int sr = 22050;
  constexpr size_t frames = 256;
  const std::vector<float> tone = generate_sine(static_cast<int>(frames), 300.0f, sr, 0.5f);
  const std::vector<float> quad = interleave({tone, tone, tone, tone}, frames);
  const std::vector<uint8_t> quad_wav = create_wav_buffer_multichannel(quad.data(), frames, 4, sr);

  auto [quad_mono, quad_sr] = load_buffer_wav(quad_wav.data(), quad_wav.size());
  REQUIRE(quad_sr == sr);
  REQUIRE(quad_mono.size() == frames);
  for (size_t i = 0; i < frames; ++i) {
    REQUIRE_THAT(quad_mono[i], Catch::Matchers::WithinAbs(tone[i], 1e-6f));
  }
}

TEST_CASE("WAV loaders size their buffer from data present, not a declared frame count",
          "[audio_io]") {
  // The declared count sits exactly at the offline decode limit, so it passes
  // every bound the loaders check and still describes 500M frames of audio that
  // are not in the file. Committing memory for it is a multi-second stall on a
  // 2 GB zero-init, or an OOM kill, in place of a prompt result.
  constexpr uint32_t kDeclaredDataBytes = 500'000'000u;
  constexpr size_t kDataBytes = 64;
  const std::vector<uint8_t> blob =
      create_wav_buffer_overdeclared_data(kDeclaredDataBytes, kDataBytes);
  // One 8-bit sample per byte present: the most this file can decode to.
  constexpr size_t kMaxDecodableSamples = kDataBytes;
  // Generous enough for a decode chunk's worth of growth, far below what the
  // declaration asks for.
  constexpr size_t kCapacityBudget = 64 * 1024;

  const auto within_budget = [](const std::chrono::steady_clock::time_point& start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start)
        .count();
  };

  SECTION("the mono loader") {
    const auto start = std::chrono::steady_clock::now();
    auto [samples, sample_rate] = load_buffer(blob.data(), blob.size());
    const auto elapsed_ms = within_budget(start);
    CAPTURE(elapsed_ms, samples.size(), samples.capacity());
    REQUIRE(sample_rate == 44100);
    REQUIRE(samples.size() <= kMaxDecodableSamples);
    // Capacity, not just size: a buffer trimmed after the fact still carries
    // whatever was committed for the declaration, for as long as the caller
    // holds the audio.
    REQUIRE(samples.capacity() <= kCapacityBudget);
    REQUIRE(elapsed_ms < 100);
  }

  SECTION("an AIFF frame count with no data behind it") {
    // The one declaration dr_wav passes through unclamped, and the only WAV
    // loader that can be reached with this container: detect_format sees no
    // RIFF/WAVE, so load_buffer routes it away and only the public
    // load_wav / load_buffer_wav entry points arrive here.
    const std::vector<uint8_t> aiff =
        create_aiff_buffer_overdeclared_frames(kDeclaredDataBytes, kDataBytes);
    auto [samples, sample_rate] = load_buffer_wav(aiff.data(), aiff.size());
    CAPTURE(samples.size(), samples.capacity());
    REQUIRE(sample_rate == 44100);
    REQUIRE(samples.size() == kDataBytes);
    REQUIRE(samples.capacity() <= kCapacityBudget);
  }

  SECTION("the interleaved loader") {
    // Only reachable by path, so the blob goes through a file.
    const std::string path = "test_wav_overdeclared_frames.wav";
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    out.write(reinterpret_cast<const char*>(blob.data()),
              static_cast<std::streamsize>(blob.size()));
    out.close();

    const auto start = std::chrono::steady_clock::now();
    auto [interleaved, sample_rate, channels] = load_audio_interleaved(path);
    const auto elapsed_ms = within_budget(start);
    std::remove(path.c_str());
    CAPTURE(elapsed_ms, interleaved.size(), interleaved.capacity());
    REQUIRE(sample_rate == 44100);
    REQUIRE(channels == 1);
    REQUIRE(interleaved.size() <= kMaxDecodableSamples);
    REQUIRE(interleaved.capacity() <= kCapacityBudget);
    REQUIRE(elapsed_ms < 100);
  }
}

TEST_CASE("a RIFF container carrying a codec the built-in decoder lacks reaches FFmpeg",
          "[audio_io]") {
  // Sniffing answers "which container", and a RIFF container can hold MPEG
  // Layer-3 (WAVE_FORMAT_MPEGLAYER3, which real encoders emit). Dispatching on
  // the container alone let the built-in sniffer's claim be final: dr_wav
  // refused the file and the FFmpeg fallback, which decodes it, sat on a branch
  // this file could never reach.
  if (std::system("command -v ffmpeg >/dev/null 2>&1") != 0) {
    SKIP("ffmpeg CLI not found on PATH");
  }
  const std::string mp3_path = "test_riff_mp3_source.mp3";
  const std::string wav_path = "test_riff_mp3.wav";
  const std::string encode =
      "ffmpeg -loglevel error -f lavfi -i "
      "sine=frequency=440:duration=0.25:sample_rate=22050 "
      "-ac 1 -c:a libmp3lame -b:a 32k -y " +
      mp3_path;
  REQUIRE(std::system(encode.c_str()) == 0);
  // Remux, not re-encode: the MP3 bitstream is copied into a RIFF/WAVE.
  const std::string remux =
      "ffmpeg -loglevel error -i " + mp3_path + " -c:a copy -f wav -y " + wav_path;
  REQUIRE(std::system(remux.c_str()) == 0);
  std::remove(mp3_path.c_str());

  std::ifstream input(wav_path, std::ios::binary);
  REQUIRE(input.is_open());
  const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(input)), {});
  input.close();

  // The container really is RIFF/WAVE, and its codec tag really is 0x0055.
  REQUIRE(detect_format(blob.data(), blob.size()) == AudioFormat::WAV);
  REQUIRE(blob.size() > 20);
  REQUIRE(blob[20] == 0x55);
  REQUIRE(blob[21] == 0x00);

#ifdef SONARE_WITH_FFMPEG
  auto [samples, sample_rate] = load_audio(wav_path);
  REQUIRE(sample_rate == 22050);
  REQUIRE(samples.size() > 1000);
  for (const float sample : samples) REQUIRE(std::isfinite(sample));
  // The buffer entry point agrees with the path entry point.
  auto [buffer_samples, buffer_rate] = load_buffer(blob.data(), blob.size());
  REQUIRE(buffer_rate == sample_rate);
  REQUIRE(buffer_samples.size() == samples.size());
  // A channel probe that disagreed with the loader would misconfigure the
  // caller that asked it which fold to apply.
  REQUIRE(audio_channel_count(wav_path) == 1);
#else
  // Without FFmpeg there is no decoder for this codec, and the failure must be
  // the decode class rather than a silent empty result.
  REQUIRE_THROWS_AS(load_audio(wav_path), sonare::SonareException);
#endif
  std::remove(wav_path.c_str());
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

TEST_CASE("load_buffer_mp3 folds stereo to the same arithmetic mean", "[audio_io][downmix]") {
  // MP3 carries at most two channels, so the only fold it can perform is the
  // stereo one -- and that fold must stay the plain arithmetic mean of the
  // decoded pair, bit for bit.
  if (std::system("command -v ffmpeg >/dev/null 2>&1") != 0) {
    SKIP("ffmpeg CLI not found on PATH");
  }
  const std::string path = "test_mp3_stereo_fold.mp3";
  const std::string command =
      "ffmpeg -loglevel error -f lavfi -i "
      "'sine=frequency=440:duration=0.25:sample_rate=22050' -f lavfi -i "
      "'sine=frequency=1300:duration=0.25:sample_rate=22050' "
      "-filter_complex '[0:a][1:a]amerge=inputs=2[a]' -map '[a]' "
      "-c:a libmp3lame -b:a 128k -y " +
      path;
  REQUIRE(std::system(command.c_str()) == 0);

  auto [interleaved, interleaved_sr, channels] = load_audio_interleaved(path);
  auto [mono, mono_sr] = load_audio(path);
  std::remove(path.c_str());

  REQUIRE(channels == 2);
  REQUIRE(mono_sr == interleaved_sr);
  REQUIRE(mono.size() == interleaved.size() / 2);
  for (size_t frame = 0; frame < mono.size(); ++frame) {
    REQUIRE(mono[frame] == 0.5f * (interleaved[2 * frame] + interleaved[2 * frame + 1]));
  }
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

TEST_CASE("a path load is bounded by its own max_file_size and nothing else",
          "[.][audio_io][slow]") {
  // The difference only shows above the buffer entry's fixed ceiling: that
  // ceiling used to apply on top of the option, so a caller that raised or
  // cleared max_file_size still lost the file -- and lost it after the whole
  // thing had been read into memory, which is exactly what the option exists to
  // avoid. The file carries a real (tiny) WAV header so format detection routes
  // it through the built-in decoder rather than short-circuiting somewhere the
  // ceiling never applied; the sparse tail past the declared data chunk is only
  // there to make the file large.
  const std::string oversize = "test_oversize_blob.wav";
  {
    const std::vector<float> one_sample(1, 0.25f);
    const std::vector<uint8_t> header =
        create_wav_buffer(one_sample.data(), one_sample.size(), 44100);
    std::ofstream out(oversize, std::ios::binary);
    REQUIRE(out.is_open());
    out.write(reinterpret_cast<const char*>(header.data()),
              static_cast<std::streamsize>(header.size()));
  }
  std::error_code ec;
  std::filesystem::resize_file(oversize, resource::kMaxAudioFileBytes + 1, ec);
  if (ec) {
    std::remove(oversize.c_str());
    SKIP("filesystem cannot hold a sparse 500 MB file");
  }

  const auto code_of = [](auto&& fn) {
    try {
      fn();
    } catch (const SonareException& e) {
      return e.code();
    }
    return ErrorCode::Ok;
  };

  AudioLoadOptions unlimited;
  unlimited.max_file_size = 0;
  // The load either succeeds on the declared (tiny) data chunk or fails on the
  // content; what it must not be is the "too large" rejection, which is the one
  // the caller cleared.
  CHECK(code_of([&] { load_audio(oversize, unlimited); }) != ErrorCode::InvalidParameter);
  CHECK(code_of([&] { load_audio_interleaved(oversize, unlimited); }) !=
        ErrorCode::InvalidParameter);
  CHECK(code_of([&] { audio_channel_count(oversize, unlimited); }) != ErrorCode::InvalidParameter);

  // A ceiling the caller did set still applies, and still applies before the
  // bytes are read.
  AudioLoadOptions bounded;
  bounded.max_file_size = 1024;
  CHECK(code_of([&] { load_audio(oversize, bounded); }) == ErrorCode::InvalidParameter);
  CHECK(code_of([&] { load_audio_interleaved(oversize, bounded); }) == ErrorCode::InvalidParameter);

  std::remove(oversize.c_str());
}

#ifndef _WIN32
TEST_CASE("a path load reads an input whose size cannot be probed", "[audio_io]") {
  constexpr int sr = 22050;
  constexpr int samples = 100;
  const std::vector<float> sine = generate_sine(samples, 440.0f, sr);
  const std::vector<uint8_t> wav_data = create_wav_buffer(sine.data(), sine.size(), sr);

  const std::filesystem::path dir = scratch_dir();
  const std::filesystem::path fifo = dir / "unseekable.wav";
  if (::mkfifo(fifo.c_str(), 0600) != 0) {
    std::filesystem::remove_all(dir);
    SKIP("platform cannot create a FIFO");
  }

  // A FIFO reports no size at all, so the loader cannot derive an allocation
  // from the probe: it must read the bytes as they arrive. The writer's open
  // and the loader's open rendezvous, which is what makes the pipe readable.
  std::thread writer([&] {
    std::ofstream out(fifo, std::ios::binary);
    if (!out.is_open()) return;
    out.write(reinterpret_cast<const char*>(wav_data.data()),
              static_cast<std::streamsize>(wav_data.size()));
  });

  std::vector<float> loaded;
  int loaded_sr = 0;
  std::string failure;
  try {
    std::tie(loaded, loaded_sr) = load_audio(fifo.string());
  } catch (const std::exception& e) {
    // std::exception, not SonareException: an allocation sized from the failed
    // probe would escape as std::length_error, which the library's error-class
    // contract forbids just as much as a wrong diagnostic.
    failure = e.what();
  }
  writer.join();

  CHECK(failure.empty());
  CHECK(loaded_sr == sr);
  CHECK(loaded.size() == static_cast<size_t>(samples));

  std::filesystem::remove_all(dir);
}
#endif

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

  const auto combined = load_audio_input_once(m4a_path, RetainChannels::StereoIfExact);
  REQUIRE(combined.source_channels == audio_channel_count(m4a_path));
  REQUIRE(combined.retained_channels == 2);
  REQUIRE(combined.retained_error == nullptr);
  REQUIRE_FALSE(combined.interleaved.empty());

  cleanup();
}

// Covers HE-AAC decoding, NOT the implicit-SBR sample-rate adoption that
// load_audio performs by preferring the first decoded frame's rate over the
// stream-declared one. aac_at emits explicit SBR signalling, so the stream
// already declares the doubled rate and both code paths agree: reverting that
// adoption leaves this case green, which was verified by ablation. Exercising
// it needs an encoder that can signal SBR implicitly (libfdk_aac's
// `-signaling implicit`), which is in no ffmpeg build this repo has access to,
// so the adoption is currently unguarded. Do not read a pass here as covering
// it, and do not rename this back without a fixture that goes red.
TEST_CASE("load_audio decodes an HE-AAC stream", "[audio_io][ffmpeg][he-aac]") {
  // `-h encoder=` exits 0 for an unknown encoder, so the probe reads the list.
  if (std::system("ffmpeg -hide_banner -encoders 2>/dev/null | grep -q ' aac_at '") != 0) {
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

TEST_CASE("load_audio folds a bed identically through the built-in and FFmpeg decoders",
          "[audio_io][ffmpeg][downmix]") {
  // The same program must reach analysis as the same mono signal no matter which
  // container it arrived in: WAV takes the built-in decoder, FLAC takes FFmpeg.
  if (std::system("command -v ffmpeg >/dev/null 2>&1") != 0) {
    SKIP("ffmpeg CLI not found on PATH");
  }

  constexpr int sample_rate = 22050;
  constexpr size_t frames = 4096;
  const std::vector<std::vector<float>> planes = {
      generate_sine(static_cast<int>(frames), 220.0f, sample_rate, 0.50f),  // L
      generate_sine(static_cast<int>(frames), 330.0f, sample_rate, 0.40f),  // R
      generate_sine(static_cast<int>(frames), 550.0f, sample_rate, 0.30f),  // C
      generate_sine(static_cast<int>(frames), 45.0f, sample_rate, 0.60f),   // LFE
      generate_sine(static_cast<int>(frames), 770.0f, sample_rate, 0.20f),  // Ls
      generate_sine(static_cast<int>(frames), 990.0f, sample_rate, 0.15f),  // Rs
  };

  ChannelLayout layout = ChannelLayout::Stereo;
  std::string transcode_filter;
  SECTION("stereo") { layout = ChannelLayout::Stereo; }
  SECTION("5.1") { layout = ChannelLayout::FivePointOne; }
  SECTION("5.1 relabelled with side surrounds") {
    // Muxers spell the 5.1 surround pair either BACK_LEFT/BACK_RIGHT or
    // SIDE_LEFT/SIDE_RIGHT. Both name the same speakers, so relabelling the
    // planes must not change the fold -- in particular it must not drop the
    // stream out of the speaker model and onto a plain average of all six.
    layout = ChannelLayout::FivePointOne;
    transcode_filter = " -af 'pan=5.1(side)|c0=c0|c1=c1|c2=c2|c3=c3|c4=c4|c5=c5'";
  }

  const int channels = channel_count(layout);
  const std::vector<float> bed = interleave(
      std::vector<std::vector<float>>(planes.begin(), planes.begin() + channels), frames);

  const std::string wav_path = "test_downmix_bed.wav";
  const std::string flac_path = "test_downmix_bed.flac";
  auto cleanup = [&]() {
    std::remove(wav_path.c_str());
    std::remove(flac_path.c_str());
  };
  save_wav_multichannel(wav_path, bed.data(), frames, channels, layout, sample_rate, 24);
  REQUIRE(std::system(("ffmpeg -loglevel error -i " + wav_path + transcode_filter +
                       " -c:a flac -y " + flac_path)
                          .c_str()) == 0);

  auto [wav_mono, wav_sr] = load_audio(wav_path);
  auto [flac_mono, flac_sr] = load_audio(flac_path);
  cleanup();

  REQUIRE(wav_sr == sample_rate);
  REQUIRE(flac_sr == sample_rate);
  REQUIRE(wav_mono.size() == frames);
  REQUIRE(flac_mono.size() == frames);

  const double wav_level = rms(wav_mono);
  const double flac_level = rms(flac_mono);
  REQUIRE(wav_level > 0.05);
  INFO("layout " << channel_layout_to_string(layout) << ": WAV RMS " << wav_level << ", FLAC RMS "
                 << flac_level);
  REQUIRE_THAT(20.0 * std::log10(flac_level / wav_level), Catch::Matchers::WithinAbs(0.0, 0.05));

  // 24-bit PCM plus FLAC's lossless round trip leaves only quantization noise.
  double worst = 0.0;
  for (size_t i = 0; i < frames; ++i) {
    worst = std::max(worst, std::abs(static_cast<double>(wav_mono[i]) - flac_mono[i]));
  }
  INFO("largest per-sample difference " << worst);
  REQUIRE(worst < 1e-4);
}

TEST_CASE("retained FFmpeg stereo survives an input rate and format renegotiation",
          "[audio_io][ffmpeg][retained]") {
  if (std::system("command -v ffmpeg >/dev/null 2>&1") != 0) {
    SKIP("ffmpeg CLI not found on PATH");
  }

  const std::string first_path = "test_retained_rate_change_22050.aac";
  const std::string second_path = "test_retained_rate_change_44100.aac";
  const std::string concat_path = "test_retained_rate_change.ts";
  auto cleanup = [&]() {
    std::remove(first_path.c_str());
    std::remove(second_path.c_str());
    std::remove(concat_path.c_str());
  };

  const std::string generate_first =
      "ffmpeg -loglevel error -f lavfi -i "
      "'sine=frequency=440:sample_rate=22050:duration=0.3' "
      "-ac 2 -c:a aac -b:a 96k -f adts -y " +
      first_path;
  const std::string generate_second =
      "ffmpeg -loglevel error -f lavfi -i "
      "'sine=frequency=880:sample_rate=44100:duration=0.3' "
      "-ac 2 -c:a aac -b:a 96k -f adts -y " +
      second_path;
  const std::string concatenate = "ffmpeg -loglevel error -i 'concat:" + first_path + "|" +
                                  second_path + "' -c copy -f mpegts -y " + concat_path;
  REQUIRE(std::system(generate_first.c_str()) == 0);
  REQUIRE(std::system(generate_second.c_str()) == 0);
  REQUIRE(std::system(concatenate.c_str()) == 0);

  const auto combined = load_audio_input_once(concat_path, RetainChannels::StereoIfExact);
  REQUIRE(combined.source_channels == 2);
  REQUIRE(combined.retained_channels == 2);
  REQUIRE(combined.retained_error == nullptr);
  REQUIRE_FALSE(combined.mono.empty());
  REQUIRE_FALSE(combined.interleaved.empty());
  REQUIRE(combined.sample_rate == 22050);

  const auto [legacy_mono, legacy_rate] = load_audio(concat_path);
  REQUIRE(legacy_rate == combined.sample_rate);
  REQUIRE(legacy_mono == combined.mono);

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
  const std::string reverse_concat_path = "test_interleaved_ffmpeg_renegotiate_reverse.ts";
  const std::string same_layout_path = "test_interleaved_ffmpeg_same_layout.ts";
  const std::string transport_fault_path = "test_interleaved_ffmpeg_transport_fault.ts";
  const std::string fault_order_path = "test_interleaved_ffmpeg_fault_order.ts";
  auto cleanup = [&]() {
    std::remove(mono_path.c_str());
    std::remove(stereo_path.c_str());
    std::remove(concat_path.c_str());
    std::remove(reverse_concat_path.c_str());
    std::remove(same_layout_path.c_str());
    std::remove(transport_fault_path.c_str());
    std::remove(fault_order_path.c_str());
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
  const std::string concatenate_reverse = "ffmpeg -loglevel error -i 'concat:" + stereo_path + "|" +
                                          mono_path + "' -c copy -f mpegts -y " +
                                          reverse_concat_path;
  const std::string remux_same_layout =
      "ffmpeg -loglevel error -i " + stereo_path + " -c copy -f mpegts -y " + same_layout_path;
  REQUIRE(std::system(generate_mono.c_str()) == 0);
  REQUIRE(std::system(generate_stereo.c_str()) == 0);
  REQUIRE(std::system(concatenate.c_str()) == 0);
  REQUIRE(std::system(concatenate_reverse.c_str()) == 0);
  REQUIRE(std::system(remux_same_layout.c_str()) == 0);

  const auto append_malformed_transport_packet = [](const std::string& source_path,
                                                    const std::string& destination_path) {
    std::ifstream input(source_path, std::ios::binary);
    REQUIRE(input.is_open());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    input.close();
    bytes.insert(bytes.end(), 188, 0);
    std::ofstream output(destination_path, std::ios::binary);
    REQUIRE(output.is_open());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
  };
  append_malformed_transport_packet(same_layout_path, transport_fault_path);
  append_malformed_transport_packet(concat_path, fault_order_path);

  // Control: with no preceding layout failure, the malformed packet is an
  // actual later read fault rather than an ignored tail.
  REQUIRE_THROWS_WITH(load_audio_interleaved(transport_fault_path),
                      Catch::Matchers::ContainsSubstring("FFmpeg: av_read_frame failed"));

  REQUIRE_THROWS_WITH(load_audio_interleaved(concat_path),
                      Catch::Matchers::ContainsSubstring(
                          "FFmpeg: interleaved decode does not support a channel layout change "
                          "midstream"));

  // A malformed transport packet follows the real layout change. If the
  // required public retained branch merely stored its failure and continued,
  // av_read_frame would reach this packet and replace the canonical layout
  // diagnostic with an unrelated transport error. The canonical error proves
  // that the decoder stopped at the first required retained failure.
  REQUIRE_THROWS_WITH(
      load_audio_interleaved(fault_order_path),
      Catch::Matchers::ContainsSubstring(
          "FFmpeg: interleaved decode does not support a channel layout change midstream"));

  // The legacy loader still downmixes the same stream while rebuilding its
  // mono resampler, so this guard does not weaken its established behavior.
  auto [decoded, decoded_rate] = load_audio(concat_path);
  REQUIRE(decoded_rate == sample_rate);
  REQUIRE(decoded.size() > 0);

  // Put the stereo segment first as well, so StereoIfExact has already created
  // its retained resampler when the stream changes to mono. The retained error
  // must be deferred while the authoritative mono result remains usable.
  const auto combined = load_audio_input_once(reverse_concat_path, RetainChannels::StereoIfExact);
  REQUIRE(combined.source_channels == 2);
  REQUIRE(combined.sample_rate == sample_rate);
  REQUIRE_FALSE(combined.mono.empty());
  REQUIRE(combined.interleaved.empty());
  REQUIRE(combined.retained_error != nullptr);

  const auto mono_only = load_audio_input_once(reverse_concat_path, RetainChannels::None);
  REQUIRE(mono_only.source_channels == 2);
  REQUIRE(mono_only.retained_error == nullptr);
  REQUIRE_FALSE(mono_only.mono.empty());

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

/// First sample byte of the RIFF data chunk.
size_t data_chunk_offset(const std::vector<uint8_t>& b) {
  const std::string tag = "data";
  const auto chunk = std::search(b.begin(), b.end(), tag.begin(), tag.end());
  REQUIRE(chunk != b.end());
  return static_cast<size_t>(chunk - b.begin()) + 8;
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
    const float decoded = static_cast<float>(raw) / 32768.0f;
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
    const float decoded = static_cast<float>(raw) / 8388608.0f;
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

  // Same bound on the mono/stereo writers, which the header documents as
  // producing bit-identical output: past it dr_wav saturates the size fields, so
  // the file is written and only a reader notices the tail is gone.
  //
  // Outside the working tree, unlike the sibling cases: this is the one case
  // that deliberately walks a writer up to its guard, so if the guard ever moved
  // below the temp-file creation the scratch would be stranded wherever this
  // points -- and a stranded file in the repo root is one `git add .` from being
  // committed.
  const std::filesystem::path mono_path = scratch_dir() / "riff_overflow.wav";
  REQUIRE_THROWS_AS(
      save_wav(mono_path.string(), &sentinel, static_cast<size_t>(1) << 31, 48000, 24),
      sonare::SonareException);
  REQUIRE_THROWS_AS(
      save_wav_multichannel(mono_path.string(), &sentinel, static_cast<size_t>(1) << 31, 1,
                            ChannelLayout::Mono, 48000, 24),
      sonare::SonareException);
  // Neither call reached the writer, so no file (not even a temporary) was left.
  REQUIRE_FALSE(std::filesystem::exists(mono_path));
  size_t stray_count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(mono_path.parent_path())) {
    CAPTURE(entry.path().string());
    ++stray_count;
  }
  REQUIRE(stray_count == 0);
  std::filesystem::remove_all(mono_path.parent_path());
}

TEST_CASE("save_wav quantizes 16-bit PCM by rounding to nearest, not truncating", "[audio_io]") {
  // Pick float samples whose scaled magnitude has a fractional part > 0.5 so
  // round-half-away-from-zero and toward-zero truncation disagree. The old
  // writer truncated (static_cast), losing up to a full LSB and ~3 dB SNR vs a
  // libsndfile-style nearest-neighbor encoder.
  const std::vector<float> original = {
      100.7f / 32768.0f,   // +: trunc 100, round 101
      -100.7f / 32768.0f,  // -: trunc -100, round -101 (away from zero)
      50.2f / 32768.0f,    // control: trunc == round == 50
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

TEST_CASE("a non-finite sample is written as silence and counted, not as full scale",
          "[audio_io]") {
  // The clamp the writers apply cannot separate these inputs: every comparison
  // against a non-finite value is false, so std::min returns its other argument
  // and each non-finite sample took the same code as the +1.0 peak beside it --
  // a value the encoder could equally have meant to produce. The stored code and
  // the reported count are what tell the two apart.
  const std::vector<float> samples = {
      1.0f,
      std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity(),
      -1.0f,
  };

  SECTION("16-bit") {
    const std::string path = "test_pcm16_non_finite.wav";
    size_t non_finite = 0;
    save_wav(path, samples, 48000, 16, &non_finite);
    auto [loaded, sr] = load_wav(path);
    std::remove(path.c_str());

    REQUIRE(sr == 48000);
    REQUIRE(loaded.size() == samples.size());
    // dr_wav reconstructs int16 V as V / 32768, so the stored integer comes back
    // exactly by rounding the loaded float.
    const std::vector<int16_t> expected = {32767, 0, 0, 0, -32768};
    for (size_t i = 0; i < expected.size(); ++i) {
      CAPTURE(i);
      REQUIRE(std::lround(static_cast<double>(loaded[i]) * 32768.0) == expected[i]);
    }
    REQUIRE(non_finite == 3);
  }

  SECTION("24-bit") {
    const std::string path = "test_pcm24_non_finite.wav";
    size_t non_finite = 0;
    save_wav(path, samples, 48000, 24, &non_finite);
    auto [loaded, sr] = load_wav(path);
    std::remove(path.c_str());

    REQUIRE(sr == 48000);
    REQUIRE(loaded.size() == samples.size());
    const std::vector<int32_t> expected = {8388607, 0, 0, 0, -8388608};
    for (size_t i = 0; i < expected.size(); ++i) {
      CAPTURE(i);
      REQUIRE(std::lround(static_cast<double>(loaded[i]) * 8388608.0) == expected[i]);
    }
    REQUIRE(non_finite == 3);
  }

  SECTION("the multichannel writers pack the same way") {
    // Both branches of save_wav_multichannel: stereo goes through plain PCM,
    // 5.1 through the hand-written EXTENSIBLE serializer, and they share one
    // packer. The data chunk is read directly so the assertion is on the byte
    // the file carries.
    const std::string stereo_path = "test_stereo_non_finite.wav";
    size_t stereo_non_finite = 0;
    save_wav_multichannel(stereo_path, samples.data(), samples.size() / 2, 2, ChannelLayout::Stereo,
                          48000, 16, &stereo_non_finite);
    std::remove(stereo_path.c_str());

    const std::string surround_path = "test_surround_non_finite.wav";
    std::vector<float> interleaved(12, 0.25f);
    interleaved[0] = 1.0f;
    interleaved[1] = std::numeric_limits<float>::quiet_NaN();
    interleaved[2] = -std::numeric_limits<float>::infinity();
    size_t surround_non_finite = 0;
    save_wav_multichannel(surround_path, interleaved.data(), 2, 6, ChannelLayout::FivePointOne,
                          48000, 16, &surround_non_finite);
    const std::vector<uint8_t> bytes = read_file_bytes(surround_path);
    std::remove(surround_path.c_str());

    const size_t data_off = 68;
    REQUIRE(static_cast<int16_t>(le16(bytes, data_off)) == 32767);
    REQUIRE(static_cast<int16_t>(le16(bytes, data_off + 2)) == 0);
    REQUIRE(static_cast<int16_t>(le16(bytes, data_off + 4)) == 0);
    REQUIRE(stereo_non_finite == 3);
    REQUIRE(surround_non_finite == 2);
  }
}

// The writer quantizes on the grid the decoder divides by, so a file read and
// written back at its own width keeps every code, both endpoints included.
TEST_CASE("save_wav round-trips every decoded PCM code at the same width", "[audio_io]") {
  SECTION("16-bit, every code") {
    std::vector<float> samples;
    samples.reserve(65536);
    for (int code = -32768; code <= 32767; ++code) {
      samples.push_back(static_cast<float>(code) / 32768.0f);
    }
    const std::string first = "test_pcm16_roundtrip_a.wav";
    const std::string second = "test_pcm16_roundtrip_b.wav";
    save_wav(first, samples, 48000, 16);
    auto [decoded, sr] = load_wav(first);
    save_wav(second, decoded, sr, 16);
    const std::vector<uint8_t> a = read_file_bytes(first);
    const std::vector<uint8_t> b = read_file_bytes(second);
    std::remove(first.c_str());
    std::remove(second.c_str());
    REQUIRE(a == b);
    const size_t data_off = data_chunk_offset(a);
    for (size_t i = 0; i < samples.size(); i += 4099) {
      CAPTURE(i);
      REQUIRE(static_cast<int16_t>(le16(a, data_off + 2 * i)) == static_cast<int>(i) - 32768);
    }
    REQUIRE(static_cast<int16_t>(le16(a, data_off)) == -32768);
    REQUIRE(static_cast<int16_t>(le16(a, data_off + 2 * 65535)) == 32767);
  }

  SECTION("24-bit, both ends, around zero and a stride through the rest") {
    std::vector<int32_t> codes;
    for (int32_t c = -8388608; c < -8388608 + 4096; ++c) codes.push_back(c);
    for (int32_t c = -4096; c < 4096; ++c) codes.push_back(c);
    for (int32_t c = 8388607 - 4095; c <= 8388607; ++c) codes.push_back(c);
    for (int32_t c = -8388608; c <= 8388607 - 997; c += 997) codes.push_back(c);
    std::vector<float> samples;
    for (int32_t c : codes) samples.push_back(static_cast<float>(c) / 8388608.0f);
    const std::string first = "test_pcm24_roundtrip_a.wav";
    const std::string second = "test_pcm24_roundtrip_b.wav";
    save_wav(first, samples, 48000, 24);
    auto [decoded, sr] = load_wav(first);
    save_wav(second, decoded, sr, 24);
    const std::vector<uint8_t> a = read_file_bytes(first);
    const std::vector<uint8_t> b = read_file_bytes(second);
    std::remove(first.c_str());
    std::remove(second.c_str());
    REQUIRE(a == b);
    const size_t data_off = data_chunk_offset(a);
    for (size_t i = 0; i < codes.size(); ++i) {
      if (signed_le24(a, data_off + 3 * i) != codes[i]) {
        CAPTURE(i, codes[i]);
        REQUIRE(signed_le24(a, data_off + 3 * i) == codes[i]);
      }
    }
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

TEST_CASE("a failed write reports an encode error, not a decode error", "[audio_io]") {
  // Writing and reading fail for unrelated reasons, so they carry unrelated
  // error codes: a caller that retries a decode failure with a different
  // decoder, or reports "corrupt input" to a user, would do the wrong thing for
  // a full disk or an unwritable directory. Both save entry points are covered
  // because they are separate implementations rather than one wrapping the
  // other.
  const std::vector<float> samples(64, 0.25f);
  const std::string unwritable = "/nonexistent-directory-for-write-failure/out.wav";

  const auto code_of = [](auto&& fn) {
    try {
      fn();
    } catch (const SonareException& e) {
      return e.code();
    }
    return ErrorCode::Ok;
  };

  REQUIRE(code_of([&] { save_wav(unwritable, samples, 48000); }) == ErrorCode::EncodeFailed);
  REQUIRE(code_of([&] {
            save_wav_multichannel(unwritable, samples.data(), samples.size() / 2, 2,
                                  ChannelLayout::Stereo, 48000);
          }) == ErrorCode::EncodeFailed);

  // A destination that resolves to a directory is the one failure that happens
  // in the atomic rename rather than when the writer opens its temp file: the
  // temp is a perfectly writable sibling, everything is packed and flushed, and
  // only the commit fails. That last stage is a stage of writing the output, so
  // it must carry the same code as the earlier ones -- a `-o` pointing at a
  // directory is a shell slip, not an undecodable input.
  // The enclosing scratch directory holds nothing but the destination, so the
  // sweep for a leftover temp sibling below sees only what these two calls made.
  const std::filesystem::path scratch = scratch_dir();
  const std::filesystem::path as_directory = scratch / "out.wav";
  REQUIRE(std::filesystem::create_directories(as_directory));
  REQUIRE(code_of([&] { save_wav(as_directory.string(), samples, 48000); }) ==
          ErrorCode::EncodeFailed);
  REQUIRE(code_of([&] {
            save_wav_multichannel(as_directory.string(), samples.data(), samples.size() / 2, 2,
                                  ChannelLayout::Stereo, 48000);
          }) == ErrorCode::EncodeFailed);
  // The rejected writes left neither the directory replaced nor a temp sibling.
  REQUIRE(std::filesystem::is_directory(as_directory));
  size_t scratch_entries = 0;
  for (const auto& entry : std::filesystem::directory_iterator(scratch)) {
    CAPTURE(entry.path().string());
    REQUIRE(entry.path().filename() == as_directory.filename());
    ++scratch_entries;
  }
  REQUIRE(scratch_entries == 1);
  std::filesystem::remove_all(scratch);

  // The read side keeps its own code: the two must not have been merged in the
  // other direction either.
  REQUIRE(code_of([&] { Audio::from_file(unwritable); }) != ErrorCode::EncodeFailed);
}

#ifndef _WIN32
TEST_CASE("a write failing at write, flush or close leaves the destination byte-identical",
          "[audio_io]") {
  // RLIMIT_FSIZE makes the kernel refuse bytes past the limit with EFBIG, the
  // same deferred failure a full disk gives: a small image fails only when stdio
  // flushes its buffer, a large one already inside fwrite.
  const std::filesystem::path scratch = scratch_dir();
  const std::string path = (scratch / "keep.wav").string();
  save_wav(path, std::vector<float>(256, 0.25f), 48000);
  const std::vector<uint8_t> before = read_file_bytes(path);
  REQUIRE(before.size() > 64);

  const auto lowest_free_fd = [] {
    const int fd = ::open("/dev/null", O_RDONLY);
    ::close(fd);
    return fd;
  };
  const auto encode_code = [](auto&& fn) {
    try {
      fn();
    } catch (const SonareException& e) {
      return e.code();
    }
    return ErrorCode::Ok;
  };

  for (const size_t n_samples : {size_t{300}, size_t{60000}}) {
    const std::vector<float> replacement(n_samples, -0.5f);
    const int fd_before = lowest_free_fd();

    struct rlimit saved {};
    REQUIRE(::getrlimit(RLIMIT_FSIZE, &saved) == 0);
    struct rlimit limited = saved;
    limited.rlim_cur = 64;
    void (*saved_handler)(int) = std::signal(SIGXFSZ, SIG_IGN);
    const bool limited_ok = ::setrlimit(RLIMIT_FSIZE, &limited) == 0;
    // Nothing may assert while the limit holds: a failed REQUIRE would leave it
    // in place for every later test.
    const ErrorCode mono = encode_code([&] { save_wav(path, replacement, 48000, 24); });
    const ErrorCode stereo = encode_code([&] {
      save_wav_multichannel(path, replacement.data(), n_samples / 2, 2, ChannelLayout::Stereo,
                            48000);
    });
    const ErrorCode surround = encode_code([&] {
      save_wav_multichannel(path, replacement.data(), n_samples / 6, 6, ChannelLayout::FivePointOne,
                            48000);
    });
    const bool restored = ::setrlimit(RLIMIT_FSIZE, &saved) == 0;
    std::signal(SIGXFSZ, saved_handler);

    CAPTURE(n_samples);
    REQUIRE(limited_ok);
    REQUIRE(restored);
    CHECK(mono == ErrorCode::EncodeFailed);
    CHECK(stereo == ErrorCode::EncodeFailed);
    CHECK(surround == ErrorCode::EncodeFailed);
    CHECK(read_file_bytes(path) == before);
    CHECK(lowest_free_fd() == fd_before);
  }

  // No temp sibling survived any of the failed writes.
  for (const auto& entry : std::filesystem::directory_iterator(scratch)) {
    CAPTURE(entry.path().string());
    CHECK(entry.path().filename() == "keep.wav");
  }
  std::filesystem::remove_all(scratch);
}
#endif
