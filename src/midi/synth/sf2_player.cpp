#include "midi/synth/sf2_player.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "midi/builtin_synth.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/ump.h"

namespace sonare::midi::synth {

namespace {

constexpr uint8_t kDrumChannel = 9;  // MIDI channel 10
constexpr uint16_t kDrumBank = 128;
constexpr uint8_t kGm2MelodicBankMsb = 0x79;
constexpr uint8_t kGm2PercussionBankMsb = 0x78;

/// Default modulator: full CC1 adds 50 cents of vibrato LFO pitch depth.
constexpr float kModWheelVibratoCents = 50.0f;
/// CC91/CC93 send depth at full controller. The SF2 default modulator says
/// 200/1000, but that leaves the GS power-on default (CC91 = 40) inaudible;
/// Roland hardware maps the same controller to a clearly audible room, so
/// the depth follows the musical calibration rather than the paper spec.
constexpr float kCcSendDepth = 0.35f;

}  // namespace

Sf2Player::Sf2Player(const Sf2PlayerConfig& config) : config_(config) {
  if (!(config_.gain > 0.0f) || !std::isfinite(config_.gain)) config_.gain = 0.5f;
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
  if (prepared_) recompute_tail();
}

void Sf2Player::recompute_tail() noexcept {
  const float release_ms = std::max(5.0f, 1000.0f * timecents_to_seconds(max_release_timecents_));
  tail_samples_ = DahdsrEnvelope::release_tail_samples(sample_rate_, release_ms);
  if (config_.synth_fallback) {
    tail_samples_ = std::max(tail_samples_, DahdsrEnvelope::release_tail_samples(
                                                sample_rate_, gm_fallback_max_release_ms()));
    // The shared body resonators (piano soundboard / sympathetic banks) ring
    // past the last voice; bound their tail like the NativeSynth host does.
    tail_samples_ += static_cast<int64_t>(2.0 * sample_rate_);
  }
#if defined(SONARE_MIDI_WITH_FX)
  // The note tail rings first, the effect tail decays after it.
  if (effects_ != nullptr) tail_samples_ += effects_->tail_samples(sample_rate_);
#endif
}

void Sf2Player::prepare(double sample_rate, int /*max_block_size*/) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  pool_.prepare(config_.polyphony);
  fallback_pool_.prepare(config_.synth_fallback ? config_.polyphony : 1);
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
  // Power-on matches GS defaults (reverb send 40): a bare SMF that never
  // sends a reset SysEx should still land in the default room, as on
  // hardware, instead of rendering bone dry.
  reset_all_state(/*reverb_send_default=*/40, /*chorus_send_default=*/8);
  mix_l_.assign(kChunkFrames, 0.0f);
  mix_r_.assign(kChunkFrames, 0.0f);
  part_bus_.assign(any_insert_ ? 16 * 2 * static_cast<size_t>(kChunkFrames) : 0, 0.0f);
#if defined(SONARE_MIDI_WITH_FX)
  if (effects_ != nullptr) effects_->prepare(sample_rate_);
#endif
  recompute_tail();
  prepared_ = true;
  // Publish the initial realised-EFX snapshot (the config static inserts; no GS
  // EFX assigned yet), so the audio thread routes bussed parts from the first
  // block. build_realized_efx() builds the kProcessor inserts via the factory.
  efx_pub_->publish(build_realized_efx());
  gs_efx_dirty_ = false;
}

void Sf2Player::reset() {
  pool_.reset();
  fallback_pool_.reset();
  reset_all_state(/*reverb_send_default=*/40, /*chorus_send_default=*/8);
  // Republish a fresh realised-EFX snapshot: rebuilding the inserts gives them
  // clean DSP state (the discontinuity's equivalent of resetting them), and the
  // old snapshot is retired/freed by the control thread, never the audio thread.
  if (prepared_) {
    efx_pub_->publish(build_realized_efx());
    gs_efx_dirty_ = false;
  }
#if defined(SONARE_MIDI_WITH_FX)
  if (effects_ != nullptr) effects_->reset();
#endif
}

void Sf2Player::reset_all_state(uint8_t reverb_send_default, uint8_t chorus_send_default) noexcept {
  channels_ = {};
  drum_params_ = {};
  // GS/GM reset selects EFX "Thru" and clears the part EFX switches. The EFX
  // mirror is owned by whichever thread realises it: offline (inline) clears it
  // here on the render thread; live leaves it to the control thread's
  // on_control_sysex (which parses the same reset and republishes empty), so the
  // audio thread never writes the mirror the builder reads.
  if (config_.realize_efx_inline) {
    efx_ = {};
    efx_part_enabled_ = {};
    gs_efx_dirty_ = true;
  }
  for (uint8_t ch = 0; ch < 16; ++ch) {
    channels_[ch].drums = ch == kDrumChannel;
    channels_[ch].reverb_send = reverb_send_default;
    channels_[ch].chorus_send = chorus_send_default;
    refresh_channel_mod(ch);
  }
  for (int part = 0; part < 16; ++part) {
    fallback_wind_[static_cast<size_t>(part)].reset();
    fallback_wind_params_[static_cast<size_t>(part)] = {};
    fallback_board_[static_cast<size_t>(part)].reset();
    fallback_reso_[static_cast<size_t>(part)].reset();
    fallback_body_[static_cast<size_t>(part)] = {};
  }
}

