#include <utility>

#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"

namespace sonare::midi::synth {

namespace {

using native_synth_detail::excitation_axis_mask;
using native_synth_detail::excitation_value;

void store_excitation_value(ExcitationAxes& out, ControllerAxis axis, float value) noexcept {
  switch (axis) {
    case ControllerAxis::kExcitation:
      out.force = value;
      break;
    case ControllerAxis::kPosition:
      out.position = value;
      break;
    case ControllerAxis::kBrightness:
      out.brightness = value;
      break;
    case ControllerAxis::kMorph:
      out.morph = value;
      break;
    case ControllerAxis::kNone:
    case ControllerAxis::kLoudness:
    case ControllerAxis::kPitchCents:
    case ControllerAxis::kVibratoDepth:
      break;
  }
}

}  // namespace

ControllerProfile default_controller_profile() noexcept {
  ControllerProfile profile;
  ControllerProfile::preset("gm", &profile);
  return profile;
}

ExcitationAxes NativeSynth::channel_excitation(const ControllerAxisState& axes,
                                               uint32_t& present) noexcept {
  ExcitationAxes out{};
  present = kAxisNone;
  // The profile decides which controller fills an axis; this maps each axis to its slot.
  for (size_t i = 0; i < kControllerAxisCount; ++i) {
    const ControllerAxis axis = static_cast<ControllerAxis>(i);
    const uint32_t mask = excitation_axis_mask(axis);
    if (mask == kAxisNone || !axes.has(axis)) continue;
    store_excitation_value(out, axis, axes.values[i]);
    present |= mask;
  }
  return out;
}

void NativeSynth::push_excitation_control(uint8_t channel, uint32_t changed_mask,
                                          bool mpe_dimension, MpeDimension dimension) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  uint32_t present = kAxisNone;
  const ExcitationAxes base = channel_excitation(st.axes, present);
  present &= changed_mask;
  for (NativeSynthVoice& v : pool_) {
    if (!v.active || (mpe_dimension && !v.key_down) || v.channel != ch || v.patch == nullptr) {
      continue;
    }
    // The axes override the preset only once a controller has reached them, so
    // a channel nothing has bound leaves every voice on its own voicing.
    if (present == kAxisNone) continue;
    for (size_t i = 0; i < kControllerAxisCount; ++i) {
      const ControllerAxis axis = static_cast<ControllerAxis>(i);
      const uint32_t engine_bit = excitation_axis_mask(axis);
      if ((present & engine_bit) == 0u) continue;
      const uint32_t bit = 1u << static_cast<uint32_t>(i);
      v.live_excitation_axes.set(axis, excitation_value(base, axis));
      if (!mpe_dimension) {
        v.live_mpe_pressure_axes &= ~bit;
        v.live_mpe_timbre_axes &= ~bit;
        v.live_mpe_bend_axes &= ~bit;
        continue;
      }
      switch (dimension) {
        case MpeDimension::kPressure:
          v.live_mpe_pressure_axes |= bit;
          v.live_mpe_timbre_axes &= ~bit;
          v.live_mpe_bend_axes &= ~bit;
          break;
        case MpeDimension::kTimbre:
          v.live_mpe_timbre_axes |= bit;
          v.live_mpe_pressure_axes &= ~bit;
          v.live_mpe_bend_axes &= ~bit;
          break;
        case MpeDimension::kBend:
          v.live_mpe_bend_axes |= bit;
          v.live_mpe_pressure_axes &= ~bit;
          v.live_mpe_timbre_axes &= ~bit;
          break;
      }
    }
    v.push_excitation(base, present);
  }
}

void NativeSynth::restore_excitation_control(uint8_t channel, bool mpe_dimension) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  for (NativeSynthVoice& v : pool_) {
    if (v.active && (!mpe_dimension || v.key_down) && v.channel == ch && v.patch != nullptr) {
      v.restore_excitation_base();
      v.live_excitation_axes.reset();
      v.live_mpe_pressure_axes = kAxisNone;
      v.live_mpe_timbre_axes = kAxisNone;
      v.live_mpe_bend_axes = kAxisNone;
      v.mpe_member_pressure = Control32::from_raw(0);
      v.mpe_member_timbre = Control32::from_raw(0);
      v.mpe_member_pressure_present = false;
      v.mpe_member_timbre_present = false;
    }
  }
}

