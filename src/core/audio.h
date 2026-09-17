#pragma once

/// @file audio.h
/// @brief Audio buffer class with efficient slicing and shared ownership.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "util/resource_limits.h"

namespace sonare {

/// @brief One channel of audio, with shared ownership and zero-copy slicing.
/// @details This models a single channel, not a multi-channel signal: samples
/// are normalized to [-1, 1] and carry no channel count. Multi-channel callers
/// hold a channel container of their own and wrap each channel in an Audio to
/// reach the analysis and repair paths, which is what the mastering chain does.
/// Slices share the underlying buffer, which is immutable, so copying an Audio
/// costs a reference count rather than the samples.
class Audio {
 public:
  /// @brief Default constructor creates an empty Audio.
  Audio();

  /// @brief Creates Audio from existing samples.
  /// @param samples Pointer to sample data (will be copied)
  /// @param size Number of samples
  /// @param sample_rate Sample rate in Hz
  /// @return Audio object
  static Audio from_buffer(const float* samples, size_t size, int sample_rate);

  /// @brief Creates Audio from a vector of samples.
  /// @param samples Vector of samples (will be moved)
  /// @param sample_rate Sample rate in Hz
  /// @return Audio object
  static Audio from_vector(std::vector<float> samples, int sample_rate);

  /// @brief Loads Audio from a file.
  /// @details The built-in decoders cover WAV (PCM, IEEE float, A-law, mu-law,
  ///          MS/IMA ADPCM) and MP3. A build with FFmpeg enabled also accepts
  ///          every container and codec FFmpeg handles, including a RIFF/WAVE
  ///          carrying a codec the built-in WAV decoder does not implement;
  ///          without FFmpeg those fail rather than falling back.
  /// @param path Path to audio file
  /// @return Audio object holding a buffer that satisfies the offline-analysis
  ///         policy below, so the decoded path accepts and rejects exactly what
  ///         the raw-buffer entry points do.
  /// @throws SonareException on file not found or decode error, including
  ///         DecodeFailed for an empty or non-finite decode result and
  ///         InvalidFormat for a declared sample rate outside
  ///         [kMinAudioSampleRate, kMaxAudioSampleRate].
  static Audio from_file(const std::string& path);

  /// @brief Loads one channel of a file without downmixing the others into it.
  /// @details Same format set and same decoded-buffer contract as
  ///          @ref from_file; what differs is only which samples reach the
  ///          handle. An Audio models one channel, so a caller that wants a
  ///          stereo file's two channels loads it twice and holds the pair — the
  ///          shape the mastering chain's stereo entry points already take.
  /// @param path Path to audio file
  /// @param channel_index Zero-based source channel.
  /// @return Audio holding that channel alone.
  /// @throws SonareException as @ref from_file does, plus InvalidParameter when
  ///         @p channel_index names no channel the file has.
  static Audio from_file_channel(const std::string& path, int channel_index);

  /// @brief Loads Audio from memory buffer.
  /// @details Accepts the same build-dependent format set as from_file.
  /// @param data Pointer to audio data
  /// @param size Size of data in bytes
  /// @return Audio object under the same decoded-buffer contract as from_file.
  /// @throws SonareException on decode error (see from_file).
  static Audio from_memory(const uint8_t* data, size_t size);

  /// @brief Returns pointer to sample data.
  const float* data() const;

  /// @brief Returns number of samples.
  size_t size() const;

  /// @brief Returns sample rate in Hz.
  int sample_rate() const { return sample_rate_; }

  /// @brief Returns duration in seconds.
  float duration() const;

  /// @brief Returns true if audio is empty.
  bool empty() const { return size() == 0; }

  /// @brief Creates a slice of this audio (shared buffer, zero-copy).
  /// @param start_time Start time in seconds
  /// @param end_time End time in seconds (negative means end of audio)
  /// @return New Audio object sharing the same buffer
  /// @note Time bounds are converted to sample indices by truncation toward zero
  ///       (floor for the non-negative times this accepts), not rounding: the
  ///       boundary sample is `floor(time * sample_rate)`. A negative start_time
  ///       clamps to 0. This truncation is the contract — surfaces must not
  ///       "fix" it to round, which would diverge the slice bounds across
  ///       bindings.
  Audio slice(float start_time, float end_time = -1.0f) const;

