#pragma once

/// @file audio_io_ffmpeg.h
/// @brief FFmpeg-based audio loading for M4A/AAC/FLAC/OGG and other formats.
/// @details This header is only meaningful when libsonare is built with
///          @c SONARE_WITH_FFMPEG=ON. The symbols are still declared so that
///          callers can reference them under an @c #ifdef guard, but the
///          implementations only exist in that configuration.

#include <cstddef>
#include <cstdint>
#include <string>

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

}  // namespace sonare
