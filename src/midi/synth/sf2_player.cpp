#include "midi/synth/sf2_player.h"

#include <algorithm>
#include <cmath>

#include "midi/builtin_synth.h"
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

Sf2Player::Sf2Player(const Sf2PlayerConfig& config) noexcept : config_(config) {
  if (!(config_.gain > 0.0f) || !std::isfinite(config_.gain)) config_.gain = 0.5f;
  config_.gain = std::min(config_.gain, 4.0f);
  config_.polyphony = config_.polyphony > 0 ? std::min(config_.polyphony, kMaxSynthVoices) : 48;
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
  if (prepared_) {
    const float release_ms = std::max(5.0f, 1000.0f * timecents_to_seconds(max_release_timecents_));
    tail_samples_ = DahdsrEnvelope::release_tail_samples(sample_rate_, release_ms);
  }
}

void Sf2Player::prepare(double sample_rate, int /*max_block_size*/) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  pool_.prepare(config_.polyphony);
  channels_ = {};
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_channel_mod(ch);
  const float release_ms = std::max(5.0f, 1000.0f * timecents_to_seconds(max_release_timecents_));
  tail_samples_ = DahdsrEnvelope::release_tail_samples(sample_rate_, release_ms);
  prepared_ = true;
}

void Sf2Player::reset() {
  pool_.reset();
  channels_ = {};
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_channel_mod(ch);
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
}

uint16_t Sf2Player::effective_bank(uint8_t channel) const noexcept {
  if ((channel & 0x0Fu) == kDrumChannel) return kDrumBank;
  return channels_[channel & 0x0Fu].bank_msb;
}

int Sf2Player::resolve_preset(uint16_t bank, uint8_t program) const noexcept {
  if (soundfont_ == nullptr) return -1;
  // Exact (bank, program).
  int idx = soundfont_->find_preset(bank, program);
  if (idx >= 0) return idx;
  // GS variation fallback: unknown variation banks fall back to the capital
  // tone (bank 0); drum banks fall back to the standard kit (program 0).
  if (bank == kDrumBank) {
    idx = soundfont_->find_preset(kDrumBank, 0);
    return idx;
  }
  if (bank != 0) {
    idx = soundfont_->find_preset(0, program);
    if (idx >= 0) return idx;
  }
  return -1;
}

void Sf2Player::note_on(uint8_t channel, uint8_t note, uint8_t velocity) noexcept {
  if (!prepared_ || soundfont_ == nullptr) return;
  const ChannelState& ch = channels_[channel & 0x0Fu];
  const int preset_idx = resolve_preset(effective_bank(channel), ch.program);
  if (preset_idx < 0) return;
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

      const Sf2VoiceParams params =
          resolve_voice_params(gens, sample, note, velocity, sample_rate_);

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

void Sf2Player::note_off(uint8_t channel, uint8_t note) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  for (Sf2Voice& v : pool_) {
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
}

void Sf2Player::reset_controllers(uint8_t channel) noexcept {
  // MIDI RP-015: reset performance controllers, keep program/bank/volume/pan.
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  st.mod_wheel = 0;
  st.expression = 127;
  st.pitch_bend = 8192;
  st.rpn_msb = 127;
  st.rpn_lsb = 127;
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
    case 6:  // Data entry MSB -> active RPN (bend range semitones)
      if (st.rpn_msb == 0 && st.rpn_lsb == 0) {
        st.bend_range_cents = 100.0f * static_cast<float>(value);
        refresh_channel_mod(ch);
      }
      break;
    case 38:  // Data entry LSB -> bend range cents
      if (st.rpn_msb == 0 && st.rpn_lsb == 0) {
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
    case 100:  // RPN LSB
      st.rpn_lsb = value;
      break;
    case 101:  // RPN MSB
      st.rpn_msb = value;
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

void Sf2Player::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_ || channels == nullptr || num_channels <= 0 || num_samples <= 0) return;
  float* left = channels[0];
  float* right = num_channels > 1 ? channels[1] : nullptr;
  const bool mono = right == nullptr;

  for (int i = 0; i < num_samples; ++i) {
    float mix_l = 0.0f;
    float mix_r = 0.0f;
    for (Sf2Voice& v : pool_) {
      if (!v.active) continue;
      const float s = v.render(channel_mods_[v.channel & 0x0Fu]);
      mix_l += s * v.gain_left;
      mix_r += s * v.gain_right;
    }
    mix_l *= config_.gain;
    mix_r *= config_.gain;
    if (left != nullptr) {
      // Mono host: fold both pan legs so centre-panned voices keep full level.
      left[i] += mono ? 0.70710678f * (mix_l + mix_r) : mix_l;
    }
    if (right != nullptr) right[i] += mix_r;
    // Fan a mono fold-down to any additional channels.
    for (int ch = 2; ch < num_channels; ++ch) {
      if (channels[ch] != nullptr) channels[ch][i] += 0.70710678f * (mix_l + mix_r);
    }
  }
}

}  // namespace sonare::midi::synth