void Sf2Player::gs_reset() noexcept {
  for (uint8_t ch = 0; ch < 16; ++ch) all_sound_off(ch);
  // GS power-on: reverb send 40 (Roland default), everything else cleared.
  reset_all_state(/*reverb_send_default=*/40, /*chorus_send_default=*/8);
}

void Sf2Player::gm_reset() noexcept {
  for (uint8_t ch = 0; ch < 16; ++ch) all_sound_off(ch);
  // GM Level 1 specifies no effect controls, but real GM devices (SC-55 in
  // GM mode) keep their power-on reverb level; match that rather than the
  // paper reading so plain GM files keep the default room.
  reset_all_state(/*reverb_send_default=*/40, /*chorus_send_default=*/8);
}

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
  const GsSysEx msg = parse_gs_sysex(data, size);
  switch (msg.kind) {
    case GsSysExKind::kGmReset:
    case GsSysExKind::kGsReset:
      // A GS/GM reset clears the EFX unit and the part switches (Thru).
      efx_ = {};
      efx_part_enabled_ = {};
      return true;
    case GsSysExKind::kEfxPartSwitch:
      efx_part_enabled_[msg.channel & 0x0Fu] = msg.value != 0;
      return true;
    case GsSysExKind::kUseForRhythm:
    case GsSysExKind::kNone:
      break;
  }
  return apply_gs_efx_sysex(efx_, data, size);
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

void Sf2Player::note_on(uint8_t channel, uint8_t note, uint8_t velocity) noexcept {
  if (!prepared_) return;
  const ChannelState& ch = channels_[channel & 0x0Fu];
  // No SoundFont / uncovered program -> the data-free synth floor.
  const int preset_idx =
      soundfont_ != nullptr ? resolve_preset(effective_bank(channel), ch.program) : -1;
  if (preset_idx < 0) {
    if (config_.synth_fallback) fallback_note_on(channel, note, velocity);
    return;
  }
  const Sf2Preset& preset = soundfont_->presets()[static_cast<size_t>(preset_idx)];
  const auto& instruments = soundfont_->instruments();
  const float* pool_data = soundfont_->sample_pool().data();
  const float vel_gain = sf2_velocity_gain(velocity);

  const Sf2Zone* preset_global =
      !preset.zones.empty() && preset.zones[0].is_global() ? &preset.zones[0] : nullptr;

  bool has_renderable_zone = false;
  for (const Sf2Zone& pzone : preset.zones) {
    if (pzone.is_global() || !pzone.matches(note, velocity)) continue;
    if (pzone.instrument < 0 || static_cast<size_t>(pzone.instrument) >= instruments.size()) {
      continue;
    }
    const Sf2Instrument& inst = instruments[static_cast<size_t>(pzone.instrument)];
    const Sf2Zone* inst_global =
        !inst.zones.empty() && inst.zones[0].is_global() ? &inst.zones[0] : nullptr;
    for (const Sf2Zone& izone : inst.zones) {
      if (izone.is_global() || !izone.matches(note, velocity)) continue;
      if (izone.sample < 0 || static_cast<size_t>(izone.sample) >= soundfont_->samples().size()) {
        continue;
      }
      const Sf2Sample& sample = soundfont_->samples()[static_cast<size_t>(izone.sample)];
      if (sample.is_rom() || sample.end <= sample.start) continue;
      has_renderable_zone = true;

      // Stack generators: defaults -> instrument global -> instrument zone
      // (absolute), then + preset global + preset zone (relative).
      Sf2GenSet gens;
      if (inst_global != nullptr) gens.apply_absolute(*inst_global);
      gens.apply_absolute(izone);
      if (preset_global != nullptr) gens.add_relative(*preset_global);
      gens.add_relative(pzone);

      Sf2VoiceParams params = resolve_voice_params(gens, sample, note, velocity, sample_rate_);

      // GS layer: NRPN part edits + per-note drum-kit overrides.
      apply_gs_part_params(params, ch.gs);
      if (ch.drums) {
        apply_gs_drum_params(params, drum_params_[channel & 0x0Fu][note & 0x7Fu]);
      }

      // Exclusive class: choke same-class voices on this channel (hi-hats).
      if (params.exclusive_class != 0) {
        for (Sf2Voice& v : pool_) {
          if (v.active && v.channel == (channel & 0x0Fu) &&
              v.params.exclusive_class == params.exclusive_class) {
            v.release();
          }
        }
      }

      Sf2Voice* voice = pool_.allocate(channel & 0x0Fu, note);
      if (voice == nullptr) continue;
      voice->start(pool_data, params, sample_rate_, vel_gain);
    }
  }
  if (!has_renderable_zone && config_.synth_fallback) fallback_note_on(channel, note, velocity);
}

