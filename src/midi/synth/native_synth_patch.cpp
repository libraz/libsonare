#include <algorithm>
#include <cmath>

#include "midi/synth/native_synth.h"

namespace sonare::midi::synth {

namespace {

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
  p.ks.slap = std::clamp(sanitize(p.ks.slap, 0.0f), 0.0f, 1.0f);
  p.ks.polarization = std::clamp(sanitize(p.ks.polarization, 0.0f), 0.0f, 1.0f);
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
  p.percussion.strike_r = std::clamp(sanitize(p.percussion.strike_r, 0.0f), 0.0f, 1.0f);
  p.percussion.strike_theta = sanitize(p.percussion.strike_theta, 0.0f);
  for (float& alpha : p.percussion.mode_alpha) {
    alpha = std::clamp(sanitize(alpha, 0.0f), 0.0f, 64.0f);
  }
  p.percussion.noise_gain = std::clamp(sanitize(p.percussion.noise_gain, 0.0f), 0.0f, 4.0f);
  p.percussion.noise_decay_ms =
      std::clamp(sanitize(p.percussion.noise_decay_ms, 150.0f), 1.0f, 20000.0f);
  p.percussion.noise_cutoff_hz =
      std::clamp(sanitize(p.percussion.noise_cutoff_hz, 2500.0f), 20.0f, 20000.0f);
  p.percussion.noise_q = std::clamp(sanitize(p.percussion.noise_q, 1.0f), 0.5f, 30.0f);
  p.percussion.shell_mix = std::clamp(sanitize(p.percussion.shell_mix, 0.0f), 0.0f, 1.0f);
  p.percussion.shell_num_modes = std::clamp(p.percussion.shell_num_modes, 0, kMaxShellModes);
  for (float& freq : p.percussion.shell_freq_hz) {
    freq = std::clamp(sanitize(freq, 0.0f), 0.0f, 20000.0f);
  }
  for (float& t60 : p.percussion.shell_t60_s) {
    t60 = std::clamp(sanitize(t60, 0.05f), 0.005f, 5.0f);
  }
  for (float& weight : p.percussion.shell_weight) {
    weight = std::clamp(sanitize(weight, 0.0f), 0.0f, 4.0f);
  }
  p.percussion.wire_buzz = std::clamp(sanitize(p.percussion.wire_buzz, 0.0f), 0.0f, 4.0f);
  p.percussion.wire_threshold = std::clamp(sanitize(p.percussion.wire_threshold, 0.1f), 0.0f, 4.0f);
  p.percussion.wire_cutoff_hz =
      std::clamp(sanitize(p.percussion.wire_cutoff_hz, 4000.0f), 20.0f, 20000.0f);
  p.percussion.shimmer = std::clamp(sanitize(p.percussion.shimmer, 0.0f), 0.0f, 16.0f);
  p.percussion.shimmer_attack_ms =
      std::clamp(sanitize(p.percussion.shimmer_attack_ms, 40.0f), 1.0f, 2000.0f);
  p.percussion.shimmer_cutoff_hz =
      std::clamp(sanitize(p.percussion.shimmer_cutoff_hz, 8000.0f), 20.0f, 20000.0f);
  p.percussion.phisem_beans = std::clamp(sanitize(p.percussion.phisem_beans, 0.0f), 0.0f, 256.0f);
  p.percussion.phisem_energy_ms =
      std::clamp(sanitize(p.percussion.phisem_energy_ms, 100.0f), 1.0f, 20000.0f);
  p.percussion.phisem_sound_ms =
      std::clamp(sanitize(p.percussion.phisem_sound_ms, 3.0f), 0.2f, 200.0f);
  p.percussion.phisem_res_hz =
      std::clamp(sanitize(p.percussion.phisem_res_hz, 0.0f), 0.0f, 20000.0f);
  p.percussion.phisem_res_q = std::clamp(sanitize(p.percussion.phisem_res_q, 1.0f), 0.5f, 30.0f);
  p.percussion.phisem_scrape_hz =
      std::clamp(sanitize(p.percussion.phisem_scrape_hz, 0.0f), 0.0f, 20000.0f);
  p.percussion.phisem_pitch_glide =
      std::clamp(sanitize(p.percussion.phisem_pitch_glide, 0.0f), -0.95f, 8.0f);
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
  p.piano.hammer_dynamics = std::clamp(sanitize(p.piano.hammer_dynamics, 0.0f), 0.0f, 1.0f);
  p.piano.soundboard = std::clamp(sanitize(p.piano.soundboard, 0.25f), 0.0f, 1.0f);
  p.piano.release_damp_s = std::clamp(sanitize(p.piano.release_damp_s, 0.1f), 0.01f, 10.0f);
  p.pipe_organ.brightness = std::clamp(sanitize(p.pipe_organ.brightness, 0.5f), 0.0f, 1.0f);
  p.pipe_organ.tone_decay_s = std::clamp(sanitize(p.pipe_organ.tone_decay_s, 4.0f), 0.05f, 60.0f);
  p.pipe_organ.breath = std::clamp(sanitize(p.pipe_organ.breath, 0.35f), 0.0f, 1.0f);
  p.pipe_organ.chiff = std::clamp(sanitize(p.pipe_organ.chiff, 0.5f), 0.0f, 1.0f);
  p.pipe_organ.chiff_ms = std::clamp(sanitize(p.pipe_organ.chiff_ms, 18.0f), 0.5f, 500.0f);
  p.pipe_organ.release_damp_s =
      std::clamp(sanitize(p.pipe_organ.release_damp_s, 0.08f), 0.01f, 10.0f);
  p.pipe_organ.reed = std::clamp(sanitize(p.pipe_organ.reed, 0.0f), 0.0f, 1.0f);
  p.pipe_organ.radiation = std::clamp(sanitize(p.pipe_organ.radiation, 0.0f), 0.0f, 1.0f);
  p.pipe_organ.rank_count = std::clamp(p.pipe_organ.rank_count, 0, kMaxPipeRanks);
  for (auto& rank : p.pipe_organ.ranks) {
    rank.footage_mult = std::clamp(sanitize(rank.footage_mult, 1.0f), 0.25f, 16.0f);
    rank.brightness = std::clamp(sanitize(rank.brightness, 0.5f), 0.0f, 1.0f);
    rank.level = std::clamp(sanitize(rank.level, 1.0f), 0.0f, 1.0f);
    rank.reed = std::clamp(sanitize(rank.reed, 0.0f), 0.0f, 1.0f);
    rank.radiation = std::clamp(sanitize(rank.radiation, 0.0f), 0.0f, 1.0f);
  }
  p.pipe_organ.tremulant_rate_hz =
      std::clamp(sanitize(p.pipe_organ.tremulant_rate_hz, 0.0f), 0.0f, 12.0f);
  p.pipe_organ.tremulant_depth =
      std::clamp(sanitize(p.pipe_organ.tremulant_depth, 0.0f), 0.0f, 1.0f);
  p.pipe_organ.wind_sag = std::clamp(sanitize(p.pipe_organ.wind_sag, 0.0f), 0.0f, 1.0f);
  p.pipe_organ.swell = std::clamp(sanitize(p.pipe_organ.swell, 0.0f), 0.0f, 1.0f);
  p.bowed_string.bow_position =
      std::clamp(sanitize(p.bowed_string.bow_position, 0.13f), 0.02f, 0.5f);
  p.bowed_string.bow_force = std::clamp(sanitize(p.bowed_string.bow_force, 0.5f), 0.0f, 1.0f);
  p.bowed_string.bow_speed = std::clamp(sanitize(p.bowed_string.bow_speed, 0.5f), 0.0f, 1.0f);
  p.bowed_string.vel_to_speed = std::clamp(sanitize(p.bowed_string.vel_to_speed, 0.6f), 0.0f, 1.0f);
  p.bowed_string.brightness = std::clamp(sanitize(p.bowed_string.brightness, 0.5f), 0.0f, 1.0f);
  p.bowed_string.damping = std::clamp(sanitize(p.bowed_string.damping, 0.4f), 0.0f, 1.0f);
  p.bowed_string.attack_ms = std::clamp(sanitize(p.bowed_string.attack_ms, 60.0f), 1.0f, 2000.0f);
  p.bowed_string.release_ms =
      std::clamp(sanitize(p.bowed_string.release_ms, 120.0f), 1.0f, 5000.0f);
  p.bowed_string.rosin = std::clamp(sanitize(p.bowed_string.rosin, 0.0f), 0.0f, 1.0f);
  p.reed.breath_pressure = std::clamp(sanitize(p.reed.breath_pressure, 0.6f), 0.0f, 1.0f);
  p.reed.vel_to_breath = std::clamp(sanitize(p.reed.vel_to_breath, 0.6f), 0.0f, 1.0f);
  p.reed.reed_stiffness = std::clamp(sanitize(p.reed.reed_stiffness, 0.5f), 0.0f, 1.0f);
  p.reed.reed_opening = std::clamp(sanitize(p.reed.reed_opening, 0.5f), 0.0f, 1.0f);
  p.reed.brightness = std::clamp(sanitize(p.reed.brightness, 0.5f), 0.0f, 1.0f);
  p.reed.damping = std::clamp(sanitize(p.reed.damping, 0.4f), 0.0f, 1.0f);
  p.reed.attack_ms = std::clamp(sanitize(p.reed.attack_ms, 40.0f), 1.0f, 2000.0f);
  p.reed.release_ms = std::clamp(sanitize(p.reed.release_ms, 80.0f), 1.0f, 5000.0f);
  p.reed.breath_noise = std::clamp(sanitize(p.reed.breath_noise, 0.12f), 0.0f, 1.0f);
  p.reed.chiff = std::clamp(sanitize(p.reed.chiff, 0.4f), 0.0f, 1.0f);
  p.reed.chiff_ms = std::clamp(sanitize(p.reed.chiff_ms, 12.0f), 1.0f, 500.0f);
  p.reed.reed_resonance = std::clamp(sanitize(p.reed.reed_resonance, 0.5f), 0.0f, 1.0f);
  p.reed.register_vent = std::clamp(sanitize(p.reed.register_vent, 0.0f), 0.0f, 1.0f);
  p.reed.growl = std::clamp(sanitize(p.reed.growl, 0.0f), 0.0f, 1.0f);
  p.reed.cone_growth = std::clamp(sanitize(p.reed.cone_growth, 0.0f), 0.0f, 1.0f);
  p.reed.tonehole = std::clamp(sanitize(p.reed.tonehole, 0.0f), 0.0f, 1.0f);
  p.brass.breath_pressure = std::clamp(sanitize(p.brass.breath_pressure, 0.7f), 0.0f, 1.0f);
  p.brass.vel_to_breath = std::clamp(sanitize(p.brass.vel_to_breath, 0.6f), 0.0f, 1.0f);
  p.brass.lip_tension = std::clamp(sanitize(p.brass.lip_tension, 0.5f), 0.0f, 1.0f);
  p.brass.lip_damping = std::clamp(sanitize(p.brass.lip_damping, 0.5f), 0.0f, 1.0f);
  p.brass.brightness = std::clamp(sanitize(p.brass.brightness, 0.5f), 0.0f, 1.0f);
  p.brass.damping = std::clamp(sanitize(p.brass.damping, 0.4f), 0.0f, 1.0f);
  p.brass.attack_ms = std::clamp(sanitize(p.brass.attack_ms, 25.0f), 1.0f, 2000.0f);
  p.brass.release_ms = std::clamp(sanitize(p.brass.release_ms, 90.0f), 1.0f, 5000.0f);
  p.brass.breath_noise = std::clamp(sanitize(p.brass.breath_noise, 0.1f), 0.0f, 1.0f);
  p.brass.chiff = std::clamp(sanitize(p.brass.chiff, 0.35f), 0.0f, 1.0f);
  p.brass.chiff_ms = std::clamp(sanitize(p.brass.chiff_ms, 10.0f), 1.0f, 500.0f);
  p.brass.brassiness = std::clamp(sanitize(p.brass.brassiness, 0.0f), 0.0f, 1.0f);
  p.brass.cuivre_dynamics = std::clamp(sanitize(p.brass.cuivre_dynamics, 0.0f), 0.0f, 1.0f);
  p.brass.mute = std::clamp(sanitize(p.brass.mute, 0.0f), 0.0f, 1.0f);
  p.brass.half_valve = std::clamp(sanitize(p.brass.half_valve, 0.0f), 0.0f, 1.0f);
  p.brass.dynamic_lip = std::clamp(sanitize(p.brass.dynamic_lip, 0.0f), 0.0f, 1.0f);
  p.flute.breath_pressure = std::clamp(sanitize(p.flute.breath_pressure, 0.55f), 0.0f, 1.0f);
  p.flute.vel_to_breath = std::clamp(sanitize(p.flute.vel_to_breath, 0.5f), 0.0f, 1.0f);
  p.flute.jet_ratio = std::clamp(sanitize(p.flute.jet_ratio, 0.5f), 0.1f, 0.9f);
  p.flute.jet_reflection = std::clamp(sanitize(p.flute.jet_reflection, 0.5f), 0.0f, 1.0f);
  p.flute.end_reflection = std::clamp(sanitize(p.flute.end_reflection, 0.5f), 0.0f, 1.0f);
  p.flute.brightness = std::clamp(sanitize(p.flute.brightness, 0.5f), 0.0f, 1.0f);
  p.flute.damping = std::clamp(sanitize(p.flute.damping, 0.35f), 0.0f, 1.0f);
  p.flute.attack_ms = std::clamp(sanitize(p.flute.attack_ms, 18.0f), 1.0f, 2000.0f);
  p.flute.release_ms = std::clamp(sanitize(p.flute.release_ms, 90.0f), 1.0f, 5000.0f);
  p.flute.breath_noise = std::clamp(sanitize(p.flute.breath_noise, 0.15f), 0.0f, 1.0f);
  p.flute.chiff = std::clamp(sanitize(p.flute.chiff, 0.4f), 0.0f, 1.0f);
  p.flute.chiff_ms = std::clamp(sanitize(p.flute.chiff_ms, 12.0f), 1.0f, 500.0f);
  p.flute.vibrato_rate_hz = std::clamp(sanitize(p.flute.vibrato_rate_hz, 5.0f), 0.1f, 12.0f);
  p.flute.vibrato_depth = std::clamp(sanitize(p.flute.vibrato_depth, 0.0f), 0.0f, 1.0f);
  p.flute.overblow = std::clamp(sanitize(p.flute.overblow, 0.0f), 0.0f, 1.0f);
  p.flute.jet_turbulence = std::clamp(sanitize(p.flute.jet_turbulence, 0.0f), 0.0f, 1.0f);
  p.flute.edge_hysteresis = std::clamp(sanitize(p.flute.edge_hysteresis, 0.0f), 0.0f, 1.0f);
  p.flute.vortex = std::clamp(sanitize(p.flute.vortex, 0.0f), 0.0f, 1.0f);
  p.plucked_string.brightness = std::clamp(sanitize(p.plucked_string.brightness, 0.7f), 0.0f, 1.0f);
  p.plucked_string.decay_s = std::clamp(sanitize(p.plucked_string.decay_s, 4.0f), 0.05f, 60.0f);
  p.plucked_string.decay_stretch =
      std::clamp(sanitize(p.plucked_string.decay_stretch, 0.5f), 0.0f, 1.0f);
  p.plucked_string.pick_position =
      std::clamp(sanitize(p.plucked_string.pick_position, 0.2f), 0.0f, 0.5f);
  p.plucked_string.exc_brightness =
      std::clamp(sanitize(p.plucked_string.exc_brightness, 0.85f), 0.0f, 1.0f);
  p.plucked_string.vel_to_brightness =
      std::clamp(sanitize(p.plucked_string.vel_to_brightness, 0.6f), 0.0f, 1.0f);
  p.plucked_string.release_damp_s =
      std::clamp(sanitize(p.plucked_string.release_damp_s, 0.12f), 0.01f, 10.0f);
  p.plucked_string.buzz = std::clamp(sanitize(p.plucked_string.buzz, 0.0f), 0.0f, 1.0f);
  p.vocal.vowel = std::clamp(p.vocal.vowel, 0, kVocalVowels - 1);
  p.vocal.brightness = std::clamp(sanitize(p.vocal.brightness, 0.5f), 0.0f, 1.0f);
  p.vocal.breath_noise = std::clamp(sanitize(p.vocal.breath_noise, 0.1f), 0.0f, 1.0f);
  p.vocal.vibrato_rate_hz = std::clamp(sanitize(p.vocal.vibrato_rate_hz, 5.5f), 0.1f, 12.0f);
  p.vocal.vibrato_depth = std::clamp(sanitize(p.vocal.vibrato_depth, 0.3f), 0.0f, 1.0f);
  p.vocal.attack_ms = std::clamp(sanitize(p.vocal.attack_ms, 30.0f), 1.0f, 2000.0f);
  p.vocal.release_ms = std::clamp(sanitize(p.vocal.release_ms, 120.0f), 1.0f, 5000.0f);
  p.free_reed.brightness = std::clamp(sanitize(p.free_reed.brightness, 0.6f), 0.0f, 1.0f);
  p.free_reed.reed_stiffness = std::clamp(sanitize(p.free_reed.reed_stiffness, 0.5f), 0.0f, 1.0f);
  p.free_reed.breath_pressure = std::clamp(sanitize(p.free_reed.breath_pressure, 0.7f), 0.0f, 1.0f);
  p.free_reed.vel_to_breath = std::clamp(sanitize(p.free_reed.vel_to_breath, 0.5f), 0.0f, 1.0f);
  p.free_reed.detune = std::clamp(sanitize(p.free_reed.detune, 0.3f), 0.0f, 1.0f);
  p.free_reed.attack_ms = std::clamp(sanitize(p.free_reed.attack_ms, 20.0f), 1.0f, 2000.0f);
  p.free_reed.release_ms = std::clamp(sanitize(p.free_reed.release_ms, 80.0f), 1.0f, 5000.0f);
  p.free_reed.breath_noise = std::clamp(sanitize(p.free_reed.breath_noise, 0.08f), 0.0f, 1.0f);
  if (static_cast<int>(p.body) < 0 || static_cast<int>(p.body) > 4) p.body = BodyType::kNone;
  p.body_mix = std::clamp(sanitize(p.body_mix, 0.0f), 0.0f, 1.0f);
  p.stereo_spread = std::clamp(sanitize(p.stereo_spread, 0.0f), 0.0f, 1.0f);
  return p;
}

}  // namespace sonare::midi::synth
