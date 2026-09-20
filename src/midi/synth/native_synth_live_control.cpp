#include "midi/synth/native_synth.h"

namespace sonare::midi::synth {

ControllerProfile default_controller_profile() noexcept {
  ControllerProfile profile;
  ControllerProfile::preset("gm", &profile);
  return profile;
}

ExcitationAxes NativeSynth::channel_excitation(const ControllerAxisState& axes,
                                               uint32_t& present) noexcept {
  ExcitationAxes out{};
  present = kAxisNone;
  // One row per axis the two layers share. Everything that decides which
  // controller fills an axis is the profile's, so this is the whole bridge.
  if (axes.has(ControllerAxis::kExcitation)) {
    out.force = axes.values[static_cast<size_t>(ControllerAxis::kExcitation)];
    present |= kAxisForce;
  }
  if (axes.has(ControllerAxis::kPosition)) {
    out.position = axes.values[static_cast<size_t>(ControllerAxis::kPosition)];
    present |= kAxisPosition;
  }
  if (axes.has(ControllerAxis::kBrightness)) {
    out.brightness = axes.values[static_cast<size_t>(ControllerAxis::kBrightness)];
    present |= kAxisBrightness;
  }
  if (axes.has(ControllerAxis::kMorph)) {
    out.morph = axes.values[static_cast<size_t>(ControllerAxis::kMorph)];
    present |= kAxisMorph;
  }
  return out;
}

void NativeSynth::push_excitation_control(uint8_t channel) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  uint32_t present = kAxisNone;
  const ExcitationAxes base = channel_excitation(st.axes, present);
  const float speed_scale = static_cast<float>(st.expression) / 127.0f;
  for (NativeSynthVoice& v : pool_) {
    if (!v.active || v.channel != ch || v.patch == nullptr) continue;
    // Expression scales the bow speed (identity at CC11 == 127); every other
    // engine takes loudness through the shared expression VCA instead.
    if (v.patch->mode == SynthEngineMode::kBowedString) {
      v.bowed_string.set_bow_speed_scale(speed_scale);
    }
    // The axes override the preset only once a controller has reached them, so
    // a channel nothing has bound leaves every voice on its own voicing.
    if (present != kAxisNone) v.push_excitation(base, present);
  }
}

bool NativeSynth::set_controller_profile(const ControllerProfile& profile) noexcept {
  controller_profile_ = profile;
  for (uint8_t ch = 0; ch < 16; ++ch) {
    channels_[ch].axes.reset();
    refresh_channel_mod(ch);
    push_excitation_control(ch);
  }
  return true;
}

bool NativeSynth::mpe_dimension_of(const Ump& ump, MpeDimension* out) noexcept {
  const uint8_t status = ump.status_nibble();
  if (status == static_cast<uint8_t>(UmpStatus::kPitchBend)) {
    *out = MpeDimension::kBend;
    return true;
  }
  if (status == static_cast<uint8_t>(UmpStatus::kChannelPressure)) {
    *out = MpeDimension::kPressure;
    return true;
  }
  if (status == static_cast<uint8_t>(UmpStatus::kControlChange) &&
      ump.note_number() == kMpeTimbreCc) {
    *out = MpeDimension::kTimbre;
    return true;
  }
  return false;
}

size_t NativeSynth::gather_mpe_notes(uint8_t channel) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  size_t count = 0;
  for (const NativeSynthVoice& v : pool_) {
    if (!v.active || v.channel != ch || count == mpe_notes_.size()) continue;
    // One entry per note, and the oldest voice of a note decides where it sits:
    // a layered patch allocates several voices for one key and they share an
    // ordering position.
    size_t at = count;
    for (size_t i = 0; i < count; ++i) {
      if (mpe_notes_[i].note == v.note) {
        at = i;
        break;
      }
    }
    if (at == count) {
      // Insertion sort by allocation age, which is the order the notes started
      // in and the only order kLastNote can be answered from. The first voice
      // of a note dates it: a layered note-on takes its voices consecutively,
      // so which of them is reached first cannot move the note past another.
      while (at > 0 && mpe_note_ages_[at - 1] > v.age) {
        mpe_notes_[at] = mpe_notes_[at - 1];
        mpe_note_ages_[at] = mpe_note_ages_[at - 1];
        --at;
      }
      mpe_notes_[at] = MpeNote{v.note, false, false};
      mpe_note_ages_[at] = v.age;
      ++count;
    }
    // Sounding past its Note Off does not count as active: per-note control
    // "shall not affect a note after the Note Off message has been received"
    // (2.4), however long a pedal or a release tail keeps it audible.
    if (v.key_down) mpe_notes_[at].active = true;
  }
  return count;
}

