#pragma once

/// @file realtime_limits.h
/// @brief The sample-rate and block-size ceilings every streaming / realtime create or prepare
///        entry admits against.
/// @details One table, so the stream analyzer, voice changer, mixer and engine answer the same
///          bad value with the same refusal; a size past a ceiling is refused before anything is
///          allocated.

#include <cmath>

#include "core/audio.h"

namespace sonare {

/// @brief Largest accepted per-call block, in frames.
inline constexpr int kMaxRealtimeBlockSize = 1 << 20;

/// @brief True when @p hz is finite and in [kMinAudioSampleRate, kMaxAudioSampleRate].
inline bool is_valid_realtime_sample_rate(double hz) noexcept {
  return std::isfinite(hz) && hz >= kMinAudioSampleRate && hz <= kMaxAudioSampleRate;
}

/// @brief True when @p frames is in [1, kMaxRealtimeBlockSize].
constexpr bool is_valid_realtime_block_size(int frames) noexcept {
  return frames >= 1 && frames <= kMaxRealtimeBlockSize;
}

}  // namespace sonare
