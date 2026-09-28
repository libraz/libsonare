#pragma once

/// @file speaker_geometry.h
/// @brief Nominal loudspeaker directions of the playback renderer (ITU-R
///        BS.2051), its headphone virtual-speaker slots, and the reflection
///        ring. Azimuth is positive to the right, elevation positive upward;
///        every nominal direction is on the horizontal plane. This table is
///        separate from the mixing panner's angle table on purpose.

#include <cstdint>

#include "core/channel_layout.h"
#include "playback/config.h"

namespace sonare::playback {

/// Direction in degrees.
struct SpeakerDirection {
  float azimuth_deg = 0.0f;
  float elevation_deg = 0.0f;
};

/// Headphone virtual-speaker slot. The 5.1 surround pair and the 7.1 rear pair
/// sit at different angles, so they are separate slots.
enum class VirtualSlot : uint8_t {
  C,
  L,
  R,
  Ls110,  ///< 5.1 Ls (and upmixed stereo Ls)
  Rs110,  ///< 5.1 Rs
  Ls135,  ///< 7.1 Ls (rear)
  Rs135,  ///< 7.1 Rs
  Lss90,  ///< 7.1 Lss (side)
  Rss90,  ///< 7.1 Rss
};
inline constexpr int kVirtualSlotCount = 9;

/// Nominal direction of a slot.
constexpr SpeakerDirection virtual_slot_direction(VirtualSlot slot) noexcept {
  switch (slot) {
    case VirtualSlot::C:
      return {0.0f, 0.0f};
    case VirtualSlot::L:
      return {-30.0f, 0.0f};
    case VirtualSlot::R:
      return {30.0f, 0.0f};
    case VirtualSlot::Ls110:
      return {-110.0f, 0.0f};
    case VirtualSlot::Rs110:
      return {110.0f, 0.0f};
    case VirtualSlot::Ls135:
      return {-135.0f, 0.0f};
    case VirtualSlot::Rs135:
      return {135.0f, 0.0f};
    case VirtualSlot::Lss90:
      return {-90.0f, 0.0f};
    case VirtualSlot::Rss90:
      return {90.0f, 0.0f};
  }
  return {0.0f, 0.0f};
}

/// Slot a non-LFE role of a source bed lands on; returns false for LFE. The
/// stereo source maps like 5.1 because its upmix produces a 5.1 bed.
constexpr bool slot_for_role(ChannelLayout source, SpeakerRole role, VirtualSlot* out) noexcept {
  switch (role) {
    case SpeakerRole::L:
      *out = VirtualSlot::L;
      return true;
    case SpeakerRole::R:
      *out = VirtualSlot::R;
      return true;
    case SpeakerRole::C:
      *out = VirtualSlot::C;
      return true;
    case SpeakerRole::LFE:
      return false;
    case SpeakerRole::Ls:
      *out = source == ChannelLayout::SevenPointOne ? VirtualSlot::Ls135 : VirtualSlot::Ls110;
      return true;
    case SpeakerRole::Rs:
      *out = source == ChannelLayout::SevenPointOne ? VirtualSlot::Rs135 : VirtualSlot::Rs110;
      return true;
    case SpeakerRole::Lss:
      *out = VirtualSlot::Lss90;
      return true;
    case SpeakerRole::Rss:
      *out = VirtualSlot::Rss90;
      return true;
  }
  return false;
}

/// Nominal speaker direction of a role in an output speaker layout.
constexpr SpeakerDirection speaker_direction(ChannelLayout layout, SpeakerRole role) noexcept {
  VirtualSlot slot = VirtualSlot::C;
  if (!slot_for_role(layout, role, &slot)) return {0.0f, 0.0f};
  return virtual_slot_direction(slot);
}

/// Slots allocated on the headphone bus for an input layout, in bus order.
/// Fixed stereo always carries the five upmix slots; `Auto` carries all nine.
struct HeadphoneSlotSet {
  VirtualSlot slots[kVirtualSlotCount];
  int count;
};

constexpr HeadphoneSlotSet headphone_slots(InputLayout input) noexcept {
  using S = VirtualSlot;
  switch (input) {
    case InputLayout::Mono:
      return {{S::C, S::C, S::C, S::C, S::C, S::C, S::C, S::C, S::C}, 1};
    case InputLayout::Stereo:
    case InputLayout::FivePointOne:
      return {{S::L, S::R, S::C, S::Ls110, S::Rs110, S::C, S::C, S::C, S::C}, 5};
    case InputLayout::SevenPointOne:
      return {{S::L, S::R, S::C, S::Ls135, S::Rs135, S::Lss90, S::Rss90, S::C, S::C}, 7};
    case InputLayout::Auto:
      return {{S::L, S::R, S::C, S::Ls110, S::Rs110, S::Ls135, S::Rs135, S::Lss90, S::Rss90}, 9};
  }
  return {{S::C, S::C, S::C, S::C, S::C, S::C, S::C, S::C, S::C}, 1};
}

/// Room-fixed horizontal reflection ring used by the early-reflection stage.
inline constexpr int kReflectionRingDirections = 8;
inline constexpr float kReflectionRingStepDeg = 45.0f;

}  // namespace sonare::playback