uint8_t NativeSynth::mpe_attributed_note(uint8_t channel, MpeDimension dimension) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  // A manager's value reaches every note in its zone (2.2.6, A.4.1) and an
  // unassigned channel's is channel-wide by definition, so neither attributes.
  if (mpe_.role(ch) != MpeChannelRole::kMember) return kControllerAnyNote;
  const NoteTracking mode = dimension == MpeDimension::kBend ? controller_profile_.bend_tracking
                            : dimension == MpeDimension::kPressure
                                ? controller_profile_.pressure_tracking
                                : controller_profile_.timbre_tracking;
  if (mode == NoteTracking::kAllNotes) return kControllerAnyNote;
  const size_t count = gather_mpe_notes(ch);
  if (count <= 1) return kControllerAnyNote;
  if (mpe_select_notes(mode, mpe_notes_.data(), count) == 0) return kControllerAnyNote;
  for (size_t i = 0; i < count; ++i) {
    if (mpe_notes_[i].selected) return mpe_notes_[i].note;
  }
  return kControllerAnyNote;
}

void NativeSynth::refresh_mpe_note_mods(uint8_t channel) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  const uint8_t bend_note = mpe_attributed_note(ch, MpeDimension::kBend);
  const uint8_t pressure_note = mpe_attributed_note(ch, MpeDimension::kPressure);
  if (bend_note == kControllerAnyNote && pressure_note == kControllerAnyNote) {
    for (NativeSynthVoice& v : pool_) {
      if (v.channel == ch) v.mpe_mod_active = false;
    }
    return;
  }
  // What a note the value was not attributed to keeps: the manager's
  // contribution, which reaches every note in the zone whatever the tracking
  // rule says about the member's own.
  const Sf2ChannelMod& mod = channel_mods_[ch];
  const float member_bend_cents =
      (mpe_.bend_semitones(ch) - mpe_.manager_bend_semitones(ch)) * 100.0f;
  const uint8_t manager =
      mpe_.zone_of(ch) == MpeZone::kLower ? kMpeLowerManagerChannel : kMpeUpperManagerChannel;
  const float manager_pressure01 = static_cast<float>(mpe_.pressure(manager)) / 127.0f;
  for (NativeSynthVoice& v : pool_) {
    if (!v.active || v.channel != ch) continue;
    const bool takes_bend = bend_note == kControllerAnyNote || v.note == bend_note;
    const bool takes_pressure = pressure_note == kControllerAnyNote || v.note == pressure_note;
    v.mpe_mod_active = !takes_bend || !takes_pressure;
    if (!v.mpe_mod_active) continue;
    v.mpe_mod = mod;
    if (!takes_bend) v.mpe_mod.pitch_cents -= member_bend_cents;
    if (!takes_pressure) v.mpe_mod.aftertouch01 = manager_pressure01;
  }
}

Ump NativeSynth::mpe_controller_message(uint8_t channel, MpeDimension dimension) const noexcept {
  return dimension == MpeDimension::kPressure
             ? make_midi1_channel_pressure(0, channel, mpe_.pressure(channel))
             : make_midi1_control_change(0, channel, kMpeTimbreCc, mpe_.timbre(channel));
}

