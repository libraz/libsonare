#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/audio_tags.h"

namespace sonare {

struct AudioEncodeOptions {
  std::string format = "wav";
  std::optional<int> bit_depth;
  std::optional<float> quality;
  AudioTags tags;
};

/// Encodes channel-major samples to an in-memory audio file.
std::vector<uint8_t> encode_audio(const float* samples, std::size_t frames, int num_channels,
                                  int sample_rate, const AudioEncodeOptions& options = {});

inline constexpr std::array<std::string_view, 5> kOptionalAudioEncodeFormats = {
    "flac", "mp3", "ogg", "opus", "m4a"};

bool audio_encode_format_available(std::string_view format) noexcept;

namespace detail {

[[noreturn]] void refuse_unavailable_audio_encoder();

std::vector<uint8_t> encode_wav_planar(const float* samples, std::size_t frames, int num_channels,
                                       int sample_rate, int bit_depth, const AudioTags& tags);

}  // namespace detail
}  // namespace sonare
