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
  p.drive = std::clamp(sanitize(p.drive, 0.0f), 0.0f, 1.0f);
  p.filter_env = clamp_env(p.filter_env);
  p.env_to_cutoff_cents = std::clamp(sanitize(p.env_to_cutoff_cents, 0.0f), -9600.0f, 9600.0f);
  p.key_track = std::clamp(sanitize(p.key_track, 0.0f), 0.0f, 1.0f);
  p.vel_to_cutoff_cents = std::clamp(sanitize(p.vel_to_cutoff_cents, 0.0f), -9600.0f, 9600.0f);
  p.lfo_rate_hz = std::clamp(sanitize(p.lfo_rate_hz, 5.0f), 0.0f, 40.0f);
  p.lfo_to_pitch_cents = std::clamp(sanitize(p.lfo_to_pitch_cents, 0.0f), 0.0f, 1200.0f);
  p.lfo2_rate_hz = std::clamp(sanitize(p.lfo2_rate_hz, 1.0f), 0.0f, 40.0f);
  p.glide_ms = std::clamp(sanitize(p.glide_ms, 0.0f), 0.0f, 5000.0f);
  for (ModRoute& route : p.mod_matrix.routes) {
    route.depth = std::clamp(sanitize(route.depth, 0.0f), -9600.0f, 9600.0f);
  }
  for (FmOperatorParams& op : p.fm.ops) {
    op.ratio = std::clamp(sanitize(op.ratio, 1.0f), 0.0f, 64.0f);
    op.detune_cents = std::clamp(sanitize(op.detune_cents, 0.0f), -1200.0f, 1200.0f);
    op.level = std::clamp(sanitize(op.level, 0.0f), 0.0f, 16.0f);
    op.env = clamp_env(op.env);
    op.vel_to_level = std::clamp(sanitize(op.vel_to_level, 0.0f), 0.0f, 1.0f);
    op.key_rate_scale = std::clamp(sanitize(op.key_rate_scale, 0.0f), 0.0f, 1.0f);
    op.feedback = std::clamp(sanitize(op.feedback, 0.0f), 0.0f, 4.0f);
  }
  p.ks.brightness = std::clamp(sanitize(p.ks.brightness, 0.6f), 0.0f, 1.0f);
  p.ks.decay_s = std::clamp(sanitize(p.ks.decay_s, 3.0f), 0.05f, 60.0f);
  p.ks.decay_stretch = std::clamp(sanitize(p.ks.decay_stretch, 0.5f), 0.0f, 1.0f);
  p.ks.pick_position = std::clamp(sanitize(p.ks.pick_position, 0.18f), 0.0f, 0.5f);
  p.ks.exc_brightness = std::clamp(sanitize(p.ks.exc_brightness, 0.85f), 0.0f, 1.0f);
  p.ks.vel_to_brightness = std::clamp(sanitize(p.ks.vel_to_brightness, 0.6f), 0.0f, 1.0f);
  p.ks.release_damp_s = std::clamp(sanitize(p.ks.release_damp_s, 0.08f), 0.01f, 10.0f);
  p.modal.num_modes = std::clamp(p.modal.num_modes, 0, kMaxModalModes);
  for (ModalMode& mode : p.modal.modes) {
    mode.ratio = std::clamp(sanitize(mode.ratio, 1.0f), 0.01f, 64.0f);
    mode.gain = std::clamp(sanitize(mode.gain, 1.0f), 0.0f, 4.0f);
    mode.decay_scale = std::clamp(sanitize(mode.decay_scale, 1.0f), 0.01f, 4.0f);
  }
  p.modal.decay_s = std::clamp(sanitize(p.modal.decay_s, 2.0f), 0.01f, 60.0f);
  p.modal.decay_stretch = std::clamp(sanitize(p.modal.decay_stretch, 0.3f), 0.0f, 1.0f);
  p.modal.strike_brightness = std::clamp(sanitize(p.modal.strike_brightness, 0.7f), 0.0f, 1.0f);
  p.modal.vel_to_brightness = std::clamp(sanitize(p.modal.vel_to_brightness, 0.6f), 0.0f, 1.0f);
  p.modal.release_damp_s = std::clamp(sanitize(p.modal.release_damp_s, 0.15f), 0.01f, 10.0f);
  for (float& level : p.additive.drawbars) level = std::clamp(sanitize(level, 0.0f), 0.0f, 8.0f);
  p.additive.key_click = std::clamp(sanitize(p.additive.key_click, 0.4f), 0.0f, 1.0f);
  p.additive.click_decay_ms = std::clamp(sanitize(p.additive.click_decay_ms, 6.0f), 0.5f, 100.0f);
  p.percussion.num_modes = std::clamp(p.percussion.num_modes, 0, kMaxPercussionModes);
  for (float& ratio : p.percussion.mode_ratios) {
    ratio = std::clamp(sanitize(ratio, 0.0f), 0.0f, 64.0f);
  }
  p.percussion.mode_decay_s = std::clamp(sanitize(p.percussion.mode_decay_s, 0.3f), 0.005f, 30.0f);
  p.percussion.tone_gain = std::clamp(sanitize(p.percussion.tone_gain, 1.0f), 0.0f, 4.0f);
  p.percussion.base_freq_hz = std::clamp(sanitize(p.percussion.base_freq_hz, 0.0f), 0.0f, 20000.0f);
  p.percussion.pitch_drop = std::clamp(sanitize(p.percussion.pitch_drop, 0.0f), 0.0f, 8.0f);
  p.percussion.pitch_drop_ms =
      std::clamp(sanitize(p.percussion.pitch_drop_ms, 40.0f), 1.0f, 2000.0f);
  p.percussion.noise_gain = std::clamp(sanitize(p.percussion.noise_gain, 0.0f), 0.0f, 4.0f);
  p.percussion.noise_decay_ms =
      std::clamp(sanitize(p.percussion.noise_decay_ms, 150.0f), 1.0f, 20000.0f);
  p.percussion.noise_cutoff_hz =
      std::clamp(sanitize(p.percussion.noise_cutoff_hz, 2500.0f), 20.0f, 20000.0f);
  p.percussion.noise_q = std::clamp(sanitize(p.percussion.noise_q, 1.0f), 0.5f, 30.0f);
  p.piano.strings = std::clamp(p.piano.strings, 1, kMaxPianoStrings);
  p.piano.detune_cents = std::clamp(sanitize(p.piano.detune_cents, 1.6f), 0.0f, 50.0f);
  p.piano.decay_fast_s = std::clamp(sanitize(p.piano.decay_fast_s, 3.0f), 0.05f, 60.0f);
  p.piano.decay_slow_s = std::clamp(sanitize(p.piano.decay_slow_s, 12.0f), 0.05f, 120.0f);
  p.piano.decay_stretch = std::clamp(sanitize(p.piano.decay_stretch, 0.7f), 0.0f, 1.0f);
  p.piano.brightness = std::clamp(sanitize(p.piano.brightness, 0.75f), 0.0f, 1.0f);
  p.piano.dispersion = std::clamp(sanitize(p.piano.dispersion, 1.0f), 0.0f, 1.0f);
  p.piano.strike_position = std::clamp(sanitize(p.piano.strike_position, 0.12f), 0.0f, 0.5f);
  p.piano.hammer_exponent = std::clamp(sanitize(p.piano.hammer_exponent, 2.5f), 1.5f, 4.0f);
  p.piano.hammer_contact_ms = std::clamp(sanitize(p.piano.hammer_contact_ms, 1.2f), 0.2f, 10.0f);
  p.piano.soundboard = std::clamp(sanitize(p.piano.soundboard, 0.25f), 0.0f, 1.0f);
  p.piano.release_damp_s = std::clamp(sanitize(p.piano.release_damp_s, 0.1f), 0.01f, 10.0f);
  return p;
}

