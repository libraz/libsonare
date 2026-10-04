#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "midi/builtin_synth.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"

namespace sonare::midi::synth {

bool Sf2Player::handle_sysex(const uint8_t* data, size_t size) noexcept {
  const GsSysEx msg = parse_gs_sysex(data, size);
  // A frame's kind is classified from its FIRST decoded byte alone (gs_layer.cpp),
  // so a case that returns here discards every later byte of the same run. The
  // resets below are whole-frame messages and still return; the two part-scoped
  // kinds record that they were handled and fall through to the appliers, which
  // walk every decoded write. Neither address has a case in those appliers
  // (apply_gs_part_sysex has none for USE FOR RHYTHM PART, apply_gs_system_sysex
  // none for the part EFX assign), so falling through cannot apply either twice.
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
      // The map number is kept, not just its truth: it selects which drum-note
      // edit slab the part reads (docs/gs.md).
      channels_[msg.channel & 0x0Fu].drum_map = msg.value;
      // The part's effective bank just moved between melodic and rhythm, and
      // both the fallback ambience floor and the bank's rig are keyed on it.
      refresh_channel_mod(msg.channel & 0x0Fu);
      refresh_part_rig(msg.channel & 0x0Fu);
      // 40 1x 16-1E (key shift, pitch offset, level, velocity sense depth and
      // offset, pan, key range low/high) are the addresses immediately after
      // this one, and a real file writes them in one run with it.
      handled = true;
      break;
    case GsSysExKind::kEfxPartSwitch:
      // Route/unroute the part through the EFX. Offline (inline) updates the
      // mirror here on the render thread; live leaves the mirror to the control
      // thread's on_control_sysex (which realises + swaps the chains wait-free).
      if (config_.realize_efx_inline) part_fx_.assign_part(msg.channel & 0x0Fu, msg.value);
      handled = true;
      break;
    case GsSysExKind::kNone:
      break;
  }
  // Part parameters (40 1x xx). These alias controllers the render thread
  // already owns, so unlike the effect blocks they apply here in both modes.
  if (apply_gs_part_sysex(data, size)) return true;
  // Master tuning / volume / pan (40 00 00-06), on the same thread split.
  if (apply_gs_master_sysex(data, size)) return true;
  // Drum setup (41 mn rr). Render thread in both modes, like the part block
  // above and for the same reason: the slab it writes is the one the drum NRPNs
  // already write from on_event, and a note-on reads it there.
  if (apply_gs_drum_sysex(data, size)) return true;
  // User drum sets (21 dn rr), on the same thread split: a note-on reads them
  // where it reads the drum setup slab.
  if (apply_gs_user_drum_sysex(data, size)) return true;
  // GS insertion-effect (EFX) block writes (40 03 xx, or 40 3u xx for one of
  // the extension's units). Offline captures the raw wire into that unit's
  // mirror so process() can realise it inline; live routes realisation through
  // the control thread (on_control_sysex), so the audio thread must not touch
  // the mirror the builder reads.
  if (config_.realize_efx_inline && part_fx_.apply_unit_sysex(data, size)) return true;
  // System-effect (40 01 30-5A), master-EQ (40 02 00-03) and part EQ switch
  // (40 4x 20) writes, on the same thread split as the EFX block above.
  if (config_.realize_efx_inline && apply_gs_system_sysex(data, size)) {
    gs_system_dirty_ = true;
    return true;
  }
  // True when the switch above consumed the frame's first byte even though no
  // applier claimed the rest, so a single-byte write of one of those two
  // addresses still reports as handled.
  return handled;
}

bool Sf2Player::apply_gs_system_sysex(const uint8_t* data, size_t size) noexcept {
  return apply_gs_system_sysex_to(sys_fx_, master_eq_, eq_part_bypassed_, data, size);
}

bool Sf2Player::apply_gs_system_sysex_to(GsSystemEffects& fx, GsMasterEq& eq,
                                         std::array<bool, 16>& eq_part, const uint8_t* data,
                                         size_t size) noexcept {
  // A file writes these blocks as multi-byte runs — the census finds up to 11
  // data bytes at 40 01 50 — so every decoded byte is applied, not just the
  // first. gs_decode_sysex reports one write per byte with its own address.
  constexpr size_t kMaxWrites = 64;
  GsWrite writes[kMaxWrites];
  const size_t decoded = gs_decode_sysex(data, size, writes, kMaxWrites, nullptr);
  bool touched = false;
  for (size_t i = 0; i < std::min(decoded, kMaxWrites); ++i) {
    const GsWrite& w = writes[i];
    // An out-of-range value is ignored rather than clamped (docs/gs.md).
    const GsAddressEntry* entry = gs_lookup_address(w.addr);
    if (entry == nullptr || !gs_value_in_range(*entry, w.value)) continue;
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
        continue;
    }
    touched = true;
  }
  return touched;
}

bool Sf2Player::apply_gs_part_sysex(const uint8_t* data, size_t size) noexcept {
  constexpr size_t kMaxWrites = 64;
  GsWrite writes[kMaxWrites];
  const size_t decoded = gs_decode_sysex(data, size, writes, kMaxWrites, nullptr);
  // One bit per part written, so a run over several parts refreshes each once
  // instead of once per byte.
  uint16_t dirty = 0;
  uint16_t rig_dirty = 0;
  bool rx_dirty = false;
  for (size_t i = 0; i < std::min(decoded, kMaxWrites); ++i) {
    const GsWrite& w = writes[i];
    const GsAddressEntry* entry = gs_lookup_address(w.addr);
    if (entry == nullptr || !gs_value_in_range(*entry, w.value)) continue;
    ChannelState& st = channels_[w.part & 0x0Fu];
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
      case GsParam::kPartToneModify:
        gs_apply_tone_modify(st.gs, w.index, w.value);
        break;
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
        continue;
    }
    dirty |= static_cast<uint16_t>(1u << (w.part & 0x0Fu));
  }
  for (uint8_t ch = 0; ch < 16; ++ch) {
    if ((dirty & (1u << ch)) != 0) refresh_channel_mod(ch);
    // Its own mask rather than `dirty`: this resolves a preset, and every other
    // part parameter in the block leaves the rig binding exactly where it was.
    if ((rig_dirty & (1u << ch)) != 0) refresh_part_rig(ch);
  }
  if (rx_dirty) refresh_rx_channels();
  return dirty != 0;
}

bool Sf2Player::apply_gs_master_sysex(const uint8_t* data, size_t size) noexcept {
  // MASTER TUNE is four nibbles, so a run of up to seven bytes reaches every
  // address in the block.
  constexpr size_t kMaxWrites = 16;
  GsWrite writes[kMaxWrites];
  const size_t decoded = gs_decode_sysex(data, size, writes, kMaxWrites, nullptr);
  bool touched = false;
  for (size_t i = 0; i < std::min(decoded, kMaxWrites); ++i) {
    const GsWrite& w = writes[i];
    const GsAddressEntry* entry = gs_lookup_address(w.addr);
    if (entry == nullptr || !gs_value_in_range(*entry, w.value)) continue;
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
        continue;
    }
    touched = true;
  }
  return touched;
}

