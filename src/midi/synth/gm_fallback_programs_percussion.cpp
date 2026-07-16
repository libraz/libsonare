#include "midi/synth/gm_fallback_data.h"

namespace sonare::midi::synth::detail {

void configure_percussion_programs(ProgramOverrides& o) noexcept {
  // Pitched percussion (GM 112-119): the membrane / struck-idiophone cores
  // voiced as melodic programs. Unlike the kit drum map these track the played
  // key (base_freq_hz = 0), are NOT one-shot (so note-off can cut a held note),
  // and pin a fixed noise-band cutoff (the drum lambdas derive it from the base
  // frequency, which is 0 here). A zero-sustain decay envelope gives the strike
  // shape without swallowing note-off, exactly like the timpani override.

  // Tinkle Bell (GM 112): a high glassy chime — sparse inharmonic metal modes.
  NativeSynthPatch& tk = o.tinkle_bell;
  tk.mode = SynthEngineMode::kPercussion;
  tk.amp_env = fallback_env(0.5f, 500.0f, 0.0f, 300.0f);
  tk.cutoff_hz = 20000.0f;
  tk.percussion.num_modes = 3;
  tk.percussion.mode_ratios = {1.0f, 1.7f, 2.4f, 0.0f, 0.0f, 0.0f};
  tk.percussion.base_freq_hz = 0.0f;
  tk.percussion.mode_decay_s = 0.4f;
  tk.percussion.tone_gain = 0.6f;
  tk.percussion.noise_gain = 0.12f;
  tk.percussion.noise_decay_ms = 8.0f;
  tk.percussion.noise_cutoff_hz = 6000.0f;
  tk.percussion.noise_output = SynthFilterOutput::kBandpass;
  tk.gain = 0.6f;

  // Agogo (GM 113): a two-tone metal bell.
  NativeSynthPatch& ag = o.agogo;
  ag = tk;
  ag.percussion.num_modes = 2;
  ag.percussion.mode_ratios = {1.0f, 2.7f, 0.0f, 0.0f, 0.0f, 0.0f};
  ag.percussion.mode_decay_s = 0.28f;
  ag.percussion.noise_cutoff_hz = 3600.0f;
  ag.gain = 0.55f;

  // Steel Drums (GM 114): a tuned steel pan — near-harmonic modes with a small
  // strike pitch drop for the "pan" attack, rung a little longer and pushed
  // forward as a melodic lead.
  NativeSynthPatch& sd = o.steel_drums;
  sd.mode = SynthEngineMode::kPercussion;
  sd.amp_env = fallback_env(0.5f, 900.0f, 0.0f, 350.0f);
  sd.cutoff_hz = 20000.0f;
  sd.percussion.num_modes = 4;
  sd.percussion.mode_ratios = {1.0f, 2.0f, 3.0f, 4.0f, 0.0f, 0.0f};
  sd.percussion.base_freq_hz = 0.0f;
  sd.percussion.mode_decay_s = 0.9f;
  sd.percussion.tone_gain = 0.7f;
  sd.percussion.pitch_drop = 0.05f;
  sd.percussion.pitch_drop_ms = 30.0f;
  sd.percussion.noise_gain = 0.15f;
  sd.percussion.noise_decay_ms = 10.0f;
  sd.percussion.noise_cutoff_hz = 4000.0f;
  sd.percussion.noise_output = SynthFilterOutput::kBandpass;
  sd.gain = 0.7f;

  // Woodblock (GM 115): a single high-Q wood resonance with a short stick click.
  NativeSynthPatch& wb = o.woodblock;
  wb.mode = SynthEngineMode::kPercussion;
  wb.amp_env = fallback_env(0.3f, 100.0f, 0.0f, 40.0f);
  wb.cutoff_hz = 20000.0f;
  wb.percussion.num_modes = 1;
  wb.percussion.mode_ratios = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  wb.percussion.base_freq_hz = 0.0f;
  wb.percussion.mode_decay_s = 0.06f;
  wb.percussion.tone_gain = 0.9f;
  wb.percussion.noise_gain = 0.3f;
  wb.percussion.noise_decay_ms = 4.0f;
  wb.percussion.noise_cutoff_hz = 2400.0f;
  wb.percussion.noise_output = SynthFilterOutput::kBandpass;
  wb.gain = 0.6f;

  // Taiko (GM 116): a large struck membrane — the full Rayleigh mode set, a
  // strong strike pitch drop and a low shell boom.
  NativeSynthPatch& ti = o.taiko;
  ti.mode = SynthEngineMode::kPercussion;
  ti.amp_env = fallback_env(0.5f, 700.0f, 0.0f, 200.0f);
  ti.cutoff_hz = 20000.0f;
  ti.percussion.num_modes = 5;
  ti.percussion.base_freq_hz = 0.0f;
  ti.percussion.mode_decay_s = 0.5f;
  ti.percussion.tone_gain = 0.9f;
  ti.percussion.pitch_drop = 0.5f;
  ti.percussion.pitch_drop_ms = 45.0f;
  ti.percussion.noise_gain = 0.2f;
  ti.percussion.noise_decay_ms = 20.0f;
  ti.percussion.noise_cutoff_hz = 1200.0f;
  ti.percussion.noise_output = SynthFilterOutput::kLowpass;
  ti.percussion.strike_r = 0.4f;
  ti.percussion.shell_mix = 0.2f;
  ti.percussion.shell_num_modes = 1;
  ti.percussion.shell_freq_hz = {90.0f, 0.0f, 0.0f, 0.0f};
  ti.percussion.shell_t60_s = {0.14f, 0.0f, 0.0f, 0.0f};
  ti.percussion.shell_weight = {1.0f, 0.0f, 0.0f, 0.0f};
  ti.gain = 1.1f;

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
  sy.cutoff_hz = 20000.0f;
  sy.percussion.num_modes = 1;
  sy.percussion.mode_ratios = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  sy.percussion.base_freq_hz = 0.0f;
  sy.percussion.mode_decay_s = 0.4f;
  sy.percussion.tone_gain = 1.0f;
  sy.percussion.pitch_drop = 1.0f;
  sy.percussion.pitch_drop_ms = 60.0f;
  sy.percussion.noise_gain = 0.1f;
  sy.percussion.noise_decay_ms = 20.0f;
  sy.percussion.noise_cutoff_hz = 1500.0f;
  sy.percussion.noise_output = SynthFilterOutput::kLowpass;
  sy.gain = 1.0f;

  // Reverse Cymbal (GM 119): the core has no reverse playback, so the swell is
  // approximated with a long attack (the wash rises over the held note) into a
  // short release (it cuts at the top on note-off). The noise band must decay
  // slower than the attack rises or the wash dies before it peaks.
  NativeSynthPatch& rc = o.reverse_cymbal;
  rc.mode = SynthEngineMode::kPercussion;
  rc.amp_env = fallback_env(1400.0f, 0.0f, 1.0f, 60.0f);  // long swell, short cut
  rc.cutoff_hz = 20000.0f;
  rc.percussion.num_modes = 4;
  rc.percussion.mode_ratios = {1.0f, 1.34f, 1.72f, 2.15f, 0.0f, 0.0f};
  rc.percussion.base_freq_hz = 3600.0f;  // unpitched crash body
  rc.percussion.mode_decay_s = 1.4f;
  rc.percussion.tone_gain = 0.2f;
  rc.percussion.noise_gain = 0.9f;
  rc.percussion.noise_decay_ms = 2000.0f;  // outlasts the attack swell
  rc.percussion.noise_cutoff_hz = 5500.0f;
  rc.percussion.noise_output = SynthFilterOutput::kHighpass;
  rc.percussion.shimmer = 6.0f;
  rc.percussion.shimmer_attack_ms = 400.0f;
  rc.percussion.shimmer_cutoff_hz = 9000.0f;
  rc.gain = 0.5f;
}

}  // namespace sonare::midi::synth::detail