// ---------------------------------------------------------------------------
// NativeSynthVoice
// ---------------------------------------------------------------------------

void NativeSynthVoice::start(const NativeSynthPatch& p, double sample_rate, uint8_t velocity,
                             uint32_t voice_index, float glide_from_hz) noexcept {
  patch = &p;
  key_down = true;
  releasing = false;

  VoiceRandomSequence seq;
  seq.reseed(voice_index, note, age);

  base_freq_hz = synth_note_to_hz(static_cast<float>(note & 0x7Fu) + p.pitch_offset_cents / 100.0f);

  const bool osc_less = p.mode != SynthEngineMode::kSubtractive;
  unison = osc_less ? 0 : std::clamp(p.unison, 1, kMaxUnisonOscs);
  osc_norm = unison > 0 ? 1.0f / std::sqrt(static_cast<float>(unison)) : 1.0f;
  if (p.mode == SynthEngineMode::kFm) fm.start(p.fm, sample_rate, note, velocity);
  if (p.mode == SynthEngineMode::kKarplusStrong) {
    ks.start(p.ks, sample_rate, note, velocity, voice_seed(voice_index, note, age));
  }
  if (p.mode == SynthEngineMode::kModal) {
    modal.start(p.modal, sample_rate, note, velocity, voice_seed(voice_index, note, age));
  }
  if (p.mode == SynthEngineMode::kAdditive) {
    additive.start(p.additive, sample_rate, note, velocity, voice_seed(voice_index, note, age));
  }
  if (p.mode == SynthEngineMode::kPercussion) {
    percussion.start(p.percussion, sample_rate, note, velocity, voice_seed(voice_index, note, age));
  }
  if (p.mode == SynthEngineMode::kPiano) {
    piano.start(p.piano, sample_rate, note, velocity, voice_seed(voice_index, note, age));
  }
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
  filter_bypass = p.filter_model == SynthFilterModel::kSvf &&
                  p.filter_output == SynthFilterOutput::kLowpass && p.cutoff_hz >= 18000.0f &&
                  p.env_to_cutoff_cents == 0.0f && static_cutoff_cents >= 0.0f &&
                  p.resonance_q <= 0.71f;
  if (p.drive > 0.0f) {
    // Gain-compensated tanh drive (same law as the Sf2 part insert).
    drive_gain = 1.0f + 9.0f * p.drive;
    drive_makeup = 1.0f / std::tanh(drive_gain);
  } else {
    drive_gain = 0.0f;
    drive_makeup = 1.0f;
  }

  amp_env.configure(sample_rate, p.amp_env);
  amp_env.kill();
  amp_env.note_on();
  filter_env.configure(sample_rate, p.filter_env);
  filter_env.kill();
  filter_env.note_on();
  filter.prepare(sample_rate);
  filter.set_model(p.filter_model);

  vibrato_lfo.start(sample_rate, 0.0f, p.lfo_rate_hz);
  lfo2.start(sample_rate, 0.0f, p.lfo2_rate_hz);
  // Per-voice drift: seeded depth (sign included) and a seeded rate offset so
  // stacked voices beat against each other instead of wobbling in unison.
  drift_depth_cents = p.drift_cents * seq.bipolar_at(101);
  drift_lfo.start(sample_rate, 0.0f, p.drift_rate_hz * (0.75f + 0.5f * seq.unipolar_at(102)));

  // Mod-matrix source constants.
  has_matrix = !p.mod_matrix.empty();
  velocity01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  key_track_octaves = (static_cast<float>(note & 0x7Fu) - 60.0f) / 12.0f;
  random_value = seq.bipolar_at(103);

  // Glide: start offset in cents from the previous note, decaying through a
  // one-pole sized so the pitch lands within ~5% in glide_ms.
  glide_cents = 0.0f;
  glide_coeff = 0.0f;
  if (p.glide_ms > 0.0f && glide_from_hz > 0.0f && base_freq_hz > 0.0f) {
    glide_cents = 1200.0f * std::log2(glide_from_hz / base_freq_hz);
    const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
    glide_coeff = static_cast<float>(std::exp(-3.0 / (p.glide_ms * 0.001 * sr)));
  }

  cached_pan_units = 1.0e9f;  // force pan recompute on first render
}

