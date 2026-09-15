#pragma once

/// @file loudness_measure.h
/// @brief Stateless LUFS / true-peak measurement helpers shared by
///        mastering processors that report loudness fields.
///
/// The layering rule restricts non-`assistant/` `mastering/` modules to
/// `core/ + util/ + rt/`. `mastering/common/loudness_measure` is the single
/// well-defined exception: it is the only `mastering/common/` translation unit
/// allowed to depend on `metering/`, and it exists to keep that dependency
/// from leaking sideways into `mastering/api/`, `mastering/maximizer/`, or
/// `mastering/match/`. All public APIs are stateless and thread-safe — they
/// take `const` audio and return scalars (or a small POD) without touching any
/// shared mutable state.

#include <cstddef>
#include <vector>

#include "core/audio.h"

namespace sonare::mastering::common {

/// @brief Default oversample factor for the true-peak meter. Matches
///        `metering::true_peak_db()`'s own default so callers do not have to
///        depend on `metering/true_peak.h` to pick a sensible value.
inline constexpr int kDefaultTruePeakOversample = 4;

/// @brief Hop between consecutive elements of either loudness series, in
///        seconds. Mirrors the value ITU-R BS.1770-4 / EBU R128 fixes, so
///        callers do not depend on `metering/lufs.h`; pinned in the .cpp.
inline constexpr float kLoudnessSeriesHopSeconds = 0.1f;

/// @brief Momentary measurement window (BS.1770-4 Annex 2), in seconds.
inline constexpr float kMomentaryWindowSeconds = 0.4f;

/// @brief Short-term measurement window (EBU R128), in seconds.
inline constexpr float kShortTermWindowSeconds = 3.0f;

/// @brief Index offset aligning the two series: `short_term_lufs[j]` ends at the
///        same frame as `momentary_lufs[j + kShortTermSeriesLead]`. That is
///        (kShortTermWindowSeconds - kMomentaryWindowSeconds) over the shared hop.
inline constexpr std::size_t kShortTermSeriesLead = 26;

/// @brief Combined LUFS / true-peak result. Use this when both numbers are
///        needed in a single pass — it makes the call site less verbose and
///        keeps the metering include hidden in `loudness_measure.cpp`.
struct LufsAndTruePeak {
  float integrated_lufs = 0.0f;
  float true_peak_dbtp = 0.0f;
};

/// @brief Full loudness summary used by the mastering-chain report.
///
/// This is intentionally a thin view of the existing offline EBU R128 meter;
/// it adds no DSP algorithm or state of its own.
struct LoudnessSummary {
  float integrated_lufs = 0.0f;
  float max_momentary_lufs = 0.0f;
  float max_short_term_lufs = 0.0f;
  float true_peak_dbtp = 0.0f;
  float loudness_range = 0.0f;
};

/// @brief The per-block series the scalar loudness meters are reduced from.
///
/// Both series advance by @ref kLoudnessSeriesHopSeconds but do not share an
/// origin: only complete windows are emitted, so element 0 of each ends at its
/// own window length. @ref kShortTermSeriesLead is the resulting index offset,
/// and reading element `k` of both as the same instant is the mistake this
/// field pair invites.
///
/// A series is empty when the input is shorter than its window - under 0.4 s for
/// @ref momentary_lufs, under 3 s for @ref short_term_lufs. That is the meter's
/// "no measurement", not a value taken over a sub-spec window. A fully silent
/// block is `-inf`, the value the scalar meters also report for silence.
struct LoudnessSeries {
  std::vector<float> momentary_lufs;
  std::vector<float> short_term_lufs;
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

/// @brief Loudness range (LRA) in LU of an interleaved multi-channel buffer.
/// @details Forwards to `metering::lufs_interleaved(...).loudness_range`, which
///          applies BS.1770 channel summing. Prefer this over downmixing to mono
///          and calling `measure_lra`: a phase-cancelling `0.5*(L+R)` downmix
///          collapses the loudness range of wide / out-of-phase stereo material.
/// @param samples Pointer to `frames * channels` interleaved samples.
/// @param frames Number of sample frames.
/// @param channels Channel count; must be positive.
/// @param sample_rate Sample rate in Hz; must be positive.
float measure_lra_interleaved(const float* samples, std::size_t frames, int channels,
                              int sample_rate);

/// @brief Inter-sample true-peak level of @p audio in dB true-peak (dBTP).
/// @param audio Mono input.
/// @param oversample_factor Oversampling ratio; must be >= 1. Defaults to
///        `kDefaultTruePeakOversample` (matches `metering::true_peak_db`).
float measure_true_peak_dbtp(const Audio& audio,
                             int oversample_factor = kDefaultTruePeakOversample);

/// @brief Stereo true peak in dBTP read straight from planar channel pointers.
/// @details Same value as taking the maximum of @ref measure_true_peak_dbtp over
///          the two channels, without the two track-length `Audio` copies that
///          wrapping them would cost. The maximum is taken in the linear domain
///          and converted once, which is equivalent because the conversion is
///          monotonic and both channels share the same silence floor.
/// @param left Pointer to the left channel (must not be null when @p frames > 0).
/// @param right Pointer to the right channel (must not be null when @p frames > 0).
/// @param frames Number of sample frames per channel.
/// @param oversample_factor Oversampling ratio; must be >= 1.
float measure_true_peak_dbtp_stereo_planar(const float* left, const float* right,
                                           std::size_t frames,
                                           int oversample_factor = kDefaultTruePeakOversample);

/// @brief Measures integrated LUFS and true-peak in one call. Useful for
///        result-struct population paths that report both numbers.
LufsAndTruePeak measure_lufs_and_true_peak(const Audio& audio,
                                           int true_peak_oversample = kDefaultTruePeakOversample);

/// @brief Return the existing LUFS, LRA, and true-peak measurements together.
LoudnessSummary measure_loudness_summary(const Audio& audio,
                                         int true_peak_oversample = kDefaultTruePeakOversample);

/// @brief Multi-channel counterpart preserving BS.1770 channel summing.
LoudnessSummary measure_loudness_summary_interleaved(
    const float* samples, std::size_t frames, int channels, int sample_rate,
    int true_peak_oversample = kDefaultTruePeakOversample);

/// @brief Stereo counterpart for callers that already hold planar channels.
/// @details Same measurements as @ref measure_loudness_summary_interleaved on
///          the interleaved form of the same audio. It exists for its memory
///          profile: an offline caller with planar buffers would otherwise
///          interleave a track-length copy and keep it alive across the call,
///          while this holds one only for the loudness step and reads the
///          caller's own buffers for the true peak.
LoudnessSummary measure_loudness_summary_stereo_planar(
    const float* left, const float* right, std::size_t frames, int sample_rate,
    int true_peak_oversample = kDefaultTruePeakOversample);

/// @brief @ref measure_loudness_summary_interleaved that also yields the series
///        its scalars are reduced from.
/// @details One K-weighting pass, not two: the series are the measurement's own
///          intermediate rather than a second measurement of the same audio.
/// @param series Receives both series; must not be null. Overwritten.
LoudnessSummary measure_loudness_summary_interleaved(const float* samples, std::size_t frames,
                                                     int channels, int sample_rate,
                                                     int true_peak_oversample,
                                                     LoudnessSeries* series);

/// @brief @ref measure_loudness_summary_stereo_planar that also yields the
///        series, keeping the same BS.1770 channel summing.
LoudnessSummary measure_loudness_summary_stereo_planar(const float* left, const float* right,
                                                       std::size_t frames, int sample_rate,
                                                       int true_peak_oversample,
                                                       LoudnessSeries* series);

/// @brief Series only, for callers that do not need the scalars.
void measure_loudness_series_interleaved(const float* samples, std::size_t frames, int channels,
                                         int sample_rate, LoudnessSeries* series);

/// @brief Stereo planar counterpart of @ref measure_loudness_series_interleaved.
void measure_loudness_series_stereo_planar(const float* left, const float* right,
                                           std::size_t frames, int sample_rate,
                                           LoudnessSeries* series);

/// @brief Per-hop level difference a stage made, in LU: `after - before`, so a
///        negative element is attenuation.
/// @details This measures the signal, NOT a processor's internal gain reduction.
///          For a stage that reshapes the spectrum the two differ, and for a
///          stage with no gain element at all this is still defined - which is
///          why it is spelled as a level delta and names no stage. Silent
///          blocks (`-inf`) are floored to the shared dB floor on both sides
///          before subtracting, so the result is finite and two silent blocks
///          report no change rather than a NaN.
/// @param before Series measured at the stage's input.
/// @param after Series measured at the stage's output. Must match @p before in
///        length, which holds whenever the stage preserved the frame count.
/// @param momentary_delta Receives the momentary difference; may be null.
/// @param short_term_delta Receives the short-term difference; may be null.
void stage_level_delta_lu(const LoudnessSeries& before, const LoudnessSeries& after,
                          std::vector<float>* momentary_delta,
                          std::vector<float>* short_term_delta);

/// @brief Loudness of the difference signal `before - after`, i.e. what a stage
///        removed rather than what it left.
/// @details Holds one frame-length temporary for the difference and releases it
///          before returning. Both pointers must be non-null when @p frames > 0.
LoudnessSummary measure_residual_loudness_summary(
    const float* before, const float* after, std::size_t frames, int sample_rate,
    int true_peak_oversample = kDefaultTruePeakOversample);

/// @brief Stereo planar counterpart of @ref measure_residual_loudness_summary.
LoudnessSummary measure_residual_loudness_summary_stereo_planar(
    const float* before_left, const float* before_right, const float* after_left,
    const float* after_right, std::size_t frames, int sample_rate,
    int true_peak_oversample = kDefaultTruePeakOversample);

}  // namespace sonare::mastering::common
