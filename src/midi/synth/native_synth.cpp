#include "midi/synth/native_synth.h"

#include <algorithm>
#include <cmath>

#include "midi/builtin_synth.h"
#include "midi/synth/voice_random.h"
#include "midi/ump.h"

namespace sonare::midi::synth {

namespace {

/// Default modulator: full CC1 adds 50 cents of vibrato depth (matches
/// Sf2Player so the fallback and SF2 voices respond alike).
constexpr float kModWheelVibratoCents = 50.0f;
/// Fixed pitch-bend range (cents); the patch surface has no RPN handling.
constexpr float kBendRangeCents = 200.0f;

float pan_angle(float pan_units) noexcept {
  const float pan = std::clamp(pan_units, -500.0f, 500.0f);
  return (pan + 500.0f) / 1000.0f * 1.57079632679f;  // 0..pi/2
}

DahdsrConfig clamp_env(const DahdsrConfig& env) noexcept {
  DahdsrConfig out;
  out.delay_ms = std::clamp(env.delay_ms, 0.0f, 5000.0f);
  out.attack_ms = std::clamp(env.attack_ms, 0.0f, 20000.0f);
  out.hold_ms = std::clamp(env.hold_ms, 0.0f, 5000.0f);
  out.decay_ms = std::clamp(env.decay_ms, 0.0f, 20000.0f);
  out.sustain = std::clamp(env.sustain, 0.0f, 1.0f);
  out.release_ms = std::clamp(env.release_ms, 1.0f, 20000.0f);
  return out;
}

float sanitize(float value, float fallback) noexcept {
  return std::isfinite(value) ? value : fallback;
}

}  // namespace

float synth_note_to_hz(float note) noexcept { return 440.0f * std::exp2((note - 69.0f) / 12.0f); }

NativeSynthPatch clamp_synth_patch(const NativeSynthPatch& patch) noexcept {
  NativeSynthPatch p = patch;
  p.unison = std::clamp(p.unison, 1, kMaxUnisonOscs);
  p.detune_cents = std::clamp(sanitize(p.detune_cents, 0.0f), 0.0f, 200.0f);
  p.drift_cents = std::clamp(sanitize(p.drift_cents, 0.0f), 0.0f, 100.0f);
  p.drift_rate_hz = std::clamp(sanitize(p.drift_rate_hz, 0.3f), 0.01f, 20.0f);
  p.pitch_offset_cents = std::clamp(sanitize(p.pitch_offset_cents, 0.0f), -4800.0f, 4800.0f);
  p.gain = std::clamp(sanitize(p.gain, 0.5f), 0.0f, 4.0f);
  p.amp_env = clamp_env(p.amp_env);
  p.cutoff_hz = std::clamp(sanitize(p.cutoff_hz, 12000.0f), 10.0f, 22000.0f);
  p.resonance_q = std::clamp(sanitize(p.resonance_q, 0.707f), 0.5f, 30.0f);
  p.filter_env = clamp_env(p.filter_env);
  p.env_to_cutoff_cents = std::clamp(sanitize(p.env_to_cutoff_cents, 0.0f), -9600.0f, 9600.0f);
  p.key_track = std::clamp(sanitize(p.key_track, 0.0f), 0.0f, 1.0f);
  p.vel_to_cutoff_cents = std::clamp(sanitize(p.vel_to_cutoff_cents, 0.0f), -9600.0f, 9600.0f);
  p.lfo_rate_hz = std::clamp(sanitize(p.lfo_rate_hz, 5.0f), 0.0f, 40.0f);
  p.lfo_to_pitch_cents = std::clamp(sanitize(p.lfo_to_pitch_cents, 0.0f), 0.0f, 1200.0f);
  return p;
}

// ---------------------------------------------------------------------------
// NativeSynthVoice
// ---------------------------------------------------------------------------

void NativeSynthVoice::start(const NativeSynthPatch& p, double sample_rate, uint8_t velocity,
                             uint32_t voice_index) noexcept {
  patch = &p;
  key_down = true;
  releasing = false;

  VoiceRandomSequence seq;
  seq.reseed(voice_index, note, age);

  base_freq_hz = synth_note_to_hz(static_cast<float>(note & 0x7Fu) + p.pitch_offset_cents / 100.0f);

  unison = std::clamp(p.unison, 1, kMaxUnisonOscs);
  osc_norm = 1.0f / std::sqrt(static_cast<float>(unison));
  for (int k = 0; k < unison; ++k) {
    // Symmetric detune positions across [-1, 1] plus a small seeded jitter so
    // the stack never phase-locks; oscillator 0 of a single-osc patch stays
    // exactly on pitch.
    float spread = 0.0f;
    if (unison > 1) {
      spread = 2.0f * static_cast<float>(k) / static_cast<float>(unison - 1) - 1.0f;
      spread += 0.1f * seq.bipolar_at(static_cast<uint64_t>(k) * 2);
    }
    const float detune = 0.5f * p.detune_cents * spread;
    detune_ratio[static_cast<size_t>(k)] = std::exp2(detune / 1200.0f);
    // Seeded start phase: identical unison oscillators starting at phase 0
    // sound phasey/static; noise gets a per-osc seed stream instead.
    const float phase = seq.unipolar_at(static_cast<uint64_t>(k) * 2 + 1);
    oscs[static_cast<size_t>(k)].start(sample_rate, p.waveform, phase,
                                       voice_seed(voice_index, note, age) ^ (k + 1));
  }

  velocity_gain = sf2_velocity_gain(velocity);
  static_cutoff_cents =
      p.vel_to_cutoff_cents * (static_cast<float>(velocity & 0x7Fu) / 127.0f - 1.0f) +
      p.key_track * 100.0f * (static_cast<float>(note & 0x7Fu) - 60.0f);
  filter_bypass = p.filter_output == SynthFilterOutput::kLowpass && p.cutoff_hz >= 18000.0f &&
                  p.env_to_cutoff_cents == 0.0f && static_cutoff_cents >= 0.0f &&
                  p.resonance_q <= 0.71f;

  amp_env.configure(sample_rate, p.amp_env);
  amp_env.kill();
  amp_env.note_on();
  filter_env.configure(sample_rate, p.filter_env);
  filter_env.kill();
  filter_env.note_on();
  filter.prepare(sample_rate);

  vibrato_lfo.start(sample_rate, 0.0f, p.lfo_rate_hz);
  // Per-voice drift: seeded depth (sign included) and a seeded rate offset so
  // stacked voices beat against each other instead of wobbling in unison.
  drift_depth_cents = p.drift_cents * seq.bipolar_at(101);
  drift_lfo.start(sample_rate, 0.0f, p.drift_rate_hz * (0.75f + 0.5f * seq.unipolar_at(102)));

  cached_pan_units = 1.0e9f;  // force pan recompute on first render
}

float NativeSynthVoice::render(const Sf2ChannelMod& mod) noexcept {
  if (!active || patch == nullptr) return 0.0f;

  // Refresh the cached stereo pan gains when the channel pan changed.
  if (mod.pan_units != cached_pan_units) {
    cached_pan_units = mod.pan_units;
    const float angle = pan_angle(mod.pan_units);
    gain_left = std::cos(angle);
    gain_right = std::sin(angle);
  }

  // --- pitch: bend + vibrato (patch LFO + mod wheel) + seeded drift ---
  const float vib = vibrato_lfo.next() * (patch->lfo_to_pitch_cents + mod.extra_vibrato_cents);
  const float drift = drift_lfo.next() * drift_depth_cents;
  const float pitch_cents = mod.pitch_cents + vib + drift;
  const float common = pitch_cents != 0.0f ? std::exp2(pitch_cents * (1.0f / 1200.0f)) : 1.0f;

  float sample = 0.0f;
  for (int k = 0; k < unison; ++k) {
    auto& osc = oscs[static_cast<size_t>(k)];
    osc.set_frequency(base_freq_hz * common * detune_ratio[static_cast<size_t>(k)]);
    sample += osc.next();
  }
  sample *= osc_norm;

  // --- filter: cutoff = patch Fc * 2^((env + velocity + keytrack)/1200) ---
  const float fenv = filter_env.next();
  if (!filter_bypass) {
    const float fc_cents = fenv * patch->env_to_cutoff_cents + static_cutoff_cents;
    const float fc = patch->cutoff_hz * std::exp2(fc_cents * (1.0f / 1200.0f));
    filter.set(fc, patch->resonance_q);
    const TptSvf::Outputs out = filter.process(sample);
    switch (patch->filter_output) {
      case SynthFilterOutput::kLowpass:
        sample = out.lp;
        break;
      case SynthFilterOutput::kBandpass:
        sample = out.bp;
        break;
      case SynthFilterOutput::kHighpass:
        sample = out.hp;
        break;
    }
  }

  // --- amplitude ---
  const float level = amp_env.next();
  if (!amp_env.active()) {
    active = false;
    return 0.0f;
  }
  return sample * level * velocity_gain * patch->gain * mod.gain;
}

void NativeSynthVoice::release() noexcept {
  key_down = false;
  // One-shot (drum) voices ring out their zero-sustain decay regardless of
  // note-off; everything else enters the release stage.
  if (patch != nullptr && patch->one_shot) return;
  releasing = true;
  amp_env.note_off();
  filter_env.note_off();
}

void NativeSynthVoice::kill() noexcept {
  amp_env.kill();
  filter_env.kill();
  active = false;
  releasing = false;
}

// ---------------------------------------------------------------------------
// NativeSynth (MidiInstrument)
// ---------------------------------------------------------------------------

NativeSynth::NativeSynth(const NativeSynthConfig& config) : config_(config) {
  config_.patch = clamp_synth_patch(config_.patch);
  if (!(config_.gain > 0.0f) || !std::isfinite(config_.gain)) config_.gain = 0.5f;
  config_.gain = std::min(config_.gain, 4.0f);
  config_.polyphony = config_.polyphony > 0 ? std::min(config_.polyphony, kMaxSynthVoices) : 16;
}

void NativeSynth::prepare(double sample_rate, int /*max_block_size*/) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  pool_.prepare(config_.polyphony);
  channels_ = {};
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_channel_mod(ch);
  tail_samples_ =
      DahdsrEnvelope::release_tail_samples(sample_rate_, config_.patch.amp_env.release_ms);
  prepared_ = true;
}

