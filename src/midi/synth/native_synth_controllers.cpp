/// @file native_synth_controllers.cpp
/// @brief NativeSynth controller handling: pedals, channel modes, pressure, controller reset,
///        bend range, Control Change and the registered / relative controller messages.

#include <algorithm>
#include <array>
#include <cmath>

#include "midi/controller_numbers.h"
#include "midi/synth/articulation.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "util/constants.h"

namespace sonare::midi::synth {

namespace {

/// Registered Controller 0/0 in bank 0: pitch bend sensitivity.
constexpr uint8_t kRcPitchBendSensitivity = 0;
/// Registered Controller 0/7 in bank 0: per-note pitch bend sensitivity.
constexpr uint8_t kRcPerNoteBendSensitivity = 7;

using native_synth_detail::excitation_axis_mask;

}  // namespace

void NativeSynth::sustain_cc(uint8_t channel, Control32 value) noexcept {
  const uint16_t scope = mpe_.channels_in_scope(channel);
  for (uint8_t ch = 0; ch < 16; ++ch) {
    if ((scope & (uint16_t{1} << ch)) != 0) sustain_channel(ch, value);
  }
}

void NativeSynth::sustain_channel(uint8_t channel, Control32 value) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  const bool was_down = st.sustain;
  st.sustain_level = value;
  st.sustain = value.u7() >= 64;
  if (st.sustain) {
    // Half-pedal: a partially raised damper still rests on the strings, so held
    // (key-up) notes ring on at an intermediate rate; a full lift (127) leaves
    // them ringing freely. Key-down and sostenuto-captured notes keep their
    // dampers mechanically off, so they are untouched.
    const float level = value.f7();
    const float strength = (127.0f - level) / 63.0f;
    for (NativeSynthVoice& v : pool_) {
      if (v.active && v.channel == ch && !v.key_down && !v.releasing && !v.sostenuto &&
          v.patch != nullptr && v.patch->mode == SynthEngineMode::kPiano) {
        v.piano.damp(strength);
      }
    }
    return;
  }
  if (!was_down) return;
  for (NativeSynthVoice& v : pool_) {
    // A sostenuto-captured note stays held even when the sustain pedal lifts.
    if (v.active && v.channel == ch && !v.key_down && !v.releasing && !v.sostenuto) v.release();
  }
}

void NativeSynth::sostenuto_pedal(uint8_t channel, bool down) noexcept {
  const uint16_t scope = mpe_.channels_in_scope(channel);
  for (uint8_t ch = 0; ch < 16; ++ch) {
    if ((scope & (uint16_t{1} << ch)) != 0) sostenuto_channel(ch, down);
  }
}

void NativeSynth::sostenuto_channel(uint8_t channel, bool down) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  // Edge-triggered: only a change of pedal position captures or releases.
  if (channels_[ch].sostenuto_down == down) return;
  channels_[ch].sostenuto_down = down;
  for (NativeSynthVoice& v : pool_) {
    if (!v.active || v.channel != ch) continue;
    if (down) {
      // Capture only the notes whose keys are down at the moment of the press.
      if (v.key_down) v.sostenuto = true;
    } else if (v.sostenuto) {
      v.sostenuto = false;
      if (!v.key_down && !v.releasing) {
        if (!channels_[ch].sustain) {
          v.release();
        } else if (v.patch != nullptr && v.patch->mode == SynthEngineMode::kPiano) {
          const float strength = (127.0f - channels_[ch].sustain_level.f7()) / 63.0f;
          v.piano.damp(strength);
        }
      }
    }
  }
}

void NativeSynth::all_notes_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  // A released key left in the legato stack would be retuned back to later.
  st.held_count = 0;
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.channel == ch && !v.releasing) {
      if (v.key_down) freeze_mpe_voice(v, ch);
      v.key_down = false;
      // All Notes Off is a key-up gesture. Sustain and sostenuto still hold a
      // captured voice, exactly as they do for an ordinary note-off.
      if (v.sostenuto) continue;
      if (!st.sustain) {
        v.release();
      } else if (st.sustain_level.f7() < 127.0f && v.patch != nullptr &&
                 v.patch->mode == SynthEngineMode::kPiano) {
        v.piano.damp((127.0f - st.sustain_level.f7()) / 63.0f);
      }
    }
  }
  recharge_percussion(ch);
}

