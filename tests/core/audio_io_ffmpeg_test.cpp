/// @file audio_io_ffmpeg_test.cpp
/// @brief Tests for the FFmpeg decode path: input confinement and damaged-packet recovery.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "core/audio_io.h"
#include "core/audio_io_detail.h"
#include "util/constants.h"
#include "util/exception.h"

#ifdef SONARE_WITH_FFMPEG

using namespace sonare;

namespace {

/// @brief A fresh empty directory under the system temp path, removed on scope exit.
struct ScratchDir {
  std::filesystem::path path;
  ScratchDir() {
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    for (int suffix = 0; suffix < 1024; ++suffix) {
      const std::filesystem::path dir = base / ("sonare_ffmpeg_test_" + std::to_string(suffix));
      if (std::filesystem::create_directories(dir)) {
        path = dir;
        return;
      }
    }
    FAIL("no free scratch directory under the system temp path");
  }
  ~ScratchDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

/// @brief Removes a file on scope exit, so a failing REQUIRE does not strand it in the cwd.
struct RemoveOnExit {
  std::string path;
  ~RemoveOnExit() { std::remove(path.c_str()); }
};

std::vector<uint8_t> read_bytes(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input.is_open());
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)), {});
}

void write_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::ofstream output(path, std::ios::binary);
  REQUIRE(output.is_open());
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

/// @brief Runs @p load and returns the code of the SonareException it throws.
template <typename F>
ErrorCode thrown_code(F&& load) {
  try {
    load();
  } catch (const SonareException& e) {
    return e.code();
  }
  FAIL("expected a SonareException");
  return ErrorCode::Ok;
}

/// @brief Offset of the first FLAC audio frame: past "fLaC" and every metadata block.
size_t flac_audio_offset(const std::vector<uint8_t>& flac) {
  REQUIRE(flac.size() > 8);
  REQUIRE(std::string(flac.begin(), flac.begin() + 4) == "fLaC");
  size_t pos = 4;
  while (true) {
    REQUIRE(pos + 4 <= flac.size());
    const bool last = (flac[pos] & 0x80) != 0;
    const size_t length = (static_cast<size_t>(flac[pos + 1]) << 16) |
                          (static_cast<size_t>(flac[pos + 2]) << 8) | flac[pos + 3];
    pos += 4 + length;
    if (last) return pos;
  }
}

/// @brief Overwrites 200 bytes at the middle of [@p begin, end) with fixed pseudo-random data.
void corrupt_middle(std::vector<uint8_t>& bytes, size_t begin) {
  constexpr size_t kDamage = 200;
  REQUIRE(bytes.size() > begin + 4 * kDamage);
  const size_t start = begin + (bytes.size() - begin) / 2;
  uint32_t state = 0x12345678u;
  for (size_t i = 0; i < kDamage; ++i) {
    state = state * 1664525u + 1013904223u;
    bytes[start + i] = static_cast<uint8_t>(state >> 24);
  }
}

}  // namespace

TEST_CASE("FFmpeg buffer input cannot open a secondary input named by its content",
          "[audio_io][ffmpeg]") {
  // A concat script resolves its relative entries against the process cwd, so the
  // target sits there; it decodes on its own, which is what makes refusal meaningful.
  const std::string target = "sonare_ffconcat_target.wav";
  RemoveOnExit target_guard{target};
  std::vector<float> tone(4410);
  for (size_t i = 0; i < tone.size(); ++i) {
    tone[i] = 0.5f * std::sin(constants::kTwoPi * 440.0f * static_cast<float>(i) / 22050.0f);
  }
  save_wav(target, tone, 22050);
  REQUIRE(std::get<0>(load_audio(target)).size() == tone.size());

  const std::string script = "ffconcat version 1.0\nfile " + target + "\n";
  const std::vector<uint8_t> buffer(script.begin(), script.end());
  REQUIRE(detect_format(buffer.data(), buffer.size()) == AudioFormat::Unknown);

  ScratchDir dir;
  const std::string script_path = (dir.path / "playlist.ffconcat").string();
  write_bytes(script_path, buffer);

  CHECK(thrown_code([&] { load_buffer(buffer.data(), buffer.size()); }) == ErrorCode::DecodeFailed);
  CHECK(thrown_code([&] { load_buffer_interleaved(buffer.data(), buffer.size()); }) ==
        ErrorCode::DecodeFailed);
  CHECK(thrown_code([&] { load_audio(script_path); }) == ErrorCode::DecodeFailed);
  CHECK(thrown_code([&] { load_audio_interleaved(script_path); }) == ErrorCode::DecodeFailed);
  CHECK(thrown_code([&] { load_audio_input_once(script_path, RetainChannels::All); }) ==
        ErrorCode::DecodeFailed);
  CHECK(thrown_code([&] { audio_channel_count(script_path); }) == ErrorCode::DecodeFailed);
}

