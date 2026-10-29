#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include "core/audio_io.h"
#include "util/exception.h"

namespace {

using sonare::AudioFormat;
using sonare::ChannelLayout;
using sonare::ErrorCode;
using sonare::SonareException;

template <typename Operation>
ErrorCode capture_error_code(const Operation& operation) {
  try {
    operation();
  } catch (const SonareException& error) {
    return error.code();
  }
  FAIL("Expected SonareException");
  return ErrorCode::InvalidParameter;
}

std::filesystem::path make_scratch_dir() {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto base = std::filesystem::temp_directory_path();
  for (int suffix = 0; suffix < 1024; ++suffix) {
    const auto directory = base / ("sonare_audio_header_domain_" + std::to_string(nonce) + "_" +
                                   std::to_string(suffix));
    if (std::filesystem::create_directory(directory)) return directory;
  }
  FAIL("no free scratch directory under the system temp path");
  return {};
}

struct ScratchDirGuard {
  std::filesystem::path path;
  ~ScratchDirGuard() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

std::vector<uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  REQUIRE(input.good());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

uint16_t read_le16(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset]) |
                               (static_cast<uint16_t>(bytes[offset + 1]) << 8));
}

uint32_t read_le32(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

}  // namespace

TEST_CASE("audio format sniffing treats null pointers as unknown", "[audio_io][domain]") {
  CHECK(sonare::detect_format(nullptr, 0) == AudioFormat::Unknown);
  CHECK(sonare::detect_format(nullptr, 11) == AudioFormat::Unknown);
  CHECK(sonare::detect_format(nullptr, 12) == AudioFormat::Unknown);
  CHECK(sonare::detect_format(nullptr, std::numeric_limits<size_t>::max()) == AudioFormat::Unknown);
}

TEST_CASE("buffer loaders reject null nonempty input and retain empty-input policy",
          "[audio_io][domain]") {
  for (const size_t size : {size_t{11}, size_t{12}}) {
    CHECK(capture_error_code([&] { (void)sonare::load_buffer(nullptr, size); }) ==
          ErrorCode::InvalidParameter);
    CHECK(capture_error_code([&] { (void)sonare::load_buffer_interleaved(nullptr, size); }) ==
          ErrorCode::InvalidParameter);
    CHECK(capture_error_code([&] { (void)sonare::load_buffer_wav(nullptr, size); }) ==
          ErrorCode::InvalidParameter);
    CHECK(capture_error_code([&] { (void)sonare::load_buffer_mp3(nullptr, size); }) ==
          ErrorCode::InvalidParameter);
  }

  const uint8_t empty_storage = 0;
  const uint8_t* const empty_data = &empty_storage;
  CHECK(capture_error_code([&] { (void)sonare::load_buffer(nullptr, 0); }) ==
        capture_error_code([&] { (void)sonare::load_buffer(empty_data, 0); }));
  CHECK(capture_error_code([&] { (void)sonare::load_buffer_interleaved(nullptr, 0); }) ==
        capture_error_code([&] { (void)sonare::load_buffer_interleaved(empty_data, 0); }));
  CHECK(capture_error_code([&] { (void)sonare::load_buffer_wav(nullptr, 0); }) ==
        capture_error_code([&] { (void)sonare::load_buffer_wav(empty_data, 0); }));
  CHECK(capture_error_code([&] { (void)sonare::load_buffer_mp3(nullptr, 0); }) ==
        capture_error_code([&] { (void)sonare::load_buffer_mp3(empty_data, 0); }));
}

