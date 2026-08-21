#pragma once

/// @file audio_io.h
/// @brief Audio file loading utilities using dr_wav and minimp3.
///
/// Every mono loader in this header folds its source down with one rule, chosen
/// by the source channel layout: layouts the speaker model covers (mono, stereo,
/// 5.1, 7.1) go through sonare::mixing::downmix(), so the center and surround
/// feeds enter the front pair at -3 dB and the LFE plane is dropped, per ITU-R
/// BS.775. Stereo therefore stays the plain average of the pair. Channel counts
/// outside the model (quad, LCR, anything above 7.1) have no speaker assignment
/// to weight, so they fall back to the unweighted mean of every plane.

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "core/channel_layout.h"
#include "util/resource_limits.h"

namespace sonare {

/// @brief Detected audio format.
enum class AudioFormat {
  Unknown,
  WAV,
  MP3,
};

/// @brief Result of audio loading: samples and sample rate.
using AudioLoadResult = std::tuple<std::vector<float>, int>;

/// @brief Result of an interleaved audio load: samples, sample rate, and channels.
/// @details Unlike @ref AudioLoadResult, samples retain their source channel
///          layout in frame-interleaved order.
using InterleavedAudioLoadResult = std::tuple<std::vector<float>, int, int>;

/// @brief Options for audio loading.
struct AudioLoadOptions {
  /// @brief Maximum file size in bytes (0 = no limit).
  /// @details Default is 500MB. Set to 0 to disable size checking.
  size_t max_file_size = resource::kMaxAudioFileBytes;
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

/// @brief Loads audio without downmixing its channels.
/// @details WAV/MP3 use the built-in decoders. When FFmpeg support is enabled,
///          containers/codecs handled by FFmpeg (including FLAC and M4A) are
///          decoded with their source channel order preserved.
/// @return Frame-interleaved normalized samples, sample rate, and source channel count.
/// @throws SonareException on file not found, unsupported format, or decode error.
InterleavedAudioLoadResult load_audio_interleaved(
    const std::string& path, const AudioLoadOptions& options = kDefaultLoadOptions);

/// @brief Returns the source channel count without converting the signal to mono.
/// @details WAV/MP3 are inspected by the built-in decoders. When FFmpeg support
///          is enabled, containers/codecs supported by FFmpeg (including FLAC,
///          OGG, and M4A/AAC) are inspected through FFmpeg as well.
/// @return A positive source channel count on success, or 0 for an unrecognized
///         format when FFmpeg support is disabled.
/// @throws SonareException when the file cannot be opened or inspected.
int audio_channel_count(const std::string& path,
                        const AudioLoadOptions& options = kDefaultLoadOptions);

/// @brief Loads audio from memory buffer (auto-detect format).
/// @details Dispatch is by codec, not container: a RIFF/WAVE declaring a codec
///          the built-in decoder does not implement goes to FFmpeg where the
///          build has it, and fails with DecodeFailed where it does not.
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
