#include "midi/synth/sf2_player.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "midi/builtin_synth.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/ump.h"

namespace sonare::midi::synth {

namespace {

constexpr uint8_t kDrumChannel = 9;  // MIDI channel 10
constexpr uint16_t kDrumBank = 128;

/// Default modulator: full CC1 adds 50 cents of vibrato LFO pitch depth.
constexpr float kModWheelVibratoCents = 50.0f;
/// Default modulator: full CC91/CC93 contributes 200/1000 send level.
constexpr float kCcSendDepth = 0.2f;

}  // namespace

Sf2Player::Sf2Player(const Sf2PlayerConfig& config) : config_(config) {
  if (!(config_.gain > 0.0f) || !std::isfinite(config_.gain)) config_.gain = 0.5f;
  config_.gain = std::min(config_.gain, 4.0f);
  config_.polyphony = config_.polyphony > 0 ? std::min(config_.polyphony, kMaxSynthVoices) : 48;
  for (Sf2PartInsert& insert : config_.part_inserts) {
    insert.amount = std::clamp(insert.amount, 0.0f, 1.0f);
    any_insert_ = any_insert_ || insert.type != Sf2InsertType::kNone;
  }
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
  reset_all_state(/*reverb_send_default=*/0);
  mix_l_.assign(kChunkFrames, 0.0f);
  mix_r_.assign(kChunkFrames, 0.0f);
  part_bus_.assign(any_insert_ ? 16 * 2 * static_cast<size_t>(kChunkFrames) : 0, 0.0f);
#if defined(SONARE_MIDI_WITH_FX)
  if (effects_ != nullptr) effects_->prepare(sample_rate_);
#endif
  recompute_tail();
  prepared_ = true;
}

void Sf2Player::reset() {
  pool_.reset();
  fallback_pool_.reset();
  reset_all_state(/*reverb_send_default=*/0);
#if defined(SONARE_MIDI_WITH_FX)
  if (effects_ != nullptr) effects_->reset();
#endif
}

void Sf2Player::reset_all_state(uint8_t reverb_send_default) noexcept {
  channels_ = {};
  drum_params_ = {};
  for (uint8_t ch = 0; ch < 16; ++ch) {
    channels_[ch].drums = ch == kDrumChannel;
    channels_[ch].reverb_send = reverb_send_default;
    refresh_channel_mod(ch);
  }
}

void Sf2Player::gs_reset() noexcept {
  for (uint8_t ch = 0; ch < 16; ++ch) all_sound_off(ch);
  // GS power-on: reverb send 40 (Roland default), everything else cleared.
  reset_all_state(/*reverb_send_default=*/40);
}

void Sf2Player::gm_reset() noexcept {
  for (uint8_t ch = 0; ch < 16; ++ch) all_sound_off(ch);
  // GM Level 1 mandates no effects: sends stay at zero.
  reset_all_state(/*reverb_send_default=*/0);
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
    case GsSysExKind::kNone:
      break;
  }
  return false;
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
}

uint16_t Sf2Player::effective_bank(uint8_t channel) const noexcept {
  if (channels_[channel & 0x0Fu].drums) return kDrumBank;
  return channels_[channel & 0x0Fu].bank_msb;
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
}

void Sf2Player::fallback_note_on(uint8_t channel, uint8_t note, uint8_t velocity) noexcept {
  const ChannelState& ch = channels_[channel & 0x0Fu];
  const NativeSynthPatch& patch = ch.drums ? gm_fallback_drum_patch(note)
                                           : gm_fallback_patch(effective_bank(channel), ch.program);
  NativeSynthVoice* voice = fallback_pool_.allocate(channel & 0x0Fu, note);
  if (voice == nullptr) return;
  const uint32_t voice_index = static_cast<uint32_t>(voice - fallback_pool_.data());
  voice->start(patch, sample_rate_, velocity, voice_index);
}

void Sf2Player::note_off(uint8_t channel, uint8_t note) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  for (Sf2Voice& v : pool_) {
    if (v.active && v.note == note && v.channel == ch && v.key_down) {
      v.key_down = false;
      if (!channels_[ch].sustain) v.release();
    }
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.note == note && v.channel == ch && v.key_down) {
      v.key_down = false;
      if (!channels_[ch].sustain) v.release();
    }
  }
}