bool Sf2Player::apply_gs_drum_sysex(const uint8_t* data, size_t size) noexcept {
  // A drum setup dump writes consecutive notes as one run, so every decoded byte
  // is applied rather than the first (apply_gs_system_sysex's pattern) and the
  // buffer holds one write per drum note, which is as long as a run can be.
  constexpr size_t kMaxWrites = 128;
  GsWrite writes[kMaxWrites];
  const size_t decoded = gs_decode_sysex(data, size, writes, kMaxWrites, nullptr);
  bool touched = false;
  for (size_t i = 0; i < std::min(decoded, kMaxWrites); ++i) {
    const GsWrite& w = writes[i];
    const GsAddressEntry* entry = gs_lookup_address(w.addr);
    if (entry == nullptr || !gs_value_in_range(*entry, w.value)) continue;
    // The address nibble is zero-based, so it indexes the slabs directly — where
    // 40 1x 15's value is one-based and goes through drum_map_slot(). A map the
    // machine does not have is ignored like any other out-of-range write.
    if (w.part >= kGsDrumMapCount) continue;
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
        continue;
    }
    touched = true;
  }
  return touched;
}

bool Sf2Player::apply_gs_user_drum_sysex(const uint8_t* data, size_t size) noexcept {
  // Same run shape as the drum setup block above: a set is written note by note
  // as one run per parameter, so the buffer holds a whole 128-note run.
  constexpr size_t kMaxWrites = 128;
  GsWrite writes[kMaxWrites];
  const size_t decoded = gs_decode_sysex(data, size, writes, kMaxWrites, nullptr);
  bool touched = false;
  for (size_t i = 0; i < std::min(decoded, kMaxWrites); ++i) {
    const GsWrite& w = writes[i];
    const GsAddressEntry* entry = gs_lookup_address(w.addr);
    if (entry == nullptr || !gs_value_in_range(*entry, w.value)) continue;
    if (w.part >= kGsUserDrumSetCount) continue;
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
        continue;
    }
    touched = true;
  }
  return touched;
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

namespace {

constexpr size_t kDirectFxBase = 0;
constexpr size_t kDirectEqBase = kGsSystemEffectFieldCount;
constexpr size_t kDirectPartEqBase = kDirectEqBase + 4;

constexpr std::array<uint8_t GsSystemEffects::*, kGsSystemEffectFieldCount> kDirectFxMembers{{
#define SONARE_GS_DIRECT_MEMBER(name, value) &GsSystemEffects::name,
    SONARE_GS_SYSTEM_EFFECT_FIELDS(SONARE_GS_DIRECT_MEMBER)
#undef SONARE_GS_DIRECT_MEMBER
}};

// A system-effect parameter indexes kDirectFxMembers by its distance from kReverbMacro.
static_assert(static_cast<size_t>(GsParam::kDelaySendToReverb) -
                      static_cast<size_t>(GsParam::kReverbMacro) + 1 ==
                  kGsSystemEffectFieldCount,
              "the system-effect GsParam run must match SONARE_GS_SYSTEM_EFFECT_FIELDS");

// Each field is one byte, so a field's offset is its kDirectFxMembers index.
constexpr size_t kDirectChorusFirst = offsetof(GsSystemEffects, chorus_macro);
constexpr size_t kDirectDelayFirst = offsetof(GsSystemEffects, delay_macro);

}  // namespace

bool Sf2Player::append_direct_gs_node(const uint8_t* data, size_t size,
                                      std::shared_ptr<const PreparedMidiSysEx> prepared,
                                      bool legacy_full_snapshot) noexcept {
  if (data == nullptr || size == 0 || size > kDirectGsMaxBytes) return false;
  try {
    sweep_direct_gs_nodes();
    auto node = std::make_unique<DirectGsNode>();
    node->seq = direct_queue_->next_seq++;
    node->size = static_cast<uint16_t>(size);
    std::memcpy(node->bytes.data(), data, size);
    node->prepared_owner = std::move(prepared);
    node->prepared_raw = node->prepared_owner != nullptr
                             ? dynamic_cast<const PreparedSysEx*>(node->prepared_owner.get())
                             : nullptr;
    node->legacy_full_snapshot = legacy_full_snapshot;
    DirectGsNode* raw = node.get();
    direct_queue_->owned.push_back(std::move(node));
    direct_queue_->control_tail->next.store(raw, std::memory_order_release);
    direct_queue_->control_tail = raw;
    direct_queue_->published_tail.store(raw, std::memory_order_release);
    return true;
  } catch (...) {
    return false;
  }
}

void Sf2Player::sweep_direct_gs_nodes() noexcept {
  const uint64_t consumed = direct_queue_->consumed_seq.load(std::memory_order_acquire);
  const uint64_t retired = direct_queue_->retired_seq.load(std::memory_order_acquire);
  for (auto it = direct_queue_->owned.begin(); it != direct_queue_->owned.end();) {
    DirectGsNode* node = it->get();
    if (node->seq <= consumed) {
      node->prepared_owner.reset();
      node->prepared_raw = nullptr;
    }
    if (node->seq <= retired) {
      it = direct_queue_->owned.erase(it);
    } else {
      ++it;
    }
  }
}

void Sf2Player::clear_direct_gs_queue() noexcept {
  // Quiescent boundary only: the audio consumer has stopped.
  direct_queue_->owned.clear();
  direct_queue_->stub.next.store(nullptr, std::memory_order_relaxed);
  direct_queue_->control_tail = &direct_queue_->stub;
  direct_queue_->audio_head = &direct_queue_->stub;
  direct_queue_->published_tail.store(&direct_queue_->stub, std::memory_order_release);
  direct_queue_->consumed_seq.store(0, std::memory_order_release);
  direct_queue_->retired_seq.store(0, std::memory_order_release);
  direct_queue_->next_seq = 1;
}

void Sf2Player::adopt_legacy_direct_snapshot() noexcept {
  // Custom DSP without a prepared plan uses the full-snapshot publication.
  part_fx_.acquire();
  const PartFxSnapshot* snapshot = part_fx_.current();
  if (snapshot == nullptr) return;
  if (prepared_runtime_active_) {
    // Drop the EFX overlay but keep the system/EQ state already applied.
    const GsSystemEffects saved_sys_fx = prepared_sys_fx_;
    const GsMasterEq saved_master_eq = prepared_master_eq_;
    const std::array<bool, 16> saved_eq_part_bypassed = prepared_eq_part_bypassed_;
    clear_prepared_audio_state();
    prepared_sys_fx_ = saved_sys_fx;
    prepared_master_eq_ = saved_master_eq;
    prepared_eq_part_bypassed_ = saved_eq_part_bypassed;
    apply_gs_system_state(prepared_sys_fx_, prepared_master_eq_, prepared_eq_part_bypassed_);
  }
  prepared_efx_ = snapshot->gs_efx_state;
  prepared_assign_ = snapshot->gs_part_assign;
}

void Sf2Player::drain_direct_gs_nodes() noexcept {
  DirectGsNode* stop = direct_queue_->published_tail.load(std::memory_order_acquire);
  while (direct_queue_->audio_head != stop) {
    DirectGsNode* old = direct_queue_->audio_head;
    DirectGsNode* next = old->next.load(std::memory_order_acquire);
    if (next == nullptr) break;
    if (next->restart_domain != 0) {
      restart_prepared_audio_runtime(next->restart_domain);
    } else if (next->legacy_full_snapshot) {
      adopt_legacy_direct_snapshot();
    } else if (next->prepared_raw != nullptr) {
      // Direct deltas update and activate the audio-owned overlay.
      apply_prepared_gs_delta(*next->prepared_raw, next->bytes.data(), next->size, false);
    }
    direct_queue_->audio_head = next;
    direct_queue_->consumed_seq.store(next->seq, std::memory_order_release);
    direct_queue_->retired_seq.store(old->seq, std::memory_order_release);
  }
}

