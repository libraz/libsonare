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
  float applied_gain_db = 0.0f;
  int latency_samples = 0;
};

/// @brief Stereo counterpart to @ref MonoAudioResult.
struct StereoAudioResult {
  std::vector<float> left;
  std::vector<float> right;
  int sample_rate = 0;
  float input_lufs = 0.0f;
  float output_lufs = 0.0f;
  float applied_gain_db = 0.0f;
  int latency_samples = 0;
};

/// @brief Gain reduction reported by a single dynamics / maximizer stage.
/// `gain_reduction_db` is the most recent (typically last-block) gain
/// reduction in dB (negative or zero); for multiband stages it is the
/// most-reduced band.
struct StageGainReduction {
  std::string stage;  // e.g. "dynamics.compressor"
  float gain_reduction_db = 0.0f;
};

/// @brief Additional measurements / annotations produced by the full
/// mastering chain on top of the common @ref MonoAudioResult /
/// @ref StereoAudioResult fields.
struct ChainMetrics {
  /// ITU-R BS.1770-4 true peak (4x oversampled).
  float output_true_peak_dbtp = 0.0f;
  /// EBU Tech 3342 Loudness Range (LU).
  float output_lra = 0.0f;
  /// Ordered list of stages that ran (e.g. "dynamics.compressor").
  std::vector<std::string> stages;
  /// Per-stage gain reductions for the dynamics / maximizer stages.
  std::vector<StageGainReduction> stage_gain_reductions;
};

}  // namespace sonare::mastering::api
