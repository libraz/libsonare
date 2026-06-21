#pragma once

/// @file audio_io.h
/// @brief Audio file loading utilities using dr_wav and minimp3.

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "core/channel_layout.h"

namespace sonare {

/// @brief Detected audio format.
enum class AudioFormat {
  Unknown,
  WAV,
  MP3,
};

/// @brief Result of audio loading: samples and sample rate.
using AudioLoadResult = std::tuple<std::vector<float>, int>;

/// @brief Result of multichannel audio loading: deinterleaved channels plus the
///        sample rate. Each inner vector is one channel of equal length.
struct AudioLoadResultMC {
  std::vector<std::vector<float>> channels;
  int sample_rate = 0;
  /// @brief True when @ref channels reflects the source's native channel layout.
  ///        False when the source was downmixed before deinterleaving (the
  ///        FFmpeg fallback path returns a single, already-mono channel), so a
  ///        @c channels.size() of 1 is a downmix rather than a mono source.
  bool native_channels = true;
};

/// @brief Options for audio loading.
struct AudioLoadOptions {
  /// @brief Maximum file size in bytes (0 = no limit).
  /// @details Default is 500MB. Set to 0 to disable size checking.
  size_t max_file_size = 500 * 1024 * 1024;
};

/// @brief Default audio load options.
inline const AudioLoadOptions kDefaultLoadOptions{};

/// @brief Detects audio format from buffer header.
/// @param data Pointer to audio data
/// @param size Size of data in bytes
/// @return Detected audio format
AudioFormat detect_format(const uint8_t* data, size_t size);

/// @brief Loads WAV file from disk.
/// @param path Path to WAV file
/// @return Tuple of (mono samples normalized to [-1,1], sample rate)
/// @throws SonareException on file not found or decode error
AudioLoadResult load_wav(const std::string& path);

/// @brief Loads MP3 file from disk.
/// @param path Path to MP3 file
/// @return Tuple of (mono samples normalized to [-1,1], sample rate)
/// @throws SonareException on file not found or decode error
AudioLoadResult load_mp3(const std::string& path);

/// @brief Loads WAV from memory buffer.
/// @param data Pointer to WAV data
/// @param size Size of data in bytes
/// @return Tuple of (mono samples normalized to [-1,1], sample rate)
/// @throws SonareException on decode error
AudioLoadResult load_buffer_wav(const uint8_t* data, size_t size);

/// @brief Loads MP3 from memory buffer.
/// @param data Pointer to MP3 data
/// @param size Size of data in bytes
/// @return Tuple of (mono samples normalized to [-1,1], sample rate)
/// @throws SonareException on decode error
AudioLoadResult load_buffer_mp3(const uint8_t* data, size_t size);

/// @brief Loads audio file (auto-detect format).
/// @param path Path to audio file
/// @param options Loading options (max file size, etc.)
/// @return Tuple of (mono samples normalized to [-1,1], sample rate)
/// @throws SonareException on file not found, unknown format, file too large, or decode error
AudioLoadResult load_audio(const std::string& path,
                           const AudioLoadOptions& options = kDefaultLoadOptions);

/// @brief Loads an audio file preserving its channels (auto-detect format).
/// @details Unlike @ref load_audio (which downmixes to mono), this returns each
///          source channel deinterleaved. WAV and MP3 are decoded to their
///          native channel count; other formats decoded via the FFmpeg fallback
///          are returned as a single (already downmixed) channel.
/// @param path Path to audio file
/// @param options Loading options (max file size, etc.)
/// @return Deinterleaved channels (each normalized to [-1,1]) plus sample rate
/// @throws SonareException on file not found, unknown format, too large, or decode error
AudioLoadResultMC load_audio_multichannel(const std::string& path,
                                          const AudioLoadOptions& options = kDefaultLoadOptions);

/// @brief Loads audio from memory buffer (auto-detect format).
/// @param data Pointer to audio data
/// @param size Size of data in bytes
/// @return Tuple of (mono samples normalized to [-1,1], sample rate)
/// @throws SonareException on unknown format or decode error
AudioLoadResult load_buffer(const uint8_t* data, size_t size);

/// @brief Saves audio samples to a WAV file.
/// @param path Output file path
/// @param samples Audio samples (mono, normalized to [-1,1])
/// @param sample_rate Sample rate in Hz
/// @param bits_per_sample Bit depth (16 or 24, default 16)
/// @throws SonareException on write error
void save_wav(const std::string& path, const float* samples, size_t n_samples, int sample_rate,
              int bits_per_sample = 16);

/// @brief Saves audio samples to a WAV file.
/// @param path Output file path
/// @param samples Audio samples (mono, normalized to [-1,1])
/// @param sample_rate Sample rate in Hz
/// @param bits_per_sample Bit depth (16 or 24, default 16)
/// @throws SonareException on write error
void save_wav(const std::string& path, const std::vector<float>& samples, int sample_rate,
              int bits_per_sample = 16);

/// @brief Saves interleaved multichannel audio to a WAV file.
/// @details Mono/stereo are written as plain WAVE_FORMAT_PCM (bit-identical to
///          the mono/stereo helpers). Surround layouts (channel_count > 2) are
///          written with a hand-built WAVE_FORMAT_EXTENSIBLE header carrying the
///          layout's speaker mask, since the pinned dr_wav cannot emit
///          EXTENSIBLE. Plane/interleave order follows the canonical
///          ChannelLayout order.
/// @param path Output file path
/// @param interleaved Interleaved samples (n_frames * channel_count), [-1,1]
/// @param n_frames Number of frames (samples per channel)
/// @param channel_count Number of channels; must equal channel_count(layout)
/// @param layout Speaker layout (drives the EXTENSIBLE channel mask)
/// @param sample_rate Sample rate in Hz
/// @param bits_per_sample Bit depth (16 or 24, default 16)
/// @throws SonareException on invalid arguments or write error
void save_wav_multichannel(const std::string& path, const float* interleaved, size_t n_frames,
                           int channel_count, ChannelLayout layout, int sample_rate,
                           int bits_per_sample = 16);

}  // namespace sonare