void NativeSynth::reset() {
  pool_.reset();
  channels_ = {};
  for (uint8_t ch = 0; ch < 16; ++ch) refresh_channel_mod(ch);
}

void NativeSynth::refresh_channel_mod(uint8_t channel) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  const ChannelState& st = channels_[ch];
  Sf2ChannelMod& mod = channel_mods_[ch];
  mod.pitch_cents = (static_cast<float>(st.pitch_bend) - 8192.0f) / 8192.0f * kBendRangeCents;
  mod.gain = sf2_cc_gain(st.volume) * sf2_cc_gain(st.expression);
  mod.extra_vibrato_cents = kModWheelVibratoCents * static_cast<float>(st.mod_wheel) / 127.0f;
  mod.pan_units = (static_cast<float>(st.pan) - 64.0f) / 63.0f * 500.0f;
}

void NativeSynth::note_on(uint8_t channel, uint8_t note, uint8_t velocity) noexcept {
  if (!prepared_) return;
  NativeSynthVoice* voice = pool_.allocate(channel & 0x0Fu, note);
  if (voice == nullptr) return;
  const uint32_t voice_index = static_cast<uint32_t>(voice - pool_.data());
  voice->start(config_.patch, sample_rate_, velocity, voice_index);
}

void NativeSynth::note_off(uint8_t channel, uint8_t note) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.note == note && v.channel == ch && v.key_down) {
      v.key_down = false;
      if (!channels_[ch].sustain) v.release();
    }
  }
}