TEST_CASE("FFmpeg buffer input still loads audio in A/V containers", "[audio_io][ffmpeg]") {
  if (std::system("command -v ffmpeg >/dev/null 2>&1") != 0) {
    SKIP("ffmpeg CLI not found on PATH");
  }
  ScratchDir dir;
  const std::pair<const char*, const char*> containers[] = {
      {"tone.ts", "-c:a aac -f mpegts"}, {"tone.avi", "-c:a pcm_s16le -f avi"},
      {"tone.mpg", "-c:a mp2 -f mpeg"},  {"tone.mka", "-c:a flac -f matroska"},
      {"tone.wma", "-c:a wmav2 -f asf"}, {"tone.ogg", "-c:a flac -f ogg"},
  };
  for (const auto& [name, codec_args] : containers) {
    DYNAMIC_SECTION(name) {
      const std::string path = (dir.path / name).string();
      const std::string encode =
          "ffmpeg -loglevel error -f lavfi -i sine=frequency=440:duration=0.5:sample_rate=22050 "
          "-ac 1 " +
          std::string(codec_args) + " -y '" + path + "'";
      REQUIRE(std::system(encode.c_str()) == 0);
      const std::vector<uint8_t> bytes = read_bytes(path);
      const auto [samples, sample_rate] = load_buffer(bytes.data(), bytes.size());
      CHECK(sample_rate > 0);
      CHECK(samples.size() > 1000);
    }
  }
}

TEST_CASE("FFmpeg decode skips a damaged packet and keeps the rest of the stream",
          "[audio_io][ffmpeg]") {
  if (std::system("command -v ffmpeg >/dev/null 2>&1") != 0) {
    SKIP("ffmpeg CLI not found on PATH");
  }
  ScratchDir dir;

  struct Fixture {
    const char* name;
    const char* codec_args;
    bool flac;
  };
  const Fixture fixtures[] = {
      {"tone.aac", "-c:a aac -b:a 64k -f adts", false},
      {"tone.flac", "-c:a flac -f flac", true},
  };

  for (const Fixture& fixture : fixtures) {
    DYNAMIC_SECTION(fixture.name) {
      const std::string clean_path = (dir.path / fixture.name).string();
      const std::string encode =
          "ffmpeg -loglevel error -f lavfi -i "
          "anoisesrc=duration=2:sample_rate=22050:amplitude=0.3:seed=7 "
          "-ac 1 " +
          std::string(fixture.codec_args) + " -y '" + clean_path + "'";
      REQUIRE(std::system(encode.c_str()) == 0);
      std::vector<uint8_t> bytes = read_bytes(clean_path);
      REQUIRE(detect_format(bytes.data(), bytes.size()) == AudioFormat::Unknown);

      const size_t clean_samples = std::get<0>(load_buffer(bytes.data(), bytes.size())).size();
      REQUIRE(clean_samples > 22050);

      corrupt_middle(bytes, fixture.flac ? flac_audio_offset(bytes) : 0);
      const std::string damaged_path =
          (dir.path / ("damaged_" + std::string(fixture.name))).string();
      write_bytes(damaged_path, bytes);

      // One damaged region costs a few frames, never the load.
      const size_t min_samples = clean_samples / 2;
      const auto [mono, mono_rate] = load_buffer(bytes.data(), bytes.size());
      CHECK(mono_rate == 22050);
      CHECK(mono.size() >= min_samples);
      for (const float sample : mono) REQUIRE(std::isfinite(sample));
      CHECK(std::get<0>(load_audio(damaged_path)).size() >= min_samples);
      const auto interleaved = load_buffer_interleaved(bytes.data(), bytes.size());
      CHECK(std::get<0>(interleaved).size() >= min_samples);
      CHECK(std::get<0>(load_audio_interleaved(damaged_path)).size() >= min_samples);
      const auto combined = load_audio_input_once(damaged_path, RetainChannels::All);
      CHECK(combined.mono.size() >= min_samples);
      CHECK(combined.interleaved.size() >= min_samples);
    }
  }
}

TEST_CASE("FFmpeg decode with no decodable frame still fails", "[audio_io][ffmpeg]") {
  if (std::system("command -v ffmpeg >/dev/null 2>&1") != 0) {
    SKIP("ffmpeg CLI not found on PATH");
  }
  ScratchDir dir;
  const std::string path = (dir.path / "tone.flac").string();
  const std::string encode =
      "ffmpeg -loglevel error -f lavfi -i "
      "anoisesrc=duration=1:sample_rate=22050:amplitude=0.3:seed=7 "
      "-ac 1 -c:a flac -f flac -y '" +
      path + "'";
  REQUIRE(std::system(encode.c_str()) == 0);
  std::vector<uint8_t> bytes = read_bytes(path);

  // Valid metadata, then zeros: no frame sync anywhere in the audio region.
  const size_t audio_offset = flac_audio_offset(bytes);
  REQUIRE(audio_offset < bytes.size());
  std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(audio_offset), bytes.end(), 0);

  CHECK(thrown_code([&] { load_buffer(bytes.data(), bytes.size()); }) == ErrorCode::DecodeFailed);
  CHECK(thrown_code([&] { load_buffer_interleaved(bytes.data(), bytes.size()); }) ==
        ErrorCode::DecodeFailed);
}

#endif  // SONARE_WITH_FFMPEG
