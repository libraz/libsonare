#pragma once

/// @file track_mixer_internal.h
/// @brief Shared file-local helpers for the TrackMixerRuntime translation units.

#include <cstddef>
#include <cstdint>

#include "engine/track_mixer.h"
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

/// A decoded meter target id; @c index is the lane or bus slot where one applies.
struct MeterTargetSlot {
  enum class Kind { Master, Lane, Bus, InputMonitor, Invalid };
  Kind kind = Kind::Invalid;
  size_t index = 0;
};

/// Inverse of lane_meter_target / bus_meter_target, plus the master and input-monitor ids.
inline constexpr MeterTargetSlot decode_meter_target(uint32_t target_id) noexcept {
  using Kind = MeterTargetSlot::Kind;
  constexpr uint32_t kMasterTarget = 0;
  constexpr uint32_t kInputMonitorTarget = 0xFFFF;
  if (target_id == kMasterTarget) return {Kind::Master, 0};
  if (target_id == kInputMonitorTarget) return {Kind::InputMonitor, 0};
  if (target_id <= TrackMixerRuntime::kMaxTrackLanes) return {Kind::Lane, target_id - 1};
  const uint32_t first_bus = bus_meter_target(0);
  if (target_id >= first_bus && target_id < first_bus + TrackMixerRuntime::kMaxBusLanes) {
    return {Kind::Bus, target_id - first_bus};
  }
  return {};
}

}  // namespace sonare::engine
