#include "midi/synth/sf2_player.h"

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
#include "midi/ump.h"
#include "util/constants.h"

namespace sonare::midi::synth {

namespace {

constexpr uint8_t kDrumChannel = 9;  // MIDI channel 10

/// Shared-bus residual attribution memory: how quickly a source's learned
/// share of the bus-wide residual (part insert/body/reverb tail after its dry
/// voice stops) decays toward the other live sources.
constexpr float kResidualTauSeconds = 0.5f;

}  // namespace

Sf2Player::Sf2Player(const Sf2PlayerConfig& config) : config_(config) {
  direct_system_patch_reader_ =
      std::make_unique<DirectSystemPatchCell::Reader>(direct_system_patch_->reader());
  per_note_bend_sensitivity_.fill(kDefaultPerNoteBendSensitivity);
  // An explicit 0 is silence; only a negative or non-finite gain is not a level.
  if (config_.gain < 0.0f || !std::isfinite(config_.gain)) config_.gain = 0.5f;
  config_.gain = std::min(config_.gain, 4.0f);
  config_.polyphony = config_.polyphony > 0 ? std::min(config_.polyphony, kMaxSynthVoices) : 48;
  for (int part = 0; part < 16; ++part) {
    Sf2PartInsert& insert = config_.part_inserts[static_cast<size_t>(part)];
    insert.amount = std::clamp(insert.amount, 0.0f, 1.0f);
    any_insert_ = any_insert_ || insert.type != Sf2InsertType::kNone;
  }
  // A player with an insert factory is EFX-capable: allocate the per-part bus so
  // a GS EFX switch can install an insert on any part at run time. Parts are not
  // bussed until an EFX is actually realised (see part_bussed_), so a dry bounce
  // stays bit-identical to one built without a factory.
  if (config_.insert_factory) any_insert_ = true;
#if defined(SONARE_MIDI_WITH_FX)
  effects_ = std::make_unique<GsEffectBus>(config_.effects);
#endif
}

Sf2Player::~Sf2Player() = default;

void Sf2Player::clear_control_owned_gs_state() {
  // prepare() and reset() are quiescent boundaries; no stale delta may cross one.
  efx_ = {};
  efx_part_assign_ = {};
  direct_legacy_efx_fallback_.fill(false);
  gs_efx_dirty_ = false;
  sys_fx_ = {};
  master_eq_ = {};
  eq_part_bypassed_ = {};
  gs_system_dirty_ = false;
  efx_param_queue_ = std::make_unique<EfxParamQueue>();
  clear_direct_gs_queue();
  direct_system_patch_control_ = {};
  direct_system_audio_fx_ = {};
  direct_system_audio_eq_ = {};
  direct_system_audio_bypassed_ = {};
  last_system_direct_seq_ = 0;
  direct_system_patch_->store(direct_system_patch_control_);
  clear_prepared_audio_state();
  seed_prepared_from_control_state();
  prepared_audio_domain_ = prepared_domain_;
  release_prepared_nodes();
}

void Sf2Player::clear_prepared_audio_state() noexcept {
  for (PreparedEfxNode*& node : prepared_active_nodes_) {
    if (node != nullptr) {
      node->audio_pins.fetch_sub(1, std::memory_order_acq_rel);
      node = nullptr;
    }
  }
  prepared_unit_overridden_.fill(false);
  for (GsEfx& efx : prepared_efx_) efx = {};
  prepared_assign_.fill(0);
  prepared_sys_fx_ = {};
  prepared_master_eq_ = {};
  prepared_eq_part_bypassed_ = {};
  prepared_part_unit_.fill(Sf2RealizedEfx::kNoUnit);
  prepared_part_bussed_.fill(false);
  prepared_default_bank_rig_.fill(false);
  prepared_default_bank_rig_mono_prefix_.fill(0);
  prepared_unit_fed_.fill(false);
  prepared_host_part_bussed_.fill(false);
  prepared_host_default_bank_rig_.fill(false);
  prepared_host_default_bank_rig_mono_prefix_.fill(0);
  prepared_host_any_bussed_ = false;
  prepared_any_unit_ = false;
  prepared_any_bussed_ = false;
  prepared_runtime_active_ = false;
  prepared_base_synced_ = false;
  prepared_empty_unit_ = {};
}

void Sf2Player::seed_prepared_from_control_state() noexcept {
  prepared_efx_ = efx_;
  prepared_assign_ = efx_part_assign_;
  prepared_sys_fx_ = sys_fx_;
  prepared_master_eq_ = master_eq_;
  prepared_eq_part_bypassed_ = eq_part_bypassed_;
}

void Sf2Player::release_prepared_nodes() noexcept {
  // CONTROL only: erase nodes neither a token lease nor an audio pin holds.
  for (auto it = prepared_nodes_.begin(); it != prepared_nodes_.end();) {
    PreparedEfxNode* node = it->get();
    if (it->use_count() == 1 && node->audio_pins.load(std::memory_order_acquire) == 0) {
      it = prepared_nodes_.erase(it);
    } else {
      ++it;
    }
  }
}

void Sf2Player::rebuild_prepared_routing() noexcept {
  prepared_part_unit_.fill(Sf2RealizedEfx::kNoUnit);
  prepared_part_bussed_ = prepared_host_part_bussed_;
  prepared_default_bank_rig_ = prepared_host_default_bank_rig_;
  prepared_default_bank_rig_mono_prefix_ = prepared_host_default_bank_rig_mono_prefix_;
  prepared_unit_fed_.fill(false);
  prepared_any_unit_ = false;
  prepared_any_bussed_ = prepared_host_any_bussed_;
  if (!config_.insert_factory) return;
  for (size_t part = 0; part < prepared_assign_.size(); ++part) {
    const int unit = gs_efx_assign_unit(prepared_assign_[part]);
    if (unit < 0 || !prepared_efx_[static_cast<size_t>(unit)].assigned) continue;
    prepared_part_unit_[part] = static_cast<uint8_t>(unit);
    prepared_part_bussed_[part] = true;
    // A routed part feeds its unit the DI; render_chunk() skips the rig chain.
    prepared_default_bank_rig_[part] = false;
    prepared_default_bank_rig_mono_prefix_[part] = 0;
    prepared_unit_fed_[static_cast<size_t>(unit)] = true;
    prepared_any_unit_ = true;
    prepared_any_bussed_ = true;
  }
}

void Sf2Player::sync_prepared_base() noexcept {
  if (!prepared_runtime_active_) return;
  // Recomputed per boundary: a program change can publish a new default rig.
  prepared_host_part_bussed_.fill(false);
  prepared_host_default_bank_rig_.fill(false);
  prepared_host_default_bank_rig_mono_prefix_.fill(0);
  prepared_host_any_bussed_ = false;
  const Sf2RealizedEfx* snapshot = efx_pub_->current();
  for (size_t part = 0; part < prepared_host_part_bussed_.size(); ++part) {
    // The snapshot's tags, never gm_rig_chain(), which allocates.
    const bool host_bussed = snapshot != nullptr && snapshot->host_part_bussed[part];
    const bool bank_rig = snapshot != nullptr && snapshot->default_bank_rig[part];
    prepared_host_default_bank_rig_[part] = bank_rig;
    if (bank_rig) {
      prepared_host_default_bank_rig_mono_prefix_[part] =
          snapshot->default_bank_rig_mono_prefix[part];
    }
    prepared_host_part_bussed_[part] = host_bussed;
    prepared_host_any_bussed_ = prepared_host_any_bussed_ || prepared_host_part_bussed_[part];
  }
  rebuild_prepared_routing();
  prepared_base_synced_ = true;
}

void Sf2Player::set_soundfont(std::shared_ptr<const Sf2File> soundfont) {
  soundfont_ = std::move(soundfont);
  // Scan for the longest volume-envelope release so tail_samples() covers the
  // slowest patch (instrument-level absolute + preset-level relative).
  max_release_timecents_ = -12000;
  if (soundfont_ != nullptr) {
    for (const Sf2Instrument& inst : soundfont_->instruments()) {
      for (const Sf2Zone& zone : inst.zones) {
        if (const Sf2Gen* g = zone.find_gen(kGenReleaseVolEnv)) {
          max_release_timecents_ =
              std::max(max_release_timecents_, static_cast<int32_t>(g->amount));
        }
      }
    }
    for (const Sf2Preset& preset : soundfont_->presets()) {
      for (const Sf2Zone& zone : preset.zones) {
        if (const Sf2Gen* g = zone.find_gen(kGenReleaseVolEnv)) {
          // Preset release gens are relative; bound with the worst case sum.
          max_release_timecents_ =
              std::max(max_release_timecents_, -12000 + static_cast<int32_t>(g->amount));
        }
      }
    }
  }
  // Which parts play the model floor just changed, and the bank rig follows it:
  // a SoundFont that covers program 30 takes that part off its amplifier, and
  // one that is unloaded puts it back on.
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_part_rig(ch);
  if (prepared_) {
    // Tokens prepared for the previous instrument fail their domain check.
    ++prepared_domain_;
    clear_prepared_audio_state();
    seed_prepared_from_control_state();
    prepared_audio_domain_ = prepared_domain_;
    recompute_tail();
    realize_gs_efx();
  }
}

void Sf2Player::recompute_tail() noexcept {
  const float release_ms = std::max(5.0f, 1000.0f * timecents_to_seconds(max_release_timecents_));
  tail_samples_ = DahdsrEnvelope::release_tail_samples(sample_rate_, release_ms);
  if (config_.synth_fallback) {
    tail_samples_ = std::max(tail_samples_, DahdsrEnvelope::release_tail_samples(
                                                sample_rate_, gm_fallback_max_release_ms()));
    // The shared body resonators (piano soundboard / sympathetic banks) ring
    // past the last voice; bound their tail like the NativeSynth host does.
    tail_samples_ += static_cast<int64_t>(kPianoBodyRingS * sample_rate_);
  }
#if defined(SONARE_MIDI_WITH_FX)
  // The note tail rings first, the effect tail decays after it.
  if (effects_ != nullptr) tail_samples_ += effects_->tail_samples(sample_rate_);
#endif
}

void Sf2Player::prepare(double sample_rate, int /*max_block_size*/) {
  ++prepared_domain_;
  release_prepared_nodes();
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  residual_splitter_.configure(sample_rate_, kResidualTauSeconds);
  residual_splitter_.reset();
  for (SourceResidualSplitter& s : part_bus_splitters_) {
    s.configure(sample_rate_, kResidualTauSeconds);
    s.reset();
  }
  for (SourceResidualSplitter& s : unit_splitters_) {
    s.configure(sample_rate_, kResidualTauSeconds);
    s.reset();
  }
  send_residual_splitter_.configure(sample_rate_, kResidualTauSeconds);
  send_residual_splitter_.reset();
  for (SourceResidualSplitter& s : body_residual_splitters_) {
    s.configure(sample_rate_, kResidualTauSeconds);
    s.reset();
  }
  pool_.prepare(config_.polyphony);
  fallback_pool_.prepare(config_.synth_fallback ? config_.polyphony : 1);
  fallback_per_note_.assign(fallback_pool_.size(), Sf2PerNoteVoice{});
  // Plucked GM fallback programs are Karplus-Strong voices: give every
  // fallback slot its delay span here (the only allocation site; voices
  // attach their span at note-on).
  fallback_ks_capacity_ = ks_buffer_capacity(sample_rate_);
  fallback_ks_buffers_.assign(
      config_.synth_fallback
          ? fallback_pool_.size() * static_cast<size_t>(ks_slab_capacity(sample_rate_))
          : 0,
      0.0f);
  fallback_piano_string_capacity_ = piano_string_capacity(sample_rate_);
  fallback_piano_buffers_.assign(
      config_.synth_fallback
          ? fallback_pool_.size() * static_cast<size_t>(piano_slab_capacity(sample_rate_))
          : 0,
      0.0f);
  // The Church Organ GM program is a flue-pipe waveguide voice; give every
  // fallback slot its full registration slab (kMaxPipeRanks pipe spans) here.
  fallback_pipe_organ_capacity_ = pipe_organ_buffer_capacity(sample_rate_);
  fallback_pipe_organ_buffers_.assign(
      config_.synth_fallback
          ? fallback_pool_.size() * static_cast<size_t>(pipe_organ_slab_capacity(sample_rate_))
          : 0,
      0.0f);
  // The acoustic waveguide GM families (bowed string / reed / brass / air-jet
  // flute) each get their per-slot delay slab here, sized by the engine's
  // *_slab_capacity(); voices attach their span at note-on.
  fallback_bowed_capacity_ = bowed_string_buffer_capacity(sample_rate_);
  fallback_bowed_buffers_.assign(
      config_.synth_fallback
          ? fallback_pool_.size() * static_cast<size_t>(bowed_string_slab_capacity(sample_rate_))
          : 0,
      0.0f);
  fallback_reed_capacity_ = reed_buffer_capacity(sample_rate_);
  fallback_reed_buffers_.assign(
      config_.synth_fallback
          ? fallback_pool_.size() * static_cast<size_t>(reed_slab_capacity(sample_rate_))
          : 0,
      0.0f);
  fallback_brass_capacity_ = brass_buffer_capacity(sample_rate_);
  fallback_brass_buffers_.assign(
      config_.synth_fallback
          ? fallback_pool_.size() * static_cast<size_t>(brass_slab_capacity(sample_rate_))
          : 0,
      0.0f);
  fallback_flute_capacity_ = flute_buffer_capacity(sample_rate_);
  fallback_flute_buffers_.assign(
      config_.synth_fallback
          ? fallback_pool_.size() * static_cast<size_t>(flute_slab_capacity(sample_rate_))
          : 0,
      0.0f);
  fallback_plucked_string_capacity_ = plucked_string_buffer_capacity(sample_rate_);
  fallback_plucked_string_buffers_.assign(
      config_.synth_fallback
          ? fallback_pool_.size() * static_cast<size_t>(plucked_string_slab_capacity(sample_rate_))
          : 0,
      0.0f);
  // A harpsichord fallback voice needs its whole registration reserved, not one
  // string: a stop drawn at note-on cannot allocate.
  fallback_harpsichord_capacity_ = harpsichord_buffer_capacity(sample_rate_);
  fallback_harpsichord_stride_ = harpsichord_slab_capacity(sample_rate_);
  fallback_harpsichord_buffers_.assign(
      config_.synth_fallback
          ? fallback_pool_.size() * static_cast<size_t>(fallback_harpsichord_stride_)
          : 0,
      0.0f);
  clear_control_owned_gs_state();
  // Power-on matches GS defaults (reverb send 40): a bare SMF that never
  // sends a reset SysEx should still land in the default room, as on
  // hardware, instead of rendering bone dry.
  reset_all_state(/*reverb_send_default=*/40, /*chorus_send_default=*/0);
  mix_l_.assign(kChunkFrames, 0.0f);
  mix_r_.assign(kChunkFrames, 0.0f);
  // Mix-bus polish: the same ~8 Hz DC blocker pole the NativeSynth host uses.
  dc_r_ = 1.0f - static_cast<float>(constants::kTwoPiD * 8.0 / sample_rate_);
  dc_x1_ = {};
  dc_y1_ = {};
  part_bus_.assign(any_insert_ ? 16 * 2 * static_cast<size_t>(kChunkFrames) : 0, 0.0f);
  body_residual_.assign(16 * kFallbackBodyKinds * 2 * static_cast<size_t>(kChunkFrames), 0.0f);
  // One bus per insertion unit, so parts sharing a unit sum into it and it runs
  // once (docs/gs.md). Allocated with the part bus and on the same condition: a
  // unit is realisable exactly where an insert factory is.
  unit_bus_.assign(any_insert_ ? kGsEfxUnitCount * 2 * static_cast<size_t>(kChunkFrames) : 0, 0.0f);
  eq_bypass_bus_.assign(2 * static_cast<size_t>(kChunkFrames), 0.0f);
#if defined(SONARE_MIDI_WITH_FX)
  if (effects_ != nullptr) effects_->prepare(sample_rate_);
#endif
  // The effect bus and the master EQ start on the GS power-on defaults, so a
  // file that sends GS Reset and nothing else gets the state it is entitled to
  // (docs/gs.md, "Reset defaults are part of the contract").
  eq_.prepare(sample_rate_);
  apply_gs_system_state(sys_fx_, master_eq_, eq_part_bypassed_);
  recompute_tail();
  prepared_ = true;
  // Publish the initial realised-EFX snapshot (the config static inserts, the
  // bank rigs the parts' current programs bind; no GS EFX assigned yet), so the
  // audio thread routes bussed parts from the first block. build_realized_efx()
  // builds the kProcessor inserts via the factory.
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_part_rig(ch);
  publish_realized_efx();
  gs_efx_dirty_ = false;
}

void Sf2Player::reset() {
  pool_.reset();
  fallback_pool_.reset();
  fallback_per_note_.assign(fallback_per_note_.size(), Sf2PerNoteVoice{});
  per_note_pitch_.clear();
  per_note_bend_sensitivity_.fill(kDefaultPerNoteBendSensitivity);
  skipped_events_ = 0;
  // The mix-bus DC blocker holds an IIR tail from whatever was sounding; a
  // reset means the next block starts from silence, so it goes with the voices.
  dc_x1_ = {};
  dc_y1_ = {};
  residual_splitter_.reset();
  for (SourceResidualSplitter& s : part_bus_splitters_) s.reset();
  for (SourceResidualSplitter& s : unit_splitters_) s.reset();
  send_residual_splitter_.reset();
  for (SourceResidualSplitter& s : body_residual_splitters_) s.reset();
  clear_control_owned_gs_state();
  reset_all_state(/*reverb_send_default=*/40, /*chorus_send_default=*/0);
  // Republish a fresh realised-EFX snapshot: rebuilding the inserts gives them
  // clean DSP state (the discontinuity's equivalent of resetting them), and the
  // old snapshot is retired/freed by the control thread, never the audio thread.
  if (prepared_) {
    publish_realized_efx();
    gs_efx_dirty_ = false;
  }
#if defined(SONARE_MIDI_WITH_FX)
  if (effects_ != nullptr) effects_->reset();
#endif
  eq_.reset();
}

void Sf2Player::reset_all_state(uint8_t reverb_send_default, uint8_t chorus_send_default) noexcept {
  channels_ = {};
  drum_params_ = {};
  user_drum_sources_ = {};
  user_drum_params_ = {};
  // Every GsMasterParams field default-constructs to its GS power-on value, so
  // the reset is the default-construct.
  master_ = {};
  // GS/GM reset selects EFX "Thru" and clears the part EFX switches. The EFX
  // mirror is owned by whichever thread realises it: offline (inline) clears it
  // here on the render thread; live leaves it to the control thread's
  // on_control_sysex, so the audio thread never writes the mirror the builder
  // reads. Live system state is similarly published by on_control_sysex.
  if (config_.realize_efx_inline) {
    efx_ = {};
    efx_part_assign_ = {};
    gs_efx_dirty_ = true;
    // The system-effect and master-EQ mirror splits the same way, and every one
    // of its fields defaults to its GS power-on value.
    sys_fx_ = {};
    master_eq_ = {};
    eq_part_bypassed_ = {};
    gs_system_dirty_ = true;
  }
  for (uint8_t ch = 0; ch < 16; ++ch) {
    channels_[ch].drum_map = ch == kDrumChannel ? kGsDrumMap1 : kGsDrumMapNone;
    channels_[ch].reverb_send = reverb_send_default;
    channels_[ch].chorus_send = chorus_send_default;
    // Every part powers on listening to its own channel, which is the slot it
    // is stored in; one default cannot say sixteen different things.
    channels_[ch].rx_channel = ch;
    refresh_channel_mod(ch);
    refresh_part_rig(ch);
  }
  refresh_rx_channels();
  for (int part = 0; part < 16; ++part) {
    fallback_wind_[static_cast<size_t>(part)].reset();
    fallback_wind_params_[static_cast<size_t>(part)] = {};
    fallback_board_[static_cast<size_t>(part)].reset();
    fallback_reso_[static_cast<size_t>(part)].reset();
    fallback_halo_[static_cast<size_t>(part)].reset();
    fallback_body_[static_cast<size_t>(part)] = {};
  }
}

void Sf2Player::gs_reset() noexcept {
  for (uint8_t ch = 0; ch < 16; ++ch) all_sound_off(ch);
  // GS power-on: reverb send 40 (Roland default), everything else cleared.
  reset_all_state(/*reverb_send_default=*/40, /*chorus_send_default=*/0);
}

void Sf2Player::gm_reset(GmLevel level) noexcept {
  for (uint8_t ch = 0; ch < 16; ++ch) all_sound_off(ch);
  // GM Level 1 specifies no effect controls, but real GM devices (SC-55 in
  // GM mode) keep their power-on reverb level; match that rather than the
  // paper reading so plain GM files keep the default room.
  reset_all_state(/*reverb_send_default=*/40, /*chorus_send_default=*/0);
  // The receive switches a System On leaves differently from a GS Reset. NRPN
  // goes off at either level, since the NRPNs it would carry are Roland's rather
  // than either GM specification's; the two bank-select switches go off for GM1
  // alone, which is what keeps a GM1 file on the plain program it names and
  // still lets a GM2 file reach a variation (docs/gs.md, 40 1x 0A / 23 / 24).
  uint32_t off = gs_rx_switch_bit(GsRxSwitch::kNrpn);
  if (level == GmLevel::kGeneralMidi1) {
    off |= gs_rx_switch_bit(GsRxSwitch::kBankSelect);
    off |= gs_rx_switch_bit(GsRxSwitch::kBankSelectLsb);
  }
  for (ChannelState& part : channels_) part.rx_switches &= ~off;
}

}  // namespace sonare::midi::synth
