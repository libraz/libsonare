#pragma once

/// @file lufs.h
/// @brief Offline LUFS loudness meter.

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
std::vector<float> momentary_lufs(const Audio& audio, const LufsConfig& config = {});
std::vector<float> short_term_lufs(const Audio& audio, const LufsConfig& config = {});

}  // namespace sonare::analysis::meter