bool NativeSynth::set_controller_profile(const ControllerProfile& profile) noexcept {
  controller_profile_ = profile;
  for (uint8_t ch = 0; ch < 16; ++ch) {
    channels_[ch].axes.reset();
    channels_[ch].mpe_pressure_axes = kAxisNone;
    channels_[ch].mpe_timbre_axes = kAxisNone;
    channels_[ch].mpe_bend_axes = kAxisNone;
    refresh_channel_mod(ch);
    restore_excitation_control(ch);
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
  const Sf2ChannelMod& mod = channel_mods_[ch];
  const float member_bend_cents =
      (mpe_.bend_semitones(ch) - mpe_.manager_bend_semitones(ch)) * 100.0f;
  const uint8_t manager = mpe_.role(ch) == MpeChannelRole::kMember
                              ? (mpe_.zone_of(ch) == MpeZone::kLower ? kMpeLowerManagerChannel
                                                                     : kMpeUpperManagerChannel)
                              : ch;
  const float manager_pressure01 = mpe_.pressure(manager) / 127.0f;

  for (NativeSynthVoice& v : pool_) {
    if (!v.active || v.channel != ch) continue;
    if (v.mpe_release_frozen) {
      refresh_frozen_mpe_voice(v, ch);
      continue;
    }
    if (bend_note == kControllerAnyNote && pressure_note == kControllerAnyNote) {
      v.mpe_mod_active = false;
    }
  }
  if (bend_note == kControllerAnyNote && pressure_note == kControllerAnyNote) {
    return;
  }
  // What a note the value was not attributed to keeps: the manager's
  // contribution, which reaches every note in the zone whatever the tracking
  // rule says about the member's own.
  for (NativeSynthVoice& v : pool_) {
    if (!v.active || !v.key_down || v.channel != ch || v.mpe_release_frozen) continue;
    const bool takes_bend = bend_note == kControllerAnyNote || v.note == bend_note;
    const bool takes_pressure = pressure_note == kControllerAnyNote || v.note == pressure_note;
    v.mpe_mod_active = !takes_bend || !takes_pressure;
    if (!v.mpe_mod_active) continue;
    v.mpe_mod = mod;
    if (!takes_bend) v.mpe_mod.pitch_cents -= member_bend_cents;
    if (!takes_pressure) v.mpe_mod.aftertouch01 = manager_pressure01;
  }
}

uint32_t NativeSynth::mpe_profile_axis_mask(MpeDimension dimension) const noexcept {
  uint32_t mask = kAxisNone;
  for (size_t i = 0; i < controller_profile_.binding_count(); ++i) {
    const ControllerBinding& binding = controller_profile_.binding_at(i);
    const bool bend =
        dimension == MpeDimension::kBend && binding.input == ControllerInput::kPitchBend;
    const bool pressure =
        dimension == MpeDimension::kPressure && binding.input == ControllerInput::kChannelPressure;
    const bool timbre = dimension == MpeDimension::kTimbre &&
                        binding.input == ControllerInput::kControlChange &&
                        binding.index == kMpeTimbreCc;
    if (bend || pressure || timbre) mask |= 1u << static_cast<uint32_t>(binding.axis);
  }
  return mask;
}

void NativeSynth::freeze_mpe_voice(NativeSynthVoice& voice, uint8_t channel) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  if (mpe_.role(ch) != MpeChannelRole::kMember) return;

  const Sf2ChannelMod& current = channel_mods_[ch];
  const Sf2ChannelMod effective = voice.mpe_mod_active ? voice.mpe_mod : current;
  voice.mpe_mod = effective;
  voice.mpe_mod_active = true;
  voice.mpe_release_frozen = true;
  voice.mpe_frozen_axis_mask = mpe_profile_axis_mask(MpeDimension::kBend) |
                               mpe_profile_axis_mask(MpeDimension::kPressure) |
                               mpe_profile_axis_mask(MpeDimension::kTimbre);
  voice.mpe_frozen_axis_present = channels_[ch].axes.present & voice.mpe_frozen_axis_mask;
  voice.mpe_frozen_source_mask = (channels_[ch].mpe_pressure_axes | channels_[ch].mpe_timbre_axes |
                                  channels_[ch].mpe_bend_axes) &
                                 voice.mpe_frozen_axis_mask;
  voice.mpe_frozen_axis_values = {};
  for (size_t i = 0; i < kControllerAxisCount; ++i) {
    const uint32_t bit = 1u << static_cast<uint32_t>(i);
    const ControllerAxis axis = static_cast<ControllerAxis>(i);
    if (excitation_axis_mask(axis) != kAxisNone) {
      // Attributed engine axes live on the voice, not its channel template.
      voice.mpe_frozen_axis_present &= ~bit;
      voice.mpe_frozen_source_mask &= ~bit;
      if (voice.live_excitation_axes.has(axis)) {
        voice.mpe_frozen_axis_present |= bit;
        voice.mpe_frozen_axis_values[i] = voice.live_excitation_axes.values[i];
      }
      if (((voice.live_mpe_pressure_axes | voice.live_mpe_timbre_axes | voice.live_mpe_bend_axes) &
           bit) != 0u) {
        voice.mpe_frozen_source_mask |= bit;
      }
      continue;
    }
    if ((voice.mpe_frozen_axis_present & bit) != 0u) {
      voice.mpe_frozen_axis_values[i] = channels_[ch].axes.values[i];
    }
  }
  voice.mpe_frozen_aftertouch01 = effective.aftertouch01;
  // The member's own bend: the effective pitch less the channel's other bend and the manager's.
  voice.mpe_frozen_member_bend_cents = effective.pitch_cents -
                                       (current.pitch_cents - mpe_.bend_semitones(ch) * 100.0f) -
                                       mpe_.manager_bend_semitones(ch) * 100.0f;
}