float NativeSynthVoice::render(const Sf2ChannelMod& mod) noexcept {
  if (!active || patch == nullptr) return 0.0f;

  // --- modulation sources ---
  const float level = amp_env.next();
  if (!amp_env.active()) {
    active = false;
    return 0.0f;
  }
  const float fenv = filter_env.next();
  const float lfo1_value = vibrato_lfo.next();
  const float drift = drift_lfo.next() * drift_depth_cents;

  // --- mod matrix ---
  ModOffsets offsets;
  if (has_matrix) {
    ModSourceValues values;
    values.amp_env = level;
    values.filter_env = fenv;
    values.lfo1 = lfo1_value;
    values.lfo2 = lfo2.next();
    values.velocity = velocity01;
    values.key_track = key_track_octaves;
    // Recover CC1 [0,1] from the shared channel snapshot's vibrato mapping.
    values.mod_wheel = mod.extra_vibrato_cents * (1.0f / kModWheelVibratoCents);
    values.random = random_value;
    offsets = evaluate_mod_matrix(patch->mod_matrix, values);
  }

  // Refresh the cached stereo pan gains when the effective pan changed.
  const float pan_units = mod.pan_units + offsets.pan_units;
  if (pan_units != cached_pan_units) {
    cached_pan_units = pan_units;
    const float angle = pan_angle(pan_units);
    gain_left = std::cos(angle);
    gain_right = std::sin(angle);
  }

  // --- glide: one-pole decay of the previous-note offset ---
  if (glide_coeff > 0.0f) {
    glide_cents *= glide_coeff;
    if (std::fabs(glide_cents) < 0.5f) {
      glide_cents = 0.0f;
      glide_coeff = 0.0f;
    }
  }

  // --- pitch: bend + vibrato (LFO1 + mod wheel) + drift + matrix + glide ---
  const float vib = lfo1_value * (patch->lfo_to_pitch_cents + mod.extra_vibrato_cents);
  const float pitch_cents = mod.pitch_cents + vib + drift + offsets.pitch_cents + glide_cents;
  const float common = pitch_cents != 0.0f ? std::exp2(pitch_cents * (1.0f / 1200.0f)) : 1.0f;

  float sample = 0.0f;
  if (patch->mode == SynthEngineMode::kFm) {
    sample = fm.render(common);
  } else if (patch->mode == SynthEngineMode::kKarplusStrong) {
    sample = ks.render(common);
  } else if (patch->mode == SynthEngineMode::kModal) {
    sample = modal.render(common);
  } else if (patch->mode == SynthEngineMode::kAdditive) {
    sample = additive.render(common);
  } else if (patch->mode == SynthEngineMode::kPercussion) {
    sample = percussion.render(common);
  } else if (patch->mode == SynthEngineMode::kPiano) {
    sample = piano.render(common);
  } else {
    for (int k = 0; k < unison; ++k) {
      auto& osc = oscs[static_cast<size_t>(k)];
      osc.set_frequency(base_freq_hz * common * detune_ratio[static_cast<size_t>(k)]);
      sample += osc.next();
    }
    sample *= osc_norm;
  }

  // --- pre-filter drive (gain-compensated tanh) ---
  if (drive_gain > 0.0f) sample = std::tanh(drive_gain * sample) * drive_makeup;

  // --- filter: cutoff = patch Fc * 2^((env + velocity + keytrack)/1200) ---
  if (!filter_bypass || offsets.cutoff_cents != 0.0f) {
    const float fc_cents =
        fenv * patch->env_to_cutoff_cents + static_cutoff_cents + offsets.cutoff_cents;
    const float fc = patch->cutoff_hz * std::exp2(fc_cents * (1.0f / 1200.0f));
    filter.set(fc, patch->resonance_q);
    sample = filter.process(sample, patch->filter_output);
  }

  // --- amplitude ---
  return sample * level * velocity_gain * patch->gain * mod.gain * offsets.amp_gain;
}