void Sf2Player::publish_direct_system_patch(bool reset, const uint8_t* data, size_t size) noexcept {
  constexpr size_t kMaxWrites = 64;
  GsWrite writes[kMaxWrites];
  const size_t decoded =
      data == nullptr ? 0 : gs_decode_sysex(data, size, writes, kMaxWrites, nullptr);
  const uint64_t seq = ++direct_system_patch_control_.publish_seq;
  if (reset) direct_system_patch_control_.last_reset_seq = seq;
  auto stamp = [&](size_t index, uint8_t value) {
    if (index >= direct_system_patch_control_.fields.size()) return;
    direct_system_patch_control_.fields[index] = {seq, value};
  };
  auto stamp_fx = [&](size_t index) {
    stamp(kDirectFxBase + index, sys_fx_.*kDirectFxMembers[index]);
  };
  auto stamp_fx_range = [&](size_t first, size_t count) {
    for (size_t i = 0; i < count && first + i < kDirectFxMembers.size(); ++i) stamp_fx(first + i);
  };
  auto stamp_eq = [&](size_t index, uint8_t value) { stamp(kDirectEqBase + index, value); };
  auto stamp_part_eq = [&](size_t part) {
    stamp(kDirectPartEqBase + (part & 0x0Fu), eq_part_bypassed_[part & 0x0Fu] ? 1 : 0);
  };

  if (!reset) {
    for (size_t i = 0; i < std::min(decoded, kMaxWrites); ++i) {
      const GsWrite& w = writes[i];
      const GsAddressEntry* entry = gs_lookup_address(w.addr);
      if (entry == nullptr || !gs_value_in_range(*entry, w.value)) continue;
      const size_t param = static_cast<size_t>(w.param);
      const size_t first_param = static_cast<size_t>(GsParam::kReverbMacro);
      const size_t last_param = static_cast<size_t>(GsParam::kDelaySendToReverb);
      if (param >= first_param && param <= last_param) {
        const size_t fx_index = param - first_param;
        if (w.param == GsParam::kReverbMacro)
          stamp_fx_range(0, kDirectChorusFirst);
        else if (w.param == GsParam::kChorusMacro)
          stamp_fx_range(kDirectChorusFirst, kDirectDelayFirst - kDirectChorusFirst);
        else if (w.param == GsParam::kDelayMacro)
          stamp_fx_range(kDirectDelayFirst, kGsSystemEffectFieldCount - kDirectDelayFirst);
        else
          stamp_fx(fx_index);
      } else {
        switch (w.param) {
          case GsParam::kEqLowFreq:
            stamp_eq(0, master_eq_.low_freq);
            break;
          case GsParam::kEqLowGain:
            stamp_eq(1, master_eq_.low_gain);
            break;
          case GsParam::kEqHighFreq:
            stamp_eq(2, master_eq_.high_freq);
            break;
          case GsParam::kEqHighGain:
            stamp_eq(3, master_eq_.high_gain);
            break;
          case GsParam::kPartEqSwitch:
            stamp_part_eq(w.part);
            break;
          default:
            break;
        }
      }
    }
  }
  direct_system_patch_->store(direct_system_patch_control_);
}

void Sf2Player::drain_direct_system_patch() noexcept {
  if (direct_system_patch_reader_ == nullptr) return;
  DirectSystemPatch patch;
  if (!direct_system_patch_reader_->try_load_into(&patch)) return;
  if (patch.publish_seq <= last_system_direct_seq_) return;
  bool changed = false;
  bool prepared_changed = false;
  const bool new_reset = patch.last_reset_seq > last_system_direct_seq_;
  const auto should_apply = [&](size_t index) {
    const DirectSystemField& field = patch.fields[index];
    // Written after the watermark, or older than a newly stamped reset.
    return field.seq > last_system_direct_seq_ || (new_reset && patch.last_reset_seq >= field.seq);
  };
  const auto value = [&](size_t index, uint8_t default_value) {
    const DirectSystemField& field = patch.fields[index];
    return patch.last_reset_seq >= field.seq ? default_value : field.value;
  };
  for (size_t i = 0; i < kDirectFxMembers.size(); ++i) {
    if (!should_apply(kDirectFxBase + i)) continue;
    const uint8_t next = value(kDirectFxBase + i, GsSystemEffects{}.*kDirectFxMembers[i]);
    if (direct_system_audio_fx_.*kDirectFxMembers[i] != next) changed = true;
    if (prepared_sys_fx_.*kDirectFxMembers[i] != next) prepared_changed = true;
    direct_system_audio_fx_.*kDirectFxMembers[i] = next;
    prepared_sys_fx_.*kDirectFxMembers[i] = next;
  }
  const GsMasterEq eq_default{};
  const uint8_t eq_values[4] = {eq_default.low_freq, eq_default.low_gain, eq_default.high_freq,
                                eq_default.high_gain};
  uint8_t* eq_fields[4] = {&direct_system_audio_eq_.low_freq, &direct_system_audio_eq_.low_gain,
                           &direct_system_audio_eq_.high_freq, &direct_system_audio_eq_.high_gain};
  uint8_t* prepared_eq_fields[4] = {&prepared_master_eq_.low_freq, &prepared_master_eq_.low_gain,
                                    &prepared_master_eq_.high_freq, &prepared_master_eq_.high_gain};
  for (size_t i = 0; i < 4; ++i) {
    if (!should_apply(kDirectEqBase + i)) continue;
    const uint8_t next = value(kDirectEqBase + i, eq_values[i]);
    if (*eq_fields[i] != next) changed = true;
    if (*prepared_eq_fields[i] != next) prepared_changed = true;
    *eq_fields[i] = next;
    *prepared_eq_fields[i] = next;
  }
  for (size_t part = 0; part < 16; ++part) {
    if (!should_apply(kDirectPartEqBase + part)) continue;
    const uint8_t next = value(kDirectPartEqBase + part, 0);
    const bool bypassed = next != 0;
    if (direct_system_audio_bypassed_[part] != bypassed) changed = true;
    if (prepared_eq_part_bypassed_[part] != bypassed) prepared_changed = true;
    direct_system_audio_bypassed_[part] = bypassed;
    prepared_eq_part_bypassed_[part] = bypassed;
  }
  last_system_direct_seq_ = patch.publish_seq;
  // The active overlay carries scheduled fields the direct mirror lacks, so it alone is applied.
  if (prepared_runtime_active_) {
    if (prepared_changed) {
      apply_gs_system_state(prepared_sys_fx_, prepared_master_eq_, prepared_eq_part_bypassed_);
    }
  } else if (changed) {
    apply_gs_system_state(direct_system_audio_fx_, direct_system_audio_eq_,
                          direct_system_audio_bypassed_);
  }
}

float Sf2Player::part_controller_position(int part, uint8_t source) const noexcept {
  // Read at full width, as refresh_channel_mod reads the same controllers.
  const ChannelState& st = channels_[static_cast<size_t>(part & 0x0F)];
  if (source == kEfxSourceBend) return (st.pitch_bend.f14() - 8192.0f) / 8192.0f;
  if (source == kEfxSourceAftertouch) return st.channel_pressure.f7() / 127.0f;
  return st.cc_position[source & 0x7Fu].f7() / 127.0f;
}

PartFxUnitOverrides Sf2Player::prepared_unit_overrides() const noexcept {
  PartFxUnitOverrides out{};
  for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
    if (!prepared_unit_overridden_[unit]) continue;
    out[unit] = prepared_active_nodes_[unit] != nullptr ? &prepared_active_nodes_[unit]->unit_rt
                                                        : &prepared_empty_unit_;
  }
  return out;
}

int Sf2Player::prepared_control_part(size_t unit) const noexcept {
  for (size_t part = 0; part < prepared_part_unit_.size(); ++part) {
    if (prepared_part_unit_[part] == unit) return static_cast<int>(part);
  }
  return -1;
}

