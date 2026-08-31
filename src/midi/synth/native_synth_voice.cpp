#include <algorithm>
#include <cmath>

#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/voice_random.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

namespace {

/// Exponent of the SoundFont velocity-to-amplitude curve, on the kit.
///
/// The curve is `(v/127)^2` and every engine but the two below is voiced with
/// it in place, so it is the exponent rather than the curve that a measured
/// engine adjusts. The spec's 2.0 swung the kit 10.2 dB wider from velocity 48
/// to 127 than one reference kit and 4.5 dB wider than the other; the exponent
/// each asks for on its own is 0.80 and 1.46, and this is the midpoint.
SONARE_TUNABLE(kPercussionVelocityExponent, 1.1f);

/// Exponent of the same curve on the tonewheel generator.
///
/// A key closes contacts on wheels that are already turning, so the instrument
/// has no velocity response at all; a reference module gives it a little
/// anyway, and that is what this is fitted to. Two registrations swing 4.0 and
/// 4.4 dB from velocity 48 to 127 where the spec's 2.0 swings 16.9, and the
/// exponent each asks for is 0.47 and 0.52.
SONARE_TUNABLE(kAdditiveVelocityExponent, 0.5f);

/// Exponent of the SoundFont velocity-to-amplitude curve for @p mode, 0 = off.
///
/// `sf2_velocity_gain` exists because a sampled note was recorded at one force
/// and has to be scaled to the force actually played. A physical engine is
/// handed the velocity as an input to the strike and already radiates the
/// level difference that follows from it, so applying the curve on top spends
/// the dynamic range twice. Measured against a concert-grand corpus, the piano
/// covered 53 dB between velocity 24 and 120 where the reference covers 23 --
/// which leaves a pianissimo some 30 dB below where it belongs, quiet enough
/// to vanish under anything else in the mix.
///
/// The harpsichord opts out for the same reason and more sharply. Its mechanism
/// gives the key almost no control over loudness at all — a captured reference
/// covers 6 dB from the softest key to the hardest, and the literature puts the
/// instrument between 3 and 6 — while the curve alone spans 28 dB between
/// velocity 24 and 120. Applied on top of a plectrum model it does not merely
/// double the dynamic, it replaces the one thing that identifies the instrument.
///
/// The kit is the case where neither end is right: with the curve the model
/// swings 19.5 dB from velocity 48 to 127 where two reference kits swing 5.8 to
/// 13.6, and without it the percussion engine's own response is under 3 dB,
/// which is less than either. So it takes an exponent instead of a switch. The
/// tonewheel organ is the same shape from the other side: the instrument has no
/// velocity response to spend, so the curve is not doubling a dynamic but
/// inventing one.
///
/// An engine opts out only once it has been measured against a reference corpus.
/// Every other physical engine was voiced with the curve in place and would
/// shift underneath its own calibration if this changed for it too.
float sampler_velocity_exponent(SynthEngineMode mode) noexcept {
  if (mode == SynthEngineMode::kPiano || mode == SynthEngineMode::kHarpsichord) return 0.0f;
  if (mode == SynthEngineMode::kPercussion) return kPercussionVelocityExponent;
  if (mode == SynthEngineMode::kAdditive) return kAdditiveVelocityExponent;
  return 2.0f;
}

/// The curve at @p exponent. The spec's exponent keeps the spec's own squaring
/// rather than reaching `pow` for it, so a build that never moves the knob is
/// bit-identical to one compiled before it existed.
float sampler_velocity_gain(uint8_t velocity, float exponent) noexcept {
  if (exponent <= 0.0f) return 1.0f;
  if (exponent == 2.0f) return sf2_velocity_gain(velocity);
  return std::pow(static_cast<float>(velocity & 0x7Fu) / 127.0f, exponent);
}

}  // namespace

// ---------------------------------------------------------------------------
// NativeSynthVoice
// ---------------------------------------------------------------------------