void NativeSynth::all_sound_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].held_count = 0;
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.channel == ch) v.kill();
  }
  recharge_percussion(ch);
  // A part's factory-backed physical-model state belongs to the channel that
  // was silenced. Clear it even when another part still has voices, so an
  // All Sound Off on one part cannot leave a board, swell filter, or wind
  // chest tail audible on a later note.
  if (part_fx_.enabled()) {
    part_soundboards_[ch].reset();
    part_resonance_[ch].reset();
  }
  part_piano_splitters_[ch].reset();
  part_organ_splitters_[ch].reset();
  part_guitar_splitters_[ch].reset();
  part_piano_state_[ch] = {};
  part_halo_[ch].reset();
  part_halo_armed_[ch] = false;
  part_wind_[ch].reset();
  organ_part_params_[ch] = {};
  part_swell_depth_[ch] = 0.0f;
  part_swell_coeff_[ch] = 1.0f;
  part_swell_lp_l_[ch] = 0.0f;
  part_swell_lp_r_[ch] = 0.0f;
  bus_fed_channels_ &= static_cast<uint16_t>(~(uint16_t{1} << ch));
  if (bus_fed_channels_ == 0) {
    // All Sound Off means silence NOW, and the instrument's bus resonators are
    // part of its output: the piano soundboard, the sympathetic bank and the
    // guitar halo ring for ~1.5 s and the swell one-pole holds a residual, so
    // killing the voices alone would leak an audible wash past the stop. They
    // are bus-level (all 16 channels feed one), so they are cleared only once
    // no other channel has fed them. The DC blocker goes with them for
    // the same reason.
    resonance_.reset();
    soundboard_.reset();
    guitar_halo_.reset();
    // An inactive voice does not imply its part's body is silent. Preserve
    // other parts' body state and the shared DC filter while they can ring.
    bool other_body = false;
    for (size_t part = 0; part < 16; ++part) {
      if (part == ch) continue;
      other_body = other_body || (part_fx_.enabled() && part_piano_state_[part].prepared) ||
                   part_halo_armed_[part] || organ_part_params_[part].armed;
    }
    if (!other_body) {
      dc_x1_ = {};
      dc_y1_ = {};
    }
  }
}

void NativeSynth::channel_pressure(uint8_t channel, Control32 pressure) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].pressure = pressure;
  // A manager's pressure is a bias on every member of its zone, so it reaches
  // further than the channel it arrived on (2.2.7).
  refresh_channel_mods_in_scope(ch);
}

void NativeSynth::poly_pressure(uint8_t channel, uint8_t note, Control32 pressure) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  // Prohibited on a member channel, where pressure is the channel's and belongs
  // to the one note living on it (2.2.7).
  if (mpe_.ignores(ch, MpeIgnorable::kPolyKeyPressure)) return;
  const float value = pressure.f7() / 127.0f;
  // Every sounding voice on the note takes it: a layered patch is several
  // voices of one key, and pressure is a property of the key.
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.note == note && v.channel == ch) v.poly_pressure01 = value;
  }
}