std::shared_ptr<Sf2Player::PreparedEfxNode> Sf2Player::find_or_build_prepared_node(size_t unit,
                                                                                   uint16_t type) {
  release_prepared_nodes();
  for (const std::shared_ptr<PreparedEfxNode>& existing : prepared_nodes_) {
    if (existing->domain == prepared_domain_ && existing->unit == unit && existing->type == type) {
      return existing;
    }
  }
  if (unit >= kGsEfxUnitCount || type == 0 || gs_efx_type_defaults(type) == nullptr) {
    return nullptr;
  }

  GsEfx defaults;
  defaults.type = type;
  defaults.type_msb = static_cast<uint8_t>(type >> 8);
  defaults.assigned = true;
  defaults.params = gs_efx_type_defaults(type)->params;
  const GsEfxRealization realization = part_fx_.realization();
  auto node = std::make_shared<PreparedEfxNode>();
  node->domain = prepared_domain_;
  node->unit = static_cast<uint8_t>(unit);
  node->type = type;
  node->unit_rt = part_fx_.build_unit(defaults);
  node->stage_count = static_cast<uint8_t>(std::min<size_t>(node->unit_rt.stages.size(), 255));
  for (size_t s = 0; s < std::min(node->unit_rt.stages.size(), node->default_enabled.size()); ++s) {
    // All on: the enable rules alone carry selector/switch state.
    node->default_enabled[s] = true;
  }

  const GsEfxRowView& rows = part_fx_.row_view();
  const uint16_t row_type = gs_efx_binding_type(rows, type);

  // A null stage is a no-DSP hole; a built one must take every row realtime-safely.
  if (realization == GsEfxRealization::kModern) {
    for (size_t i = 0; i < rows.n_rows; ++i) {
      const GsEfxBindingRow& row = rows.rows[i];
      if (row.type != row_type) continue;
      const Sf2EfxRowTarget target = sf2_resolve_efx_row(node->unit_rt, row);
      if (target.status == Sf2EfxRowResolution::kNoStage ||
          target.status == Sf2EfxRowResolution::kNoProcessor) {
        continue;
      }
      if (target.status != Sf2EfxRowResolution::kResolved) return nullptr;
      if (node->param_dest_count >= node->param_dests.size()) return nullptr;
      PreparedEfxParamDest& dest = node->param_dests[node->param_dest_count++];
      dest.row = row;
      dest.stage_index = static_cast<uint8_t>(target.stage_index);
      dest.param_id = target.param_id;
    }
    for (size_t i = 0; i < rows.n_enables; ++i) {
      const GsEfxEnable& enable = rows.enables[i];
      if (enable.type != row_type) continue;
      if (node->enable_plan_count >= node->enable_plans.size()) return nullptr;
      PreparedEfxEnablePlan& plan = node->enable_plans[node->enable_plan_count++];
      plan.rule = enable;
      plan.stage_indices.fill(0xFF);
      for (uint8_t s = 0; s < enable.n_stages && s < plan.stage_indices.size(); ++s) {
        if (enable.stages[s] >= kGsEfxRowStages.size()) return nullptr;
        const int stage_index = sf2_find_efx_stage(node->unit_rt, kGsEfxRowStages[enable.stages[s]],
                                                   enable.ordinals[s]);
        if (stage_index >= 0) plan.stage_indices[s] = static_cast<uint8_t>(stage_index);
      }
    }
  } else if (!node->unit_rt.stages.empty() && node->unit_rt.stages.front().proc != nullptr) {
    const rt::ProcessorBase* proc = node->unit_rt.stages.front().proc.get();
    for (size_t slot = 0; slot < 20; ++slot) {
      if (!proc->parameter_is_realtime_safe(static_cast<unsigned int>(slot))) return nullptr;
    }
  }

  // Freeze the CONTROL fan-out, every marked row for the slot, with the node.
  for (size_t k = 0; k < node->controls.size(); ++k) {
    Sf2EfxControlRt& control = node->controls[k];
    const uint8_t mark = k == 0 ? '+' : '#';
    const GsEfxBindingRow* first = nullptr;
    for (size_t i = 0; i < rows.n_rows; ++i) {
      const GsEfxBindingRow& row = rows.rows[i];
      if (row.type != row_type || row.printed_mark != mark) continue;
      if (first == nullptr) first = &row;
      if (row.slot != first->slot) continue;
      if (realization == GsEfxRealization::kClassic) {
        control.dest[0] = {0, row.slot, nullptr};
        control.n_dest = 1;
        break;
      }
      const Sf2EfxRowTarget target = sf2_resolve_efx_row(node->unit_rt, row);
      if (target.status != Sf2EfxRowResolution::kResolved) continue;
      if (control.n_dest >= control.dest.size()) continue;
      control.dest[control.n_dest++] = {static_cast<uint8_t>(target.stage_index), target.param_id,
                                        &row};
    }
    if (first == nullptr || control.n_dest == 0) continue;
    control.slot = first->slot;
    const int states = gs_efx_printed_states(type, control.slot);
    if (states > 0) {
      control.lo = 0;
      control.hi = static_cast<uint8_t>(states - 1);
    } else if (first->byte_lo < first->byte_hi) {
      control.lo = first->byte_lo;
      control.hi = first->byte_hi;
      if (first->kind == kGsEfxRowDesigned && first->law.form == kGsEfxFormEnum) {
        control.states = first->law.n_states;
      }
    } else {
      control.lo = 0;
      control.hi = 0x7F;
    }
    control.base_byte = defaults.params[control.slot];
    control.applied_byte = control.base_byte;
  }

  prepared_nodes_.push_back(node);
  return node;
}

bool Sf2Player::prepare_sysex(const uint8_t* data, size_t size,
                              std::shared_ptr<const PreparedMidiSysEx>& out) {
  out.reset();
  if (data == nullptr || size == 0) return false;
  // Unprepared: a null token; the engine re-prepares every clip SysEx at prepare().
  if (!prepared_) return true;
  // A moved-from player lost its identity; only this CONTROL path recreates it.
  if (prepared_owner_identity_ == nullptr) {
    prepared_owner_identity_ = std::make_shared<PreparedOwnerIdentity>();
  }

  // bad_alloc propagates: the engine reports it apart from a missing binding.
  try {
    auto token = std::make_shared<PreparedSysEx>();
    token->owner_identity = prepared_owner_identity_;
    token->domain = prepared_domain_;

    const int addressed_unit = gs_efx_addressed_unit(data, size);
    if (addressed_unit >= 0) {
      token->efx_block = true;
      token->unit = static_cast<uint8_t>(addressed_unit);
      constexpr size_t kMaxWrites = 64;
      std::array<GsWrite, kMaxWrites> writes{};
      const size_t decoded = std::min(
          gs_decode_sysex(data, size, writes.data(), writes.size(), nullptr), writes.size());
      bool has_msb = false;
      bool has_lsb = false;
      uint8_t message_msb = 0;
      uint8_t message_lsb = 0;
      for (size_t i = 0; i < decoded; ++i) {
        if (writes[i].param != GsParam::kEfxType) continue;
        if (writes[i].index == 0) {
          has_msb = true;
          message_msb = writes[i].value;
        } else if (writes[i].index == 1) {
          has_lsb = true;
          message_lsb = writes[i].value;
        }
      }
      // Only an LSB resolves a type; other writes reuse the node active at event time.
      token->full_reapply = has_lsb;

      std::array<uint16_t, kMaxPreparedCandidates> types{};
      size_t type_count = 0;
      const auto add_type = [&](uint16_t type) {
        if (type == 0 || gs_efx_type_defaults(type) == nullptr || type_count >= types.size()) {
          return;
        }
        for (size_t i = 0; i < type_count; ++i) {
          if (types[i] == type) return;
        }
        types[type_count++] = type;
      };
      if (has_lsb) {
        if (has_msb) {
          add_type(static_cast<uint16_t>((static_cast<uint16_t>(message_msb) << 8) |
                                         static_cast<uint16_t>(message_lsb)));
        } else {
          // An LSB alone pairs with every measured MSB carrying that low byte.
          for (const GsEfxTypeDefaults& defaults : kGsEfxTypeDefaults) {
            if (static_cast<uint8_t>(defaults.type & 0x7Fu) == message_lsb) {
              add_type(defaults.type);
            }
            if (type_count == types.size()) break;
          }
        }
      }

      for (size_t type_index = 0; type_index < type_count; ++type_index) {
        const uint16_t type = types[type_index];
        const std::shared_ptr<PreparedEfxNode> node =
            find_or_build_prepared_node(static_cast<size_t>(addressed_unit), type);
        if (node == nullptr) {
          out.reset();
          return false;
        }
        PreparedEfxCandidate candidate;
        candidate.unit = static_cast<uint8_t>(addressed_unit);
        candidate.type = type;
        candidate.node = node.get();
        candidate.lease = node;
        candidate.target.type = type;
        candidate.target.type_msb = static_cast<uint8_t>(type >> 8);
        candidate.target.assigned = true;
        candidate.target.params = gs_efx_type_defaults(type)->params;
        // The parser's own order: type defaults first, later bytes survive.
        if (!apply_gs_efx_sysex(candidate.target, data, size, nullptr)) {
          out.reset();
          return false;
        }
        token->candidates[token->candidate_count++] = std::move(candidate);
      }
    }

    out = std::shared_ptr<const PreparedMidiSysEx>(std::move(token));
    return true;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    out.reset();
    return false;
  }
}