TEST_CASE("WAV byte rate is validated in its 32-bit header domain", "[audio_io][domain]") {
  struct LayoutCase {
    ChannelLayout layout;
    int channels;
  };
  constexpr std::array<LayoutCase, 4> layouts = {{{ChannelLayout::Mono, 1},
                                                  {ChannelLayout::Stereo, 2},
                                                  {ChannelLayout::FivePointOne, 6},
                                                  {ChannelLayout::SevenPointOne, 8}}};
  const auto directory = make_scratch_dir();
  const ScratchDirGuard cleanup{directory};

  for (const auto& layout : layouts) {
    for (const int bits_per_sample : {16, 24}) {
      const uint64_t block_align =
          static_cast<uint64_t>(layout.channels) * static_cast<uint64_t>(bits_per_sample / 8);
      const uint64_t max_rate = std::numeric_limits<uint32_t>::max() / block_align;
      REQUIRE(max_rate <= static_cast<uint64_t>(std::numeric_limits<int>::max()));
      const auto path = directory / ("valid_" + std::to_string(layout.channels) + "_" +
                                     std::to_string(bits_per_sample) + ".wav");
      const std::vector<float> samples(static_cast<size_t>(layout.channels), 0.0f);

      sonare::save_wav_multichannel(path.string(), samples.data(), 1, layout.channels,
                                    layout.layout, static_cast<int>(max_rate), bits_per_sample);
      const auto bytes = read_bytes(path);
      REQUIRE(read_le16(bytes, 22) == layout.channels);
      REQUIRE(read_le32(bytes, 24) == max_rate);
      REQUIRE(read_le32(bytes, 28) == max_rate * block_align);
      REQUIRE(read_le16(bytes, 32) == block_align);

      const auto below_path = directory / ("valid_below_" + std::to_string(layout.channels) + "_" +
                                           std::to_string(bits_per_sample) + ".wav");
      sonare::save_wav_multichannel(below_path.string(), samples.data(), 1, layout.channels,
                                    layout.layout, static_cast<int>(max_rate - 1), bits_per_sample);
      const auto below_bytes = read_bytes(below_path);
      REQUIRE(read_le32(below_bytes, 24) == max_rate - 1);
      REQUIRE(read_le32(below_bytes, 28) == (max_rate - 1) * block_align);

      const uint64_t first_invalid_rate = max_rate + 1;
      if (first_invalid_rate <= static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        const auto new_path = directory / ("rejected_new_" + std::to_string(layout.channels) + "_" +
                                           std::to_string(bits_per_sample) + ".wav");
        size_t non_finite_samples = 37;
        const std::vector<float> non_finite_samples_data(static_cast<size_t>(layout.channels),
                                                         std::numeric_limits<float>::quiet_NaN());
        REQUIRE(capture_error_code([&] {
                  sonare::save_wav_multichannel(new_path.string(), non_finite_samples_data.data(),
                                                1, layout.channels, layout.layout,
                                                static_cast<int>(first_invalid_rate),
                                                bits_per_sample, &non_finite_samples);
                }) == ErrorCode::InvalidParameter);
        REQUIRE_FALSE(std::filesystem::exists(new_path));
        REQUIRE(non_finite_samples == 37);

        const auto sentinel_path = directory / ("sentinel_" + std::to_string(layout.channels) +
                                                "_" + std::to_string(bits_per_sample) + ".wav");
        const std::vector<uint8_t> sentinel = {0x53, 0x4F, 0x4E, 0x41, 0x52, 0x45};
        {
          std::ofstream output(sentinel_path, std::ios::binary);
          REQUIRE(output.good());
          output.write(reinterpret_cast<const char*>(sentinel.data()),
                       static_cast<std::streamsize>(sentinel.size()));
          REQUIRE(output.good());
        }
        non_finite_samples = 41;
        REQUIRE(capture_error_code([&] {
                  sonare::save_wav_multichannel(sentinel_path.string(),
                                                non_finite_samples_data.data(), 1, layout.channels,
                                                layout.layout, static_cast<int>(first_invalid_rate),
                                                bits_per_sample, &non_finite_samples);
                }) == ErrorCode::InvalidParameter);
        REQUIRE(read_bytes(sentinel_path) == sentinel);
        REQUIRE(non_finite_samples == 41);
      }
    }
  }

  // Mono 16-bit at INT_MAX is valid: 2 * INT_MAX still fits uint32_t exactly
  // below its maximum. Exercise the separate mono writer entry point as well.
  const auto mono_path = directory / "mono_int_max.wav";
  const std::array<float, 1> mono_sample = {0.0f};
  sonare::save_wav(mono_path.string(), mono_sample.data(), mono_sample.size(),
                   std::numeric_limits<int>::max(), 16);
  const auto mono_bytes = read_bytes(mono_path);
  REQUIRE(read_le32(mono_bytes, 24) == static_cast<uint32_t>(std::numeric_limits<int>::max()));
  REQUIRE(read_le32(mono_bytes, 28) == 4294967294u);

  // The mono writer performs the same pre-pack validation. Its rejected 24-bit
  // rate must leave both a new destination and an existing destination alone.
  const uint64_t mono_24_max_rate = std::numeric_limits<uint32_t>::max() / 3;
  const auto rejected_mono_path = directory / "mono_24_overflow.wav";
  const float non_finite_sample = std::numeric_limits<float>::quiet_NaN();
  size_t non_finite_samples = 43;
  REQUIRE(capture_error_code([&] {
            sonare::save_wav(rejected_mono_path.string(), &non_finite_sample, 1,
                             static_cast<int>(mono_24_max_rate + 1), 24, &non_finite_samples);
          }) == ErrorCode::InvalidParameter);
  REQUIRE_FALSE(std::filesystem::exists(rejected_mono_path));
  REQUIRE(non_finite_samples == 43);

  const auto rejected_mono_sentinel_path = directory / "mono_24_sentinel.wav";
  const std::vector<uint8_t> mono_sentinel = {0x4D, 0x4F, 0x4E, 0x4F};
  {
    std::ofstream output(rejected_mono_sentinel_path, std::ios::binary);
    REQUIRE(output.good());
    output.write(reinterpret_cast<const char*>(mono_sentinel.data()),
                 static_cast<std::streamsize>(mono_sentinel.size()));
    REQUIRE(output.good());
  }
  non_finite_samples = 47;
  REQUIRE(capture_error_code([&] {
            sonare::save_wav(rejected_mono_sentinel_path.string(), &non_finite_sample, 1,
                             static_cast<int>(mono_24_max_rate + 1), 24, &non_finite_samples);
          }) == ErrorCode::InvalidParameter);
  REQUIRE(read_bytes(rejected_mono_sentinel_path) == mono_sentinel);
  REQUIRE(non_finite_samples == 47);
}

TEST_CASE("mono and multichannel mono WAV headers remain byte-identical", "[audio_io][domain]") {
  const auto directory = make_scratch_dir();
  const ScratchDirGuard cleanup{directory};
  const std::array<float, 4> samples = {0.125f, -0.25f, 0.0f, 0.375f};
  for (const int bits_per_sample : {16, 24}) {
    const auto mono_path = directory / ("mono_" + std::to_string(bits_per_sample) + ".wav");
    const auto layout_path = directory / ("layout_" + std::to_string(bits_per_sample) + ".wav");
    sonare::save_wav(mono_path.string(), samples.data(), samples.size(), 48000, bits_per_sample);
    sonare::save_wav_multichannel(layout_path.string(), samples.data(), samples.size(), 1,
                                  ChannelLayout::Mono, 48000, bits_per_sample);
    CHECK(read_bytes(mono_path) == read_bytes(layout_path));
  }
}