void NativeSynth::reset_controllers(uint8_t channel) noexcept {
  // MIDI RP-015: reset performance controllers, keep volume/pan.
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  st.mod_wheel = Control32::from7(0);
  st.expression = Control32::from7(127);
  st.pitch_bend = Bend32::center();
  st.breath = Control32::from7(0);
  st.pressure = Control32::from7(0);
  for (const size_t controller :
       {size_t{cc::kModulationWheel}, size_t{cc::kBreathController}, size_t{cc::kDamperPedal},
        size_t{cc::kPortamentoOnOff}, size_t{cc::kSostenuto}, size_t{cc::kSoftPedal}}) {
    st.cc_position[controller] = Control32::from7(0);
  }
  for (const size_t controller : {size_t{cc::kExpression}, size_t{cc::kNrpnLsb},
                                  size_t{cc::kNrpnMsb}, size_t{cc::kRpnLsb}, size_t{cc::kRpnMsb}}) {
    st.cc_position[controller] = Control32::from7(gs_default_cc_positions()[controller]);
  }
  for (NativeSynthVoice& v : pool_) {
    if (v.channel == ch) v.poly_pressure01 = 0.0f;
  }
  const uint32_t previous_pressure_axes = st.mpe_pressure_axes;
  const uint32_t previous_timbre_axes = st.mpe_timbre_axes;
  st.axes.reset();
  st.mpe_pressure_axes = kAxisNone;
  st.mpe_timbre_axes = kAxisNone;
  st.mpe_bend_axes = kAxisNone;
  st.params.reset();
  // Inside a zone the same three controllers are tracked by the zone model,
  // which is where every reader of them takes their combined value -- leaving
  // them here would reset the channel and change nothing that is heard.
  mpe_.reset_controls(static_cast<uint16_t>(uint16_t{1} << ch));
  sustain_channel(ch, Control32::from7(0));
  sostenuto_channel(ch, false);
  st.una_corda = false;
  if (mpe_.role(ch) == MpeChannelRole::kManager) {
    // The manager's values were a bias on every member, so each member has to
    // resolve its own again without them.
    const auto resolve_candidate = [&](uint8_t member, MpeDimension dimension, Control32 own,
                                       bool present) noexcept {
      ControllerAxisState candidate;
      if (!present) return candidate;
      const Ump message = dimension == MpeDimension::kPressure
                              ? make_midi2_channel_pressure(0, member, own.raw)
                              : make_midi2_control_change(0, member, kMpeTimbreCc, own.raw);
      std::array<ControllerAxisValue, kMaxControllerBindings> resolved{};
      const size_t count = controller_profile_.resolve(message, resolved.data(), resolved.size());
      for (size_t i = 0; i < count; ++i) candidate.set(resolved[i].axis, resolved[i].value);
      return candidate;
    };
    const auto reconcile = [&](ControllerAxisState& axes, uint32_t& pressure_mask,
                               uint32_t& timbre_mask, const ControllerAxisState& pressure,
                               const ControllerAxisState& timbre) noexcept {
      for (size_t i = 0; i < kControllerAxisCount; ++i) {
        const uint32_t bit = 1u << static_cast<uint32_t>(i);
        const ControllerAxis axis = static_cast<ControllerAxis>(i);
        const ControllerAxisState* candidate = (pressure_mask & bit) != 0u ? &pressure
                                               : (timbre_mask & bit) != 0u ? &timbre
                                                                           : nullptr;
        if (candidate == nullptr) continue;
        if (candidate->has(axis)) {
          axes.set(axis, candidate->values[i]);
        } else {
          axes.present &= ~bit;
          axes.values[i] = 0.0f;
          pressure_mask &= ~bit;
          timbre_mask &= ~bit;
        }
      }
    };
    const uint16_t members = static_cast<uint16_t>(mpe_.channels_in_scope(ch) & ~(1u << ch));
    for (uint8_t member = 0; member < 16; ++member) {
      if ((members & (uint16_t{1} << member)) == 0) continue;
      ChannelState& member_state = channels_[member];
      Control32 pressure = Control32::from_raw(0);
      Control32 timbre = Control32::from_raw(0);
      const bool pressure_present = mpe_.own_control(member, MpeDimension::kPressure, &pressure);
      const bool timbre_present = mpe_.own_control(member, MpeDimension::kTimbre, &timbre);
      reconcile(member_state.axes, member_state.mpe_pressure_axes, member_state.mpe_timbre_axes,
                resolve_candidate(member, MpeDimension::kPressure, pressure, pressure_present),
                resolve_candidate(member, MpeDimension::kTimbre, timbre, timbre_present));
      for (NativeSynthVoice& voice : pool_) {
        if (!voice.active || !voice.key_down || voice.channel != member || voice.patch == nullptr) {
          continue;
        }
        // The voice's own captured member input, so a newer note cannot reassign it.
        reconcile(voice.live_excitation_axes, voice.live_mpe_pressure_axes,
                  voice.live_mpe_timbre_axes,
                  resolve_candidate(member, MpeDimension::kPressure, voice.mpe_member_pressure,
                                    voice.mpe_member_pressure_present),
                  resolve_candidate(member, MpeDimension::kTimbre, voice.mpe_member_timbre,
                                    voice.mpe_member_timbre_present));
        voice.restore_excitation_base();
        uint32_t present = kAxisNone;
        const ExcitationAxes live = channel_excitation(voice.live_excitation_axes, present);
        if (present != kAxisNone) voice.push_excitation(live, present);
      }
    }
    restore_excitation_control(ch);
    refresh_all_channel_mods();
  } else {
    if (mpe_.role(ch) != MpeChannelRole::kMember) restore_excitation_control(ch);
    if (mpe_.role(ch) == MpeChannelRole::kMember) {
      // A released tail drops the ordinary values and keeps its NoteOff MPE snapshot.
      for (NativeSynthVoice& voice : pool_) {
        if (!voice.active || voice.channel != ch || voice.key_down || !voice.mpe_release_frozen ||
            voice.patch == nullptr) {
          continue;
        }
        const uint32_t ordinary_frozen = voice.mpe_frozen_axis_mask & ~voice.mpe_frozen_source_mask;
        voice.mpe_frozen_axis_present &= ~ordinary_frozen;
        for (size_t i = 0; i < kControllerAxisCount; ++i) {
          if ((ordinary_frozen & (1u << static_cast<uint32_t>(i))) != 0u) {
            voice.mpe_frozen_axis_values[i] = 0.0f;
          }
        }
        voice.restore_excitation_base();
        voice.live_excitation_axes.reset();
        voice.live_mpe_pressure_axes = kAxisNone;
        voice.live_mpe_timbre_axes = kAxisNone;
        voice.live_mpe_bend_axes = kAxisNone;
        ControllerAxisState frozen_axes;
        for (size_t i = 0; i < kControllerAxisCount; ++i) {
          const ControllerAxis axis = static_cast<ControllerAxis>(i);
          const uint32_t bit = 1u << static_cast<uint32_t>(i);
          if ((voice.mpe_frozen_source_mask & bit) == 0u ||
              (voice.mpe_frozen_axis_present & bit) == 0u ||
              excitation_axis_mask(axis) == kAxisNone) {
            continue;
          }
          frozen_axes.set(axis, voice.mpe_frozen_axis_values[i]);
          voice.live_excitation_axes.set(axis, voice.mpe_frozen_axis_values[i]);
          voice.live_mpe_pressure_axes |= bit;
        }
        uint32_t frozen_present = kAxisNone;
        const ExcitationAxes frozen = channel_excitation(frozen_axes, frozen_present);
        if (frozen_present != kAxisNone) voice.push_excitation(frozen, frozen_present);
      }
    }
    if (mpe_.role(ch) == MpeChannelRole::kMember) {
      const uint8_t manager =
          mpe_.zone_of(ch) == MpeZone::kLower ? kMpeLowerManagerChannel : kMpeUpperManagerChannel;
      const auto candidate = [&](MpeDimension dimension) noexcept {
        ControllerAxisState result;
        Control32 value = Control32::from_raw(0);
        if (!mpe_.own_control(manager, dimension, &value)) return result;
        const Ump message = dimension == MpeDimension::kPressure
                                ? make_midi2_channel_pressure(0, ch, value.raw)
                                : make_midi2_control_change(0, ch, kMpeTimbreCc, value.raw);
        std::array<ControllerAxisValue, kMaxControllerBindings> resolved{};
        const size_t count = controller_profile_.resolve(message, resolved.data(), resolved.size());
        for (size_t i = 0; i < count; ++i) result.set(resolved[i].axis, resolved[i].value);
        return result;
      };
      const ControllerAxisState pressure = candidate(MpeDimension::kPressure);
      const ControllerAxisState timbre = candidate(MpeDimension::kTimbre);
      const auto restore_manager_axes = [&](ControllerAxisState& axes, uint32_t& pressure_mask,
                                            uint32_t& timbre_mask, bool engine_only) noexcept {
        const uint32_t old_pressure = pressure_mask;
        const uint32_t old_timbre = timbre_mask;
        axes.reset();
        pressure_mask = timbre_mask = kAxisNone;
        for (size_t i = 0; i < kControllerAxisCount; ++i) {
          const ControllerAxis axis = static_cast<ControllerAxis>(i);
          if (engine_only && excitation_axis_mask(axis) == kAxisNone) continue;
          const uint32_t bit = 1u << static_cast<uint32_t>(i);
          const ControllerAxisState* selected = nullptr;
          if ((old_pressure & bit) != 0u && pressure.has(axis))
            selected = &pressure;
          else if ((old_timbre & bit) != 0u && timbre.has(axis))
            selected = &timbre;
          else if (channels_[manager].last_mpe_controller == MpeDimension::kPressure &&
                   pressure.has(axis))
            selected = &pressure;
          else if (timbre.has(axis))
            selected = &timbre;
          else if (pressure.has(axis))
            selected = &pressure;
          if (selected == nullptr) continue;
          axes.set(axis, selected->values[i]);
          (selected == &pressure ? pressure_mask : timbre_mask) |= bit;
        }
      };
      // The reset clears the member's contribution, not a surviving manager's source.
      st.mpe_pressure_axes = previous_pressure_axes;
      st.mpe_timbre_axes = previous_timbre_axes;
      restore_manager_axes(st.axes, st.mpe_pressure_axes, st.mpe_timbre_axes, false);
      for (NativeSynthVoice& voice : pool_) {
        if (!voice.active || !voice.key_down || voice.channel != ch) continue;
        voice.restore_excitation_base();
        restore_manager_axes(voice.live_excitation_axes, voice.live_mpe_pressure_axes,
                             voice.live_mpe_timbre_axes, true);
        voice.clear_mpe_member();
        uint32_t present = kAxisNone;
        const ExcitationAxes live = channel_excitation(voice.live_excitation_axes, present);
        if (present != kAxisNone) voice.push_excitation(live, present);
      }
    }
    refresh_channel_mod(ch);
  }
}