void NativeSynth::refresh_frozen_mpe_voice(NativeSynthVoice& voice, uint8_t channel) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& state = channels_[ch];
  const Sf2ChannelMod& current = channel_mods_[ch];
  const float frozen_bend01 = voice.mpe_mod.pitch_bend01;
  voice.mpe_mod = current;
  voice.mpe_mod.pitch_bend01 = frozen_bend01;

  const auto axis_has = [&](ControllerAxis axis) noexcept {
    return (voice.mpe_frozen_axis_mask & (1u << static_cast<uint32_t>(axis))) != 0u;
  };
  const auto axis_present = [&](ControllerAxis axis) noexcept {
    return (voice.mpe_frozen_axis_present & (1u << static_cast<uint32_t>(axis))) != 0u;
  };
  if (axis_has(ControllerAxis::kLoudness)) {
    const float axis =
        axis_present(ControllerAxis::kLoudness)
            ? voice.mpe_frozen_axis_values[static_cast<size_t>(ControllerAxis::kLoudness)]
            : 1.0f;
    voice.mpe_mod.gain = sf2_cc_gain(state.volume) * sf2_cc_gain(state.expression) * axis;
  }
  if (axis_has(ControllerAxis::kPitchCents)) {
    const float current_axis =
        state.axes.has(ControllerAxis::kPitchCents)
            ? state.axes.values[static_cast<size_t>(ControllerAxis::kPitchCents)]
            : 0.0f;
    const float frozen_axis =
        axis_present(ControllerAxis::kPitchCents)
            ? voice.mpe_frozen_axis_values[static_cast<size_t>(ControllerAxis::kPitchCents)]
            : 0.0f;
    voice.mpe_mod.pitch_cents += frozen_axis - current_axis;
  }
  if (axis_has(ControllerAxis::kVibratoDepth)) {
    const float current_axis =
        state.axes.has(ControllerAxis::kVibratoDepth)
            ? state.axes.values[static_cast<size_t>(ControllerAxis::kVibratoDepth)]
            : 0.0f;
    const float frozen_axis =
        axis_present(ControllerAxis::kVibratoDepth)
            ? voice.mpe_frozen_axis_values[static_cast<size_t>(ControllerAxis::kVibratoDepth)]
            : 0.0f;
    voice.mpe_mod.extra_vibrato_cents += frozen_axis - current_axis;
  }

  const float current_member_bend_cents =
      (mpe_.bend_semitones(ch) - mpe_.manager_bend_semitones(ch)) * 100.0f;
  voice.mpe_mod.pitch_cents += voice.mpe_frozen_member_bend_cents - current_member_bend_cents;
  voice.mpe_mod.aftertouch01 = voice.mpe_frozen_aftertouch01;
  voice.mpe_mod_active = true;
}