void NativeSynth::sustain_pedal(uint8_t channel, bool down) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  if (channels_[ch].sustain == down) return;
  channels_[ch].sustain = down;
  if (down) return;
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.channel == ch && !v.key_down && !v.releasing) v.release();
  }
}

void NativeSynth::all_notes_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].sustain = false;
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.channel == ch && !v.releasing) {
      v.key_down = false;
      v.release();
    }
  }
}

void NativeSynth::all_sound_off(uint8_t channel) noexcept {
  if (!prepared_) return;
  const uint8_t ch = channel & 0x0Fu;
  channels_[ch].sustain = false;
  for (NativeSynthVoice& v : pool_) {
    if (v.active && v.channel == ch) v.kill();
  }
}

void NativeSynth::reset_controllers(uint8_t channel) noexcept {
  // MIDI RP-015: reset performance controllers, keep volume/pan.
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  st.mod_wheel = 0;
  st.expression = 127;
  st.pitch_bend = 8192;
  sustain_pedal(ch, false);
  refresh_channel_mod(ch);
}

void NativeSynth::control_change(uint8_t channel, uint8_t controller, uint8_t value) noexcept {
  const uint8_t ch = channel & 0x0Fu;
  ChannelState& st = channels_[ch];
  switch (controller) {
    case 1:
      st.mod_wheel = value;
      refresh_channel_mod(ch);
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

void NativeSynth::on_event(uint32_t /*destination_id*/, const MidiEvent& event) noexcept {
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
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kPitchBend)) {
    const uint8_t ch = u.channel() & 0x0Fu;
    if (u.message_type() == UmpMessageType::kMidi1ChannelVoice) {
      channels_[ch].pitch_bend =
          static_cast<uint16_t>((static_cast<uint16_t>(u.data2_7bit()) << 7) | u.note_number());
    } else {
      channels_[ch].pitch_bend = static_cast<uint16_t>(u.words[1] >> 18);
    }
    refresh_channel_mod(ch);
  } else if (u.status_nibble() == static_cast<uint8_t>(UmpStatus::kControlChange)) {
    const uint8_t value7 = u.message_type() == UmpMessageType::kMidi1ChannelVoice
                               ? u.data2_7bit()
                               : scale_cc_32_to_7(u.words[1]);
    control_change(u.channel(), u.note_number(), value7);
  }
}

void NativeSynth::process(float* const* channels, int num_channels, int num_samples) {
  if (!prepared_ || channels == nullptr || num_channels <= 0 || num_samples <= 0) return;
  float* left = channels[0];
  float* right = num_channels > 1 ? channels[1] : nullptr;
  const bool mono = right == nullptr;

  for (int i = 0; i < num_samples; ++i) {
    float mix_l = 0.0f;
    float mix_r = 0.0f;
    for (NativeSynthVoice& v : pool_) {
      if (!v.active) continue;
      const Sf2ChannelMod& mod = channel_mods_[v.channel & 0x0Fu];
      const float s = v.render(mod);
      mix_l += s * v.gain_left;
      mix_r += s * v.gain_right;
    }
    mix_l *= config_.gain;
    mix_r *= config_.gain;
    if (left != nullptr) {
      // Mono host: fold both pan legs so centre-panned voices keep level.
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