void Sf2Player::sustain_pedal(uint8_t channel, bool down) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  if (channels_[ch].sustain == down) return;
  channels_[ch].sustain = down;
  if (down) return;
  for (Sf2Voice& v : pool_) {
    if (v.active && v.channel == ch && !v.key_down && !v.releasing) v.release();
  }
  for (NativeSynthVoice& v : fallback_pool_) {
    if (v.active && v.channel == ch && !v.key_down && !v.releasing) v.release();
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
  if (st.nrpn_msb == 0x01) {
    // GS part parameters (relative offsets onto the SoundFont generators).
    switch (st.nrpn_lsb) {
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
  GsDrumNoteParams& d = drum_params_[ch][st.nrpn_lsb & 0x7Fu];
  switch (st.nrpn_msb) {
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
  st.param_mode = ChannelState::ParamMode::kNone;
  st.rpn_msb = 127;
  st.rpn_lsb = 127;
  st.nrpn_msb = 127;
  st.nrpn_lsb = 127;
  sustain_pedal(ch, false);
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
      if (st.param_mode == ChannelState::ParamMode::kRpn && st.rpn_msb == 0 && st.rpn_lsb == 0) {
        st.bend_range_cents = 100.0f * static_cast<float>(value);
        refresh_channel_mod(ch);
      } else if (st.param_mode == ChannelState::ParamMode::kNrpn) {
        apply_nrpn(ch, value);
      }
      break;
    case 38:  // Data entry LSB -> bend range cents
      if (st.param_mode == ChannelState::ParamMode::kRpn && st.rpn_msb == 0 && st.rpn_lsb == 0) {
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
      st.nrpn_lsb = value;
      st.param_mode = ChannelState::ParamMode::kNrpn;
      break;
    case 99:  // NRPN MSB
      st.nrpn_msb = value;
      st.param_mode = ChannelState::ParamMode::kNrpn;
      break;
    case 100:  // RPN LSB
      st.rpn_lsb = value;
      st.param_mode = ChannelState::ParamMode::kRpn;
      break;
    case 101:  // RPN MSB
      st.rpn_msb = value;
      st.param_mode = ChannelState::ParamMode::kRpn;
      break;
    case 64:
      sustain_pedal(ch, value >= 64);
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
    channels_[u.channel() & 0x0Fu].program = u.note_number();  // data1 slot
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
  if (any_insert_ && !part_bus_.empty()) {
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

  for (int i = 0; i < n; ++i) {
    for (Sf2Voice& v : pool_) {
      if (!v.active) continue;
      const uint8_t part = v.channel & 0x0Fu;
      const Sf2ChannelMod& mod = channel_mods_[part];
      const float s = v.render(mod);
      const float l = s * v.gain_left;
      const float r = s * v.gain_right;
      if (any_insert_) {
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
    for (NativeSynthVoice& v : fallback_pool_) {
      if (!v.active) continue;
      const uint8_t part = v.channel & 0x0Fu;
      const Sf2ChannelMod& mod = channel_mods_[part];
      const float s = v.render(mod);
      const float l = s * v.gain_left;
      const float r = s * v.gain_right;
      if (any_insert_) {
        float* bus = part_bus_.data() + static_cast<size_t>(part) * 2 * kChunkFrames;
        bus[i] += l;
        bus[kChunkFrames + i] += r;
      } else {
        mix_l_[static_cast<size_t>(i)] += l;
        mix_r_[static_cast<size_t>(i)] += r;
      }
#if defined(SONARE_MIDI_WITH_FX)
      if (rev_l != nullptr) {
        if (mod.reverb_send > 0.0f) {
          rev_l[i] += l * mod.reverb_send;
          rev_r[i] += r * mod.reverb_send;
        }
        if (mod.chorus_send > 0.0f) {
          cho_l[i] += l * mod.chorus_send;
          cho_r[i] += r * mod.chorus_send;
        }
        if (mod.delay_send > 0.0f) {
          dly_l[i] += l * mod.delay_send;
          dly_r[i] += r * mod.delay_send;
        }
      }
#endif
    }
  }

  // Per-part insert processing, then sum the parts into the dry mix.
  if (any_insert_) {
    for (int part = 0; part < 16; ++part) {
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