void Sf2Player::fallback_note_on(uint8_t channel, uint8_t note, uint8_t velocity) noexcept {
  const ChannelState& ch = channels_[channel & 0x0Fu];
  const NativeSynthPatch& patch = ch.drums ? gm_fallback_drum_patch(note)
                                           : gm_fallback_patch(effective_bank(channel), ch.program);
  // GM kit exclusive/mute groups (hi-hats etc.): choke the ringing group voice
  // on this channel before allocating the new strike.
  if (ch.drums && patch.percussion.exclusive_class != 0) {
    const uint8_t excl = patch.percussion.exclusive_class;
    for (NativeSynthVoice& v : fallback_pool_) {
      if (v.active && v.channel == (channel & 0x0Fu) && v.patch != nullptr &&
          v.patch->mode == SynthEngineMode::kPercussion &&
          v.patch->percussion.exclusive_class == excl) {
        v.choke();
      }
    }
  }
  NativeSynthVoice* voice = fallback_pool_.allocate(channel & 0x0Fu, note);
  if (voice == nullptr) return;
  const uint32_t voice_index = static_cast<uint32_t>(voice - fallback_pool_.data());
  // KS patches get their delay span before start() (pointer wiring only).
  if (!fallback_ks_buffers_.empty()) {
    voice->ks.attach(
        fallback_ks_buffers_.data() + static_cast<size_t>(voice_index) * 3 * fallback_ks_capacity_,
        fallback_ks_capacity_);
  }
  if (!fallback_piano_buffers_.empty()) {
    voice->piano.attach(fallback_piano_buffers_.data() + static_cast<size_t>(voice_index) *
                                                             kMaxPianoStrings *
                                                             fallback_piano_string_capacity_,
                        fallback_piano_string_capacity_);
  }
  if (!fallback_pipe_organ_buffers_.empty()) {
    // Each rank holds TWO spans (bore + jet), so the per-voice stride is
    // 2 * kMaxPipeRanks spans; without the 2, adjacent voices' slabs overlap
    // and simultaneous (legato) organ voices corrupt each other's bores.
    voice->pipe_organ.attach(
        fallback_pipe_organ_buffers_.data() +
            static_cast<size_t>(voice_index) * 2 * kMaxPipeRanks * fallback_pipe_organ_capacity_,
        fallback_pipe_organ_capacity_);
  }
  if (!fallback_bowed_buffers_.empty()) {
    voice->bowed_string.attach(fallback_bowed_buffers_.data() +
                                   static_cast<size_t>(voice_index) * 3 * fallback_bowed_capacity_,
                               fallback_bowed_capacity_);
  }
  if (!fallback_reed_buffers_.empty()) {
    voice->reed.attach(
        fallback_reed_buffers_.data() + static_cast<size_t>(voice_index) * fallback_reed_capacity_,
        fallback_reed_capacity_);
  }
  if (!fallback_brass_buffers_.empty()) {
    voice->brass.attach(fallback_brass_buffers_.data() +
                            static_cast<size_t>(voice_index) * fallback_brass_capacity_,
                        fallback_brass_capacity_);
  }
  if (!fallback_flute_buffers_.empty()) {
    voice->flute.attach(fallback_flute_buffers_.data() +
                            static_cast<size_t>(voice_index) * 2 * fallback_flute_capacity_,
                        fallback_flute_capacity_);
  }
  if (!fallback_plucked_string_buffers_.empty()) {
    voice->plucked_string.attach(
        fallback_plucked_string_buffers_.data() +
            static_cast<size_t>(voice_index) * fallback_plucked_string_capacity_,
        fallback_plucked_string_capacity_);
  }
  // GS drum-kit variation: the drum channel's program picks the kit (Room /
  // Power / TR-808 / ...); melodic fallback voices pass 0 (no kit).
  const uint8_t drum_kit = ch.drums ? gm_fallback_drum_kit(ch.program) : 0;
  // GS per-note drum NRPN edits (pitch coarse / TVA level / absolute pan),
  // mirroring apply_gs_drum_params for the model floor (reverb/chorus sends stay
  // on the SF2 path).
  DrumVoiceMod drum_mod;
  if (ch.drums) {
    const GsDrumNoteParams& gd = drum_params_[channel & 0x0Fu][note & 0x7Fu];
    if ((gd.flags & GsDrumNoteParams::kPitch) != 0 && gd.pitch_coarse != 0) {
      drum_mod.pitch_ratio = std::exp2(static_cast<float>(gd.pitch_coarse) / 12.0f);
    }
    if ((gd.flags & GsDrumNoteParams::kLevel) != 0) {
      const float v = static_cast<float>(gd.level & 0x7Fu) / 127.0f;
      drum_mod.level_gain = v * v;  // same square law as CC7 / velocity
    }
    if ((gd.flags & GsDrumNoteParams::kPan) != 0) {
      drum_mod.pan_units = (static_cast<float>(gd.pan & 0x7Fu) - 64.0f) / 63.0f * 500.0f;
    }
  }
  voice->start(patch, sample_rate_, velocity, voice_index, 0.0f, ch.una_corda, drum_kit, drum_mod);

  // Pipe-organ patches share a per-part wind chest (tremulant / wind sag).
  // Re-prepare only when the parameters change so the tremulant phase stays
  // continuous across notes.
  const uint8_t part = channel & 0x0Fu;
  if (patch.mode == SynthEngineMode::kPipeOrgan) {
    FallbackWindParams& wp = fallback_wind_params_[part];
    const float rate = patch.pipe_organ.tremulant_rate_hz;
    const float depth = patch.pipe_organ.tremulant_depth;
    const float sag = patch.pipe_organ.wind_sag;
    if (wp.rate != rate || wp.depth != depth || wp.sag != sag) {
      wp = {rate, depth, sag};
      fallback_wind_[part].prepare(sample_rate_, rate, depth, sag);
    }
  }

  // Bus-level body resonators (the components the NativeSynth host folds in):
  // the piano's modal soundboard + pedal-gated sympathetic bank, and the
  // plucked-string open-string halo. Re-prepared only when the part's patch
  // kind or soundboard mix changes.
  FallbackBodyState& body = fallback_body_[part];
  if (patch.mode == SynthEngineMode::kPiano) {
    if (body.kind != FallbackBodyKind::kPiano || body.soundboard_mix != patch.piano.soundboard) {
      body.kind = FallbackBodyKind::kPiano;
      body.soundboard_mix = patch.piano.soundboard;
      fallback_board_[part].prepare(sample_rate_, patch.piano.soundboard);
      fallback_reso_[part].prepare(sample_rate_);
    }
  } else if (patch.mode == SynthEngineMode::kKarplusStrong && patch.ks.sympathetic) {
    if (body.kind != FallbackBodyKind::kGuitarHalo) {
      body.kind = FallbackBodyKind::kGuitarHalo;
      body.soundboard_mix = -1.0f;
      fallback_reso_[part].prepare_guitar_sympathetic(sample_rate_);
    }
  } else if (body.kind != FallbackBodyKind::kNone && !ch.drums) {
    // The part moved to a program with no body resonator.
    body.kind = FallbackBodyKind::kNone;
    body.soundboard_mix = -1.0f;
    body.ringout = 0;
  }
}