void Sf2Player::activate_prepared_node(size_t unit, PreparedEfxNode* node) noexcept {
  if (unit >= kGsEfxUnitCount || prepared_active_nodes_[unit] == node) return;
  if (node != nullptr) node->audio_pins.fetch_add(1, std::memory_order_acq_rel);
  if (prepared_active_nodes_[unit] != nullptr) {
    prepared_active_nodes_[unit]->audio_pins.fetch_sub(1, std::memory_order_acq_rel);
  }
  prepared_active_nodes_[unit] = node;
}

void Sf2Player::apply_prepared_node_plan(PreparedEfxNode& node, const GsEfx& target,
                                         bool preserve_enable_fade) noexcept {
  if (node.unit_rt.realization == GsEfxRealization::kClassic) {
    if (!node.unit_rt.stages.empty() && node.unit_rt.stages.front().proc != nullptr) {
      rt::ProcessorBase* proc = node.unit_rt.stages.front().proc.get();
      for (size_t slot = 0; slot < target.params.size(); ++slot) {
        if (proc->parameter_is_realtime_safe(static_cast<unsigned int>(slot))) {
          proc->set_parameter(static_cast<unsigned int>(slot),
                              static_cast<float>(target.params[slot]));
        }
      }
    }
    return;
  }

  for (size_t i = 0; i < node.param_dest_count; ++i) {
    const PreparedEfxParamDest& dest = node.param_dests[i];
    if (dest.stage_index >= node.unit_rt.stages.size()) continue;
    Sf2EfxStageRt& stage = node.unit_rt.stages[dest.stage_index];
    if (stage.proc == nullptr || !stage.proc->parameter_is_realtime_safe(dest.param_id)) continue;
    stage.proc->set_parameter(dest.param_id,
                              gs_efx_binding_value(dest.row, target.params[dest.row.slot]));
  }

  // AND every rule in table order over the raw bytes, as gs_efx_insert_chain does.
  std::array<bool, 32> enabled{};
  const size_t stage_count = std::min(node.unit_rt.stages.size(), enabled.size());
  for (size_t s = 0; s < stage_count; ++s) enabled[s] = node.default_enabled[s];
  for (size_t i = 0; i < node.enable_plan_count; ++i) {
    const PreparedEfxEnablePlan& plan = node.enable_plans[i];
    const uint8_t byte = plan.rule.slot < target.params.size() ? target.params[plan.rule.slot] : 0;
    for (uint8_t s = 0; s < plan.rule.n_stages && s < plan.stage_indices.size(); ++s) {
      const uint8_t stage_index = plan.stage_indices[s];
      if (stage_index < stage_count) {
        enabled[stage_index] = enabled[stage_index] && gs_efx_enable_on(plan.rule, byte, s);
      }
    }
  }
  for (size_t s = 0; s < stage_count; ++s) {
    Sf2EfxStageRt& stage = node.unit_rt.stages[s];
    const bool on = enabled[s];
    if (preserve_enable_fade) {
      if (on && !stage.enabled_now && stage.fade <= 0.0f && stage.proc != nullptr) {
        stage.proc->reset();
      }
      stage.enabled_target = on;
      stage.enabled_now = on;
    } else {
      stage.enabled_target = on;
      stage.enabled_now = on;
      stage.fade = on ? 1.0f : 0.0f;
    }
  }
}

void Sf2Player::apply_prepared_efx_controls() noexcept {
  if (!prepared_runtime_active_) return;
  const GsEfx& efx = prepared_efx_[0];
  // The controllers are the lowest part the overlay routes to unit 0 at this block.
  size_t routed_part = 0;
  while (routed_part < prepared_part_unit_.size() && prepared_part_unit_[routed_part] != 0) {
    ++routed_part;
  }
  if (routed_part >= channels_.size()) return;
  const auto apply_controls = [&](const auto& controls, const auto& stages) noexcept {
    for (size_t k = 0; k < controls.size() && k < efx.control_source.size(); ++k) {
      const auto& control = controls[k];
      const uint8_t source = efx.control_source[k];
      if (source == 0 || source > kEfxSourceBend || control.n_dest == 0 ||
          control.slot >= efx.params.size()) {
        continue;
      }
      Sf2EfxControlRt effective;
      effective.source = source;
      effective.depth = efx.control_depth[k];
      effective.slot = control.slot;
      effective.lo = control.lo;
      effective.hi = control.hi;
      effective.states = control.states;
      effective.base_byte = efx.params[control.slot];
      const uint8_t byte = efx_control_byte(
          effective, part_controller_position(static_cast<int>(routed_part), effective.source));
      for (uint8_t d = 0; d < control.n_dest; ++d) {
        const Sf2EfxControlDest& dest = control.dest[d];
        if (dest.stage_index >= stages.size()) continue;
        rt::ProcessorBase* proc = stages[dest.stage_index].proc.get();
        if (proc == nullptr || !proc->parameter_is_realtime_safe(dest.param_id)) continue;
        const float value =
            dest.binding != nullptr ? gs_efx_binding_value(*dest.binding, byte) : byte;
        proc->set_parameter(dest.param_id, value);
      }
    }
  };

  if (prepared_active_nodes_[0] != nullptr && prepared_unit_overridden_[0]) {
    PreparedEfxNode& node = *prepared_active_nodes_[0];
    apply_controls(node.controls, node.unit_rt.stages);
    return;
  }
  if (prepared_unit_overridden_[0]) return;
  const PartFxSnapshot* snapshot = part_fx_.current();
  if (snapshot == nullptr || !snapshot->unit_fed[0] || snapshot->gs_efx_state[0].type != efx.type) {
    return;
  }
  apply_controls(snapshot->units[0].legacy_controls, snapshot->units[0].stages);
}

