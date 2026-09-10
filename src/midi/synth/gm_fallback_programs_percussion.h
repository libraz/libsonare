#pragma once

#include "midi/synth/gm_fallback_data.h"

namespace sonare::midi::synth::detail {

constexpr void configure_percussion_programs(ProgramOverrides& o) noexcept {
  // Pitched percussion (GM 112-119): the membrane / struck-idiophone cores
  // voiced as melodic programs. Unlike the kit drum map these track the played
  // key (base_freq_hz = 0), are NOT one-shot (so note-off can cut a held note),
  // and pin a fixed noise-band cutoff (the drum lambdas derive it from the base
  // frequency, which is 0 here). A zero-sustain decay envelope gives the strike
  // shape without swallowing note-off, exactly like the timpani override.

  // Tinkle Bell (GM 112): a high glassy chime, and a long one. Its lines are
  // the played key and the key's fourth partial, both on the harmonic grid;
  // the 1.70 and 2.40 this patch used to voice are in neither of the two GM
  // sources measured. The fall is 60 dB in 1.48 s at every note AND every
  // velocity, which is a sampler's envelope rather than an instrument's, so
  // nothing here is key-tracked either.
  NativeSynthPatch& tk = o.tinkle_bell;
  tk.mode = SynthEngineMode::kPercussion;
  // Nothing damps a small bell, so the release is the fall's own length: below
  // that a short note is cut while the instrument still has a second to run.
  tk.amp_env = fallback_env(0.5f, 900.0f, 0.0f, 1500.0f);
  // Held at the strike while the modes fall alone, which is the knee: the
  // reference's first 20 dB take 715 ms and its last 20 take 295.
  tk.amp_env.hold_ms = 900.0f;
  tk.cutoff_hz = 20000.0f;
  tk.percussion.num_modes = 2;
  tk.percussion.mode_ratios = {1.0f, 4.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  tk.percussion.base_freq_hz = 0.0f;
  tk.percussion.mode_decay_s = 2.1f;
  tk.percussion.tone_gain = 0.6f;
  // The bell's inharmonic metal. The reference carries it as fixed-frequency
  // lines that move with the sample zone rather than the key, so a mode cannot
  // hold them; a band can. 68 dB under the note and 88 over the lead-in floor.
  tk.percussion.noise_gain = 0.003f;
  tk.percussion.noise_decay_ms = 900.0f;
  tk.percussion.noise_cutoff_hz = 2500.0f;
  tk.percussion.noise_output = SynthFilterOutput::kBandpass;
  tk.gain = 1.18f;

  // Agogo (GM 113): a two-tone metal bell, and the second program here that is
  // not a membrane. Both GM sources measured put its loudest line at twice the
  // played key with nothing at the key itself, and its ladder falls far steeper
  // than the membrane core's fixed 1/(k+1) — the second-loudest partial sits
  // 1.8 dB under the first and the next 17 under that. So it runs on the modal
  // core, whose modes carry their own gain and decay. Read at five notes over a
  // major sixth, which agreed to about a decibel; the reference is one sample
  // transposed, so those five say one thing.
  NativeSynthPatch& ag = o.agogo;
  ag.mode = SynthEngineMode::kModal;
  ag.amp_env = fallback_env(0.5f, 0.0f, 1.0f, 600.0f);
  // A lowpass fixed in Hz rather than tracked, because that is what the
  // reference has: its partials lose about 12 dB per octave of ABSOLUTE
  // frequency, so the same bell struck an octave up comes out 3.5 dB quieter
  // with its bright partial 13 dB down. One corner reproduces both.
  ag.cutoff_hz = 1800.0f;
  ag.modal.num_modes = 8;
  // Two modes on the sounded note, because its fall is two-stage and one
  // resonator is one exponential: a fast part carries the first 0.2 s at about
  // -120 dB/s and a slow one the rest at -49.
  ag.modal.modes[0] = {2.0f, 0.56f, 1.0f};  // the sounded note, an octave up
  ag.modal.modes[1] = {2.0f, 1.2f, 0.2f};
  ag.modal.modes[2] = {2.5f, 0.139f, 0.519f};
  ag.modal.modes[3] = {4.65f, 0.101f, 0.53f};
  ag.modal.modes[4] = {5.18f, 0.044f, 0.413f};
  ag.modal.modes[5] = {7.25f, 0.118f, 0.503f};
  ag.modal.modes[6] = {8.11f, 0.816f, 0.395f};  // the bell's own bright partial
  ag.modal.modes[7] = {14.04f, 0.159f, 0.423f};
  ag.modal.decay_s = 1.22f;
  // Flat: the reference's fall to -60 dB moves 40 ms over the whole compass,
  // and upward, so there is no bar-size stretch to track.
  ag.modal.decay_stretch = 0.0f;
  // The stick carries the reference's velocity tilt: from the softest row to
  // the hardest its partials rise 3 dB at the third mode and 38 at the eighth.
  ag.modal.strike_brightness = 1.0f;
  ag.modal.vel_to_brightness = 0.42f;
  ag.modal.release_damp_s = 1.22f;  // a struck bell is not damped by the key
  ag.gain = 0.62f;

  // Steel Drums (GM 114): a tuned pan, and the one program here that is not a
  // membrane. A pan note is tuned so its octave and twelfth are true and the
  // octave comes out LOUDER than the note — a reference reads it 1.1 to 2.2 dB
  // over the fundamental at every note, with the fourth partial level with the
  // fundamental too. The membrane core cannot say that: its mode weight is
  // 1/(k+1) in the mode index, fixed, so the second mode starts 8 dB down and
  // no strike position reaches past it. So this one runs on the modal core,
  // whose modes carry their own gain and decay. Measured at three notes a
  // tritone apart, which agreed to about a decibel.
  NativeSynthPatch& sd = o.steel_drums;
  sd.mode = SynthEngineMode::kModal;
  sd.amp_env = fallback_env(0.5f, 0.0f, 1.0f, 400.0f);
  sd.cutoff_hz = 20000.0f;
  sd.modal.num_modes = 8;
  sd.modal.modes[0] = {1.0f, 1.0f, 1.0f};
  sd.modal.modes[1] = {2.0f, 1.21f, 1.25f};   // the tuned octave, the loudest
  sd.modal.modes[2] = {3.01f, 0.19f, 1.06f};  // the tuned twelfth
  sd.modal.modes[3] = {3.08f, 0.14f, 0.89f};  // and its partner, 4 dB under it
  sd.modal.modes[4] = {4.01f, 1.24f, 1.04f};
  sd.modal.modes[5] = {5.03f, 0.05f, 1.28f};
  sd.modal.modes[6] = {6.02f, 0.36f, 0.88f};
  sd.modal.modes[7] = {8.02f, 0.27f, 0.74f};
  sd.modal.decay_s = 0.45f;
  sd.modal.decay_stretch = 0.73f;
  // The mallet carries the reference's own velocity tilt: its upper partials
  // rise 1 to 12 dB from the softest row to the loudest, steeper the higher the
  // mode, which is the curve's shape.
  sd.modal.strike_brightness = 0.969458f;
  sd.modal.vel_to_brightness = 0.164306f;
  sd.modal.release_damp_s = 0.45f;  // nothing damps a pan; it rings its own fall
  // The modal core radiates about 8 dB hotter than the membrane one did for the
  // same gain, so this is set from the reference's own peak rather than kept.
  sd.gain = 0.52f;
  sd.amp_env.attack_ms = 0.030005f;

  // Woodblock (GM 115): a single high-Q wood resonance with a short stick click.
  NativeSynthPatch& wb = o.woodblock;
  wb.mode = SynthEngineMode::kPercussion;
  wb.amp_env = fallback_env(0.3f, 100.0f, 0.0f, 40.0f);
  wb.cutoff_hz = 5582.96f;
  wb.percussion.num_modes = 1;
  wb.percussion.mode_ratios = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  wb.percussion.base_freq_hz = 0.0f;
  wb.percussion.mode_decay_s = 0.134876f;
  wb.percussion.tone_gain = 4.0f;
  wb.percussion.noise_gain = 1.37027f;
  wb.percussion.noise_decay_ms = 14.579f;
  wb.percussion.noise_cutoff_hz = 2904.06f;
  wb.percussion.noise_output = SynthFilterOutput::kBandpass;
  wb.gain = 0.6f;
  wb.amp_env.attack_ms = 0.039043f;
  wb.amp_env.decay_ms = 30.0073f;
  wb.amp_env.release_ms = 704.853f;
  wb.amp_env.sustain = 0.35353f;
  wb.percussion.mode_ratios[0] = 2.77762f;
  wb.percussion.noise_q = 1.52151f;
  wb.percussion.plate_hf_ratio = 0.278273f;
  wb.percussion.plate_low_hz = 798.233f;
  wb.percussion.plate_t60_s = 8.16114f;
  wb.percussion.tone_direct = 0.23688f;
  wb.percussion.wire_threshold = 2.58015f;
  wb.resonance_q = 3.36119f;

  // Taiko (GM 116): a large struck membrane — the full Rayleigh mode set, a
  // strong strike pitch drop and a low shell boom.
  NativeSynthPatch& ti = o.taiko;
  ti.mode = SynthEngineMode::kPercussion;
  ti.amp_env = fallback_env(0.5f, 700.0f, 0.0f, 200.0f);
  ti.cutoff_hz = 15123.5f;
  ti.percussion.num_modes = 5;
  ti.percussion.base_freq_hz = 0.0f;
  ti.percussion.mode_decay_s = 1.55947f;
  ti.percussion.tone_gain = 3.63643f;
  ti.percussion.pitch_drop = 0.134927f;
  ti.percussion.pitch_drop_ms = 29.931f;
  ti.percussion.noise_gain = 2.93126f;
  ti.percussion.noise_decay_ms = 1.94051f;
  ti.percussion.noise_cutoff_hz = 161.898f;
  ti.percussion.noise_output = SynthFilterOutput::kLowpass;
  ti.percussion.strike_r = 0.559041f;
  ti.percussion.shell_mix = 0.806885f;
  ti.percussion.shell_num_modes = 1;
  ti.percussion.shell_freq_hz = {90.0f, 0.0f, 0.0f, 0.0f};
  ti.percussion.shell_t60_s = {0.14f, 0.0f, 0.0f, 0.0f};
  ti.percussion.shell_weight = {1.0f, 0.0f, 0.0f, 0.0f};
  ti.gain = 1.1f;
  ti.amp_env.attack_ms = 0.0415506f;
  ti.amp_env.decay_ms = 687.96f;
  ti.amp_env.release_ms = 11471.0f;
  ti.amp_env.sustain = 0.26644f;
  ti.percussion.contact = 0.0f;
  ti.percussion.contact_ms = 0.761066f;
  ti.percussion.mode_ratios[0] = 1.1535f;
  ti.percussion.mode_ratios[1] = 0.624269f;
  ti.percussion.noise_q = 0.619676f;
  ti.percussion.plate_hf_ratio = 0.0336278f;
  ti.percussion.plate_low_hz = 623.604f;
  ti.percussion.plate_t60_s = 0.234847f;
  ti.percussion.shell_t60_s[0] = 0.111498f;
  ti.percussion.shell_weight[0] = 2.42255f;
  ti.percussion.tone_direct = 0.692916f;
  ti.percussion.wire_threshold = 2.85629f;
  ti.resonance_q = 1.09065f;

  // Melodic Tom (GM 117): a pitched tom — note-tracked membrane with a pitch
  // drop and a shell body, one patch for every tom size.
  NativeSynthPatch& mt = o.melodic_tom;
  mt.mode = SynthEngineMode::kPercussion;
  mt.amp_env = fallback_env(0.5f, 500.0f, 0.0f, 150.0f);
  mt.cutoff_hz = 20000.0f;
  mt.percussion.num_modes = 5;
  mt.percussion.base_freq_hz = 0.0f;
  mt.percussion.mode_decay_s = 0.3f;
  mt.percussion.tone_gain = 0.9f;
  mt.percussion.pitch_drop = 0.6f;
  mt.percussion.pitch_drop_ms = 55.0f;
  mt.percussion.noise_gain = 0.25f;
  mt.percussion.noise_decay_ms = 30.0f;
  mt.percussion.noise_cutoff_hz = 1500.0f;
  mt.percussion.noise_output = SynthFilterOutput::kLowpass;
  mt.percussion.strike_r = 0.6f;
  mt.percussion.shell_mix = 0.25f;
  mt.percussion.shell_num_modes = 2;
  mt.percussion.shell_freq_hz = {0.0f, 330.0f, 0.0f, 0.0f};
  mt.percussion.shell_t60_s = {0.12f, 0.06f, 0.0f, 0.0f};
  mt.percussion.shell_weight = {1.0f, 0.4f, 0.0f, 0.0f};
  mt.gain = 1.0f;

  // Synth Drum (GM 118): a synthetic decaying-sine tom (the TR-808 recipe) —
  // one membrane mode, a strong pitch drop, little noise.
  NativeSynthPatch& sy = o.synth_drum;
  sy.mode = SynthEngineMode::kPercussion;
  sy.amp_env = fallback_env(0.5f, 500.0f, 0.0f, 150.0f);
  sy.cutoff_hz = 19737.0f;
  sy.percussion.num_modes = 3;
  sy.percussion.mode_ratios = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  sy.percussion.base_freq_hz = 0.0f;
  sy.percussion.mode_decay_s = 0.327662f;
  sy.percussion.tone_gain = 2.1383f;
  sy.percussion.pitch_drop = 0.125f;
  sy.percussion.pitch_drop_ms = 158.244f;
  sy.percussion.noise_gain = 2.45124f;
  sy.percussion.noise_decay_ms = 19.0869f;
  sy.percussion.noise_cutoff_hz = 1598.29f;
  sy.percussion.noise_output = SynthFilterOutput::kLowpass;
  sy.gain = 1.0f;
  sy.amp_env.attack_ms = 0.359235f;
  sy.amp_env.decay_ms = 354.08f;
  sy.amp_env.release_ms = 125.499f;
  sy.percussion.mode_ratios[0] = 0.942332f;
  sy.percussion.noise_q = 0.704471f;
  sy.resonance_q = 3.41885f;

  // Reverse Cymbal (GM 119): the core has no reverse playback, so the swell is
  // approximated with a long attack (the wash rises over the held note) into a
  // short release (it cuts at the top on note-off). The noise band must decay
  // slower than the attack rises or the wash dies before it peaks.
  NativeSynthPatch& rc = o.reverse_cymbal;
  rc.mode = SynthEngineMode::kPercussion;
  rc.amp_env = fallback_env(1400.0f, 0.0f, 1.0f, 60.0f);  // long swell, short cut
  rc.cutoff_hz = 20000.0f;
  rc.percussion.num_modes = 2;
  rc.percussion.mode_ratios = {1.0f, 1.34f, 1.72f, 2.15f, 0.0f, 0.0f};
  rc.percussion.base_freq_hz = 3600.0f;  // unpitched crash body
  rc.percussion.mode_decay_s = 2.33456f;
  rc.percussion.tone_gain = 1.56376f;
  rc.percussion.noise_gain = 3.57332f;
  rc.percussion.noise_decay_ms = 13114.9f;  // outlasts the attack swell
  rc.percussion.noise_cutoff_hz = 2004.84f;
  rc.percussion.noise_output = SynthFilterOutput::kHighpass;
  rc.percussion.shimmer = 14.0612f;
  rc.percussion.shimmer_attack_ms = 172.315f;
  rc.percussion.shimmer_cutoff_hz = 530.586f;
  rc.gain = 0.5f;
  rc.amp_env.attack_ms = 420.044f;
  rc.amp_env.release_ms = 630.421f;
  rc.percussion.contact_ms = 0.0507509f;
  rc.percussion.mode_ratios[0] = 1.1172f;
  rc.percussion.mode_ratios[1] = 44.9033f;
  rc.percussion.mode_ratios[2] = 1.25982f;
  rc.percussion.mode_ratios[3] = 41.3911f;
  rc.percussion.noise_q = 0.628894f;
  rc.percussion.plate_hf_ratio = 0.472183f;
  rc.percussion.plate_low_hz = 153.571f;
  rc.percussion.plate_t60_s = 0.168682f;
  rc.percussion.shell_t60_s[0] = 0.0275134f;
  rc.percussion.shell_t60_s[2] = 0.008628f;
  rc.percussion.shell_t60_s[3] = 0.00686487f;
  rc.percussion.shell_weight[2] = 0.781797f;
  rc.percussion.shell_weight[3] = 1.52081f;
  rc.percussion.tone_direct = 0.854284f;
  rc.percussion.wire_threshold = 0.0268937f;
  rc.resonance_q = 0.765824f;
}

}  // namespace sonare::midi::synth::detail
