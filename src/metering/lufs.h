#pragma once

/// @file lufs.h
/// @brief Offline LUFS loudness meter.

#include <cstddef>
#include <vector>

#include "core/audio.h"

namespace sonare::metering {

inline constexpr float kLufsAbsoluteGate = -70.0f;
inline constexpr float kLufsIntegratedRelativeGate = -10.0f;
inline constexpr float kLufsRangeRelativeGate = -20.0f;

/// @brief ITU-R BS.1770-4 Annex 2 momentary overlap (75% over 400 ms = 100 ms hop).
/// @details Fixed by spec; not user-configurable. See ITU-R BS.1770-4 §2.4.
inline constexpr float kLufsMomentaryOverlap = 0.75f;

/// @brief ITU-R BS.1770-4 / EBU R128 short-term and gating window hop (100 ms).
/// @details Used together with `short_term_duration_sec` (3 s) to derive the
///          short-term overlap that yields a 100 ms hop. Fixed by spec.
inline constexpr float kLufsShortTermHopSec = 0.1f;

struct LufsResult {
  float integrated_lufs = 0.0f;
  float momentary_lufs = 0.0f;
  float short_term_lufs = 0.0f;
  float loudness_range = 0.0f;
};

struct LufsConfig {
  float absolute_gate_lufs = kLufsAbsoluteGate;
  float relative_gate_lu = kLufsIntegratedRelativeGate;
  float block_duration_sec = 0.400f;
  /// @brief Overlap used for the **integrated** LUFS gating blocks only.
  /// @details The momentary (400 ms) and short-term (3 s) overlaps are fixed by
  ///          ITU-R BS.1770-4 (75% / 100 ms hop respectively) and are NOT
  ///          affected by this value. Changing `block_overlap` only alters the
  ///          density of gating blocks used to compute integrated loudness.
  float block_overlap = 0.75f;
  float momentary_duration_sec = 0.400f;
  float short_term_duration_sec = 3.0f;
};

LufsResult lufs(const Audio& audio, const LufsConfig& config = {});
LufsResult lufs_interleaved(const float* samples, size_t frames, int channels, int sample_rate,
                            const LufsConfig& config = {});
std::vector<float> momentary_lufs(const Audio& audio, const LufsConfig& config = {});
std::vector<float> short_term_lufs(const Audio& audio, const LufsConfig& config = {});

/// @brief Computes the EBU R128 Loudness Range (LRA) in LU.
/// @param audio Input audio (mono only). Multi-channel input throws
///              SonareException with ErrorCode::InvalidParameter; downmix the
///              signal yourself (or use the multi-channel `lufs_interleaved`
///              API for a proper BS.1770 loudness measurement) before calling.
/// @details Implements the standard EBU Tech 3342 algorithm: K-weighted short-term
///          loudness over 3 s windows with 100 ms hops, an absolute gate of -70 LUFS,
///          and a relative gate 20 LU below the ungated mean loudness. The LRA is the
///          difference between the 95th and 10th percentiles of the gated distribution.
/// @return Loudness range in LU (0 if insufficient data).
/// @throws SonareException (InvalidParameter) if @p audio is not mono.
float ebur128_loudness_range(const Audio& audio);

/// @brief Computes the EBU Tech 3342 Loudness Range (LRA) from short-term loudness blocks.
/// @param short_term_lufs Per-block short-term loudness values (LUFS). Non-finite
///                        values are ignored.
/// @details Applies the full two-stage gate (absolute gate at -70 LUFS, then a
///          relative gate 20 LU below the energy-domain mean of the absolute-gated
///          blocks) and returns the difference between the 95th and 10th percentiles
///          of the gated distribution. This is the single shared implementation used
///          by both `lufs_interleaved`'s `loudness_range` field and
///          `ebur128_loudness_range`; it is exposed for reuse from the C-API.
/// @return Loudness range in LU (0 if fewer than two gated blocks remain).
float lra_from_short_term_blocks(const std::vector<float>& short_term_lufs);

}  // namespace sonare::metering