void Sf2Player::apply_prepared_candidate(const PreparedSysEx& token,
                                         const PreparedEfxCandidate& candidate) noexcept {
  if (candidate.node == nullptr || candidate.unit >= kGsEfxUnitCount) return;
  const PreparedEfxNode* previous = prepared_active_nodes_[candidate.unit];
  const bool switching = previous != candidate.node;
  // A newly selected node is reset after its parameters are written; a kept one keeps its tail.
  activate_prepared_node(candidate.unit, candidate.node);
  prepared_unit_overridden_[candidate.unit] = true;
  // The candidate selects the node; the audio-owned raw state supplies every byte.
  apply_prepared_node_plan(*candidate.node, prepared_efx_[candidate.unit], !switching);
  if (switching) {
    sf2_reset_efx_unit(candidate.node->unit_rt);
  }
  (void)token;
}

void Sf2Player::apply_prepared_gs_delta(const PreparedSysEx& token, const uint8_t* data,
                                        size_t size, bool apply_performance) noexcept {
  if (prepared_owner_identity_ == nullptr || token.owner_identity == nullptr ||
      token.owner_identity.get() != prepared_owner_identity_.get() ||
      token.domain != prepared_audio_domain_ || data == nullptr || size == 0) {
    return;
  }
  if (!prepared_runtime_active_) {
    // The overlay was seeded at the last quiescent boundary, never from control mirrors.
    prepared_runtime_active_ = true;
    prepared_base_synced_ = false;
  }
  sync_prepared_base();

  const GsSysEx msg = parse_gs_sysex(data, size);
  if (msg.kind == GsSysExKind::kGm1Reset || msg.kind == GsSysExKind::kGm2Reset ||
      msg.kind == GsSysExKind::kGsReset) {
    // Host/static part chains stay in the snapshot and keep their tails.
    if (apply_performance) handle_sysex(data, size);
    for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
      activate_prepared_node(unit, nullptr);
      prepared_unit_overridden_[unit] = true;
    }
    prepared_efx_ = {};
    prepared_assign_ = {};
    prepared_part_unit_.fill(PartFxSnapshot::kNoUnit);
    prepared_unit_fed_.fill(false);
    prepared_any_unit_ = false;
    if (prepared_base_synced_) rebuild_prepared_routing();
    if (apply_performance) {
      prepared_sys_fx_ = {};
      prepared_master_eq_ = {};
      prepared_eq_part_bypassed_ = {};
      apply_gs_system_state(prepared_sys_fx_, prepared_master_eq_, prepared_eq_part_bypassed_);
    }
    return;
  }

  // Channel bytes first; EFX and system state below use only the overlay.
  if (apply_performance) handle_sysex(data, size);
  if (msg.kind == GsSysExKind::kEfxPartSwitch) {
    prepared_assign_[msg.channel & 0x0Fu] = msg.value;
    if (prepared_base_synced_) rebuild_prepared_routing();
  }

  if (token.efx_block && token.unit < kGsEfxUnitCount) {
    bool type_changed = false;
    apply_gs_efx_sysex(prepared_efx_[token.unit], data, size, &type_changed);
    const bool was_overridden = prepared_unit_overridden_[token.unit];
    PreparedEfxNode* selected = nullptr;
    for (uint8_t i = 0; i < token.candidate_count; ++i) {
      if (token.candidates[i].unit == token.unit &&
          token.candidates[i].type == prepared_efx_[token.unit].type) {
        selected = token.candidates[i].node;
        apply_prepared_candidate(token, token.candidates[i]);
        break;
      }
    }
    if (selected == nullptr) {
      // Keep the active node, else a same-type published unit, else an empty override.
      PreparedEfxNode* active = prepared_active_nodes_[token.unit];
      if (!token.full_reapply && active != nullptr &&
          active->type == prepared_efx_[token.unit].type) {
        prepared_unit_overridden_[token.unit] = true;
        apply_prepared_node_plan(*active, prepared_efx_[token.unit], true);
      } else if (!token.full_reapply && !was_overridden && active == nullptr) {
        const PartFxSnapshot* snapshot = part_fx_.current();
        const bool matching_legacy =
            snapshot != nullptr && snapshot->unit_fed[token.unit] &&
            snapshot->gs_efx_state[token.unit].type == prepared_efx_[token.unit].type;
        if (matching_legacy) {
          // Not overridden: the published processors keep running with their tail.
          part_fx_.apply_legacy_plan(token.unit, prepared_efx_[token.unit],
                                     prepared_control_part(token.unit), *this);
        } else {
          activate_prepared_node(token.unit, nullptr);
          prepared_unit_overridden_[token.unit] = true;
        }
      } else {
        activate_prepared_node(token.unit, nullptr);
        prepared_unit_overridden_[token.unit] = true;
      }
    }
    if (prepared_base_synced_) rebuild_prepared_routing();
    (void)type_changed;
  }

  if (apply_performance && apply_gs_system_sysex_to(prepared_sys_fx_, prepared_master_eq_,
                                                    prepared_eq_part_bypassed_, data, size)) {
    apply_gs_system_state(prepared_sys_fx_, prepared_master_eq_, prepared_eq_part_bypassed_);
  }
}

void Sf2Player::realize_gs_efx() {
  part_fx_.clear_dirty();
  if (!prepared_) return;
  part_fx_.publish();
}