void NativeSynth::bend_range_msb(uint8_t channel, uint8_t semitones) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  // Inside a zone the range is the zone's, and a value sent to one member
  // reaches every member of it (2.2.5), so the refresh is zone-wide.
  if (mpe_.apply_bend_sensitivity(ch, static_cast<float>(semitones))) {
    refresh_all_channel_mods();
  } else {
    channels_[ch].bend_range_cents = constants::kCentsPerSemitone * static_cast<float>(semitones);
    refresh_channel_mod(ch);
  }
}

void NativeSynth::bend_range_lsb(uint8_t channel, uint8_t cents) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  // The fractional semitone. MPE recommends senders leave it at zero and
  // permits rather than requires a receiver to answer it, so the zone
  // takes it on the same terms the channel always has.
  if (mpe_.role(ch) != MpeChannelRole::kUnassigned) {
    const float whole = std::floor(mpe_.bend_sensitivity(ch));
    mpe_.apply_bend_sensitivity(ch,
                                whole + static_cast<float>(cents) / constants::kCentsPerSemitone);
    refresh_all_channel_mods();
  } else {
    st.bend_range_cents = constants::kCentsPerSemitone *
                              std::floor(st.bend_range_cents / constants::kCentsPerSemitone) +
                          static_cast<float>(cents);
    refresh_channel_mod(ch);
  }
}