void NativeSynth::update_frozen_mpe_axis(uint8_t channel, ControllerAxis axis,
                                         float value) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  const uint32_t bit = 1u << static_cast<uint32_t>(axis);
  for (NativeSynthVoice& voice : pool_) {
    if (!voice.active || !voice.mpe_release_frozen || voice.channel != ch ||
        (voice.mpe_frozen_axis_mask & bit) == 0u) {
      continue;
    }
    voice.mpe_frozen_axis_present |= bit;
    voice.mpe_frozen_axis_values[static_cast<size_t>(axis)] = value;
    voice.mpe_frozen_source_mask &= ~bit;
  }
}

void NativeSynth::apply_mpe_channel_axes(uint8_t channel, MpeDimension dimension,
                                         Control32 combined) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  const Ump message = dimension == MpeDimension::kPressure
                          ? make_midi2_channel_pressure(0, ch, combined.raw)
                          : make_midi2_control_change(0, ch, kMpeTimbreCc, combined.raw);
  std::array<ControllerAxisValue, kMaxControllerBindings> resolved{};
  const size_t count = controller_profile_.resolve(message, resolved.data(), resolved.size());
  ChannelState& state = channels_[ch];
  for (size_t i = 0; i < count; ++i) {
    const ControllerAxisValue& value = resolved[i];
    state.axes.set(value.axis, value.value);
    const uint32_t bit = 1u << static_cast<uint32_t>(value.axis);
    switch (dimension) {
      case MpeDimension::kPressure:
        state.mpe_pressure_axes |= bit;
        state.mpe_timbre_axes &= ~bit;
        state.mpe_bend_axes &= ~bit;
        break;
      case MpeDimension::kTimbre:
        state.mpe_timbre_axes |= bit;
        state.mpe_pressure_axes &= ~bit;
        state.mpe_bend_axes &= ~bit;
        break;
      case MpeDimension::kBend:
        state.mpe_bend_axes |= bit;
        state.mpe_pressure_axes &= ~bit;
        state.mpe_timbre_axes &= ~bit;
        break;
    }
  }
  if (count != 0) refresh_channel_mod(ch);
}

