#include "midi/mpe.h"

#include <algorithm>

namespace sonare::midi {

namespace {

/// A 14-bit pitch bend centres here, and Appendix C.5's receiver equation
/// divides the displacement by one less than that. The asymmetry is the spec's
/// own and is kept rather than tidied: a receiver that normalizes by 8192
/// instead lands a full-scale bend a fraction of a cent short of the semitone
/// count the sender computed from the same table.
constexpr uint16_t kBendCentre = 8192;
constexpr float kBendScale = 8191.0f;

constexpr uint8_t kMaxCc = 127;

constexpr uint8_t present_bit(MpeDimension dimension) noexcept {
  return static_cast<uint8_t>(1u << static_cast<uint8_t>(dimension));
}

/// Adds two 7-bit controller values as Appendix D's first strategy asks, with
/// the ceiling the domain has.
uint8_t add_cc(uint8_t a, uint8_t b) noexcept {
  const unsigned sum = static_cast<unsigned>(a) + static_cast<unsigned>(b);
  return sum > kMaxCc ? kMaxCc : static_cast<uint8_t>(sum);
}

}  // namespace

size_t mpe_select_notes(NoteTracking mode, MpeNote* notes, size_t count) noexcept {
  if (notes == nullptr) return 0;
  for (size_t i = 0; i < count; ++i) notes[i].selected = false;

  if (mode == NoteTracking::kAllNotes) {
    size_t selected = 0;
    for (size_t i = 0; i < count; ++i) {
      if (!notes[i].active) continue;
      notes[i].selected = true;
      ++selected;
    }
    return selected;
  }

  // The other three name exactly one note, so the scan keeps an index rather
  // than a flag and writes once at the end.
  size_t chosen = count;
  for (size_t i = 0; i < count; ++i) {
    if (!notes[i].active) continue;
    if (chosen == count) {
      chosen = i;
      continue;
    }
    switch (mode) {
      case NoteTracking::kLastNote:
        chosen = i;  // the list is oldest-first, so the last survivor wins
        break;
      case NoteTracking::kLowestNote:
        if (notes[i].note < notes[chosen].note) chosen = i;
        break;
      case NoteTracking::kHighestNote:
        if (notes[i].note > notes[chosen].note) chosen = i;
        break;
      case NoteTracking::kAllNotes:
        break;
    }
  }
  if (chosen == count) return 0;
  notes[chosen].selected = true;
  return 1;
}

uint16_t MpeState::zone_mask(MpeZone zone, uint8_t member_count) noexcept {
  if (member_count == 0) return 0;
  // Manager plus its members, contiguous from the end of the channel space the
  // zone grows out of (2.2.1).
  const uint16_t span = static_cast<uint16_t>((1u << (member_count + 1)) - 1u);
  return zone == MpeZone::kLower ? span : static_cast<uint16_t>(span << (15 - member_count));
}

uint16_t MpeState::occupied_mask() const noexcept {
  return static_cast<uint16_t>(zone_mask(MpeZone::kLower, zones_[0].member_count) |
                               zone_mask(MpeZone::kUpper, zones_[1].member_count));
}

uint16_t MpeState::manager_mask() const noexcept {
  return static_cast<uint16_t>(
      (zones_[0].active ? uint16_t{1} << kMpeLowerManagerChannel : uint16_t{0}) |
      (zones_[1].active ? uint16_t{1} << kMpeUpperManagerChannel : uint16_t{0}));
}

uint8_t MpeState::manager_of(uint8_t channel) const noexcept {
  return zone_of(channel) == MpeZone::kLower ? kMpeLowerManagerChannel : kMpeUpperManagerChannel;
}

bool MpeState::apply_mcm(uint8_t manager_channel, uint8_t member_count,
                         uint16_t* out_reconfigured) noexcept {
  if (manager_channel != kMpeLowerManagerChannel && manager_channel != kMpeUpperManagerChannel) {
    return false;
  }
  if (member_count > 15) return false;

  const MpeZone zone =
      manager_channel == kMpeLowerManagerChannel ? MpeZone::kLower : MpeZone::kUpper;
  const size_t index = static_cast<size_t>(zone);
  const size_t other = 1u - index;

  const uint16_t before_mask = occupied_mask();
  const uint16_t before_managers = manager_mask();

  zones_[index] = Zone{};
  zones_[index].active = member_count > 0;
  zones_[index].member_count = member_count;

  // "No MIDI Channel shall be assigned to more than one Zone at a time ... the
  // most recent message shall take precedence", and a zone left with no member
  // channels is deactivated (2.2.1). Both zones are contiguous and grow towards
  // each other, so yielding is a shrink of the older one's member count.
  if (zones_[index].active && zones_[other].active) {
    const int room = 14 - static_cast<int>(member_count);
    const int kept = std::min<int>(zones_[other].member_count, std::max(room, 0));
    zones_[other].member_count = static_cast<uint8_t>(kept);
    if (kept == 0) zones_[other] = Zone{};
  }

  const uint16_t after_mask = occupied_mask();
  const uint16_t after_managers = manager_mask();

  // 2.2.3 asks for the channels entering or leaving MPE control. A channel that
  // only changed role is reported as well: its bend sensitivity moves by a
  // factor of 24, so a note left sounding on it would hang at a range nothing
  // sent it. Reporting more than the section names is safe; reporting less
  // leaves exactly the hanging note the section exists to prevent.
  if (out_reconfigured != nullptr) {
    *out_reconfigured =
        static_cast<uint16_t>((before_mask ^ after_mask) | (before_managers ^ after_managers));
  }
  return true;
}

bool MpeState::apply_bend_sensitivity(uint8_t channel, float semitones) noexcept {
  const MpeChannelRole channel_role = role(channel);
  if (channel_role == MpeChannelRole::kUnassigned) return false;
  const float clamped = std::clamp(semitones, 0.0f, kMpeMaxBendSemitones);
  Zone& zone = zones_[static_cast<size_t>(zone_of(channel))];
  if (channel_role == MpeChannelRole::kManager) {
    zone.manager_bend = clamped;
  } else {
    // One value for every member of the zone, which 2.2.5 requires rather than
    // permits.
    zone.member_bend = clamped;
  }
  return true;
}

bool MpeState::apply_midi_mode(uint8_t channel, MpeMidiMode mode) noexcept {
  const MpeChannelRole channel_role = role(channel);
  if (channel_role != MpeChannelRole::kMember) return false;
  zones_[static_cast<size_t>(zone_of(channel))].mode = mode;
  return true;
}

void MpeState::track_bend(uint8_t channel, uint16_t bend14) noexcept {
  Channel& state = channels_[channel & 0x0Fu];
  state.bend14 = bend14;
  state.present |= present_bit(MpeDimension::kBend);
}

void MpeState::track_pressure(uint8_t channel, uint8_t value) noexcept {
  Channel& state = channels_[channel & 0x0Fu];
  state.pressure = value;
  state.present |= present_bit(MpeDimension::kPressure);
}

void MpeState::track_timbre(uint8_t channel, uint8_t value) noexcept {
  Channel& state = channels_[channel & 0x0Fu];
  state.timbre = value;
  state.present |= present_bit(MpeDimension::kTimbre);
}

void MpeState::reset_controls(uint16_t channels) noexcept {
  for (uint8_t ch = 0; ch < 16; ++ch) {
    if ((channels & (uint16_t{1} << ch)) == 0) continue;
    channels_[ch] = Channel{};
  }
}

void MpeState::reset() noexcept {
  zones_[0] = Zone{};
  zones_[1] = Zone{};
  reset_controls(0xFFFFu);
}

MpeChannelRole MpeState::role(uint8_t channel) const noexcept {
  const uint8_t ch = channel & 0x0Fu;
  for (size_t i = 0; i < kMpeZoneCount; ++i) {
    if (!zones_[i].active) continue;
    const MpeZone zone = static_cast<MpeZone>(i);
    const uint16_t mask = zone_mask(zone, zones_[i].member_count);
    if ((mask & (uint16_t{1} << ch)) == 0) continue;
    const uint8_t manager =
        zone == MpeZone::kLower ? kMpeLowerManagerChannel : kMpeUpperManagerChannel;
    return ch == manager ? MpeChannelRole::kManager : MpeChannelRole::kMember;
  }
  return MpeChannelRole::kUnassigned;
}

MpeZone MpeState::zone_of(uint8_t channel) const noexcept {
  const uint8_t ch = channel & 0x0Fu;
  if (zones_[1].active &&
      (zone_mask(MpeZone::kUpper, zones_[1].member_count) & (uint16_t{1} << ch)) != 0) {
    return MpeZone::kUpper;
  }
  return MpeZone::kLower;
}

MpeMidiMode MpeState::midi_mode(MpeZone zone) const noexcept {
  return zones_[static_cast<size_t>(zone)].mode;
}

uint8_t MpeState::member_count(MpeZone zone) const noexcept {
  const Zone& state = zones_[static_cast<size_t>(zone)];
  return state.active ? state.member_count : uint8_t{0};
}

float MpeState::bend_sensitivity(uint8_t channel) const noexcept {
  const MpeChannelRole channel_role = role(channel);
  if (channel_role == MpeChannelRole::kUnassigned) return 0.0f;
  const Zone& zone = zones_[static_cast<size_t>(zone_of(channel))];
  return channel_role == MpeChannelRole::kManager ? zone.manager_bend : zone.member_bend;
}

bool MpeState::ignores(uint8_t channel, MpeIgnorable what) const noexcept {
  const MpeChannelRole channel_role = role(channel);
  if (channel_role == MpeChannelRole::kUnassigned) return false;
  const bool poly_mode = midi_mode(zone_of(channel)) == MpeMidiMode::kPoly;
  switch (what) {
    case MpeIgnorable::kPolyKeyPressure:
      return channel_role == MpeChannelRole::kMember;
    case MpeIgnorable::kModeMessage:
      return channel_role == MpeChannelRole::kManager;
    case MpeIgnorable::kBankSelect:
    case MpeIgnorable::kProgramChange:
      return channel_role == MpeChannelRole::kMember && poly_mode;
  }
  return false;
}

float MpeState::manager_bend_semitones(uint8_t channel) const noexcept {
  if (role(channel) == MpeChannelRole::kUnassigned) return 0.0f;
  const MpeZone zone = zone_of(channel);
  const Channel& state = channels_[manager_of(channel)];
  if ((state.present & present_bit(MpeDimension::kBend)) == 0) return 0.0f;
  const float displacement = static_cast<float>(static_cast<int>(state.bend14) - kBendCentre);
  return zones_[static_cast<size_t>(zone)].manager_bend * displacement / kBendScale;
}

float MpeState::bend_semitones(uint8_t channel) const noexcept {
  const MpeChannelRole channel_role = role(channel);
  if (channel_role == MpeChannelRole::kUnassigned) return 0.0f;
  if (channel_role == MpeChannelRole::kManager) return manager_bend_semitones(channel);
  const Channel& state = channels_[channel & 0x0Fu];
  float member = 0.0f;
  if ((state.present & present_bit(MpeDimension::kBend)) != 0) {
    const float displacement = static_cast<float>(static_cast<int>(state.bend14) - kBendCentre);
    member = bend_sensitivity(channel) * displacement / kBendScale;
  }
  return member + manager_bend_semitones(channel);
}

uint8_t MpeState::pressure(uint8_t channel) const noexcept {
  const MpeChannelRole channel_role = role(channel);
  if (channel_role == MpeChannelRole::kUnassigned) return 0;
  const Channel& own = channels_[channel & 0x0Fu];
  const uint8_t mine = (own.present & present_bit(MpeDimension::kPressure)) != 0 ? own.pressure : 0;
  if (channel_role == MpeChannelRole::kManager) return mine;
  const Channel& bias = channels_[manager_of(channel)];
  if ((bias.present & present_bit(MpeDimension::kPressure)) == 0) return mine;
  return add_cc(mine, bias.pressure);
}

uint8_t MpeState::timbre(uint8_t channel) const noexcept {
  const MpeChannelRole channel_role = role(channel);
  if (channel_role == MpeChannelRole::kUnassigned) return 0;
  const Channel& own = channels_[channel & 0x0Fu];
  const uint8_t mine = (own.present & present_bit(MpeDimension::kTimbre)) != 0 ? own.timbre : 0;
  if (channel_role == MpeChannelRole::kManager) return mine;
  const Channel& bias = channels_[manager_of(channel)];
  if ((bias.present & present_bit(MpeDimension::kTimbre)) == 0) return mine;
  return add_cc(mine, bias.timbre);
}

bool MpeState::has(uint8_t channel, MpeDimension dimension) const noexcept {
  const MpeChannelRole channel_role = role(channel);
  if (channel_role == MpeChannelRole::kUnassigned) return false;
  const uint8_t bit = present_bit(dimension);
  if ((channels_[channel & 0x0Fu].present & bit) != 0) return true;
  return (channels_[manager_of(channel)].present & bit) != 0;
}

}  // namespace sonare::midi
