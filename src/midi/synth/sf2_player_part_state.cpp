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

void Sf2Player::realize_gs_efx() {
  part_fx_.clear_dirty();
  if (!prepared_) return;
  part_fx_.publish();
}

bool Sf2Player::set_part_rig(uint8_t part, const PartRig& rig) noexcept {
  return part_fx_.set_part_rig_and_publish(part, rig, prepared_);
}

std::vector<std::string> Sf2Player::part_rig_stage_names(uint8_t part) const {
  return part_fx_.part_rig_stage_names(part);
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

bool Sf2Player::part_plays_model_floor(uint8_t channel) const noexcept {
  if (!config_.synth_fallback) return false;
  const uint8_t ch = channel & 0x0Fu;
  const uint16_t bank = effective_bank(ch);
  const uint8_t program = channels_[ch].program;
  return soundfont_ == nullptr || resolve_preset(bank, program) < 0 ||
         (config_.prefer_model_for_modeled_families &&
          gm_program_has_dedicated_model(bank, program));
}

void Sf2Player::refresh_part_rig(uint8_t channel) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  uint8_t id = 0;
  // Resolved whether or not the part takes the bank's rig: an entry may select
  // it later, and the stage decides whether this id reaches a chain. Only where
  // the note plays the model floor: a SoundFont's electric guitar was recorded
  // through an amplifier, so a second one on top of it is the bake
  // docs/voicing.md exists to remove.
  if (part_plays_model_floor(ch))
    id = gm_fallback_rig(effective_bank(ch), channels_[ch].program).id;
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
  mod.breath01 = st.cc_position[cc::kBreathController].f7() / 127.0f;
  mod.aftertouch01 = st.channel_pressure.f7() / 127.0f;
  mod.expression01 = st.cc_position[cc::kExpression].f7() / 127.0f;
  mod.pitch_bend01 =
      (st.pitch_bend.f14() - static_cast<float>(kBend14Center)) / static_cast<float>(kBend14Center);
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
  mod.vib_rate_scale = lfo_rate_cents != 0.0f ? cents_to_ratio(lfo_rate_cents) : 1.0f;
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

void Sf2Player::set_transport(const transport::TransportState& state) noexcept {
  if (path_recorder_ != nullptr) path_recorder_->set_block_frame(state.render_frame);
}

bool Sf2Player::set_render_path_recorder(RenderPathRecorder* recorder) noexcept {
  if (recorder != nullptr && !config_.realize_efx_inline) return false;
  path_recorder_ = recorder;
  part_fx_.set_path_recorder(recorder);
  return true;
}

void Sf2Player::record_render_path() {
  RenderPathRecorder& recorder = *path_recorder_;
  if (!recorder.recording()) return;
  // What render_chunk() reads: the prepared overlay's routing and units while it
  // is active, the adopted snapshot's otherwise; part chains are the snapshot's.
  const PartFxSnapshot* snapshot = part_fx_.current();
  const bool overlay = prepared_runtime_active_;
  const PartFxUnitOverrides overrides = overlay ? prepared_unit_overrides() : PartFxUnitOverrides{};
  RenderPathTopology topology;
  topology.parts.resize(16);
  for (uint8_t part = 0; part < 16; ++part) {
    RenderPathPart& out = topology.parts[part];
    out.part = part;
    out.program = channels_[part].program;
    out.bank = effective_bank(part);
    if (part_plays_model_floor(part)) {
      out.backend = "model";
    } else if (soundfont_ != nullptr &&
               resolve_preset(effective_bank(part), channels_[part].program) >= 0) {
      out.backend = "sf2";
    }
    out.rig_source = part_fx_.rig_source_name(part);
    if (snapshot != nullptr) out.stages = snapshot->stage_names[part];
    out.skipped = recorder.refused_stages(part);
    const uint8_t unit = overlay               ? prepared_part_unit_[part]
                         : snapshot != nullptr ? snapshot->part_unit[part]
                                               : PartFxSnapshot::kNoUnit;
    out.unit = unit == PartFxSnapshot::kNoUnit ? -1 : static_cast<int>(unit);
    out.mono_prefix = overlay               ? prepared_mono_prefix_[part]
                      : snapshot != nullptr ? snapshot->mono_prefix[part]
                                            : 0;
#if defined(SONARE_MIDI_WITH_FX)
    if (effects_ != nullptr) out.send_tap = out.unit >= 0 ? "post_unit" : "pre_rig";
#endif
  }
  for (size_t u = 0; u < kGsEfxUnitCount; ++u) {
    const bool fed = overlay ? prepared_unit_fed_[u] : snapshot != nullptr && snapshot->unit_fed[u];
    if (!fed) continue;
    const Sf2EfxUnitRt* unit_rt = overrides[u];
    if (unit_rt == nullptr && snapshot != nullptr) unit_rt = &snapshot->unit(u);
    if (unit_rt == nullptr) continue;
    RenderPathUnit out;
    out.unit = static_cast<uint8_t>(u);
    out.type = overlay ? prepared_efx_[u].type : snapshot->gs_efx_state[u].type;
    out.realization = unit_rt->realization;
    for (const Sf2EfxStageRt& stage : unit_rt->stages) {
      out.stages.push_back(stage.name);
      out.enabled.push_back(stage.enabled_target);
      if (stage.proc == nullptr) out.skipped.push_back(stage.name);
    }
    topology.units.push_back(std::move(out));
  }
  recorder.record_topology(std::move(topology));
}

}  // namespace sonare::midi::synth
