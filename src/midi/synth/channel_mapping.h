#pragma once

/// @file channel_mapping.h
/// @brief Scalar mappings from channel controller values to the synth hosts' mod fields.

#include "midi/control_value.h"

namespace sonare::midi::synth {

/// @brief Pan units (-500..+500) for a 7-bit pan value, centred at 64.
inline float cc_pan_units(float pan7) noexcept { return (pan7 - 64.0f) / 63.0f * 500.0f; }

/// @brief Pitch bend as a bipolar fraction of full deflection (-1..+1), keeping a 32-bit bend's
/// fraction.
inline float bend_bipolar(Bend32 bend) noexcept {
  return (bend.f14() - static_cast<float>(kBend14Center)) / static_cast<float>(kBend14Center);
}

}  // namespace sonare::midi::synth
