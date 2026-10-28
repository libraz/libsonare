#pragma once

/// @file psola_shifter.h
/// @brief Low-latency TD-PSOLA pitch shifter producing one output per voice.

#include "editing/voice_changer/voice_transform_types.h"

namespace sonare::editing::voice_changer {

/// Latency is ceil(sr / f0_min); targets are interpolated linearly across each block.
class PsolaShifter {
 public:
  void prepare(double sr, int max_block, float f0_min_hz);
  void reset();
  void push_estimates(const F0Estimate* e, int count) noexcept;
  /// @param voice_out `targets.count` output buffers of `n` samples.
  void process(const float* in, const VoiceTargets& targets, float* const* voice_out,
               int n) noexcept;
  /// @return ceil(sr / f0_min).
  int latency_samples() const noexcept;
};

}  // namespace sonare::editing::voice_changer
