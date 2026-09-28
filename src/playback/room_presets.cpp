#include "playback/room_presets.h"

#include <algorithm>
#include <cmath>

#include "util/constants.h"

namespace sonare::playback {

using sonare::constants::kPi;

namespace {

constexpr float kDegToRad = kPi / 180.0f;
/// A nominal speaker that would land on or behind a wall is pulled this far
/// inside it, so its first image never coincides with the source.
constexpr float kSpeakerWallMarginM = 0.25f;

}  // namespace

bool room_preset_spec(RoomPreset preset, RoomPresetSpec* out) noexcept {
  switch (preset) {
    case RoomPreset::None:
      return false;
    case RoomPreset::LivingRoom:
      *out = {{5.0f, 4.0f, 2.6f}, 0.35f, 2.0f};
      return true;
    case RoomPreset::HomeTheater:
      *out = {{7.0f, 5.0f, 3.0f}, 0.55f, 2.0f};
      return true;
    case RoomPreset::ScreeningRoom:
      *out = {{12.0f, 8.0f, 4.5f}, 0.6f, 4.0f};
      return true;
  }
  return false;
}

// Room axes: +x (length) is the listener's front, +y (width) the listener's
// right, +z up. The room is mirror-symmetric about the listener's y.
acoustic::Vec3 listener_position(const RoomPresetSpec& spec) noexcept {
  const float depth = spec.dims.length;
  return {depth * 0.5f - depth / 6.0f, spec.dims.width * 0.5f, kListenerEarHeightM};
}

acoustic::Vec3 virtual_speaker_position(const RoomPresetSpec& spec,
                                        SpeakerDirection direction) noexcept {
  const float az = direction.azimuth_deg * kDegToRad;
  const float el = direction.elevation_deg * kDegToRad;
  const acoustic::Vec3 offset{std::cos(el) * std::cos(az), std::cos(el) * std::sin(az),
                              std::sin(el)};
  const acoustic::Vec3 p = listener_position(spec) + offset * spec.speaker_distance_m;
  auto inside = [](float v, float extent) {
    return std::clamp(v, kSpeakerWallMarginM, extent - kSpeakerWallMarginM);
  };
  return {inside(p.x, spec.dims.length), inside(p.y, spec.dims.width),
          inside(p.z, spec.dims.height)};
}

}  // namespace sonare::playback
