#pragma once

/// @file synthetic_voice.h
/// @brief Synthetic voiced / unvoiced test signals with known f0 and formants.

#include <array>
#include <vector>

namespace sonare::voicelab {

/// One stretch of the synthetic voice: a glide from f0_start_hz to f0_end_hz with optional vibrato.
struct VoiceSegment {
  double duration_s;
  float f0_start_hz;
  float f0_end_hz;
  float vibrato_cents;
  float vibrato_hz;
  bool voiced;
  int vowel;  ///< 0..4 = /a e i o u/
  float level_db;
  float ramp_ms;  ///< raised-cosine ramp at each end of the segment
};

/// @return F1..F3 in Hz of a vowel index (0..4 = /a e i o u/).
std::array<float, 3> vowel_formants_hz(int vowel);

/// @brief Renders the segments; the per-sample true f0 (0 when unvoiced) is optional.
std::vector<float> synthesize_voice(const std::vector<VoiceSegment>& segments, double sr,
                                    std::vector<float>* true_f0_per_sample = nullptr);

}  // namespace sonare::voicelab
