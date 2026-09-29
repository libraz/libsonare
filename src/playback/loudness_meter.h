#pragma once

/// @file loudness_meter.h
/// @brief Integrated loudness of multichannel program material, pushed in
///        non-empty chunks of any length, for the renderer's loudness alignment.
///
/// Runs `mixing::MeterProcessor` (LUFS on, true peak off) on 4096-frame
/// deinterleaved blocks with the BS.1770 channel weights of the channel count.

#include <cstddef>
#include <memory>

namespace sonare::playback {

/// Internal deinterleave block length.
inline constexpr int kPlaybackLoudnessBlockFrames = 4096;

class PlaybackLoudnessMeter {
 public:
  /// @p channels is 1, 2, 6 or 8.
  /// @throws SonareException(InvalidParameter) on another channel count or a
  ///         non-positive sample rate.
  PlaybackLoudnessMeter(int channels, int sample_rate);
  ~PlaybackLoudnessMeter();
  PlaybackLoudnessMeter(const PlaybackLoudnessMeter&) = delete;
  PlaybackLoudnessMeter& operator=(const PlaybackLoudnessMeter&) = delete;

  /// Feeds @p frames interleaved frames.
  /// @throws SonareException(InvalidParameter) if @p frames is 0, @p samples
  ///         is null, or any sample is non-finite.
  void push_interleaved(const float* samples, size_t frames);
  /// Integrated loudness of everything pushed so far, in LUFS.
  float integrated_lufs() const;
  int channels() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sonare::playback