void Sf2Player::note_off(uint8_t channel, uint8_t note) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  for (Sf2Voice& v : pool_) {
    if (v.active && v.note == note && v.channel == ch && v.key_down) {
      v.key_down = false;
      // A sostenuto capture holds the note regardless of the sustain pedal.
      if (v.sostenuto) continue;
      if (!st.sustain) v.release();
    }
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.note == note && v.channel == ch && v.key_down) {
      v.key_down = false;
      if (v.sostenuto) continue;
      if (!st.sustain) {
        v.release();
      } else if (st.sustain_level < 127 && v.patch != nullptr &&
                 v.patch->mode == SynthEngineMode::kPiano) {
        // Half-pedal: the partially raised damper rests on the string.
        v.piano.damp(static_cast<float>(127 - st.sustain_level) / 63.0f);
      }
    }
  }
}

void Sf2Player::sustain_pedal(uint8_t channel, bool down) noexcept {
  sustain_cc(channel, down ? 127 : 0);
}

void Sf2Player::sustain_cc(uint8_t channel, uint8_t value) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  const bool was_down = st.sustain;
  st.sustain_level = value;
  st.sustain = value >= 64;
  if (st.sustain) {
    // Half-pedal: a partially raised damper still rests on the piano strings,
    // so held-but-released fallback notes are damped rather than ringing
    // freely. Key-down and sostenuto-captured notes keep their dampers off.
    if (value < 127) {
      const float strength = static_cast<float>(127 - value) / 63.0f;
      for (NativeSynthVoice& v : fallback_pool_) {
        if (v.active && v.channel == ch && !v.key_down && !v.sostenuto && v.patch != nullptr &&
            v.patch->mode == SynthEngineMode::kPiano) {
          v.piano.damp(strength);
        }
      }
    }
    return;
  }
  if (!was_down) return;
  // Pedal up: the dampers fall on every held-but-released note. A
  // sostenuto-captured note stays held even when the sustain pedal lifts.
  for (Sf2Voice& v : pool_) {
    if (v.active && v.channel == ch && !v.key_down && !v.releasing && !v.sostenuto) v.release();
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == ch && !v.key_down && !v.releasing && !v.sostenuto) v.release();
  }
}

void Sf2Player::sostenuto_pedal(uint8_t channel, bool down) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  if (st.sostenuto_down == down) return;
  st.sostenuto_down = down;
  if (down) {
    // Capture exactly the keys held at the down edge.
    for (Sf2Voice& v : pool_) {
      if (v.active && v.channel == ch && v.key_down) v.sostenuto = true;
    }
    for (NativeSynthVoice& v : fallback_pool_) {
      if (v.active && v.channel == ch && v.key_down) v.sostenuto = true;
    }
    return;
  }
  for (Sf2Voice& v : pool_) {
    if (v.active && v.channel == ch && v.sostenuto) {
      v.sostenuto = false;
      if (!v.key_down && !v.releasing && !st.sustain) v.release();
    }
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == ch && v.sostenuto) {
      v.sostenuto = false;
      if (!v.key_down && !v.releasing && !st.sustain) v.release();
    }
  }
}

