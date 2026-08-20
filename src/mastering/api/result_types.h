#pragma once

/// @file result_types.h
/// @brief Shared audio-result base types for the mastering API surface.
///
/// All mastering operations that take audio in and produce audio out share a
/// common set of return-value fields (sample buffer, sample rate, input /
/// output LUFS, applied gain, reported latency). This header defines those
/// common bases so the per-processor (`MonoResult` / `StereoResult` in
/// named_processor.h) and the full-chain (`MonoChainResult` /
/// `StereoChainResult` in chain.h) result types can share fields without
/// duplicating definitions.
///
/// The C ABI mirrors of these types (`SonareMasteringResult` etc.) keep their
/// flat field layout for ABI stability and are intentionally not derived from
/// these C++ bases.

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace sonare::mastering::api {

/// @brief Common scalar fields populated by every mono mastering operation.
/// Derived structs (e.g. @ref MonoResult, @ref MonoChainResult) extend this
/// with additional information.
struct MonoAudioResult {
  std::vector<float> samples;
  int sample_rate = 0;
  float input_lufs = 0.0f;
  float output_lufs = 0.0f;
  /// Static gain applied by loudness normalization before any limiter gain
  /// reduction. Limiter GR is reported separately in stage_gain_reductions.
  float applied_gain_db = 0.0f;
  int latency_samples = 0;
};

/// @brief Every scalar one named-processor dispatch computes for one channel.
///
/// The dispatch reports through this struct rather than through a list of
/// out-parameters so a caller copies the whole outcome instead of transcribing
/// the fields it happens to know about: a value added here reaches the result
/// types through the two @c apply() overloads below and cannot be silently
/// dropped at the call site.
struct ProcessorOutcome {
  int latency_samples = 0;
  /// Static gain the processor applied (loudness normalization); limiter gain
  /// reduction is not included.
  float applied_gain_db = 0.0f;
  /// True when a true-peak ceiling clamped the requested LUFS normalization
  /// gain, so the reported output LUFS is the achieved value and not the
  /// requested target. Only the loudness-normalizing processors set it.
  bool loudness_target_limited = false;
};

/// @brief Result of one mono named-processor call: the shared audio fields plus
/// the dispatch outcome.
struct MonoProcessorResult : MonoAudioResult {
  /// @copydoc ProcessorOutcome::loudness_target_limited
  bool loudness_target_limited = false;

  /// Copies a whole dispatch outcome into the result.
  void apply(const ProcessorOutcome& outcome) {
    latency_samples = outcome.latency_samples;
    applied_gain_db += outcome.applied_gain_db;
    loudness_target_limited = loudness_target_limited || outcome.loudness_target_limited;
  }
};

/// @brief Stereo counterpart to @ref MonoAudioResult.
struct StereoAudioResult {
  std::vector<float> left;
  std::vector<float> right;
  int sample_rate = 0;
  float input_lufs = 0.0f;
  float output_lufs = 0.0f;
  /// Static gain applied by loudness normalization before any limiter gain
  /// reduction. Limiter GR is reported separately in stage_gain_reductions.
  float applied_gain_db = 0.0f;
  int latency_samples = 0;
};

/// @brief Result of one stereo named-processor call: the shared audio fields
/// plus the dispatch outcome.
struct StereoProcessorResult : StereoAudioResult {
  /// @copydoc ProcessorOutcome::loudness_target_limited
  bool loudness_target_limited = false;

  /// Copies both per-channel dispatch outcomes of the generic stereo fallback
  /// (the mono processor run independently on each channel).
  void apply(const ProcessorOutcome& left_outcome, const ProcessorOutcome& right_outcome) {
    latency_samples = std::max(left_outcome.latency_samples, right_outcome.latency_samples);
    applied_gain_db += 0.5f * (left_outcome.applied_gain_db + right_outcome.applied_gain_db);
    loudness_target_limited = loudness_target_limited || left_outcome.loudness_target_limited ||
                              right_outcome.loudness_target_limited;
  }
};

/// @brief Gain reduction reported by a single dynamics / maximizer stage.
/// `gain_reduction_db` is the gain reduction in dB (negative or zero) reported
/// by the stage; for multiband stages it is the most-reduced band. Offline
/// true-peak limiter stages retain their most-negative program value across
/// latency-drain blocks.
struct StageGainReduction {
  std::string stage;  // e.g. "dynamics.compressor"
  float gain_reduction_db = 0.0f;
};

/// @brief Whole-program loudness values used by a mastering report.
///
/// These fields use the same EBU R128 meter as the existing chain loudness
/// fields. Max-M and Max-S are the maximum 400 ms and 3 s windows,
/// respectively; they are deliberately not the final momentary / short-term
/// window values.
struct MasteringLoudnessSummary {
  float integrated_lufs = 0.0f;
  float max_momentary_lufs = 0.0f;
  float max_short_term_lufs = 0.0f;
  float true_peak_dbtp = 0.0f;
  float loudness_range = 0.0f;
};

/// @brief Explanation-oriented measurements for a complete mastering run.
///
/// @c band_energy_delta_db contains 32 logarithmically-spaced long-term
/// spectral-energy deltas (after minus before) from 20 Hz to Nyquist. The
/// values are intended for compact host visualizations, not corrective-EQ
/// decisions. @c max_gain_reduction_db is the most-negative stage-reported
/// gain reduction, or zero when none ran.
inline constexpr std::size_t kMasteringReportBandCount = 32;

struct MasteringReport {
  MasteringLoudnessSummary before;
  MasteringLoudnessSummary after;
  float applied_gain_db = 0.0f;
  float max_gain_reduction_db = 0.0f;
  bool loudness_target_limited = false;
  std::array<float, kMasteringReportBandCount> band_energy_delta_db{};
};

/// @brief Additional measurements / annotations produced by the full
/// mastering chain on top of the common @ref MonoAudioResult /
/// @ref StereoAudioResult fields.
struct ChainMetrics {
  /// ITU-R BS.1770-4 true peak measured with the chain's configured loudness
  /// true-peak oversample factor (default 4x).
  float output_true_peak_dbtp = 0.0f;
  /// EBU Tech 3342 Loudness Range (LU).
  float output_lra = 0.0f;
  /// True when the loudness stage could not apply target-input LUFS gain
  /// because doing so would exceed its true-peak ceiling.
  bool loudness_target_limited = false;
  /// Ordered list of stages that ran (e.g. "dynamics.compressor").
  std::vector<std::string> stages;
  /// Per-stage gain reductions for the dynamics / maximizer stages.
  std::vector<StageGainReduction> stage_gain_reductions;
  /// Complete before/after explanation payload. The scalar fields above remain
  /// available for source compatibility and mirror the matching report values.
  MasteringReport report;
};

}  // namespace sonare::mastering::api
