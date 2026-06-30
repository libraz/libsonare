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

inline mixing::PanLaw to_pan_law(int law) {
  switch (law) {
    case 1:
      return mixing::PanLaw::Const4p5dB;
    case 2:
      return mixing::PanLaw::Const6dB;
    case 3:
      return mixing::PanLaw::Linear0dB;
    case 0:
    default:
      return mixing::PanLaw::Const3dB;
  }
}

inline constexpr uint32_t lane_meter_target(size_t lane_index) noexcept {
  return static_cast<uint32_t>(lane_index + 1);
}

inline constexpr uint32_t bus_meter_target(size_t bus_index) noexcept {
  return static_cast<uint32_t>(33 + bus_index);
}

}  // namespace sonare::engine