void Sf2Player::all_notes_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].sustain = false;
  for (Sf2Voice& v : pool_) {
    if (v.active && v.channel == ch && !v.releasing) {
      v.key_down = false;
      v.release();
    }
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == ch && !v.releasing) {
      v.key_down = false;
      v.release();
    }
  }
}

void Sf2Player::all_sound_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].sustain = false;
  for (Sf2Voice& v : pool_) {
    if (v.active && v.channel == ch) {
      v.env.kill();
      v.active = false;
      v.releasing = false;
    }
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == ch) v.kill();
  }
}

void Sf2Player::apply_nrpn(uint8_t channel, uint8_t value) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  const int8_t offset = static_cast<int8_t>(static_cast<int>(value & 0x7Fu) - 64);
  if (st.params.nrpn_msb == 0x01) {
    // GS part parameters (relative offsets onto the SoundFont generators).
    switch (st.params.nrpn_lsb) {
      case 0x08:
        st.gs.vibrato_rate = offset;
        break;
      case 0x09:
        st.gs.vibrato_depth = offset;
        break;
      case 0x0A:
        st.gs.vibrato_delay = offset;
        break;
      case 0x20:
        st.gs.tvf_cutoff = offset;
        break;
      case 0x21:
        st.gs.tvf_resonance = offset;
        break;
      case 0x63:
        st.gs.eg_attack = offset;
        break;
      case 0x64:
        st.gs.eg_decay = offset;
        break;
      case 0x66:
        st.gs.eg_release = offset;
        break;
      default:
        break;
    }
    return;
  }
  if (!st.drums) return;
  // GS drum-kit NRPNs: msb selects the parameter, lsb is the drum note.
  GsDrumNoteParams& d = drum_params_[ch][st.params.nrpn_lsb & 0x7Fu];
  switch (st.params.nrpn_msb) {
    case 0x18:
      d.pitch_coarse = offset;
      d.flags |= GsDrumNoteParams::kPitch;
      break;
    case 0x1A:
      d.level = value;
      d.flags |= GsDrumNoteParams::kLevel;
      break;
    case 0x1C:
      d.pan = value;
      d.flags |= GsDrumNoteParams::kPan;
      break;
    case 0x1D:
      d.reverb = value;
      d.flags |= GsDrumNoteParams::kReverb;
      break;
    case 0x1E:
      d.chorus = value;
      d.flags |= GsDrumNoteParams::kChorus;
      break;
    default:
      break;
  }
}

void Sf2Player::reset_controllers(uint8_t channel) noexcept {
  // MIDI RP-015: reset performance controllers, keep program/bank/volume/pan.
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  st.mod_wheel = 0;
  st.expression = 127;
  st.pitch_bend = 8192;
  st.params.reset();
  sustain_cc(ch, 0);
  sostenuto_pedal(ch, false);
  st.una_corda = false;
  refresh_channel_mod(ch);
}

void Sf2Player::control_change(uint8_t channel, uint8_t controller, uint8_t value) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  switch (controller) {
    case 0:  // Bank select MSB (GS variation bank)
      st.bank_msb = value;
      break;
    case 32:  // Bank select LSB
      st.bank_lsb = value;
      break;
    case 1:
      st.mod_wheel = value;
      refresh_channel_mod(ch);
      break;
    case 6:  // Data entry MSB -> active RPN (bend range) or GS NRPN
      if (st.params.selected_rpn(0, 0)) {
        st.bend_range_cents = 100.0f * static_cast<float>(value);
        refresh_channel_mod(ch);
      } else if (st.params.selected_nrpn()) {
        apply_nrpn(ch, value);
      }
      break;
    case 38:  // Data entry LSB -> bend range cents
      if (st.params.selected_rpn(0, 0)) {
        st.bend_range_cents =
            100.0f * std::floor(st.bend_range_cents / 100.0f) + static_cast<float>(value);
        refresh_channel_mod(ch);
      }
      break;
    case 7:
      st.volume = value;
      refresh_channel_mod(ch);
      break;
    case 10:
      st.pan = value;
      refresh_channel_mod(ch);
      break;
    case 11:
      st.expression = value;
      refresh_channel_mod(ch);
      break;
    case 91:
      st.reverb_send = value;
      refresh_channel_mod(ch);
      break;
    case 93:
      st.chorus_send = value;
      refresh_channel_mod(ch);
      break;
    case 94:  // GS delay send (no SF2 generator; channel-level only)
      st.delay_send = value;
      refresh_channel_mod(ch);
      break;
    case 98:  // NRPN LSB
      st.params.select_nrpn_lsb(value);
      break;
    case 99:  // NRPN MSB
      st.params.select_nrpn_msb(value);
      break;
    case 100:  // RPN LSB
      st.params.select_rpn_lsb(value);
      break;
    case 101:  // RPN MSB
      st.params.select_rpn_msb(value);
      break;
    case 64:
      sustain_cc(ch, value);
      break;
    case 66:
      sostenuto_pedal(ch, value >= 64);
      break;
    case 67:
      // Una corda: shifts the piano fallback action for notes STARTED while
      // down (the hammer strikes fewer strings); sample-playback voices keep
      // their recorded voicing.
      st.una_corda = value >= 64;
      break;
    case 120:
      all_sound_off(ch);
      break;
    case 121:
      reset_controllers(ch);
      break;
    case 123:
    case 124:
    case 125:
    case 126:
    case 127:
      all_notes_off(ch);
      break;
    default:
      break;
  }
}

