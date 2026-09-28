#include "playback/room_presets.h"

namespace sonare::playback {

bool room_preset_spec(RoomPreset preset, RoomPresetSpec* out) noexcept {
  (void)preset;
  (void)out;
  return false;
}

acoustic::Vec3 listener_position(const RoomPresetSpec& spec) noexcept {
  (void)spec;
  return {};
}

acoustic::Vec3 virtual_speaker_position(const RoomPresetSpec& spec,
                                        SpeakerDirection direction) noexcept {
  (void)spec;
  (void)direction;
  return {};
}

}  // namespace sonare::playback