void NativeSynth::control_change(uint8_t channel, uint8_t controller, Control32 control) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  // Continuous controllers keep the full-width value for their float laws; the
  // switches, bank select, the pedals and the parameter-number machinery read
  // the 7-bit value.
  const uint8_t value = control.u7();
  st.cc_position[controller & 0x7Fu] = control;
  switch (controller) {
    case cc::kBankSelectMsb:
      // Bank select is prohibited on a member channel in MIDI Mode 3 and
      // permitted in Mode 4 (Appendix E Table 5), where a controller gives each
      // string its own program.
      if (mpe_.ignores(ch, MpeIgnorable::kBankSelect)) break;
      st.bank_msb = value;
      refresh_part_rig(ch);
      break;
    case cc::kModulationWheel:
      st.mod_wheel = control;
      refresh_channel_mod(ch);
      break;
    case cc::kChannelVolume:
      st.volume = control;
      refresh_channel_mod(ch);
      break;
    case cc::kPan:
      st.pan = control;
      refresh_channel_mod(ch);
      break;
    case cc::kBreathController:
      // The matrix source. Which axis CC2 additionally reaches, if any, is the
      // controller profile's to say and is applied before this switch runs.
      st.breath = control;
      refresh_channel_mod(ch);
      break;
    case cc::kExpression:
      st.expression = control;
      refresh_channel_mod(ch);  // every engine's loudness rides the expression VCA
      break;
    case cc::kBankSelectLsb:
      if (mpe_.ignores(ch, MpeIgnorable::kBankSelect)) break;
      st.bank_lsb = value;
      refresh_part_rig(ch);
      break;
    case cc::kDataEntryMsb:
      // RPN 00 06 is the MPE Configuration Message, which every MPE-compatible
      // device shall support (2.2.1). It is tried first because it is the one
      // that can turn the zone model on.
      if (st.params.selected_rpn(0, 6)) {
        apply_mcm(ch, value);
      } else if (st.params.selected_rpn(0, 0)) {
        bend_range_msb(ch, value);
      } else if (st.params.selected_nrpn() && st.rx_nrpn) {
        const GsPartParams before = st.gs;
        gs_apply_part_nrpn(st.gs, st.params.nrpn_msb, st.params.nrpn_lsb, value);
        if (gs_eg_times_differ(before, st.gs)) raise_tail();
      }
      break;
    case cc::kDataEntryLsb:
      if (st.params.selected_rpn(0, 0)) bend_range_lsb(ch, value);
      break;
    // TONE MODIFY 1-8 by controller: the storage 40 1x 30-37 and NRPN 01 xx write (docs/gs.md).
    case cc::kSoundController2:
    case cc::kSoundController3:
    case cc::kSoundController4:
    case kMpeTimbreCc:
    case cc::kSoundController6:
    case cc::kSoundController7:
    case cc::kSoundController8:
    case cc::kSoundController9: {
      // Inside a zone CC74 is the timbre dimension (2.2.8), already tracked and profiled.
      if (controller == kMpeTimbreCc && mpe_.role(ch) != MpeChannelRole::kUnassigned) break;
      // A controller the profile routes to an axis is the profile's, not the alias's.
      if (profile_binds_cc(controller)) break;
      const GsPartParams before = st.gs;
      gs_apply_tone_modify_cc(st.gs, controller, value);
      if (gs_eg_times_differ(before, st.gs)) raise_tail();
      break;
    }
    case cc::kDamperPedal:
      sustain_cc(ch, control);
      break;
    case cc::kSostenuto:
      sostenuto_pedal(ch, value >= 64);
      break;
    case cc::kSoftPedal:
      st.una_corda = value >= 64;  // soft pedal (affects notes struck while held)
      break;
    // The selection moves whatever Rx.NRPN says; the switch gates data entry (case 6), so a
    // refused NRPN cannot leave the previous RPN selected for the next data entry.
    case cc::kNrpnLsb:
      st.params.select_nrpn_lsb(value);
      break;
    case cc::kNrpnMsb:
      st.params.select_nrpn_msb(value);
      break;
    case cc::kRpnLsb:
      st.params.select_rpn_lsb(value);
      st.params.deselect_on_rpn_null();
      break;
    case cc::kRpnMsb:
      st.params.select_rpn_msb(value);
      st.params.deselect_on_rpn_null();
      break;
    case cc::kAllSoundOff:
      all_sound_off(ch);
      break;
    case cc::kResetAllControllers:
      reset_controllers(ch);
      break;
    case cc::kAllNotesOff:
    case cc::kOmniModeOff:
    case cc::kOmniModeOn:
      all_notes_off(ch);
      break;
    case cc::kMonoModeOn:
    case cc::kPolyModeOn:
      // The two mode messages. Prohibited on a manager channel, where they are
      // ignored outright, and on a member channel they select the zone's mode
      // (2.2.4.3). Outside a zone they keep the channel-mode meaning they have
      // always had here, which is why the all-notes-off stays below them.
      if (mpe_.ignores(ch, MpeIgnorable::kModeMessage)) break;
      if (mpe_.role(ch) == MpeChannelRole::kUnassigned) {
        st.articulation = controller == cc::kMonoModeOn ? ArticulationMode::kMonoRetrigger
                                                        : ArticulationMode::kPoly;
      }
      mpe_.apply_midi_mode(ch,
                           controller == cc::kMonoModeOn ? MpeMidiMode::kMono : MpeMidiMode::kPoly);
      all_notes_off(ch);
      break;
    default:
      break;
  }
}