void NativeSynth::apply_mpe_voice_axes(NativeSynthVoice& voice, uint8_t channel,
                                       MpeDimension dimension, Control32 combined,
                                       bool update_member_raw, Control32 member_raw,
                                       bool member_present) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  if (update_member_raw) {
    if (dimension == MpeDimension::kPressure) {
      voice.mpe_member_pressure = member_raw;
      voice.mpe_member_pressure_present = member_present;
    } else if (dimension == MpeDimension::kTimbre) {
      voice.mpe_member_timbre = member_raw;
      voice.mpe_member_timbre_present = member_present;
    }
  }
  const Ump message = dimension == MpeDimension::kPressure
                          ? make_midi2_channel_pressure(0, ch, combined.raw)
                          : make_midi2_control_change(0, ch, kMpeTimbreCc, combined.raw);
  std::array<ControllerAxisValue, kMaxControllerBindings> resolved{};
  const size_t count = controller_profile_.resolve(message, resolved.data(), resolved.size());
  uint32_t present = kAxisNone;
  ExcitationAxes axes{};
  for (size_t i = 0; i < count; ++i) {
    const ControllerAxisValue& value = resolved[i];
    const uint32_t engine_bit = excitation_axis_mask(value.axis);
    if (engine_bit == kAxisNone) continue;
    const uint32_t axis_bit = 1u << static_cast<uint32_t>(value.axis);
    store_excitation_value(axes, value.axis, value.value);
    present |= engine_bit;
    voice.live_excitation_axes.set(value.axis, value.value);
    if (dimension == MpeDimension::kPressure) {
      voice.live_mpe_pressure_axes |= axis_bit;
      voice.live_mpe_timbre_axes &= ~axis_bit;
      voice.live_mpe_bend_axes &= ~axis_bit;
    } else {
      voice.live_mpe_timbre_axes |= axis_bit;
      voice.live_mpe_pressure_axes &= ~axis_bit;
      voice.live_mpe_bend_axes &= ~axis_bit;
    }
  }
  if (present != kAxisNone) voice.push_excitation(axes, present);
}

void NativeSynth::push_mpe_controller_axis(uint8_t channel, MpeDimension dimension) noexcept {
  const MpeChannelRole channel_role = mpe_.role(channel);
  if (channel_role == MpeChannelRole::kUnassigned) return;
  Control32 own = Control32::from_raw(0);
  const bool own_present = mpe_.own_control(channel, dimension, &own);
  if (channel_role == MpeChannelRole::kMember) {
    const Control32 combined = dimension == MpeDimension::kPressure ? mpe_.pressure_control(channel)
                                                                    : mpe_.timbre_control(channel);
    apply_mpe_channel_axes(channel, dimension, combined);
    const uint8_t target = mpe_attributed_note(channel, dimension);
    for (NativeSynthVoice& voice : pool_) {
      if (!voice.active || !voice.key_down || voice.channel != (channel & 0x0Fu) ||
          (target != kControllerAnyNote && voice.note != target)) {
        continue;
      }
      apply_mpe_voice_axes(voice, channel, dimension, combined, true, own, own_present);
    }
    return;
  }

  // The manager biases every member voice, each combined with its own value (2.2.7, 2.2.8).
  const Control32 manager = own;
  apply_mpe_channel_axes(channel, dimension, manager);
  for (NativeSynthVoice& voice : pool_) {
    if (!voice.active || voice.channel != (channel & 0x0Fu)) continue;
    apply_mpe_voice_axes(voice, channel, dimension, manager, false, {}, false);
  }
  const MpeZone zone = mpe_.zone_of(channel);
  for (uint8_t member = 0; member < 16; ++member) {
    if (mpe_.role(member) != MpeChannelRole::kMember || mpe_.zone_of(member) != zone) continue;
    Control32 member_own = Control32::from_raw(0);
    const bool member_own_present = mpe_.own_control(member, dimension, &member_own);
    const Control32 combined =
        MpeState::combine_control(member_own, member_own_present, manager, own_present);
    apply_mpe_channel_axes(member, dimension, combined);
    for (NativeSynthVoice& voice : pool_) {
      if (!voice.active || !voice.key_down || voice.channel != member) continue;
      Control32 voice_own = member_own;
      bool voice_own_present = member_own_present;
      if (dimension == MpeDimension::kPressure) {
        voice_own = voice.mpe_member_pressure;
        voice_own_present = voice.mpe_member_pressure_present;
      } else {
        voice_own = voice.mpe_member_timbre;
        voice_own_present = voice.mpe_member_timbre_present;
      }
      const Control32 voice_combined =
          MpeState::combine_control(voice_own, voice_own_present, manager, own_present);
      apply_mpe_voice_axes(voice, member, dimension, voice_combined, false, {}, false);
    }
  }
}

