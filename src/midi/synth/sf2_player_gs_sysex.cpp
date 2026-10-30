#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "midi/builtin_synth.h"
#include "midi/control_value.h"
#include "midi/controller_numbers.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "util/dsp_primitives.h"

namespace sonare::midi::synth {

bool Sf2Player::handle_sysex(const uint8_t* data, size_t size) noexcept {
  const GsSysEx msg = parse_gs_sysex(data, size);
  // A frame's kind is classified from its FIRST decoded byte alone (gs_layer.cpp).
  // Resets are whole-frame messages and still return; part-scoped writes fall
  // through to the decoded-write appliers so a bulk run cannot lose a later row.
  bool handled = false;
  switch (msg.kind) {
    case GsSysExKind::kGm1Reset:
      gm_reset(GmLevel::kGeneralMidi1);
      return true;
    case GsSysExKind::kGm2Reset:
      gm_reset(GmLevel::kGeneralMidi2);
      return true;
    case GsSysExKind::kGsReset:
      gs_reset();
      return true;
    case GsSysExKind::kUseForRhythm:
    case GsSysExKind::kEfxPartSwitch:
    case GsSysExKind::kNone:
      break;
  }
  // PART EFX ASSIGN may be a later byte in a part-block run. The control path
  // owns the mirror for live players; only inline mode commits the copy here.
  // Avoid reading the control-owned mirror on the live audio path: the control
  // thread may publish a new snapshot concurrently.
  bool efx_assign_touched = false;
  if (config_.realize_efx_inline) {
    std::array<uint8_t, 16> assignments = part_fx_.part_assign();
    if (apply_gs_efx_assign_sysex(&assignments, data, size)) {
      efx_assign_touched = true;
      const std::array<uint8_t, 16>& current = part_fx_.part_assign();
      for (size_t part = 0; part < assignments.size(); ++part) {
        if (assignments[part] != current[part]) {
          part_fx_.assign_part(static_cast<uint8_t>(part), assignments[part]);
        }
      }
    }
  } else {
    efx_assign_touched = apply_gs_efx_assign_sysex(nullptr, data, size);
  }
  if (efx_assign_touched) handled = true;

  // Part parameters (40 1x xx). These alias controllers the render thread
  // already owns, so unlike the effect blocks they apply here in both modes.
  if (apply_gs_part_sysex(data, size)) handled = true;
  // Master tuning / volume / pan (40 00 00-06), on the same thread split.
  if (apply_gs_master_sysex(data, size)) handled = true;
  // Drum setup (41 mn rr). Render thread in both modes, like the part block
  // above and for the same reason: the slab it writes is the one the drum NRPNs
  // already write from on_event, and a note-on reads it there.
  if (apply_gs_drum_sysex(data, size)) handled = true;
  // User drum sets (21 dn rr), on the same thread split: a note-on reads them
  // where it reads the drum setup slab.
  if (apply_gs_user_drum_sysex(data, size)) handled = true;
  // GS insertion-effect (EFX) block writes (40 03 xx, or 40 3u xx for one of
  // the extension's units). Offline captures the raw wire into that unit's
  // mirror so process() can realise it inline; live routes realisation through
  // the control thread (on_control_sysex), so the audio thread must not touch
  // the mirror the builder reads.
  if (config_.realize_efx_inline && part_fx_.apply_unit_sysex(data, size)) handled = true;
  // System-effect (40 01 30-5A), master-EQ (40 02 00-03) and part EQ switch
  // (40 4x 20) writes, on the same thread split as the EFX block above.
  if (config_.realize_efx_inline && apply_gs_system_sysex(data, size)) {
    gs_system_dirty_ = true;
    handled = true;
  }
  return handled;
}

bool Sf2Player::apply_gs_system_sysex(const uint8_t* data, size_t size) noexcept {
  return apply_gs_system_sysex_to(sys_fx_, master_eq_, eq_part_bypassed_, data, size);
}

bool Sf2Player::apply_gs_system_sysex_to(GsSystemEffects& fx, GsMasterEq& eq,
                                         std::array<bool, 16>& eq_part, const uint8_t* data,
                                         size_t size) noexcept {
  // A file writes these blocks as multi-byte runs. Walk every decoded byte,
  // including bytes that cross from system effects into master EQ.
  return gs_for_each_sysex_write(data, size, [&](const GsWrite& w) noexcept {
    // An out-of-range value is ignored rather than clamped (docs/gs.md).
    const GsAddressEntry* entry = gs_lookup_address(w.addr);
    if (entry == nullptr || !gs_value_in_range(*entry, w.value)) return false;
    switch (w.param) {
      // A macro is a one-shot write of the parameters it covers, so it lands
      // through gs_apply_*_macro rather than on a field of its own.
      case GsParam::kReverbMacro:
        gs_apply_reverb_macro(fx, w.value);
        break;
      case GsParam::kReverbCharacter:
        fx.reverb_character = w.value;
        break;
      case GsParam::kReverbPreLpf:
        fx.reverb_pre_lpf = w.value;
        break;
      case GsParam::kReverbLevel:
        fx.reverb_level = w.value;
        break;
      case GsParam::kReverbTime:
        fx.reverb_time = w.value;
        break;
      case GsParam::kReverbDelayFeedback:
        fx.reverb_delay_feedback = w.value;
        break;
      case GsParam::kReverbPredelay:
        fx.reverb_predelay = w.value;
        break;
      case GsParam::kChorusMacro:
        gs_apply_chorus_macro(fx, w.value);
        break;
      case GsParam::kChorusPreLpf:
        fx.chorus_pre_lpf = w.value;
        break;
      case GsParam::kChorusLevel:
        fx.chorus_level = w.value;
        break;
      case GsParam::kChorusFeedback:
        fx.chorus_feedback = w.value;
        break;
      case GsParam::kChorusDelay:
        fx.chorus_delay = w.value;
        break;
      case GsParam::kChorusRate:
        fx.chorus_rate = w.value;
        break;
      case GsParam::kChorusDepth:
        fx.chorus_depth = w.value;
        break;
      case GsParam::kChorusSendToReverb:
        fx.chorus_send_to_reverb = w.value;
        break;
      case GsParam::kChorusSendToDelay:
        fx.chorus_send_to_delay = w.value;
        break;
      case GsParam::kDelayMacro:
        gs_apply_delay_macro(fx, w.value);
        break;
      case GsParam::kDelayPreLpf:
        fx.delay_pre_lpf = w.value;
        break;
      case GsParam::kDelayTimeCenter:
        fx.delay_time_center = w.value;
        break;
      case GsParam::kDelayTimeRatioLeft:
        fx.delay_time_ratio_left = w.value;
        break;
      case GsParam::kDelayTimeRatioRight:
        fx.delay_time_ratio_right = w.value;
        break;
      case GsParam::kDelayLevelCenter:
        fx.delay_level_center = w.value;
        break;
      case GsParam::kDelayLevelLeft:
        fx.delay_level_left = w.value;
        break;
      case GsParam::kDelayLevelRight:
        fx.delay_level_right = w.value;
        break;
      case GsParam::kDelayLevel:
        fx.delay_level = w.value;
        break;
      case GsParam::kDelayFeedback:
        fx.delay_feedback = w.value;
        break;
      case GsParam::kDelaySendToReverb:
        fx.delay_send_to_reverb = w.value;
        break;
      case GsParam::kEqLowFreq:
        eq.low_freq = w.value;
        break;
      case GsParam::kEqLowGain:
        eq.low_gain = w.value;
        break;
      case GsParam::kEqHighFreq:
        eq.high_freq = w.value;
        break;
      case GsParam::kEqHighGain:
        eq.high_gain = w.value;
        break;
      case GsParam::kPartEqSwitch:
        eq_part[w.part & 0x0Fu] = w.value == 0;
        break;
      default:
        return false;
    }
    return true;
  });
}

bool Sf2Player::apply_gs_part_sysex(const uint8_t* data, size_t size) noexcept {
  // One bit per part written, so a run over several parts refreshes each once
  // instead of once per byte.
  uint16_t dirty = 0;
  uint16_t rig_dirty = 0;
  bool rx_dirty = false;
  bool eg_moved = false;
  // A valid DT1 may carry a whole dump; the masks run across the walk's chunks,
  // so no later part row is discarded at a write-buffer boundary.
  gs_for_each_sysex_write(data, size, [&](const GsWrite& w) noexcept {
    const GsAddressEntry* entry = gs_lookup_address(w.addr);
    if (entry == nullptr) return false;
    ChannelState& st = channels_[w.part & 0x0Fu];
    // USE FOR RHYTHM PART is the one GS row whose out-of-range value has a
    // defined divergence: every value above map 2 selects map 1. Do this
    // before the ordinary row-range rejection, because real files place it in
    // the middle of a bulk run (40 1x 14, 15, ...).
    if (w.param == GsParam::kUseForRhythmPart) {
      st.drum_map = w.value <= 2 ? w.value : 1;
      dirty |= static_cast<uint16_t>(1u << (w.part & 0x0Fu));
      rig_dirty |= static_cast<uint16_t>(1u << (w.part & 0x0Fu));
      return true;
    }
    if (!gs_value_in_range(*entry, w.value)) return false;
    switch (w.param) {
      case GsParam::kPartLevel:
        st.volume = Control32::from7(w.value);
        break;
      case GsParam::kPartPanpot:
        // The manual's own "= CC#10, except RANDOM": 00 is RANDOM at this
        // address and hard left at the controller. Randomness has no place in a
        // bit-identical bounce, so it answers centre (docs/gs.md).
        st.pan = Control32::from7(w.value == 0 ? 0x40 : w.value);
        break;
      case GsParam::kPartToneNumber:
        // The same two storage locations CC0 and a program change write, so the
        // byte lands on the controller's field rather than on a second copy.
        // A file may write either byte alone; the index is which.
        if (w.index == 0) {
          st.bank_msb = w.value;
        } else {
          reset_drum_map_on_set_change(st, w.value);
          st.program = w.value;
        }
        rig_dirty |= static_cast<uint16_t>(1u << (w.part & 0x0Fu));
        break;
      case GsParam::kPartToneMapNumber:
        // The storage Bank Select LSB writes, not a second copy of it. The two
        // were left apart while it was unclear whether they were one parameter,
        // and a measured unit settles it: CC#32 lands on this address verbatim
        // (tools/gs/docs/unit-diff.md). It resolves a preset, so the rig binding
        // is refreshed for the reason a tone number refreshes it.
        st.bank_lsb = w.value;
        rig_dirty |= static_cast<uint16_t>(1u << (w.part & 0x0Fu));
        break;
      case GsParam::kPartRxChannel:
        st.rx_channel = w.value;
        rx_dirty = true;
        break;
      // The eighteen receive switches share one body: the bit comes from the
      // address, so a row added to either block is carried without a case of its
      // own having to agree with the enumerator order.
      case GsParam::kPartRxPitchBend:
      case GsParam::kPartRxChannelPressure:
      case GsParam::kPartRxProgramChange:
      case GsParam::kPartRxControlChange:
      case GsParam::kPartRxPolyPressure:
      case GsParam::kPartRxNoteMessage:
      case GsParam::kPartRxRpn:
      case GsParam::kPartRxNrpn:
      case GsParam::kPartRxModulation:
      case GsParam::kPartRxVolume:
      case GsParam::kPartRxPanpot:
      case GsParam::kPartRxExpression:
      case GsParam::kPartRxHold1:
      case GsParam::kPartRxPortamento:
      case GsParam::kPartRxSostenuto:
      case GsParam::kPartRxSoft:
      case GsParam::kPartRxBankSelect:
      case GsParam::kPartRxBankSelectLsb: {
        const uint32_t bit = gs_rx_switch_bit(w.addr);
        st.rx_switches = w.value != 0 ? (st.rx_switches | bit) : (st.rx_switches & ~bit);
        break;
      }
      case GsParam::kPartScaleTuning:
        if (w.index < st.scale_tuning.size()) st.scale_tuning[w.index] = w.value;
        break;
      case GsParam::kPartVelocitySenseDepth:
        st.velocity_sense_depth = w.value;
        break;
      case GsParam::kPartVelocitySenseOffset:
        st.velocity_sense_offset = w.value;
        break;
      case GsParam::kPartKeyRangeLow:
        st.key_range_low = w.value;
        break;
      case GsParam::kPartKeyRangeHigh:
        st.key_range_high = w.value;
        break;
      case GsParam::kPartChorusSend:
        st.chorus_send = w.value;
        break;
      case GsParam::kPartReverbSend:
        st.reverb_send = w.value;
        break;
      case GsParam::kPartDelaySend:
        st.delay_send = w.value;
        break;
      case GsParam::kPartPitchFineTune:
        // The same 14-bit word RPN 00 01 writes, MSB first.
        st.pitch_fine_tune = w.index == 0
                                 ? static_cast<uint16_t>((static_cast<uint16_t>(w.value) << 7) |
                                                         (st.pitch_fine_tune & 0x7Fu))
                                 : static_cast<uint16_t>((st.pitch_fine_tune & 0x3F80u) | w.value);
        break;
      case GsParam::kPartMonoPoly:
        st.mono_poly = w.value;
        break;
      case GsParam::kPartAssignMode:
        st.assign_mode = w.value;
        break;
      case GsParam::kPartKeyShift:
        // Held raw and decoded at the render, where the rhythm-part exclusion
        // is: a part that becomes drums later still has to stop taking it.
        st.pitch_key_shift = w.value;
        break;
      case GsParam::kPartPitchOffsetFine:
        // Nibblized, high nibble first. The aggregate 08-F8 is not enforced
        // here: a pair arrives a byte at a time, so the intermediate word can
        // sit outside a range the finished one is inside of.
        st.pitch_offset_fine =
            w.index == 0
                ? static_cast<uint8_t>(((w.value & 0x0Fu) << 4) | (st.pitch_offset_fine & 0x0Fu))
                : static_cast<uint8_t>((st.pitch_offset_fine & 0xF0u) | (w.value & 0x0Fu));
        break;
      case GsParam::kPartToneModify: {
        const GsPartParams before = st.gs;
        gs_apply_tone_modify(st.gs, w.index, w.value);
        eg_moved |= gs_eg_times_differ(before, st.gs);
        break;
      }
      case GsParam::kPartCtrlSourceNumber:
        st.assignable_cc[w.index & 1u] = w.value;
        break;
      // The block is a matrix: the row names the destination and the address
      // names the source, so one case serves every source that reaches it.
      case GsParam::kPartCtrlPitch:
        st.ctrl_dest[gs_ctrl_source_index(w.addr)].pitch_cents = gs_mod_pitch_cents(w.value);
        break;
      case GsParam::kPartCtrlTvfCutoff:
        st.ctrl_dest[gs_ctrl_source_index(w.addr)].cutoff_cents = gs_mod_cutoff_cents(w.value);
        break;
      case GsParam::kPartCtrlAmplitude:
        st.ctrl_dest[gs_ctrl_source_index(w.addr)].amp_fraction = gs_mod_amp_fraction(w.value);
        break;
      case GsParam::kPartCtrlLfo1Rate:
        st.ctrl_dest[gs_ctrl_source_index(w.addr)].lfo_rate = w.value;
        break;
      case GsParam::kPartCtrlLfo1PitchDepth:
        st.ctrl_dest[gs_ctrl_source_index(w.addr)].vib_depth_cents = gs_mod_depth_cents(w.value);
        break;
      case GsParam::kPartCtrlLfo1TvfDepth:
        st.ctrl_dest[gs_ctrl_source_index(w.addr)].tvf_lfo_cents = gs_lfo_tvf_depth_cents(w.value);
        break;
      case GsParam::kPartCtrlLfo1TvaDepth:
        st.ctrl_dest[gs_ctrl_source_index(w.addr)].tva_depth = gs_mod_tva_depth(w.value);
        break;
      case GsParam::kPartBendPitchControl:
        // The same range RPN 00 00 writes, in whole semitones above 40; the
        // cents its LSB carries have no address of their own here.
        st.bend_range_cents = 100.0f * static_cast<float>(w.value - 0x40);
        break;
      default:
        return false;
    }
    dirty |= static_cast<uint16_t>(1u << (w.part & 0x0Fu));
    return true;
  });
  for (uint8_t ch = 0; ch < 16; ++ch) {
    if ((dirty & (1u << ch)) != 0) refresh_channel_mod(ch);
    // Its own mask rather than `dirty`: this resolves a preset, and every other
    // part parameter in the block leaves the rig binding exactly where it was.
    if ((rig_dirty & (1u << ch)) != 0) refresh_part_rig(ch);
  }
  if (rx_dirty) refresh_rx_channels();
  if (eg_moved) raise_tail();
  return dirty != 0;
}

bool Sf2Player::apply_gs_master_sysex(const uint8_t* data, size_t size) noexcept {
  return gs_for_each_sysex_write(data, size, [&](const GsWrite& w) noexcept {
    const GsAddressEntry* entry = gs_lookup_address(w.addr);
    if (entry == nullptr || !gs_value_in_range(*entry, w.value)) return false;
    switch (w.param) {
      case GsParam::kMasterTune:
        master_.tune[w.index & 0x03u] = w.value;
        break;
      case GsParam::kMasterVolume:
        master_.volume = w.value;
        break;
      case GsParam::kMasterKeyShift:
        master_.key_shift = w.value;
        break;
      case GsParam::kMasterPan:
        master_.pan = w.value;
        break;
      default:
        return false;
    }
    return true;
  });
}

bool Sf2Player::apply_gs_drum_sysex(const uint8_t* data, size_t size) noexcept {
  return gs_for_each_sysex_write(data, size, [&](const GsWrite& w) noexcept {
    const GsAddressEntry* entry = gs_lookup_address(w.addr);
    if (entry == nullptr || !gs_value_in_range(*entry, w.value)) return false;
    // The address nibble is zero-based, so it indexes the slabs directly — where
    // 40 1x 15's value is one-based and goes through drum_map_slot(). A map the
    // machine does not have is ignored like any other out-of-range write.
    if (w.part >= kGsDrumMapCount) return false;
    GsDrumNoteParams& d = drum_params_[w.part][w.index & 0x7Fu];
    switch (w.param) {
      case GsParam::kDrumPlayNote:
        d.play_note = w.value;
        d.flags |= GsDrumNoteParams::kPlayNote;
        break;
      case GsParam::kDrumLevel:
        d.level = w.value;
        d.flags |= GsDrumNoteParams::kLevel;
        break;
      case GsParam::kDrumRxNoteOn:
        d.rx_note_on = w.value;
        d.flags |= GsDrumNoteParams::kRxNoteOn;
        break;
      case GsParam::kDrumAssignGroup:
        d.assign_group = w.value;
        d.flags |= GsDrumNoteParams::kAssignGroup;
        break;
      case GsParam::kDrumPanpot:
        // 00 is RANDOM at this address and hard left through NRPN 1C, the same
        // split 40 1x 1C and CC10 have; randomness answers centre (docs/gs.md).
        d.pan = w.value == 0 ? 0x40 : w.value;
        d.flags |= GsDrumNoteParams::kPan;
        break;
      case GsParam::kDrumReverbSend:
        d.reverb = w.value;
        d.flags |= GsDrumNoteParams::kReverb;
        break;
      case GsParam::kDrumChorusSend:
        d.chorus = w.value;
        d.flags |= GsDrumNoteParams::kChorus;
        break;
      case GsParam::kDrumDelaySend:
        d.delay = w.value;
        d.flags |= GsDrumNoteParams::kDelay;
        break;
      default:
        return false;
    }
    return true;
  });
}

bool Sf2Player::apply_gs_user_drum_sysex(const uint8_t* data, size_t size) noexcept {
  return gs_for_each_sysex_write(data, size, [&](const GsWrite& w) noexcept {
    const GsAddressEntry* entry = gs_lookup_address(w.addr);
    if (entry == nullptr || !gs_value_in_range(*entry, w.value)) return false;
    if (w.part >= kGsUserDrumSetCount) return false;
    GsUserDrumSource& src = user_drum_sources_[w.part][w.index & 0x7Fu];
    GsDrumNoteParams& d = user_drum_params_[w.part][w.index & 0x7Fu];
    switch (w.param) {
      case GsParam::kUserDrumSourceProgram:
        src.program = w.value;
        break;
      case GsParam::kUserDrumSourceNote:
        src.source_note = w.value;
        src.flags |= GsUserDrumSource::kSourceNote;
        break;
      // Nibbles 1-9 are the drum setup block's parameters stored in the set, so
      // they land in the same struct and read the same way — including PANPOT's
      // 00, which is RANDOM at an address and answers centre (docs/gs.md).
      case GsParam::kUserDrumPlayNote:
        d.play_note = w.value;
        d.flags |= GsDrumNoteParams::kPlayNote;
        break;
      case GsParam::kUserDrumLevel:
        d.level = w.value;
        d.flags |= GsDrumNoteParams::kLevel;
        break;
      case GsParam::kUserDrumAssignGroup:
        d.assign_group = w.value;
        d.flags |= GsDrumNoteParams::kAssignGroup;
        break;
      case GsParam::kUserDrumPanpot:
        d.pan = w.value == 0 ? 0x40 : w.value;
        d.flags |= GsDrumNoteParams::kPan;
        break;
      case GsParam::kUserDrumReverbSend:
        d.reverb = w.value;
        d.flags |= GsDrumNoteParams::kReverb;
        break;
      case GsParam::kUserDrumChorusSend:
        d.chorus = w.value;
        d.flags |= GsDrumNoteParams::kChorus;
        break;
      case GsParam::kUserDrumRxNoteOn:
        d.rx_note_on = w.value;
        d.flags |= GsDrumNoteParams::kRxNoteOn;
        break;
      case GsParam::kUserDrumDelaySend:
        d.delay = w.value;
        d.flags |= GsDrumNoteParams::kDelay;
        break;
      default:
        return false;
    }
    return true;
  });
}

void Sf2Player::apply_gs_system_state(const GsSystemEffects& fx, const GsMasterEq& eq,
                                      const std::array<bool, 16>& eq_part) noexcept {
#if defined(SONARE_MIDI_WITH_FX)
  if (effects_ != nullptr) effects_->set_config(gs_effects_config_from(fx));
#else
  (void)fx;
#endif
  eq_.set(eq);
  eq_bypassed_ = eq_part;
  // REVERB TIME and DELAY TIME move the ring-out an offline bounce has to
  // capture; the recomputation is table lookups and arithmetic, no allocation.
  if (prepared_) recompute_tail();
}

}  // namespace sonare::midi::synth
