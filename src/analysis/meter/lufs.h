#pragma once

/// @file lufs.h
/// @brief Offline LUFS loudness meter.

#include <cstddef>
#include <vector>

#include "core/audio.h"

namespace sonare::analysis::meter {

struct LufsResult {
  float integrated_lufs = 0.0f;
  float momentary_lufs = 0.0f;
  float short_term_lufs = 0.0f;
  float loudness_range = 0.0f;
};

struct LufsConfig {
  float absolute_gate_lufs = -70.0f;
  float relative_gate_lu = -10.0f;
  float block_duration_sec = 0.400f;
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
/// @param audio Input audio (mono)
/// @details Implements the standard EBU Tech 3342 algorithm: K-weighted short-term
///          loudness over 3 s windows with 100 ms hops, an absolute gate of -70 LUFS,
///          and a relative gate 20 LU below the ungated mean loudness. The LRA is the
///          difference between the 95th and 10th percentiles of the gated distribution.
/// @return Loudness range in LU (0 if insufficient data).
float ebur128_loudness_range(const Audio& audio);

}  // namespace sonare::analysis::meter