void NativeSynthVoice::start(const NativeSynthPatch& p, double sample_rate, uint8_t velocity,
                             uint32_t voice_index, float glide_from_hz, bool una_corda,
                             uint8_t drum_kit, DrumVoiceMod drum_mod, bool organ_percussion,
                             GsPartMod part_mod) noexcept {
  patch = &p;
  key_down = true;
  releasing = false;
  sostenuto = false;
  // GS per-note drum edits: pitch coarse, absolute pan and the three send
  // multiplicands carry into render; the TVA level folds into velocity_gain
  // below. Defaults are no-ops.
  drum_pitch_ratio = drum_mod.pitch_ratio;
  drum_pan_units = drum_mod.pan_units;
  drum_reverb_scale = drum_mod.reverb_scale;
  drum_chorus_scale = drum_mod.chorus_scale;
  drum_delay_scale = drum_mod.delay_scale;
  // GS melodic part edits: the two the render reads, plus the flag that engages
  // the filter stage. The other six fold into the envelope, the cutoff offset
  // and the vibrato LFO below.
  gs_resonance_gain = part_mod.resonance_gain;
  gs_vib_depth_cents = part_mod.vib_depth_cents;
  gs_filter_edited = part_mod.filter_edited;
  // SCALE TUNING joins the render's pitch sum rather than base_freq_hz: every
  // engine but the subtractive one is started from the note NUMBER, so a
  // fractional offset folded into the frequency would reach one of thirteen.
  gs_scale_cents = part_mod.pitch_cents;

  // A GS kit variation may retune the resolved drum patch's percussion + amp
  // envelope at note-on and scale its level (Standard patch stays shared; no
  // per-kit table). kit_gain folds into velocity_gain, amp_cfg into the VCA.
  DahdsrConfig amp_cfg = p.amp_env;
  float kit_gain = 1.0f;

  // Everything below voices the note rather than tracking the key, so a GS note
  // substitution — PLAY NOTE NUMBER or a user drum set's source — reaches all of
  // it; `note` stays the struck one, which is what a note-off matches.
  const uint8_t voiced_note =
      drum_mod.play_note >= 0 ? static_cast<uint8_t>(drum_mod.play_note) : note;
  exclusive_class = drum_mod.exclusive_class >= 0 ? static_cast<uint8_t>(drum_mod.exclusive_class)
                                                  : p.percussion.exclusive_class;

  VoiceRandomSequence seq;
  seq.reseed(voice_index, voiced_note, age);

  base_freq_hz =
      synth_note_to_hz(static_cast<float>(voiced_note & 0x7Fu) + p.pitch_offset_cents / 100.0f);

  const bool osc_less = p.mode != SynthEngineMode::kSubtractive;
  unison = osc_less ? 0 : std::clamp(p.unison, 1, kMaxUnisonOscs);
  osc_norm = unison > 0 ? 1.0f / std::sqrt(static_cast<float>(unison)) : 1.0f;
  if (p.mode == SynthEngineMode::kFm) fm.start(p.fm, sample_rate, voiced_note, velocity);
  if (p.mode == SynthEngineMode::kKarplusStrong) {
    ks.start(p.ks, sample_rate, voiced_note, velocity, voice_seed(voice_index, voiced_note, age));
  }
  if (p.mode == SynthEngineMode::kModal) {
    modal.start(p.modal, sample_rate, voiced_note, velocity,
                voice_seed(voice_index, voiced_note, age));
  }
  if (p.mode == SynthEngineMode::kAdditive) {
    additive.start(p.additive, sample_rate, voiced_note, velocity,
                   voice_seed(voice_index, voiced_note, age), organ_percussion);
  }
  if (p.mode == SynthEngineMode::kPercussion) {
    PercussionPatchParams kit_perc = p.percussion;
    if (drum_kit != 0) kit_gain = apply_gs_drum_kit(kit_perc, amp_cfg, drum_kit, voiced_note);
    percussion.start(kit_perc, sample_rate, voiced_note, velocity,
                     voice_seed(voice_index, voiced_note, age));
  }
  if (p.mode == SynthEngineMode::kPiano) {
    piano.start(p.piano, sample_rate, voiced_note, velocity,
                voice_seed(voice_index, voiced_note, age), una_corda);
  }
  if (p.mode == SynthEngineMode::kPipeOrgan) {
    pipe_organ.start(p.pipe_organ, sample_rate, voiced_note, velocity,
                     voice_seed(voice_index, voiced_note, age));
  }
  if (p.mode == SynthEngineMode::kBowedString) {
    bowed_string.start(p.bowed_string, sample_rate, voiced_note, velocity,
                       voice_seed(voice_index, voiced_note, age));
  }
  if (p.mode == SynthEngineMode::kReed) {
    reed.start(p.reed, sample_rate, voiced_note, velocity,
               voice_seed(voice_index, voiced_note, age));
  }
  if (p.mode == SynthEngineMode::kBrass) {
    brass.start(p.brass, sample_rate, voiced_note, velocity,
                voice_seed(voice_index, voiced_note, age));
  }
  if (p.mode == SynthEngineMode::kFlute) {
    flute.start(p.flute, sample_rate, voiced_note, velocity,
                voice_seed(voice_index, voiced_note, age));
  }
  if (p.mode == SynthEngineMode::kPluckedString) {
    plucked_string.start(p.plucked_string, sample_rate, voiced_note, velocity,
                         voice_seed(voice_index, voiced_note, age));
  }
  if (p.mode == SynthEngineMode::kVocal) {
    vocal.start(p.vocal, sample_rate, voiced_note, velocity,
                voice_seed(voice_index, voiced_note, age));
  }
  if (p.mode == SynthEngineMode::kFreeReed) {
    free_reed.start(p.free_reed, sample_rate, voiced_note, velocity,
                    voice_seed(voice_index, voiced_note, age));
  }
  if (p.mode == SynthEngineMode::kHarpsichord) {
    harpsichord.start(p.harpsichord, sample_rate, voiced_note, velocity,
                      voice_seed(voice_index, voiced_note, age));
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
                                       voice_seed(voice_index, voiced_note, age) ^ (k + 1));
  }

  const float sampler_vel = sampler_velocity_gain(velocity, sampler_velocity_exponent(p.mode));
  velocity_gain = sampler_vel * kit_gain * drum_mod.level_gain;
  static_cutoff_cents =
      p.vel_to_cutoff_cents * (static_cast<float>(velocity & 0x7Fu) / 127.0f - 1.0f) +
      p.key_track * 100.0f * (static_cast<float>(voiced_note & 0x7Fu) - 60.0f) +
      part_mod.cutoff_cents;
  if (p.drive > 0.0f) {
    // Gain-compensated tanh drive (same law as the Sf2 part insert).
    drive_gain = 1.0f + 9.0f * p.drive;
    drive_makeup = 1.0f / std::tanh(drive_gain);
  } else {
    drive_gain = 0.0f;
    drive_makeup = 1.0f;
  }

  // After any kit variation, so a GS part edit scales the envelope the kit
  // actually gave this note rather than the patch's own.
  amp_cfg.attack_ms *= part_mod.attack_scale;
  amp_cfg.decay_ms *= part_mod.decay_scale;
  amp_cfg.release_ms *= part_mod.release_scale;
  amp_env.configure(sample_rate, amp_cfg);
  amp_env.note_on();
  filter_env.configure(sample_rate, p.filter_env);
  filter_env.note_on();
  filter.prepare(sample_rate);
  filter.set_model(p.filter_model);

  // The model bank's LFO has no onset delay of its own, so a GS vibrato-delay
  // edit is the only thing that can give it one.
  vibrato_lfo.start(sample_rate, gs_vib_delay_seconds(0.0f, part_mod.vib_delay_scale),
                    p.lfo_rate_hz * part_mod.vib_rate_scale);
  lfo2.start(sample_rate, 0.0f, p.lfo2_rate_hz);
  // Per-voice drift: seeded depth (sign included) and a seeded rate offset so
  // stacked voices beat against each other instead of wobbling in unison.
  drift_depth_cents = p.drift_cents * seq.bipolar_at(101);
  drift_lfo.start(sample_rate, 0.0f, p.drift_rate_hz * (0.75f + 0.5f * seq.unipolar_at(102)));

  // Mod-matrix source constants.
  has_matrix = !p.mod_matrix.empty();
  velocity01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;
  key_track_octaves = (static_cast<float>(voiced_note & 0x7Fu) - 60.0f) / 12.0f;
  random_value = seq.bipolar_at(103);

  // Body/formant resonance + seeded stereo scatter (realism polish).
  body.start(p.body, sample_rate, base_freq_hz, p.body_mix);
  pan_spread_units = 500.0f * p.stereo_spread * seq.bipolar_at(104);

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

