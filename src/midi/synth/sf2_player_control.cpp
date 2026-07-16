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
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"

namespace sonare::midi::synth {

namespace {

constexpr uint16_t kDrumBank = 128;
constexpr uint8_t kGm2MelodicBankMsb = 0x79;
constexpr uint8_t kGm2PercussionBankMsb = 0x78;
constexpr float kModWheelVibratoCents = 50.0f;
constexpr float kCcSendDepth = 0.35f;
}  // namespace

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
  // GS insertion-effect (EFX) block writes (address 40 03 xx). Offline captures
  // the raw wire into the mirror so process() can realise it inline; live routes
  // realisation through the control thread (on_control_sysex), so the audio
  // thread must not touch the mirror the builder reads.
  if (config_.realize_efx_inline && apply_gs_efx_sysex(efx_, data, size)) {
    gs_efx_dirty_ = true;
    return true;
  }
  return false;
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
  if (st.drums || st.bank_msb == kGm2PercussionBankMsb) return kDrumBank;
  if (st.bank_msb == kGm2MelodicBankMsb) return st.bank_lsb;
  return st.bank_msb;
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