void NativeSynth::registered_controller(const Ump& ump, const ChannelVoiceEvent& ev) noexcept {
  const uint8_t ch = ev.channel & 0x0Fu;
  if (ev.kind == ChannelVoiceKind::RegisteredController && ev.bank == 0 &&
      ev.index == kRcPitchBendSensitivity) {
    // RPN 0/0's Data Entry MSB and LSB, read by truncation, applied in the order
    // the MIDI 1.0 gesture applies them.
    const uint16_t range14 = ev.value.u14();
    bend_range_msb(ch, static_cast<uint8_t>(range14 >> 7));
    bend_range_lsb(ch, static_cast<uint8_t>(range14 & 0x7Fu));
    return;
  }
  if (ev.kind == ChannelVoiceKind::RegisteredController && ev.bank == 0 &&
      ev.index == kRcPerNoteBendSensitivity) {
    per_note_bend_sensitivity_[ch] = ev.value;
    refresh_per_note_voices(ch, 0, true);
    return;
  }
  // The RPN / NRPN gesture in its MIDI 2.0 form takes the path its four MIDI
  // 1.0 messages do, so parameter selection and data entry treat it the same.
  const Midi1MessageList lowered = midi2_to_midi1_messages(ump);
  for (uint8_t i = 0; i < lowered.count; ++i) {
    control_change(ch, lowered.messages[i].note_number(),
                   Control32::from7(lowered.messages[i].data2_7bit()));
  }
}