void NativeSynthVoice::release() noexcept {
  key_down = false;
  // One-shot (drum) voices ring out their zero-sustain decay regardless of
  // note-off; everything else enters the release stage.
  if (patch != nullptr && patch->one_shot) return;
  releasing = true;
  amp_env.note_off();
  filter_env.note_off();
  if (patch != nullptr && patch->mode == SynthEngineMode::kFm) fm.release();
  if (patch != nullptr && patch->mode == SynthEngineMode::kKarplusStrong) ks.release();
  if (patch != nullptr && patch->mode == SynthEngineMode::kModal) modal.release();
  if (patch != nullptr && patch->mode == SynthEngineMode::kPiano) piano.release();
}

void NativeSynthVoice::kill() noexcept {
  amp_env.kill();
  filter_env.kill();
  fm.kill();
  ks.kill();
  modal.kill();
  additive.kill();
  percussion.kill();
  piano.kill();
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
  // KS strings need a per-voice delay slab (the only allocation site; voices
  // attach their span at note-on).
  ks_capacity_ = ks_buffer_capacity(sample_rate_);
  if (config_.patch.mode == SynthEngineMode::kKarplusStrong) {
    ks_buffers_.assign(pool_.size() * static_cast<size_t>(ks_capacity_), 0.0f);
  } else {
    ks_buffers_.clear();
  }
  piano_string_capacity_ = piano_string_capacity(sample_rate_);
  if (config_.patch.mode == SynthEngineMode::kPiano) {
    piano_buffers_.assign(pool_.size() * static_cast<size_t>(piano_slab_capacity(sample_rate_)),
                          0.0f);
  } else {
    piano_buffers_.clear();
  }
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
  const uint8_t ch = channel & 0x0Fu;
  NativeSynthVoice* voice = pool_.allocate(ch, note);
  if (voice == nullptr) return;
  const uint32_t voice_index = static_cast<uint32_t>(voice - pool_.data());
  // KS patches get their delay span before start() (pointer wiring only).
  if (!ks_buffers_.empty()) {
    voice->ks.attach(ks_buffers_.data() + static_cast<size_t>(voice_index) * ks_capacity_,
                     ks_capacity_);
  }
  if (!piano_buffers_.empty()) {
    voice->piano.attach(piano_buffers_.data() + static_cast<size_t>(voice_index) *
                                                    kMaxPianoStrings * piano_string_capacity_,
                        piano_string_capacity_);
  }
  // Portamento: glide from the channel's previous note when enabled.
  const float glide_from = config_.patch.glide_ms > 0.0f ? channels_[ch].last_freq_hz : 0.0f;
  voice->start(config_.patch, sample_rate_, velocity, voice_index, glide_from);
  channels_[ch].last_freq_hz = voice->base_freq_hz;
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
