#pragma once

/// @file audio_io_ffmpeg.h
/// @brief FFmpeg-based audio loading for M4A/AAC/FLAC/OGG and other formats.
/// @details This header is only meaningful when libsonare is built with
///          @c SONARE_WITH_FFMPEG=ON. The symbols are still declared so that
///          callers can reference them under an @c #ifdef guard, but the
///          implementations only exist in that configuration.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/audio_io.h"

namespace sonare {

/// @brief Loads audio from a memory buffer using FFmpeg (libavformat/libavcodec).
/// @details Decodes the first audio stream of any container/codec supported by
///          the linked FFmpeg build, downmixes to mono float32, and returns the
///          samples at the source sample rate (no resampling is applied here).
/// @param data Pointer to the encoded audio data.
/// @param size Size of the encoded data in bytes.
/// @return Tuple of (mono samples normalized to [-1,1], sample rate).
/// @throws SonareException on any FFmpeg or decoding error.
AudioLoadResult load_buffer_ffmpeg(const uint8_t* data, size_t size);

/// @brief Loads audio from a memory buffer while preserving source channels.
/// @details The decoded samples remain frame-interleaved and retain the first
///          audio stream's channel layout/order. No resampling is applied.
/// @param data Pointer to the encoded audio data.
/// @param size Size of the encoded audio data in bytes.
/// @return Tuple of (interleaved samples normalized to [-1,1], sample rate,
///         source channel count).
/// @throws SonareException with @c DecodeFailed if the decoded stream changes
///         channel layout or count after its first output frame.
/// @throws SonareException on any FFmpeg or decoding error.
InterleavedAudioLoadResult load_buffer_ffmpeg_interleaved(const uint8_t* data, size_t size);

/// @brief Probes the first audio stream's source channel count without decoding.
int probe_channels_ffmpeg(const uint8_t* data, size_t size);

/// @brief Reads raw format and first-audio-stream tags without decoding PCM.
std::vector<std::pair<std::string, std::string>> read_raw_audio_tags_ffmpeg(const uint8_t* data,
                                                                            size_t size);

/// @brief Returns whether FFmpeg has both an encoder and muxer for @p format.
bool ffmpeg_audio_encode_available(std::string_view format) noexcept;

/// @brief Encodes channel-major float audio into an FFmpeg-supported container.
std::vector<uint8_t> encode_audio_ffmpeg(const float* planar, size_t frames, int num_channels,
                                         int sample_rate, std::string_view format, int bit_depth,
                                         float quality,
                                         const std::map<std::string, std::string>& tags);

}  // namespace sonare
