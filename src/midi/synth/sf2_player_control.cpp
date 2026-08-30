#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "midi/builtin_synth.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"

namespace sonare::midi::synth {

bool Sf2Player::handle_sysex(const uint8_t* data, size_t size) noexcept {
  const GsSysEx msg = parse_gs_sysex(data, size);
  switch (msg.kind) {
    case GsSysExKind::kGmReset:
      gm_reset();
      return true;
    case GsSysExKind::kGsReset:
      gs_reset();
      return true;
    case GsSysExKind::kUseForRhythm:
      channels_[msg.channel & 0x0Fu].drums = msg.value != 0;
      // The part's effective bank just moved between melodic and rhythm, and
      // the fallback ambience floor is keyed on it.
      refresh_channel_mod(msg.channel & 0x0Fu);
      return true;
    case GsSysExKind::kEfxPartSwitch:
      // Route/unroute the part through the EFX. Offline (inline) updates the
      // mirror here on the render thread; live leaves the mirror to the control
      // thread's on_control_sysex (which realises + swaps the chains wait-free).
      if (config_.realize_efx_inline) {
        efx_part_enabled_[msg.channel & 0x0Fu] = msg.value != 0;
        gs_efx_dirty_ = true;
      }
      return true;
    case GsSysExKind::kNone:
      break;
  }
  // Part parameters (40 1x xx). These alias controllers the render thread
  // already owns, so unlike the effect blocks they apply here in both modes.
  if (apply_gs_part_sysex(data, size)) return true;
  // Master tuning / volume / pan (40 00 00-06), on the same thread split.
  if (apply_gs_master_sysex(data, size)) return true;
  // GS insertion-effect (EFX) block writes (address 40 03 xx). Offline captures
  // the raw wire into the mirror so process() can realise it inline; live routes
  // realisation through the control thread (on_control_sysex), so the audio
  // thread must not touch the mirror the builder reads.
  if (config_.realize_efx_inline && apply_gs_efx_sysex(efx_, data, size)) {
    gs_efx_dirty_ = true;
    return true;
  }
  // System-effect (40 01 30-5A), master-EQ (40 02 00-03) and part EQ switch
  // (40 4x 20) writes, on the same thread split as the EFX block above.
  if (config_.realize_efx_inline && apply_gs_system_sysex(data, size)) {
    gs_system_dirty_ = true;
    return true;
  }
  return false;
}

bool Sf2Player::apply_gs_system_sysex(const uint8_t* data, size_t size) noexcept {
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
        gs_apply_reverb_macro(sys_fx_, w.value);
        break;
      case GsParam::kReverbCharacter:
        sys_fx_.reverb_character = w.value;
        break;
      case GsParam::kReverbPreLpf:
        sys_fx_.reverb_pre_lpf = w.value;
        break;
      case GsParam::kReverbLevel:
        sys_fx_.reverb_level = w.value;
        break;
      case GsParam::kReverbTime:
        sys_fx_.reverb_time = w.value;
        break;
      case GsParam::kReverbDelayFeedback:
        sys_fx_.reverb_delay_feedback = w.value;
        break;
      case GsParam::kReverbPredelay:
        sys_fx_.reverb_predelay = w.value;
        break;
      case GsParam::kChorusMacro:
        gs_apply_chorus_macro(sys_fx_, w.value);
        break;
      case GsParam::kChorusPreLpf:
        sys_fx_.chorus_pre_lpf = w.value;
        break;
      case GsParam::kChorusLevel:
        sys_fx_.chorus_level = w.value;
        break;
      case GsParam::kChorusFeedback:
        sys_fx_.chorus_feedback = w.value;
        break;
      case GsParam::kChorusDelay:
        sys_fx_.chorus_delay = w.value;
        break;
      case GsParam::kChorusRate:
        sys_fx_.chorus_rate = w.value;
        break;
      case GsParam::kChorusDepth:
        sys_fx_.chorus_depth = w.value;
        break;
      case GsParam::kChorusSendToReverb:
        sys_fx_.chorus_send_to_reverb = w.value;
        break;
      case GsParam::kChorusSendToDelay:
        sys_fx_.chorus_send_to_delay = w.value;
        break;
      case GsParam::kDelayMacro:
        gs_apply_delay_macro(sys_fx_, w.value);
        break;
      case GsParam::kDelayPreLpf:
        sys_fx_.delay_pre_lpf = w.value;
        break;
      case GsParam::kDelayTimeCenter:
        sys_fx_.delay_time_center = w.value;
        break;
      case GsParam::kDelayTimeRatioLeft:
        sys_fx_.delay_time_ratio_left = w.value;
        break;
      case GsParam::kDelayTimeRatioRight:
        sys_fx_.delay_time_ratio_right = w.value;
        break;
      case GsParam::kDelayLevelCenter:
        sys_fx_.delay_level_center = w.value;
        break;
      case GsParam::kDelayLevelLeft:
        sys_fx_.delay_level_left = w.value;
        break;
      case GsParam::kDelayLevelRight:
        sys_fx_.delay_level_right = w.value;
        break;
      case GsParam::kDelayLevel:
        sys_fx_.delay_level = w.value;
        break;
      case GsParam::kDelayFeedback:
        sys_fx_.delay_feedback = w.value;
        break;
      case GsParam::kDelaySendToReverb:
        sys_fx_.delay_send_to_reverb = w.value;
        break;
      case GsParam::kEqLowFreq:
        master_eq_.low_freq = w.value;
        break;
      case GsParam::kEqLowGain:
        master_eq_.low_gain = w.value;
        break;
      case GsParam::kEqHighFreq:
        master_eq_.high_freq = w.value;
        break;
      case GsParam::kEqHighGain:
        master_eq_.high_gain = w.value;
        break;
      case GsParam::kPartEqSwitch:
        eq_part_bypassed_[w.part & 0x0Fu] = w.value == 0;
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
  for (size_t i = 0; i < std::min(decoded, kMaxWrites); ++i) {
    const GsWrite& w = writes[i];
    const GsAddressEntry* entry = gs_lookup_address(w.addr);
    if (entry == nullptr || !gs_value_in_range(*entry, w.value)) continue;
    ChannelState& st = channels_[w.part & 0x0Fu];
    switch (w.param) {
      case GsParam::kPartLevel:
        st.volume = w.value;
        break;
      case GsParam::kPartPanpot:
        // The manual's own "= CC#10, except RANDOM": 00 is RANDOM at this
        // address and hard left at the controller. Randomness has no place in a
        // bit-identical bounce, so it answers centre (docs/gs.md).
        st.pan = w.value == 0 ? 0x40 : w.value;
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
      case GsParam::kPartKeyShift:
        // Held raw and decoded at the render, where the rhythm-part exclusion
        // is: a part that becomes drums later still has to stop taking it.
        st.pitch_key_shift = w.value;
        break;
      case GsParam::kPartToneModify:
        gs_apply_tone_modify(st.gs, w.index, w.value);
        break;
      default:
        continue;
    }
    dirty |= static_cast<uint16_t>(1u << (w.part & 0x0Fu));
  }
  for (uint8_t ch = 0; ch < 16; ++ch) {
    if ((dirty & (1u << ch)) != 0) refresh_channel_mod(ch);
  }
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

void Sf2Player::drain_gs_system_updates() noexcept {
  GsSystemUpdate update;
  bool pending = false;
  while (sys_queue_->pop(update)) pending = true;
  // The state is absolute, so only the newest entry means anything.
  if (pending) apply_gs_system_state(update.fx, update.eq, update.eq_part_bypassed);
}

namespace {

/// Reads the numeric value for @p key out of a flat JSON object string
/// (`{"key":number,...}`). The realised EFX stage params are always flat
/// key -> number objects, so a full JSON parser is unnecessary here. Returns
/// false (leaving @p out untouched) when the key is absent or has no number.
bool json_find_number(std::string_view json, std::string_view key, float& out) {
  std::string needle;
  needle.reserve(key.size() + 2);
  needle.push_back('"');
  needle.append(key.data(), key.size());
  needle.push_back('"');
  const size_t kpos = json.find(needle);
  if (kpos == std::string_view::npos) return false;
  size_t p = kpos + needle.size();
  while (p < json.size() && (json[p] == ' ' || json[p] == ':')) ++p;
  const size_t start = p;
  while (p < json.size()) {
    const char c = json[p];
    if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E') {
      ++p;
    } else {
      break;
    }
  }
  if (p == start) return false;
  const std::string token(json.substr(start, p - start));
  char* end = nullptr;
  const double v = std::strtod(token.c_str(), &end);
  if (end == token.c_str()) return false;
  out = static_cast<float>(v);
  return true;
}

}  // namespace

std::shared_ptr<Sf2RealizedEfx> Sf2Player::build_realized_efx() const {
  auto out = std::make_shared<Sf2RealizedEfx>();
  for (int part = 0; part < 16; ++part) {
    const Sf2PartInsert& insert = config_.part_inserts[static_cast<size_t>(part)];
    const bool static_insert = insert.type != Sf2InsertType::kNone;
    std::vector<std::unique_ptr<rt::ProcessorBase>>& chain = out->chains[static_cast<size_t>(part)];
    // A config kProcessor slot is a caller-owned static insert built once from
    // its name; it always busses the part regardless of the EFX unit.
    if (insert.type == Sf2InsertType::kProcessor) {
      if (config_.insert_factory && !insert.insert_name.empty()) {
        auto proc = config_.insert_factory(insert.insert_name, insert.insert_params_json);
        if (proc != nullptr) {
          proc->prepare(sample_rate_, kChunkFrames);
          chain.push_back(std::move(proc));
        }
      }
      out->part_bussed[static_cast<size_t>(part)] = true;
      out->any_bussed = true;
      continue;
    }
    if (efx_.assigned && efx_part_enabled_[static_cast<size_t>(part)] && config_.insert_factory) {
      // Realise the EFX chain (single-effect = one stage, composite = its block
      // chain). Stages whose factory build returns null (e.g. an FX stage in a
      // no-FX build) are skipped, so a partial chain still runs.
      for (const GsEfxStage& stage : gs_efx_insert_chain(efx_)) {
        auto proc = config_.insert_factory(stage.name, stage.params_json);
        if (proc != nullptr) {
          proc->prepare(sample_rate_, kChunkFrames);
          chain.push_back(std::move(proc));
        }
      }
    }
    // Buss the part only when it carries a static insert (kDrive) or a live EFX
    // chain, so unaffected parts keep adding straight to the dry mix.
    out->part_bussed[static_cast<size_t>(part)] = static_insert || !chain.empty();
    out->any_bussed = out->any_bussed || out->part_bussed[static_cast<size_t>(part)];
  }
  return out;
}

void Sf2Player::realize_gs_efx() {
  gs_efx_dirty_ = false;
  if (!prepared_) return;
  efx_pub_->publish(build_realized_efx());
}

bool Sf2Player::apply_efx_sysex(const uint8_t* data, size_t size) noexcept {
  // Returns true when a full chain rebuild + republish is required, false when
  // the message was handled without one (applied in place, or not an EFX
  // message). The caller (on_control_sysex) only realises on a true return.
  const GsSysEx msg = parse_gs_sysex(data, size);
  switch (msg.kind) {
    case GsSysExKind::kGmReset:
    case GsSysExKind::kGsReset:
      // A GS/GM reset clears the EFX unit and the part switches (Thru): the
      // routing structure changes, so a full rebuild is required.
      efx_ = {};
      efx_part_enabled_ = {};
      return true;
    case GsSysExKind::kEfxPartSwitch:
      // Routing a part in/out of the EFX changes the active-part set: rebuild.
      efx_part_enabled_[msg.channel & 0x0Fu] = msg.value != 0;
      return true;
    case GsSysExKind::kUseForRhythm:
    case GsSysExKind::kNone:
      break;
  }
  // EFX-block write (40 03 xx). A TYPE change restructures the insert chain and
  // needs a rebuild; a parameter/send-only edit is applied to the already-built
  // processors WITHOUT rebuilding, so their DSP state (reverb/delay tails)
  // survives (no click/tail dropout). The parameter values are resolved to
  // {part, stage, param_id, value} tuples on THIS (control) thread and handed to
  // the audio thread through a wait-free SPSC queue; the audio thread applies
  // set_parameter serialized with process() (never a cross-thread mutation of a
  // live processor). If nothing maps to an automatable parameter, or a parameter
  // is not realtime-safe, fall back to a full rebuild.
  bool type_changed = false;
  if (!apply_gs_efx_sysex(efx_, data, size, &type_changed)) return false;
  if (type_changed) return true;
  return enqueue_efx_param_updates();
}

bool Sf2Player::enqueue_efx_param_updates() {
  // CONTROL thread. Reads the last-published routing (control_current) purely to
  // discover each built stage processor's JSON-key -> param-id bridge
  // (parameter_descriptors() is const and safe to read concurrently with the
  // audio thread); it never mutates a processor here. Only GS-EFX-realised parts
  // (config insert kNone, bussed solely because a GS EFX chain was built) share
  // the single EFX unit's parameters; static config-insert parts are left alone.
  const Sf2RealizedEfx* snapshot = efx_pub_->control_current().get();
  if (snapshot == nullptr) return true;  // nothing built yet -> rebuild
  const std::vector<GsEfxStage> stages = gs_efx_insert_chain(efx_);
  if (stages.empty()) return true;  // Thru / unmapped -> no chain, rebuild
  size_t enqueued = 0;
  for (int part = 0; part < 16; ++part) {
    if (config_.part_inserts[static_cast<size_t>(part)].type != Sf2InsertType::kNone) continue;
    if (!snapshot->part_bussed[static_cast<size_t>(part)]) continue;
    const std::vector<std::unique_ptr<rt::ProcessorBase>>& chain =
        snapshot->chains[static_cast<size_t>(part)];
    // The published chain skips stages the factory could not build, so it is a
    // prefix of the stage list; align positionally over the built stages.
    const size_t count = std::min(chain.size(), stages.size());
    for (size_t s = 0; s < count; ++s) {
      const rt::ProcessorBase* proc = chain[s].get();
      if (proc == nullptr) continue;
      for (const rt::ParamDescriptor& d : proc->parameter_descriptors()) {
        float value = 0.0f;
        if (!json_find_number(stages[s].params_json, d.key, value)) continue;
        // A parameter that is not realtime-safe would allocate/rebuild in
        // set_parameter, which is illegal on the audio thread -> rebuild instead.
        if (!proc->parameter_is_realtime_safe(d.id)) return true;
        EfxParamUpdate update;
        update.part = static_cast<uint8_t>(part);
        update.stage_index = static_cast<uint8_t>(s);
        update.param_id = d.id;
        update.value = value;
        if (efx_param_queue_->push(update)) ++enqueued;
      }
    }
  }
  // Nothing matched an automatable parameter -> rebuild so the edit is not lost.
  return enqueued == 0;
}

void Sf2Player::drain_efx_param_updates() noexcept {
  // AUDIO thread, at block start after acquire(): apply every pending parameter
  // update to the current published chain. set_parameter runs here, serialized
  // with process() on this same thread — the contract it honours — so there is
  // no cross-thread race and no rebuild (the chain objects, and thus their
  // reverb/delay tails, are preserved). Tuples whose indices no longer fit the
  // current chain (a rebuild republished a different shape) are dropped: a
  // rebuild already bakes the new parameter values in, so nothing is lost.
  const Sf2RealizedEfx* snapshot = efx_pub_->current();
  EfxParamUpdate update;
  while (efx_param_queue_->pop(update)) {
    if (snapshot == nullptr || update.part >= 16) continue;
    const std::vector<std::unique_ptr<rt::ProcessorBase>>& chain = snapshot->chains[update.part];
    if (update.stage_index >= chain.size()) continue;
    rt::ProcessorBase* proc = chain[update.stage_index].get();
    if (proc == nullptr) continue;
    // Only touch parameters the processor declares realtime-safe. This read is on
    // the audio thread, serialized with set_parameter below, so it is race-free
    // here; it also guarantees we never take a non-noexcept rebuild/validate path
    // (this function is noexcept) and skips a stale tuple that, after a type-change
    // rebuild, would land on a different processor's non-realtime-safe parameter.
    if (!proc->parameter_is_realtime_safe(update.param_id)) continue;
    proc->set_parameter(update.param_id, update.value);  // scalar set; bool ignored
  }
}

void Sf2Player::on_control_sysex(const uint8_t* data, size_t size) noexcept {
  if (!prepared_ || data == nullptr || size == 0) return;
  if (apply_efx_sysex(data, size)) {
    realize_gs_efx();
  }
  // The system-effect and master-EQ blocks are control-owned in a live engine
  // for the same reason the EFX mirror is. They need no rebuild: the new state
  // goes to the audio thread as coefficients through the queue.
  const GsSysEx msg = parse_gs_sysex(data, size);
  bool changed = msg.kind == GsSysExKind::kGsReset || msg.kind == GsSysExKind::kGmReset;
  if (changed) {
    sys_fx_ = {};
    master_eq_ = {};
    eq_part_bypassed_ = {};
  }
  changed = apply_gs_system_sysex(data, size) || changed;
  if (changed) sys_queue_->push({sys_fx_, master_eq_, eq_part_bypassed_});
}

void Sf2Player::refresh_channel_mod(uint8_t channel) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  Sf2ChannelMod& mod = channel_mods_[ch];
  mod.pitch_cents = (static_cast<float>(st.pitch_bend) - 8192.0f) / 8192.0f * st.bend_range_cents;
  mod.gain = sf2_cc_gain(st.volume) * sf2_cc_gain(st.expression);
  mod.extra_vibrato_cents = kModWheelVibratoCents * static_cast<float>(st.mod_wheel) / 127.0f;
  mod.pan_units = (static_cast<float>(st.pan) - 64.0f) / 63.0f * 500.0f;
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
  return gs_effective_bank(st.bank_msb, st.bank_lsb, st.drums);
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
