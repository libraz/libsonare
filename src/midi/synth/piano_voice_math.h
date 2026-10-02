#pragma once

/// @file piano_voice_math.h
/// @brief Private pure math helpers for the piano voice.

#include <cmath>

namespace sonare::midi::synth::piano_detail {

/// Drive gain that makes a unit impulse ring at the modal loop's normalized
/// amplitude after its period is bent by @p ratio.
inline float piano_modal_drive_gain(float weight, float ratio, float omega,
                                    float base_period) noexcept {
  return weight * 2.0f * ratio * std::sin(omega) / base_period;
}

}  // namespace sonare::midi::synth::piano_detail