bool NativeSynth::relative_controller(const ChannelVoiceEvent& ev) noexcept {
  if (ev.kind != ChannelVoiceKind::RelativeRegistered || ev.bank != 0) return false;
  const uint8_t ch = ev.channel & 0x0Fu;
  switch (ev.index) {
    case kRcPitchBendSensitivity: {
      // The held range in RPN 0/0 form (semitones in the top seven bits, cents below them), moved
      // and applied as the absolute message would be.
      const bool zoned = mpe_.role(ch) != MpeChannelRole::kUnassigned;
      const float held_cents = zoned ? mpe_.bend_sensitivity(ch) * constants::kCentsPerSemitone
                                     : channels_[ch].bend_range_cents;
      const int semitones =
          std::clamp(static_cast<int>(held_cents / constants::kCentsPerSemitone), 0, 127);
      const int cents = std::clamp(
          static_cast<int>(held_cents -
                           constants::kCentsPerSemitone * static_cast<float>(semitones) + 0.5f),
          0, 127);
      const auto held = static_cast<uint32_t>((semitones << 7) | cents) << 18;
      const uint16_t range14 = add_saturating(Control32::from_raw(held), ev.value).u14();
      bend_range_msb(ch, static_cast<uint8_t>(range14 >> 7));
      bend_range_lsb(ch, static_cast<uint8_t>(range14 & 0x7Fu));
      return true;
    }
    case kRcPerNoteBendSensitivity:
      per_note_bend_sensitivity_[ch] = add_saturating(per_note_bend_sensitivity_[ch], ev.value);
      refresh_per_note_voices(ch, 0, true);
      return true;
    default:
      return false;
  }
}

}  // namespace sonare::midi::synth