void NativeSynth::track_mpe_input(const Ump& ump) noexcept {
  MpeDimension dimension = MpeDimension::kPressure;
  // Bend is tracked where the protocol dispatch decodes it, at its own width.
  if (!mpe_dimension_of(ump, &dimension) || dimension == MpeDimension::kBend) return;
  const bool midi1 = ump.message_type() == UmpMessageType::kMidi1ChannelVoice;
  const uint8_t ch = ump.channel() & 0x0Fu;
  channels_[ch].last_mpe_controller = dimension;
  if (dimension == MpeDimension::kPressure) {
    mpe_.track_pressure(
        ch, midi1 ? Control32::from7(ump.note_number()) : Control32::from_raw(ump.words[1]));
  } else {
    mpe_.track_timbre(
        ch, midi1 ? Control32::from7(ump.data2_7bit()) : Control32::from_raw(ump.words[1]));
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

ExcitationAxes NativeSynth::apply_note_on_controller_input(const Ump& ump,
                                                           uint32_t* out_mask) noexcept {
  ExcitationAxes velocity_axes{};
  if (out_mask == nullptr) return velocity_axes;
  *out_mask = kAxisNone;

  std::array<ControllerAxisValue, kMaxControllerBindings> resolved{};
  const size_t count = controller_profile_.resolve(ump, resolved.data(), resolved.size());
  if (count == 0) return velocity_axes;
  const uint8_t ch = ump.channel() & 0x0Fu;
  bool channel_moved = false;
  for (size_t i = 0; i < count; ++i) {
    const ControllerAxisValue& value = resolved[i];
    const uint32_t excitation = excitation_axis_mask(value.axis);
    if (excitation != kAxisNone) {
      store_excitation_value(velocity_axes, value.axis, value.value);
      *out_mask |= excitation;
      continue;
    }
    channels_[ch].axes.set(value.axis, value.value);
    const uint32_t bit = 1u << static_cast<uint32_t>(value.axis);
    channels_[ch].mpe_pressure_axes &= ~bit;
    channels_[ch].mpe_timbre_axes &= ~bit;
    channels_[ch].mpe_bend_axes &= ~bit;
    update_frozen_mpe_axis(ch, value.axis, value.value);
    channel_moved = true;
  }
  if (channel_moved) refresh_channel_mod(ch);
  return velocity_axes;
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
  const bool has_mpe_dimension = mpe_dimension_of(ump, &dimension);
  const bool mpe_dimension = mpe_.role(ch) == MpeChannelRole::kMember && has_mpe_dimension;
  const uint8_t attributed =
      has_mpe_dimension ? mpe_attributed_note(ch, dimension) : kControllerAnyNote;
  bool channel_moved = false;
  uint32_t channel_changed_mask = kAxisNone;
  for (size_t i = 0; i < count; ++i) {
    ControllerAxisValue value = resolved[i];
    if (value.note == kControllerAnyNote && attributed != kControllerAnyNote &&
        controller_axis_is_excitation(value.axis)) {
      value.note = attributed;
    }
    if (value.note == kControllerAnyNote) {
      channels_[ch].axes.set(value.axis, value.value);
      const uint32_t bit = 1u << static_cast<uint32_t>(value.axis);
      if (mpe_dimension) {
        switch (dimension) {
          case MpeDimension::kBend:
            channels_[ch].mpe_bend_axes |= bit;
            channels_[ch].mpe_pressure_axes &= ~bit;
            channels_[ch].mpe_timbre_axes &= ~bit;
            break;
          case MpeDimension::kPressure:
            channels_[ch].mpe_pressure_axes |= bit;
            channels_[ch].mpe_bend_axes &= ~bit;
            channels_[ch].mpe_timbre_axes &= ~bit;
            break;
          case MpeDimension::kTimbre:
            channels_[ch].mpe_timbre_axes |= bit;
            channels_[ch].mpe_pressure_axes &= ~bit;
            channels_[ch].mpe_bend_axes &= ~bit;
            break;
        }
      } else {
        channels_[ch].mpe_pressure_axes &= ~bit;
        channels_[ch].mpe_timbre_axes &= ~bit;
        channels_[ch].mpe_bend_axes &= ~bit;
        update_frozen_mpe_axis(ch, value.axis, value.value);
      }
      channel_moved = true;
      channel_changed_mask |= excitation_axis_mask(value.axis);
      continue;
    }
    // A per-note value reaches the voices on that note and no further. It is
    // not stored on the channel, so a later channel-wide value on the same axis
    // overwrites it in those voices: per-voice controller state is what MPE
    // adds, and until then the precedence is simply last writer wins.
    const uint32_t present = excitation_axis_mask(value.axis);
    const uint32_t axis_bit = 1u << static_cast<uint32_t>(value.axis);
    ExcitationAxes axes{};
    store_excitation_value(axes, value.axis, value.value);
    if (present == kAxisNone) continue;
    for (NativeSynthVoice& v : pool_) {
      if (v.active && (!mpe_dimension || v.key_down) && v.note == value.note && v.channel == ch &&
          v.patch != nullptr) {
        v.live_excitation_axes.set(value.axis, value.value);
        if (!mpe_dimension) {
          v.live_mpe_pressure_axes &= ~axis_bit;
          v.live_mpe_timbre_axes &= ~axis_bit;
          v.live_mpe_bend_axes &= ~axis_bit;
        } else {
          switch (dimension) {
            case MpeDimension::kBend:
              v.live_mpe_bend_axes |= axis_bit;
              v.live_mpe_pressure_axes &= ~axis_bit;
              v.live_mpe_timbre_axes &= ~axis_bit;
              break;
            case MpeDimension::kPressure:
              v.live_mpe_pressure_axes |= axis_bit;
              v.live_mpe_timbre_axes &= ~axis_bit;
              v.live_mpe_bend_axes &= ~axis_bit;
              break;
            case MpeDimension::kTimbre:
              v.live_mpe_timbre_axes |= axis_bit;
              v.live_mpe_pressure_axes &= ~axis_bit;
              v.live_mpe_bend_axes &= ~axis_bit;
              break;
          }
        }
        v.push_excitation(axes, present);
      }
    }
  }
  if (!channel_moved) return;
  refresh_channel_mod(ch);
  push_excitation_control(ch, channel_changed_mask, mpe_dimension, dimension);
}

float NativeSynth::part_controller_position(int part, uint8_t source) const noexcept {
  // Read at full width, as refresh_channel_mod reads the same controllers.
  const ChannelState& st = channels_[static_cast<size_t>(part & 0x0F)];
  if (source == kEfxSourceBend) return (st.pitch_bend.f14() - 8192.0f) / 8192.0f;
  if (source == kEfxSourceAftertouch) return st.pressure.f7() / 127.0f;
  return st.cc_position[source & 0x7Fu].f7() / 127.0f;
}

void NativeSynth::refresh_part_rig(uint8_t channel) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  uint8_t id = 0;
  // Only GM playback resolves a program to a bank voice, so only it binds a rig.
  if (config_.use_gm_programs) {
    const ChannelState& st = channels_[ch];
    id = gm_fallback_rig(gs_effective_bank(st.bank_msb, st.bank_lsb, st.drums), st.program).id;
  }
  // Live, the rig the part had stays until the control thread next rebuilds.
  if (part_fx_.publish_part_rig(ch, id) && config_.realize_efx_inline) part_fx_.mark_dirty();
}

void NativeSynth::apply_efx_sysex(const uint8_t* data, size_t size) noexcept {
  const GsSysEx msg = parse_gs_sysex(data, size);
  if (gs_sysex_resets(msg.kind)) {
    part_fx_.clear_efx();
    return;
  }
  if (msg.kind == GsSysExKind::kEfxPartSwitch) {
    part_fx_.assign_part(msg.channel, msg.value);
    return;
  }
  (void)part_fx_.apply_unit_sysex(data, size);
}

void NativeSynth::realize_part_fx() {
  part_fx_.clear_dirty();
  if (prepared_) part_fx_.publish();
}

void NativeSynth::on_control_sysex(const uint8_t* data, size_t size) noexcept {
  // Offline the event stream is the EFX writer; there must be only one.
  if (!prepared_ || data == nullptr || size == 0 || !config_.use_gm_programs ||
      config_.realize_efx_inline || !part_fx_.enabled()) {
    return;
  }
  const GsSysEx msg = parse_gs_sysex(data, size);
  if (!gs_sysex_resets(msg.kind) && msg.kind != GsSysExKind::kEfxPartSwitch &&
      gs_efx_addressed_unit(data, size) < 0) {
    return;
  }
  const PartFxStage::Checkpoint before = part_fx_.checkpoint();
  try {
    if (part_fx_.apply_control_sysex(data, size)) {
      part_fx_.clear_dirty();
      part_fx_.publish();
    }
  } catch (...) {
    // Translation allocates; keep the published generation and its mirror.
    part_fx_.restore(before);
  }
}

bool NativeSynth::set_part_rig(uint8_t part, const PartRig& rig) noexcept {
  if (!validate_part_rig(part, rig)) return false;
  try {
    PartFxStage::RigTable previous = part_fx_.rig_table();
    if (!part_fx_.set_part_rig(part, rig)) return false;
    if (prepared_) {
      try {
        part_fx_.publish();
      } catch (...) {
        part_fx_.restore_rig_table(std::move(previous));
        throw;
      }
    }
  } catch (...) {
    return false;
  }
  return true;
}

std::vector<std::string> NativeSynth::part_rig_stage_names(uint8_t part) const {
  const PartFxSnapshot* snapshot = part_fx_.control_current();
  if (snapshot == nullptr || part >= 16) return {};
  return snapshot->stage_names[part];
}

void NativeSynth::set_transport(const transport::TransportState& state) noexcept {
  if (path_recorder_ != nullptr) path_recorder_->set_block_frame(state.render_frame);
}

bool NativeSynth::set_render_path_recorder(RenderPathRecorder* recorder) noexcept {
  if (recorder != nullptr && !config_.realize_efx_inline) return false;
  path_recorder_ = recorder;
  part_fx_.set_path_recorder(recorder);
  return true;
}

void NativeSynth::record_render_path() {
  RenderPathRecorder& recorder = *path_recorder_;
  if (!recorder.recording()) return;
  const PartFxSnapshot* snapshot = part_fx_.enabled() ? part_fx_.current() : nullptr;
  RenderPathTopology topology;
  topology.parts.resize(16);
  for (uint8_t part = 0; part < 16; ++part) {
    const ChannelState& st = channels_[part];
    RenderPathPart& out = topology.parts[part];
    out.part = part;
    out.program = st.program;
    out.bank = gs_effective_bank(st.bank_msb, st.bank_lsb, st.drums);
    out.backend = "model";
    out.rig_source = part_fx_.rig_source_name(part);
    if (snapshot != nullptr) out.stages = snapshot->stage_names[part];
    out.skipped = recorder.refused_stages(part);
    const uint8_t unit = snapshot != nullptr ? snapshot->part_unit[part] : PartFxSnapshot::kNoUnit;
    out.unit = unit == PartFxSnapshot::kNoUnit ? -1 : static_cast<int>(unit);
    out.mono_prefix = snapshot != nullptr ? snapshot->mono_prefix[part] : 0;
    // No system effects here, so nothing taps a send.
    out.send_tap = "none";
  }
  for (size_t u = 0; snapshot != nullptr && u < kGsEfxUnitCount; ++u) {
    if (!snapshot->unit_fed[u]) continue;
    const Sf2EfxUnitRt& unit_rt = snapshot->units[u];
    RenderPathUnit out;
    out.unit = static_cast<uint8_t>(u);
    out.type = snapshot->gs_efx_state[u].type;
    out.realization = unit_rt.realization;
    for (const Sf2EfxStageRt& stage : unit_rt.stages) {
      out.stages.push_back(stage.name);
      out.enabled.push_back(stage.enabled_target);
      if (stage.proc == nullptr) out.skipped.push_back(stage.name);
    }
    topology.units.push_back(std::move(out));
  }
  recorder.record_topology(std::move(topology));
}

}  // namespace sonare::midi::synth