void Sf2Player::on_event(uint32_t /*destination_id*/, const MidiEvent& event) noexcept {
  if (!prepared_) return;
  const Ump& u = event.ump;
  if (u.message_type() != UmpMessageType::kMidi1ChannelVoice &&
      u.message_type() != UmpMessageType::kMidi2ChannelVoice) {
    // SysEx events arrive with a control-thread-resolved payload view (the UMP
    // itself only carries a handle): feed the GS layer so GS Reset / GM System
    // On / "use for rhythm part" inside an arrangement take effect.
    if (event.sysex_payload != nullptr && event.sysex_payload_size > 0) {
      handle_sysex(event.sysex_payload, event.sysex_payload_size);
    }
    return;
  }
  if (u.is_note_on()) {
    uint8_t vel7 = 0;
    if (u.message_type() == UmpMessageType::kMidi1ChannelVoice) {
      vel7 = u.data2_7bit();
    } else {
      vel7 = static_cast<uint8_t>(((u.words[1] >> 16) & 0xFFFFu) >> 9);
      if (vel7 == 0 && ((u.words[1] >> 16) & 0xFFFFu) != 0) vel7 = 1;
    }
    note_on(u.channel(), u.note_number(), vel7);
  } else if (u.is_note_off()) {
    note_off(u.channel(), u.note_number());
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kProgramChange)) {
    const uint8_t ch = u.channel() & 0x0Fu;
    if (u.message_type() == UmpMessageType::kMidi2ChannelVoice) {
      channels_[ch].program = static_cast<uint8_t>((u.words[1] >> 24) & 0x7Fu);
      if ((u.words[0] & 0x01u) != 0) {
        channels_[ch].bank_msb = static_cast<uint8_t>((u.words[1] >> 8) & 0x7Fu);
        channels_[ch].bank_lsb = static_cast<uint8_t>(u.words[1] & 0x7Fu);
      }
    } else {
      channels_[ch].program = u.note_number();
    }
    // The fallback ambience floor is program-keyed; keep the mod snapshot
    // in step with the new program.
    refresh_channel_mod(ch);
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kPitchBend)) {
    const uint8_t ch = u.channel() & 0x0Fu;
    if (u.message_type() == UmpMessageType::kMidi1ChannelVoice) {
      // MIDI 1.0: 14-bit value, LSB in data1 (bits 8..14), MSB in data2.
      channels_[ch].pitch_bend =
          static_cast<uint16_t>((static_cast<uint16_t>(u.data2_7bit()) << 7) | u.note_number());
    } else {
      // MIDI 2.0: 32-bit value in word[1]; keep the top 14 bits.
      channels_[ch].pitch_bend = static_cast<uint16_t>(u.words[1] >> 18);
    }
    refresh_channel_mod(ch);
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange)) {
    const uint8_t controller = u.note_number();
    const uint8_t value7 = u.message_type() == UmpMessageType::kMidi1ChannelVoice
                               ? u.data2_7bit()
                               : scale_cc_32_to_7(u.words[1]);
    control_change(u.channel(), controller, value7);
  }
}