float NativeSynthVoice::render(const Sf2ChannelMod& mod, float wind_pitch,
                               float wind_gain) noexcept {
  if (!active || patch == nullptr) return 0.0f;

  // --- modulation sources ---
  const float level = amp_env.next();
  if (!amp_env.active()) {
    active = false;
    return 0.0f;
  }
  const float fenv = filter_env.next();
  const float lfo1_value = vibrato_lfo.next(mod.vib_rate_scale);
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
    values.mod_wheel = mod.mod_wheel01;
    values.random = random_value;
    offsets = evaluate_mod_matrix(patch->mod_matrix, values);
  }

  // Refresh the cached stereo pan gains when the effective pan changed. A GS
  // per-note drum pan overrides the channel pan (absolute); otherwise the
  // channel pan stands.
  const float base_pan = drum_pan_units < 1.0e8f ? drum_pan_units : mod.pan_units;
  const float pan_units = base_pan + offsets.pan_units + pan_spread_units;
  if (pan_units != cached_pan_units) {
    cached_pan_units = pan_units;
    const rt::PanGains gains = voice_pan_gains(pan_units);
    gain_left = gains.left;
    gain_right = gains.right;
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
  // The GS depth edit is signed and the patch depth is an amount, so the sum is
  // floored — but only when there is an edit, so an untouched voice keeps
  // whatever the patch asked for.
  float vib_depth = patch->lfo_to_pitch_cents;
  if (gs_vib_depth_cents != 0.0f) vib_depth = std::max(0.0f, vib_depth + gs_vib_depth_cents);
  const float vib = lfo1_value * (vib_depth + mod.extra_vibrato_cents);
  const float mode_pitch_offset =
      patch->mode == SynthEngineMode::kSubtractive ? 0.0f : patch->pitch_offset_cents;
  const float pitch_cents = mode_pitch_offset + mod.pitch_cents + gs_scale_cents + vib + drift +
                            offsets.pitch_cents + glide_cents;
  float common = pitch_cents != 0.0f ? std::exp2(pitch_cents * (1.0f / 1200.0f)) : 1.0f;
  // Shared organ wind: the tremulant / wind-sag pitch factor (1.0 for every
  // non-pipe voice, which the host always passes through).
  common *= wind_pitch;
  // GS per-note drum pitch coarse (1.0 = untouched).
  common *= drum_pitch_ratio;

  float sample = 0.0f;
  // Percussion only; summed past the envelope at the foot of this function.
  float contact = 0.0f;
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
    contact = percussion.next_contact();
  } else if (patch->mode == SynthEngineMode::kPiano) {
    sample = piano.render(common);
  } else if (patch->mode == SynthEngineMode::kPipeOrgan) {
    sample = pipe_organ.render(common);
  } else if (patch->mode == SynthEngineMode::kBowedString) {
    sample = bowed_string.render(common);
  } else if (patch->mode == SynthEngineMode::kReed) {
    sample = reed.render(common);
  } else if (patch->mode == SynthEngineMode::kBrass) {
    sample = brass.render(common);
  } else if (patch->mode == SynthEngineMode::kFlute) {
    sample = flute.render(common);
  } else if (patch->mode == SynthEngineMode::kPluckedString) {
    sample = plucked_string.render(common);
  } else if (patch->mode == SynthEngineMode::kVocal) {
    sample = vocal.render(common);
  } else if (patch->mode == SynthEngineMode::kFreeReed) {
    sample = free_reed.render(common);
  } else if (patch->mode == SynthEngineMode::kHarpsichord) {
    sample = harpsichord.render(common);
  } else {
    for (int k = 0; k < unison; ++k) {
      auto& osc = oscs[static_cast<size_t>(k)];
      osc.set_frequency(base_freq_hz * common * detune_ratio[static_cast<size_t>(k)]);
      sample += osc.next();
    }
    sample *= osc_norm;
  }

  // --- body/formant resonance (after the string/source, before the amp) ---
  if (body.active()) sample = body.process(sample);

  // --- pre-filter drive (gain-compensated tanh) ---
  if (drive_gain > 0.0f) sample = std::tanh(drive_gain * sample) * drive_makeup;

  // --- filter: cutoff = patch Fc * 2^((env + velocity + keytrack)/1200) ---
  if (!filter_inaudible() || offsets.cutoff_cents != 0.0f) {
    const float fc_cents = fenv * patch->env_to_cutoff_cents + static_cutoff_cents +
                           offsets.cutoff_cents + mod.mod_cutoff_cents +
                           lfo1_value * mod.lfo_cutoff_cents;
    const float fc = patch->cutoff_hz * std::exp2(fc_cents * (1.0f / 1200.0f));
    float q = patch->resonance_q;
    if (gs_resonance_gain != 1.0f) q = std::max(0.5f, q * gs_resonance_gain);
    filter.set(fc, q);
    sample = filter.process(sample, patch->filter_output);
  }

  // --- amplitude (wind_gain is the shared tremulant/sag level; 1.0 otherwise) ---
  // The contact transient joins here rather than upstream: it is the strike
  // radiating directly, so none of the drive, the filter or the amplitude
  // envelope applies to it, and each of the three removes it outright — the
  // drive's tanh is already saturated by the layers it shapes, and a patch with
  // an envelope pre-delay gates the whole pulse away before it starts.
  // The LFO's peak is the part's own level and the depth is spent below it.
  const float tremolo =
      mod.tremolo_depth01 != 0.0f ? 1.0f - mod.tremolo_depth01 * (1.0f - lfo1_value) * 0.5f : 1.0f;
  return (sample * level + contact) * velocity_gain * patch->gain * mod.gain * offsets.amp_gain *
         wind_gain * tremolo;
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
  if (patch != nullptr && patch->mode == SynthEngineMode::kPipeOrgan) pipe_organ.release();
  if (patch != nullptr && patch->mode == SynthEngineMode::kBowedString) bowed_string.release();
  if (patch != nullptr && patch->mode == SynthEngineMode::kReed) reed.release();
  if (patch != nullptr && patch->mode == SynthEngineMode::kBrass) brass.release();
  if (patch != nullptr && patch->mode == SynthEngineMode::kFlute) flute.release();
  if (patch != nullptr && patch->mode == SynthEngineMode::kPluckedString) plucked_string.release();
  if (patch != nullptr && patch->mode == SynthEngineMode::kVocal) vocal.release();
  if (patch != nullptr && patch->mode == SynthEngineMode::kFreeReed) free_reed.release();
  if (patch != nullptr && patch->mode == SynthEngineMode::kHarpsichord) harpsichord.release();
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
  pipe_organ.kill();
  bowed_string.kill();
  reed.kill();
  brass.kill();
  flute.kill();
  plucked_string.kill();
  vocal.kill();
  free_reed.kill();
  harpsichord.kill();
  active = false;
  releasing = false;
}

void NativeSynthVoice::choke_fast(double sample_rate) noexcept {
  if (patch != nullptr) {
    // configure() recomputes stage rates and touches neither the level nor the
    // stage, so the fade starts from wherever the envelope already is.
    DahdsrConfig fast = patch->amp_env;
    fast.release_ms = kChokeReleaseMs;
    amp_env.configure(sample_rate, fast);
  }
  choke();
}

void NativeSynthVoice::choke() noexcept {
  // Force the amp envelope into its release stage even for one-shot (drum)
  // voices, which otherwise ignore note-off. Same-group strikes use this to cut
  // a ringing voice with a short fade rather than an abrupt kill.
  key_down = false;
  releasing = true;
  amp_env.note_off();
}

}  // namespace sonare::midi::synth
