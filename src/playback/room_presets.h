#pragma once

/// @file room_presets.h
/// @brief Room presets of the headphone renderer: uniform-absorption shoebox
///        rooms, the listener position and the virtual-speaker positions in
///        room coordinates (metres, `acoustic::ShoeboxRoom` axes).

#include "acoustic/geometry.h"
#include "acoustic/room_types.h"
#include "playback/config.h"
#include "playback/speaker_geometry.h"

namespace sonare::playback {

/// Listener ear height above the floor.
inline constexpr float kListenerEarHeightM = 1.2f;
/// Late-reverberation IRs are truncated to this length.
inline constexpr float kLateTailMaxSeconds = 1.0f;

/// Geometry and absorption of a preset.
struct RoomPresetSpec {
  RoomDimensions dims{};
  float absorption = 0.0f;
  float speaker_distance_m = 0.0f;  ///< listener to virtual speaker
};

/// Returns false for `RoomPreset::None`.
bool room_preset_spec(RoomPreset preset, RoomPresetSpec* out) noexcept;

/// Listener position: room centre moved back by a sixth of the depth, at ear
/// height.
acoustic::Vec3 listener_position(const RoomPresetSpec& spec) noexcept;

/// Room position of a virtual speaker at its nominal direction, at
/// `speaker_distance_m` from the listener.
acoustic::Vec3 virtual_speaker_position(const RoomPresetSpec& spec,
                                        SpeakerDirection direction) noexcept;

}  // namespace sonare::playback
