#pragma once

/// @file track_mixer_internal.h
/// @brief Shared file-local helpers for the TrackMixerRuntime translation units.

#include <cstddef>
#include <cstdint>

#include "mixing/channel_strip.h"

namespace sonare::engine {

inline mixing::PanMode to_pan_mode(int mode) {
  switch (mode) {
    case 0:
      return mixing::PanMode::Balance;
    case 1:
      return mixing::PanMode::StereoPan;
    case 2:
      return mixing::PanMode::DualPan;
    default:
      return mixing::PanMode::Balance;
  }
}

// The pan law has no engine-local mapping: mixing::pan_law_from_index() owns the
// wire encoding, including the out-of-range fallback, and is called directly.

inline constexpr uint32_t lane_meter_target(size_t lane_index) noexcept {
  return static_cast<uint32_t>(lane_index + 1);
}

inline constexpr uint32_t bus_meter_target(size_t bus_index) noexcept {
  return static_cast<uint32_t>(33 + bus_index);
}

}  // namespace sonare::engine