bool Sf2Player::set_part_rig(uint8_t part, const PartRig& rig) noexcept {
  if (!validate_part_rig(part, rig)) return false;
  try {
    PartFxStage::RigTable previous = part_fx_.rig_table();
    part_fx_.set_part_rig(part, rig);
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

std::vector<std::string> Sf2Player::part_rig_stage_names(uint8_t part) const {
  const PartFxSnapshot* snapshot = part_fx_.control_current();
  if (snapshot == nullptr || part >= 16) return {};
  return snapshot->stage_names[part];
}

Sf2Player::DirectGsNode* Sf2Player::reserve_restart_node() {
  if (!prepared_) return nullptr;
  sweep_direct_gs_nodes();
  direct_queue_->owned.push_back(std::make_unique<DirectGsNode>());
  DirectGsNode* node = direct_queue_->owned.back().get();
  node->seq = direct_queue_->next_seq++;
  return node;
}

void Sf2Player::restart_prepared_runtime(DirectGsNode* reserved) noexcept {
  ++prepared_domain_;
  if (reserved != nullptr) {
    reserved->restart_domain = prepared_domain_;
    direct_queue_->control_tail->next.store(reserved, std::memory_order_release);
    direct_queue_->control_tail = reserved;
    direct_queue_->published_tail.store(reserved, std::memory_order_release);
  }
  release_prepared_nodes();
}

void Sf2Player::restart_prepared_audio_runtime(uint64_t domain) noexcept {
  prepared_audio_domain_ = domain;
  // The rebuilt snapshot was published before this node.
  part_fx_.acquire();
  const PartFxSnapshot* snapshot = part_fx_.current();
  for (size_t unit = 0; unit < kGsEfxUnitCount; ++unit) {
    activate_prepared_node(unit, nullptr);
    prepared_unit_overridden_[unit] = false;
    if (!prepared_runtime_active_) continue;
    const bool matching = snapshot != nullptr && snapshot->unit_fed[unit] &&
                          snapshot->gs_efx_state[unit].type == prepared_efx_[unit].type;
    if (matching) {
      part_fx_.apply_legacy_plan(unit, prepared_efx_[unit], prepared_control_part(unit), *this);
    } else {
      prepared_unit_overridden_[unit] = true;
    }
  }
}

void Sf2Player::set_gs_efx_realization(GsEfxRealization realization) {
  if (part_fx_.realization() == realization) return;
  DirectGsNode* restart = reserve_restart_node();
  const GsEfxRealization previous = part_fx_.realization();
  const PartFxStage::Checkpoint saved = part_fx_.checkpoint();
  part_fx_.set_realization(realization);
  try {
    realize_gs_efx();
  } catch (...) {
    part_fx_.set_realization(previous);
    part_fx_.restore(saved);
    if (restart != nullptr) direct_queue_->owned.pop_back();
    throw;
  }
  restart_prepared_runtime(restart);
}

void Sf2Player::set_gs_efx_rows(const GsEfxRowView* rows) {
  if (part_fx_.rows() == rows) return;
  DirectGsNode* restart = reserve_restart_node();
  const GsEfxRowView* previous_rows = part_fx_.rows();
  const PartFxStage::Checkpoint saved = part_fx_.checkpoint();
  part_fx_.set_rows(rows);
  try {
    // A throwing factory leaves the published graph and its tokens usable.
    realize_gs_efx();
  } catch (...) {
    part_fx_.set_rows(previous_rows);
    part_fx_.restore(saved);
    if (restart != nullptr) direct_queue_->owned.pop_back();
    throw;
  }
  restart_prepared_runtime(restart);
}

int Sf2Player::gs_efx_control_byte(size_t control) const noexcept {
  if (prepared_runtime_active_ && prepared_active_nodes_[0] != nullptr) {
    const PreparedEfxNode& node = *prepared_active_nodes_[0];
    if (control >= node.controls.size()) return -1;
    Sf2EfxControlRt effective = node.controls[control];
    const GsEfx& efx = prepared_efx_[0];
    const uint8_t source = efx.control_source[control];
    if (source == 0 || source > kEfxSourceBend || effective.n_dest == 0) return -1;
    size_t part = 0;
    while (part < prepared_part_unit_.size() && prepared_part_unit_[part] != 0) ++part;
    if (part >= prepared_part_unit_.size()) return -1;
    effective.source = source;
    effective.depth = efx.control_depth[control];
    if (effective.slot >= efx.params.size()) return -1;
    effective.base_byte = efx.params[effective.slot];
    return efx_control_byte(effective,
                            part_controller_position(static_cast<int>(part), effective.source));
  }
  const PartFxSnapshot* snapshot = part_fx_.current();
  if (snapshot == nullptr || control >= snapshot->controls.size()) return -1;
  const Sf2EfxControlRt& c = snapshot->controls[control];
  return c.n_dest == 0 ? -1 : c.applied_byte;
}

void Sf2Player::on_control_sysex(const uint8_t* data, size_t size) noexcept {
  if (!prepared_ || data == nullptr || size == 0) return;
  const GsSysEx msg = parse_gs_sysex(data, size);
  const bool efx_message = gs_sysex_resets(msg.kind) || msg.kind == GsSysExKind::kEfxPartSwitch ||
                           gs_efx_addressed_unit(data, size) >= 0;
  // A checkpoint, so a throwing rebuild under this noexcept hook leaves the mirror whole.
  const PartFxStage::Checkpoint mirror_before = part_fx_.checkpoint();
  const std::array<bool, kGsEfxUnitCount> fallback_before = direct_legacy_efx_fallback_;
  bool direct_rebuild_failed = false;
  const auto restore_direct_mirror = [&]() noexcept {
    part_fx_.restore(mirror_before);
    direct_legacy_efx_fallback_ = fallback_before;
  };
  std::shared_ptr<const PreparedMidiSysEx> prepared;
  bool prepared_ok = true;
  if (efx_message) {
    // The same plan as a scheduled event; a non-RT custom processor uses the full snapshot.
    try {
      prepared_ok = prepare_sysex(data, size, prepared);
    } catch (...) {
      prepared_ok = false;
      prepared.reset();
    }
  }
  // Only this hook rejects a plan, and only while a custom unit renders from the full snapshot.
  const PreparedSysEx* prepared_token =
      prepared != nullptr ? dynamic_cast<const PreparedSysEx*>(prepared.get()) : nullptr;
  const bool has_legacy_fallback =
      std::any_of(direct_legacy_efx_fallback_.begin(), direct_legacy_efx_fallback_.end(),
                  [](bool fallback) { return fallback; });
  bool forced_legacy_direct = false;
  bool legacy_candidate_replaces_unit = false;
  uint8_t legacy_candidate_unit = 0;
  if (prepared_ok && prepared_token != nullptr && has_legacy_fallback &&
      (prepared_token->efx_block || msg.kind == GsSysExKind::kEfxPartSwitch)) {
    // The overlay would replace the custom unit with a null node; stay on the snapshot.
    forced_legacy_direct = true;
    if (prepared_token->efx_block && prepared_token->candidate_count != 0 &&
        prepared_token->unit < kGsEfxUnitCount) {
      legacy_candidate_replaces_unit = true;
      legacy_candidate_unit = prepared_token->unit;
    }
    prepared_ok = false;
    prepared.reset();
    prepared_token = nullptr;
  }
  if (efx_message) {
    if (prepared_ok) {
      // Update the mirror only; a rebuild would overwrite scheduled raw state and cut tails.
      part_fx_.mirror_sysex(data, size);
      switch (msg.kind) {
        case GsSysExKind::kGm1Reset:
        case GsSysExKind::kGm2Reset:
        case GsSysExKind::kGsReset:
          direct_legacy_efx_fallback_.fill(false);
          break;
        case GsSysExKind::kEfxPartSwitch:
          break;
        case GsSysExKind::kUseForRhythm:
        case GsSysExKind::kNone: {
          const int unit = gs_efx_addressed_unit(data, size);
          // A selected prepared node ends this unit's full-snapshot fallback.
          if (unit >= 0 && prepared_token != nullptr && prepared_token->candidate_count != 0) {
            direct_legacy_efx_fallback_[static_cast<size_t>(unit)] = false;
          }
          break;
        }
      }
      part_fx_.clear_dirty();
    } else {
      const int unit = gs_efx_addressed_unit(data, size);
      if (unit >= 0 && !forced_legacy_direct)
        direct_legacy_efx_fallback_[static_cast<size_t>(unit)] = true;
      bool rebuilt = false;
      try {
        rebuilt = part_fx_.apply_control_sysex(data, size);
      } catch (...) {
        // Translation allocates; keep the previous generation and the mirror.
        restore_direct_mirror();
        direct_rebuild_failed = true;
      }
      if (rebuilt) {
        // A non-RT custom processor uses the direct full-snapshot publication.
        try {
          realize_gs_efx();
        } catch (...) {
          restore_direct_mirror();
          direct_rebuild_failed = true;
        }
      }
      if (rebuilt && !direct_rebuild_failed && legacy_candidate_replaces_unit)
        direct_legacy_efx_fallback_[legacy_candidate_unit] = false;
    }
  }
  if (efx_message && !direct_rebuild_failed) {
    const bool queued = append_direct_gs_node(data, size, std::move(prepared), !prepared_ok);
    if (!queued && prepared_ok) {
      // A node that could not be queued falls back to the full-snapshot publication.
      try {
        realize_gs_efx();
        const int unit = gs_efx_addressed_unit(data, size);
        if (unit >= 0) direct_legacy_efx_fallback_[static_cast<size_t>(unit)] = true;
      } catch (...) {
        restore_direct_mirror();
      }
    }
  }

  bool changed = gs_sysex_resets(msg.kind);
  if (changed) {
    sys_fx_ = {};
    master_eq_ = {};
    eq_part_bypassed_ = {};
  }
  changed = apply_gs_system_sysex(data, size) || changed;
  if (changed) publish_direct_system_patch(gs_sysex_resets(msg.kind), data, size);
}

void Sf2Player::on_prepared_sysex_accepted(const uint8_t* data, size_t size,
                                           const PreparedMidiSysEx* prepared) noexcept {
  // The audio event applies it at its frame; never mirror it here, whatever the token.
  (void)data;
  (void)size;
  (void)prepared;
}

void Sf2Player::refresh_rx_channels() noexcept {
  rx_parts_ = {};
  for (uint8_t part = 0; part < 16; ++part) {
    // 16 is RX CHANNEL OFF, and so is anything above it: the part is in no
    // channel's word and therefore reached by nothing.
    const uint8_t ch = channels_[part].rx_channel;
    if (ch < 16) rx_parts_[ch] |= static_cast<uint16_t>(1u << part);
  }
}

void Sf2Player::refresh_part_rig(uint8_t channel) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  uint8_t id = 0;
  // Resolved whether or not the part takes the bank's rig: an entry may select
  // it later, and the stage decides whether this id reaches a chain.
  if (config_.synth_fallback) {
    const uint16_t bank = effective_bank(ch);
    const uint8_t program = channels_[ch].program;
    // Only where the note plays the model floor. A SoundFont's electric guitar
    // was recorded through an amplifier, so a second one on top of it is the
    // bake docs/voicing.md exists to remove.
    const bool model_floor = soundfont_ == nullptr || resolve_preset(bank, program) < 0 ||
                             (config_.prefer_model_for_modeled_families &&
                              gm_program_has_dedicated_model(bank, program));
    if (model_floor) id = gm_fallback_rig(bank, program).id;
  }
  // Same thread split as every other realise trigger: offline rebuilds inline at
  // the next block, live waits for the control thread to come past. A live
  // program change therefore keeps the rig it had, which is the reach a
  // sequenced insertion-effect SysEx already has.
  if (part_fx_.publish_part_rig(ch, id) && config_.realize_efx_inline) part_fx_.mark_dirty();
}

void Sf2Player::refresh_channel_mod(uint8_t channel) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  Sf2ChannelMod& mod = channel_mods_[ch];
  const float bend_cents = channel_bend_cents(st.pitch_bend, st.bend_range_cents);
  mod.mod_wheel01 = st.mod_wheel.f7() / 127.0f;
  // The mod matrix's live sources. Filled here as well as in NativeSynth so the
  // same route reads the same controller whichever player owns the voice; poly
  // aftertouch has no position on this path, so the voice's own stays at rest.
  mod.breath01 = st.cc_position[2].f7() / 127.0f;
  mod.aftertouch01 = st.channel_pressure.f7() / 127.0f;
  mod.expression01 = st.cc_position[11].f7() / 127.0f;
  mod.pitch_bend01 = (st.pitch_bend.f14() - 8192.0f) / 8192.0f;
  // Where each controller source presently sits, in the block's own order. The
  // bend and polyphonic aftertouch have no position here and stay at rest, so
  // the sum below is over six terms and four of them can move.
  std::array<float, kGsCtrlSourceCount> source01{};
  source01[static_cast<size_t>(GsCtrlSource::kModulation)] = mod.mod_wheel01;
  source01[static_cast<size_t>(GsCtrlSource::kChannelAftertouch)] =
      st.channel_pressure.f7() / 127.0f;
  // Read through the number rather than stored beside it, so pointing a source
  // at a controller that is already somewhere reads where it is: a file writes
  // the assignment and the controller in whichever order it likes.
  source01[static_cast<size_t>(GsCtrlSource::kCc1)] =
      st.cc_position[st.assignable_cc[0] & 0x7Fu].f7() / 127.0f;
  source01[static_cast<size_t>(GsCtrlSource::kCc2)] =
      st.cc_position[st.assignable_cc[1] & 0x7Fu].f7() / 127.0f;

  // Each destination is the sum of what its sources are worth where they sit.
  // Pitch starts at the bend rather than at zero because the two add on one
  // field, as MASTER TUNE and RPN 00 01 do.
  float pitch_cents = bend_cents;
  float cutoff_cents = 0.0f;
  float amp_fraction = 0.0f;
  float vib_cents = 0.0f;
  float tvf_lfo_cents = 0.0f;
  float tva_depth = 0.0f;
  // The rate is summed in cents and taken through one exponent below, so an
  // untouched set of sources is exactly 1 rather than a product of numbers
  // near it.
  float lfo_rate_cents = 0.0f;
  for (size_t s = 0; s < kGsCtrlSourceCount; ++s) {
    const GsDestinationSet& d = st.ctrl_dest[s];
    const float at = source01[s];
    pitch_cents += d.pitch_cents * at;
    cutoff_cents += d.cutoff_cents * at;
    amp_fraction += d.amp_fraction * at;
    vib_cents += d.vib_depth_cents * at;
    tvf_lfo_cents += d.tvf_lfo_cents * at;
    tva_depth += d.tva_depth * at;
    lfo_rate_cents += gs_mod_lfo_rate_cents(d.lfo_rate) * at;
  }

  mod.pitch_cents = pitch_cents;
  // AMPLITUDE CONTROL folds into the part's gain rather than into a field of its
  // own: it is a percentage of the part's level, and this is that level. Floored
  // at zero, where two sources each asking for -100 % would otherwise arrive at
  // a negative gain, which is a phase inversion rather than a quieter part.
  mod.gain =
      sf2_cc_gain(st.volume) * sf2_cc_gain(st.expression) * std::max(0.0f, 1.0f + amp_fraction);
  mod.extra_vibrato_cents = vib_cents;
  mod.mod_cutoff_cents = cutoff_cents;
  mod.vib_rate_scale = lfo_rate_cents != 0.0f ? std::exp2(lfo_rate_cents / 1200.0f) : 1.0f;
  // Clamped at 1 for the reason the gain is floored at 0: past full depth the
  // trough would take the amplitude through zero and out the other side.
  mod.tremolo_depth01 = std::min(1.0f, tva_depth);
  // Not clamped, unlike the two above it: a filter swing has no end of its own
  // to pass, and the cutoff it lands on is bounded where every other cutoff is.
  mod.lfo_cutoff_cents = tvf_lfo_cents;
  mod.pan_units = (st.pan.f7() - 64.0f) / 63.0f * 500.0f;
  mod.reverb_send = kCcSendDepth * static_cast<float>(st.reverb_send) / 127.0f;
  mod.chorus_send = kCcSendDepth * static_cast<float>(st.chorus_send) / 127.0f;
  mod.delay_send = kCcSendDepth * static_cast<float>(st.delay_send) / 127.0f;
  // Fallback voices have no zone send generators; weight the channel sends
  // by the program's ambience profile for that path only. Multiplicative, so
  // CC 0 stays fully dry and the controllers keep their meaning.
  const GmFallbackSends sends = gm_fallback_sends(effective_bank(ch), st.program);
  mod.fallback_reverb_send = std::min(1.0f, mod.reverb_send * sends.reverb_scale);
  mod.fallback_chorus_send = std::min(1.0f, mod.chorus_send * sends.chorus_scale);
}

uint16_t Sf2Player::effective_bank(uint8_t channel) const noexcept {
  const ChannelState& st = channels_[channel & 0x0Fu];
  return gs_effective_bank(st.bank_msb, st.bank_lsb, st.is_drum());
}

int resolve_gs_preset(const Sf2File& soundfont, uint16_t bank, uint8_t program) noexcept {
  // Exact (bank, program).
  int idx = soundfont.find_preset(bank, program);
  if (idx >= 0) return idx;
  // GS variation fallback: unknown variation banks fall back to the capital
  // tone (bank 0); drum banks fall back to the standard kit (program 0).
  if (bank == kDrumBank) {
    idx = soundfont.find_preset(kDrumBank, 0);
    return idx;
  }
  if (bank != 0) {
    idx = soundfont.find_preset(0, program);
    if (idx >= 0) return idx;
  }
  return -1;
}

int Sf2Player::resolve_preset(uint16_t bank, uint8_t program) const noexcept {
  if (soundfont_ == nullptr) return -1;
  return resolve_gs_preset(*soundfont_, bank, program);
}

}  // namespace sonare::midi::synth
