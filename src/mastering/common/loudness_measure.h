#pragma once

/// @file loudness_measure.h
/// @brief Stateless LUFS / true-peak measurement helpers shared by
///        mastering processors that report loudness fields.
///
/// CLAUDE.md restricts non-`assistant/` `mastering/` modules to
/// `core/ + util/ + rt/`. `mastering/common/loudness_measure` is the single
/// well-defined exception: it is the only `mastering/common/` translation unit
/// allowed to depend on `metering/`, and it exists to keep that dependency
/// from leaking sideways into `mastering/api/`, `mastering/maximizer/`, or
/// `mastering/match/`. All public APIs are stateless and thread-safe — they
/// take `const` audio and return scalars (or a small POD) without touching any
/// shared mutable state.

#include <cstddef>

#include "core/audio.h"

namespace sonare::mastering::common {

/// @brief Default oversample factor for the true-peak meter. Matches
///        `metering::true_peak_db()`'s own default so callers do not have to
///        depend on `metering/true_peak.h` to pick a sensible value.
inline constexpr int kDefaultTruePeakOversample = 4;

/// @brief Combined LUFS / true-peak result. Use this when both numbers are
///        needed in a single pass — it makes the call site less verbose and
///        keeps the metering include hidden in `loudness_measure.cpp`.
struct LufsAndTruePeak {
  float integrated_lufs = 0.0f;
  float true_peak_dbtp = 0.0f;
};

/// @brief Integrated LUFS of @p audio (BS.1770-4 / EBU R128).
/// @details Forwards to `metering::lufs(audio).integrated_lufs`. Returns a
///          non-finite value when the input is below the absolute gate.
float measure_lufs(const Audio& audio);

/// @brief Integrated LUFS of a contiguous mono sample buffer.
/// @param samples Pointer to mono audio (must not be null when @p length > 0).
/// @param length Number of samples in @p samples.
/// @param sample_rate Sample rate in Hz; must be positive.
float measure_lufs(const float* samples, std::size_t length, int sample_rate);

/// @brief Integrated LUFS of an interleaved multi-channel buffer.
/// @param samples Pointer to `frames * channels` interleaved samples.
/// @param frames Number of sample frames.
/// @param channels Channel count; must be positive.
/// @param sample_rate Sample rate in Hz; must be positive.
float measure_lufs_interleaved(const float* samples, std::size_t frames, int channels,
                               int sample_rate);

/// @brief Loudness range (LRA) in LU. Forwards to
///        `metering::lufs(audio).loudness_range`.
float measure_lra(const Audio& audio);

/// @brief Inter-sample true-peak level of @p audio in dB true-peak (dBTP).
/// @param audio Mono input.
/// @param oversample_factor Oversampling ratio; must be >= 1. Defaults to
///        `kDefaultTruePeakOversample` (matches `metering::true_peak_db`).
float measure_true_peak_dbtp(const Audio& audio,
                             int oversample_factor = kDefaultTruePeakOversample);

/// @brief Measures integrated LUFS and true-peak in one call. Useful for
///        result-struct population paths that report both numbers.
LufsAndTruePeak measure_lufs_and_true_peak(const Audio& audio,
                                           int true_peak_oversample = kDefaultTruePeakOversample);

}  // namespace sonare::mastering::common