void Sf2Player::render_chunk(int n) noexcept {
  std::memset(mix_l_.data(), 0, sizeof(float) * static_cast<size_t>(n));
  std::memset(mix_r_.data(), 0, sizeof(float) * static_cast<size_t>(n));
  // Read the realised-EFX routing for this block from the snapshot the control
  // thread published (adopted in process() via acquire()). A null snapshot (none
  // published yet) or an all-dry one routes everything straight to the dry mix.
  const Sf2RealizedEfx* efx = efx_pub_->current();
  static constexpr std::array<bool, 16> kNoBus{};
  const std::array<bool, 16>& part_bussed = efx != nullptr ? efx->part_bussed : kNoBus;
  const bool any_bussed = efx != nullptr && efx->any_bussed;
  if (any_bussed && !part_bus_.empty()) {
    std::memset(part_bus_.data(), 0, sizeof(float) * part_bus_.size());
  }

#if defined(SONARE_MIDI_WITH_FX)
  float* rev_l = nullptr;
  float* rev_r = nullptr;
  float* cho_l = nullptr;
  float* cho_r = nullptr;
  float* dly_l = nullptr;
  float* dly_r = nullptr;
  if (effects_ != nullptr) {
    effects_->begin_chunk();
    rev_l = effects_->reverb_in(0);
    rev_r = effects_->reverb_in(1);
    cho_l = effects_->chorus_in(0);
    cho_r = effects_->chorus_in(1);
    dly_l = effects_->delay_in(0);
    dly_r = effects_->delay_in(1);
  }
#endif

  // Organ wind demand per part (sounding pipe-organ fallback voices): the
  // shared wind chest advances once per sample per part, not per voice.
  int organ_demand[16] = {0};
  bool any_wind = false;
  bool body_has_voice[16] = {false};
  for (const NativeSynthVoice& v : fallback_pool_) {
    if (!v.active || v.patch == nullptr) continue;
    const uint8_t part = v.channel & 0x0Fu;
    if (v.patch->mode == SynthEngineMode::kPipeOrgan) {
      ++organ_demand[part];
      any_wind = any_wind || fallback_wind_[part].active();
    }
    if (fallback_body_[part].kind != FallbackBodyKind::kNone) body_has_voice[part] = true;
  }
  // Body resonators keep ringing for a bounded tail after the last voice dies
  // (the bank's own decay), then stop costing anything.
  bool body_active[16] = {false};
  bool any_body = false;
  for (int part = 0; part < 16; ++part) {
    FallbackBodyState& body = fallback_body_[static_cast<size_t>(part)];
    if (body.kind == FallbackBodyKind::kNone) continue;
    if (body_has_voice[part]) {
      body.ringout = static_cast<int64_t>(2.0 * sample_rate_);
    } else if (body.ringout > 0) {
      body.ringout = std::max<int64_t>(0, body.ringout - n);
    }
    body_active[part] = body_has_voice[part] || body.ringout > 0;
    any_body = any_body || body_active[part];
  }

  for (int i = 0; i < n; ++i) {
    OrganWindSupply::State wind_state[16];
    if (any_wind) {
      for (int part = 0; part < 16; ++part) {
        if (organ_demand[part] > 0 && fallback_wind_[static_cast<size_t>(part)].active()) {
          wind_state[part] = fallback_wind_[static_cast<size_t>(part)].process(organ_demand[part]);
        }
      }
    }
    for (Sf2Voice& v : pool_) {
      if (!v.active) continue;
      const uint8_t part = v.channel & 0x0Fu;
      const Sf2ChannelMod& mod = channel_mods_[part];
      const float s = v.render(mod);
      const float l = s * v.gain_left;
      const float r = s * v.gain_right;
      if (part_bussed[part]) {
        float* bus = part_bus_.data() + static_cast<size_t>(part) * 2 * kChunkFrames;
        bus[i] += l;
        bus[kChunkFrames + i] += r;
      } else {
        mix_l_[static_cast<size_t>(i)] += l;
        mix_r_[static_cast<size_t>(i)] += r;
      }
#if defined(SONARE_MIDI_WITH_FX)
      if (rev_l != nullptr) {
        const float rs = std::min(1.0f, v.params.reverb_send + mod.reverb_send);
        if (rs > 0.0f) {
          rev_l[i] += l * rs;
          rev_r[i] += r * rs;
        }
        const float cs = std::min(1.0f, v.params.chorus_send + mod.chorus_send);
        if (cs > 0.0f) {
          cho_l[i] += l * cs;
          cho_r[i] += r * cs;
        }
        if (mod.delay_send > 0.0f) {
          dly_l[i] += l * mod.delay_send;
          dly_r[i] += r * mod.delay_send;
        }
      }
#endif
    }
    // Synth-fallback voices: same bus routing, channel-level (CC) sends only
    // (no zone send generators).
    float body_dry[16] = {0.0f};
    for (NativeSynthVoice& v : fallback_pool_) {
      if (!v.active) continue;
      const uint8_t part = v.channel & 0x0Fu;
      const Sf2ChannelMod& mod = channel_mods_[part];
      const OrganWindSupply::State& wind = wind_state[part];
      const float s = v.render(mod, wind.pitch_ratio, wind.gain);
      float l = s * v.gain_left;
      float r = s * v.gain_right;
      if (body_active[part]) body_dry[part] += 0.5f * (l + r);
      // Piano radiates mostly through the board (the body block below); only
      // the direct share of the raw string waveform stays in the voice path.
      if (fallback_body_[part].kind == FallbackBodyKind::kPiano) {
        l *= kPianoDirectGain;
        r *= kPianoDirectGain;
      }
      if (part_bussed[part]) {
        float* bus = part_bus_.data() + static_cast<size_t>(part) * 2 * kChunkFrames;
        bus[i] += l;
        bus[kChunkFrames + i] += r;
      } else {
        mix_l_[static_cast<size_t>(i)] += l;
        mix_r_[static_cast<size_t>(i)] += r;
      }
#if defined(SONARE_MIDI_WITH_FX)
      if (rev_l != nullptr) {
        if (mod.fallback_reverb_send > 0.0f) {
          rev_l[i] += l * mod.fallback_reverb_send;
          rev_r[i] += r * mod.fallback_reverb_send;
        }
        if (mod.fallback_chorus_send > 0.0f) {
          cho_l[i] += l * mod.fallback_chorus_send;
          cho_r[i] += r * mod.fallback_chorus_send;
        }
        if (mod.delay_send > 0.0f) {
          dly_l[i] += l * mod.delay_send;
          dly_r[i] += r * mod.delay_send;
        }
      }
#endif
    }
    // Shared body resonators, fed by the part's summed fallback dry signal and
    // folded back centre-panned (the same bus-level coupling the NativeSynth
    // host applies): the piano's soundboard + pedal-gated sympathetic bank,
    // the plucked halo held open (no dampers).
    if (any_body) {
      for (int part = 0; part < 16; ++part) {
        if (!body_active[part]) continue;
        const FallbackBodyState& body = fallback_body_[static_cast<size_t>(part)];
        const float dry = body_dry[part];
        float add = 0.0f;
        if (body.kind == FallbackBodyKind::kPiano) {
          PianoSoundboard& board = fallback_board_[static_cast<size_t>(part)];
          add = board.process(dry) +
                fallback_reso_[static_cast<size_t>(part)].process(
                    board.last_diffused(), channels_[static_cast<size_t>(part)].sustain);
        } else {
          add = fallback_reso_[static_cast<size_t>(part)].process(dry, /*damper_open=*/true);
        }
        if (add == 0.0f) continue;
        if (part_bussed[part]) {
          float* bus = part_bus_.data() + static_cast<size_t>(part) * 2 * kChunkFrames;
          bus[i] += add;
          bus[kChunkFrames + i] += add;
        } else {
          mix_l_[static_cast<size_t>(i)] += add;
          mix_r_[static_cast<size_t>(i)] += add;
        }
#if defined(SONARE_MIDI_WITH_FX)
        if (rev_l != nullptr) {
          const Sf2ChannelMod& mod = channel_mods_[part];
          if (mod.fallback_reverb_send > 0.0f) {
            rev_l[i] += add * mod.fallback_reverb_send;
            rev_r[i] += add * mod.fallback_reverb_send;
          }
          if (mod.fallback_chorus_send > 0.0f) {
            cho_l[i] += add * mod.fallback_chorus_send;
            cho_r[i] += add * mod.fallback_chorus_send;
          }
        }
#endif
      }
    }
  }

  // Per-part insert processing, then sum the parts into the dry mix.
  if (any_bussed) {
    for (int part = 0; part < 16; ++part) {
      if (!part_bussed[static_cast<size_t>(part)]) continue;
      float* bus_l = part_bus_.data() + static_cast<size_t>(part) * 2 * kChunkFrames;
      float* bus_r = bus_l + kChunkFrames;
      const Sf2PartInsert& insert = config_.part_inserts[static_cast<size_t>(part)];
      if (insert.type == Sf2InsertType::kDrive && insert.amount > 0.0f) {
        // Gain-compensated tanh drive: normalise so a full-scale input keeps
        // roughly unit level regardless of the drive amount.
        const float drive = 1.0f + 9.0f * insert.amount;
        const float makeup = 1.0f / std::tanh(drive);
        for (int i = 0; i < n; ++i) {
          bus_l[i] = std::tanh(drive * bus_l[i]) * makeup;
          bus_r[i] = std::tanh(drive * bus_r[i]) * makeup;
        }
      } else {
        // A built insert chain (config kProcessor slot, or a GS-EFX-installed
        // one — single-stage or a composite's multi-stage rig) runs in series
        // in place on the part's stereo bus. An empty chain is an inert no-op.
        float* chans[2] = {bus_l, bus_r};
        for (auto& proc : efx->chains[static_cast<size_t>(part)]) {
          proc->process(chans, 2, n);
        }
      }
      for (int i = 0; i < n; ++i) {
        mix_l_[static_cast<size_t>(i)] += bus_l[i];
        mix_r_[static_cast<size_t>(i)] += bus_r[i];
      }
    }
  }

#if defined(SONARE_MIDI_WITH_FX)
  if (effects_ != nullptr) effects_->render_returns(mix_l_.data(), mix_r_.data(), n);
#endif
}