  /// @brief Creates a slice by sample indices (shared buffer, zero-copy).
  /// @param start_sample Start sample index
  /// @param end_sample End sample index (negative means end of audio)
  /// @return New Audio object sharing the same buffer
  Audio slice_samples(size_t start_sample, size_t end_sample = static_cast<size_t>(-1)) const;

  /// @brief Access sample by index.
  float operator[](size_t index) const;

  /// @brief Iterator support.
  /// @details A default-constructed Audio has data()==nullptr, but its iterator
  ///          pair uses a stable non-null sentinel so pointer arithmetic and
  ///          subtraction on the empty range remain defined.
  const float* begin() const {
    static constexpr float kEmptySentinel = 0.0f;
    const float* samples = data();
    return samples != nullptr ? samples : &kEmptySentinel;
  }
  const float* end() const { return begin() + size(); }

 private:
  /// @brief Private constructor for creating slices.
  Audio(std::shared_ptr<const std::vector<float>> buffer, size_t offset, size_t length,
        int sample_rate);

  std::shared_ptr<const std::vector<float>> buffer_;
  size_t offset_;
  size_t length_;
  int sample_rate_;
};

/// @name Offline-analysis input limits
/// Shared sample-rate / buffer-size bounds enforced on every offline-analysis
/// entry point. The C ABI (validate_audio_params) and the WASM bindings — which
/// bypass the C-ABI translation unit and call the C++ core directly — both
/// funnel through validate_offline_audio_input, so the empty-input / range /
/// finite policy is identical on every surface.
/// @{
inline constexpr int kMinAudioSampleRate = 8000;
inline constexpr int kMaxAudioSampleRate = 384000;
inline constexpr std::size_t kMaxAudioBufferSize = resource::kMaxOfflineAudioSamples;

/// @brief The O(1) half of the policy: the rules that describe a buffer's extent
///        rather than its contents.
/// @details Exposed for a surface that cannot hand over a pointer to the whole
///          buffer -- the WASM bindings copy only the window a windowed call
///          reads, so the emptiness, @p sample_rate and size rules still have to
///          be asked about the whole length, and only the finiteness scan
///          narrows. Both validators below are expressed in terms of this, so
///          there is one statement of the policy rather than a second copy.
/// @throws SonareException(InvalidParameter) for an empty buffer, a
///         @p sample_rate outside [kMinAudioSampleRate, kMaxAudioSampleRate], or
///         a @p length above kMaxAudioBufferSize.
void validate_offline_audio_extent(std::size_t length, int sample_rate);

/// @brief Validates an offline-analysis audio buffer (single source of truth).
/// @param samples       Pointer to mono/interleaved float sample data.
/// @param length        Number of float samples.
/// @param sample_rate   Sample rate in Hz.
/// @throws SonareException(InvalidParameter) for a null/empty buffer, a
///         @p sample_rate outside [kMinAudioSampleRate, kMaxAudioSampleRate], a
///         @p length above kMaxAudioBufferSize, or any non-finite sample. Empty
///         audio is never a valid zero-length analysis, matching the C ABI.
void validate_offline_audio_input(const float* samples, std::size_t length, int sample_rate);

/// @brief Validates an offline-analysis buffer whose analysis reads one window of it.
/// @param samples       Pointer to mono/interleaved float sample data.
/// @param length        Number of float samples in the whole buffer.
/// @param sample_rate   Sample rate in Hz.
/// @param scan_offset   First sample index of the analysed window; clamped to @p length.
/// @param scan_count    Window length in samples; clamped to the end of the buffer.
/// @throws SonareException(InvalidParameter) on the same null/empty, @p sample_rate
///         and @p length conditions as @ref validate_offline_audio_input, and on a
///         non-finite sample within [@p scan_offset, @p scan_offset + @p scan_count).
///         Samples outside that window are not read, so their value is not a
///         precondition of the call.
void validate_offline_audio_window(const float* samples, std::size_t length, int sample_rate,
                                   std::size_t scan_offset, std::size_t scan_count);
/// @}

}  // namespace sonare