void NativeSynth::push_mpe_controller_axis(uint8_t channel, MpeDimension dimension) noexcept {
  const MpeChannelRole channel_role = mpe_.role(channel);
  if (channel_role == MpeChannelRole::kUnassigned) return;
  apply_resolved_input(mpe_controller_message(channel, dimension));
  if (channel_role != MpeChannelRole::kManager) return;
  // A member's combined value is its own plus the manager's bias, not the bias
  // alone, so each member is resolved from its own channel rather than handed
  // the manager's message (2.2.7, 2.2.8).
  const MpeZone zone = mpe_.zone_of(channel);
  for (uint8_t member = 0; member < 16; ++member) {
    if (mpe_.role(member) != MpeChannelRole::kMember || mpe_.zone_of(member) != zone) continue;
    apply_resolved_input(mpe_controller_message(member, dimension));
  }
}

void NativeSynth::track_mpe_input(const Ump& ump) noexcept {
  MpeDimension dimension = MpeDimension::kPressure;
  // Bend is tracked where the protocol dispatch decodes it, at its own width.
  if (!mpe_dimension_of(ump, &dimension) || dimension == MpeDimension::kBend) return;
  const bool midi1 = ump.message_type() == UmpMessageType::kMidi1ChannelVoice;
  const uint8_t ch = ump.channel() & 0x0Fu;
  if (dimension == MpeDimension::kPressure) {
    mpe_.track_pressure(ch, midi1 ? ump.note_number() : scale_cc_32_to_7(ump.words[1]));
  } else {
    mpe_.track_timbre(ch, midi1 ? ump.data2_7bit() : scale_cc_32_to_7(ump.words[1]));
  }
}

void NativeSynth::apply_controller_input(const Ump& ump) noexcept {
  const uint8_t ch = ump.channel() & 0x0Fu;
  MpeDimension dimension = MpeDimension::kPressure;
  // Bend is excluded from the fold rather than from the zone: it combines in
  // semitones across two sensitivities, which no 14-bit value spells, and the
  // synth applies the combination itself.
  if (mpe_.role(ch) == MpeChannelRole::kUnassigned || !mpe_dimension_of(ump, &dimension) ||
      dimension == MpeDimension::kBend) {
    apply_resolved_input(ump);
    return;
  }

  push_mpe_controller_axis(ch, dimension);
}

void NativeSynth::apply_resolved_input(const Ump& ump) noexcept {
  std::array<ControllerAxisValue, kMaxControllerBindings> resolved{};
  const size_t count = controller_profile_.resolve(ump, resolved.data(), resolved.size());
  if (count == 0) return;
  const uint8_t ch = ump.channel() & 0x0Fu;
  // Inside a zone a value addressed to the whole channel belongs to the note
  // the tracking rule names rather than to every note sounding on it (2.2.4.1).
  // Only the four excitation axes can carry it: the other three are channel
  // state here, which is the same reason bind() refuses a per-note binding for
  // them, so they stay channel-wide rather than being narrowed and dropped.
  MpeDimension dimension = MpeDimension::kPressure;
  const uint8_t attributed =
      mpe_dimension_of(ump, &dimension) ? mpe_attributed_note(ch, dimension) : kControllerAnyNote;
  bool channel_moved = false;
  for (size_t i = 0; i < count; ++i) {
    ControllerAxisValue value = resolved[i];
    if (value.note == kControllerAnyNote && attributed != kControllerAnyNote &&
        controller_axis_is_excitation(value.axis)) {
      value.note = attributed;
    }
    if (value.note == kControllerAnyNote) {
      channels_[ch].axes.set(value.axis, value.value);
      channel_moved = true;
      continue;
    }
    // A per-note value reaches the voices on that note and no further. It is
    // not stored on the channel, so a later channel-wide value on the same axis
    // overwrites it in those voices: per-voice controller state is what MPE
    // adds, and until then the precedence is simply last writer wins.
    ControllerAxisState note_axes = channels_[ch].axes;
    note_axes.set(value.axis, value.value);
    uint32_t present = kAxisNone;
    const ExcitationAxes axes = channel_excitation(note_axes, present);
    if (present == kAxisNone) continue;
    for (NativeSynthVoice& v : pool_) {
      if (v.active && v.note == value.note && v.channel == ch && v.patch != nullptr) {
        v.push_excitation(axes, present);
      }
    }
  }
  if (!channel_moved) return;
  refresh_channel_mod(ch);
  push_excitation_control(ch);
}

}  // namespace sonare::midi::synth