void Sf2Player::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_ || channels == nullptr || num_channels <= 0 || num_samples <= 0) return;
  // Offline hosts realise a pending EFX change inline (an EFX SysEx dispatched
  // for this block installs its inserts before the block renders). This
  // allocates, so it is gated to single-threaded/offline use; the live engine
  // realises on the control thread via on_control_sysex instead.
  if (config_.realize_efx_inline && gs_efx_dirty_) realize_gs_efx();
  // Adopt the newest realised-EFX snapshot for this block (wait-free, no alloc).
  // Offline this picks up the inline publish just above; live it picks up the
  // control thread's on_control_sysex publish.
  efx_pub_->acquire();
  if (mix_l_.size() < static_cast<size_t>(kChunkFrames)) return;
  float* left = channels[0];
  float* right = num_channels > 1 ? channels[1] : nullptr;
  const bool mono = right == nullptr;

  int offset = 0;
  while (offset < num_samples) {
    const int n = std::min(kChunkFrames, num_samples - offset);
    render_chunk(n);
    for (int i = 0; i < n; ++i) {
      const float mix_l = mix_l_[static_cast<size_t>(i)] * config_.gain;
      const float mix_r = mix_r_[static_cast<size_t>(i)] * config_.gain;
      if (left != nullptr) {
        // Mono host: fold both pan legs so centre-panned voices keep level.
        left[offset + i] += mono ? 0.70710678f * (mix_l + mix_r) : mix_l;
      }
      if (right != nullptr) right[offset + i] += mix_r;
      // Fan a mono fold-down to any additional channels.
      for (int ch = 2; ch < num_channels; ++ch) {
        if (channels[ch] != nullptr) channels[ch][offset + i] += 0.70710678f * (mix_l + mix_r);
      }
    }
    offset += n;
  }
}

}  // namespace sonare::midi::synth
