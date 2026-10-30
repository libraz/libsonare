#pragma once

/// @file patch_clamp.h
/// @brief constexpr clamp_synth_patch: every NativeSynthPatch field forced into a safe range.

#include <algorithm>
#include <limits>

#include "midi/synth/envelope.h"
#include "midi/synth/native_synth_patch.h"

namespace sonare::midi::synth {

/// Patch-clamp helpers. Named rather than local because `clamp_synth_patch`
/// is `constexpr`: the fallback tables it feeds are constant-initialised, so
/// its whole call tree has to be visible here rather than in a translation
/// unit of its own.
namespace patch_clamp_detail {

/// Finite test usable during constant evaluation. `std::isfinite` is not
/// `constexpr` in C++17; `value == value` rejects NaN and the magnitude
/// bounds reject both infinities, which is the same predicate for `float`.
constexpr float sanitize(float value, float fallback) noexcept {
  return (value == value && value <= std::numeric_limits<float>::max() &&
          value >= std::numeric_limits<float>::lowest())
             ? value
             : fallback;
}

constexpr DahdsrConfig clamp_env(const DahdsrConfig& env) noexcept {
  DahdsrConfig out{};
  out.delay_ms = std::clamp(sanitize(env.delay_ms, 0.0f), 0.0f, 5000.0f);
  out.attack_ms = std::clamp(sanitize(env.attack_ms, 5.0f), 0.0f, 20000.0f);
  out.hold_ms = std::clamp(sanitize(env.hold_ms, 0.0f), 0.0f, 5000.0f);
  out.decay_ms = std::clamp(sanitize(env.decay_ms, 60.0f), 0.0f, 20000.0f);
  out.sustain = std::clamp(sanitize(env.sustain, 0.7f), 0.0f, 1.0f);
  out.release_ms = std::clamp(sanitize(env.release_ms, 120.0f), 1.0f, 20000.0f);
  return out;
}

}  // namespace patch_clamp_detail

/// Returns a copy of @p patch with every field clamped to a safe range.
///
/// `constexpr` so the GM fallback tables (`program_overrides()`,
/// `drum_note_table()`, `family_patches()`) are constant-initialised into the
/// data section rather than assembled field by field by start-up code, which
/// the WebAssembly size budget cannot afford. Caller-supplied patches still
/// clamp at run time through the same definition.
constexpr NativeSynthPatch clamp_synth_patch(const NativeSynthPatch& patch) noexcept {
  NativeSynthPatch p = patch;
  p.unison = std::clamp(p.unison, 1, kMaxUnisonOscs);
  p.detune_cents = std::clamp(patch_clamp_detail::sanitize(p.detune_cents, 0.0f), 0.0f, 200.0f);
  p.drift_cents = std::clamp(patch_clamp_detail::sanitize(p.drift_cents, 0.0f), 0.0f, 100.0f);
  p.drift_rate_hz = std::clamp(patch_clamp_detail::sanitize(p.drift_rate_hz, 0.3f), 0.01f, 20.0f);
  p.pitch_offset_cents =
      std::clamp(patch_clamp_detail::sanitize(p.pitch_offset_cents, 0.0f), -4800.0f, 4800.0f);
  // The one scale applied after the drive, the filter and the envelope, so the
  // only lever that moves a voice's level without its timbre. The drum bank's
  // raw voices span 33 dB and at 4 it ran out under the quietest of them.
  p.gain = std::clamp(patch_clamp_detail::sanitize(p.gain, 0.5f), 0.0f, 16.0f);
  p.amp_env = patch_clamp_detail::clamp_env(p.amp_env);
  p.cutoff_hz = std::clamp(patch_clamp_detail::sanitize(p.cutoff_hz, 12000.0f), 10.0f, 22000.0f);
  p.resonance_q = std::clamp(patch_clamp_detail::sanitize(p.resonance_q, constants::kButterworthQ),
                             0.5f, 30.0f);
  p.hp_cutoff_hz = std::clamp(patch_clamp_detail::sanitize(p.hp_cutoff_hz, 0.0f), 0.0f, 22000.0f);
  p.drive = std::clamp(patch_clamp_detail::sanitize(p.drive, 0.0f), 0.0f, 1.0f);
  // The lower bound is a rate the stage can still be heard as a rate at; below
  // it the held value lasts long enough to be a note of its own.
  p.sample_hold_hz =
      std::clamp(patch_clamp_detail::sanitize(p.sample_hold_hz, 0.0f), 0.0f, 192000.0f);
  if (p.sample_hold_hz > 0.0f) p.sample_hold_hz = std::max(p.sample_hold_hz, 100.0f);
  // 24 is the ceiling because a float voice already resolves further than that;
  // 1 bit is a square wave, which is where the stage stops being a converter and
  // is still a thing someone reaches for.
  p.bit_depth = std::clamp(patch_clamp_detail::sanitize(p.bit_depth, 0.0f), 0.0f, 24.0f);
  if (p.bit_depth > 0.0f) p.bit_depth = std::max(p.bit_depth, 1.0f);
  p.filter_env = patch_clamp_detail::clamp_env(p.filter_env);
  p.env_to_cutoff_cents =
      std::clamp(patch_clamp_detail::sanitize(p.env_to_cutoff_cents, 0.0f), -9600.0f, 9600.0f);
  p.key_track = std::clamp(patch_clamp_detail::sanitize(p.key_track, 0.0f), 0.0f, 1.0f);
  p.vel_to_cutoff_cents =
      std::clamp(patch_clamp_detail::sanitize(p.vel_to_cutoff_cents, 0.0f), -9600.0f, 9600.0f);
  p.lfo_rate_hz = std::clamp(patch_clamp_detail::sanitize(p.lfo_rate_hz, 5.0f), 0.0f, 40.0f);
  p.lfo_to_pitch_cents =
      std::clamp(patch_clamp_detail::sanitize(p.lfo_to_pitch_cents, 0.0f), 0.0f, 1200.0f);
  p.lfo2_rate_hz = std::clamp(patch_clamp_detail::sanitize(p.lfo2_rate_hz, 1.0f), 0.0f, 40.0f);
  p.glide_ms = std::clamp(patch_clamp_detail::sanitize(p.glide_ms, 0.0f), 0.0f, 5000.0f);
  p.va.osc2_interval_cents =
      std::clamp(patch_clamp_detail::sanitize(p.va.osc2_interval_cents, 0.0f), -2400.0f, 2400.0f);
  p.va.osc2_level = std::clamp(patch_clamp_detail::sanitize(p.va.osc2_level, 0.0f), 0.0f, 4.0f);
  p.va.osc2_dry = std::clamp(patch_clamp_detail::sanitize(p.va.osc2_dry, 0.0f), 0.0f, 1.0f);
  p.va.noise_level = std::clamp(patch_clamp_detail::sanitize(p.va.noise_level, 0.0f), 0.0f, 4.0f);
  p.va.noise_am = std::clamp(patch_clamp_detail::sanitize(p.va.noise_am, 0.0f), 0.0f, 1.0f);
  p.va.band_noise_level =
      std::clamp(patch_clamp_detail::sanitize(p.va.band_noise_level, 0.0f), 0.0f, 4.0f);
  p.va.band_noise_low_hz =
      std::clamp(patch_clamp_detail::sanitize(p.va.band_noise_low_hz, 20.0f), 10.0f, 20000.0f);
  p.va.band_noise_low_key_track =
      std::clamp(patch_clamp_detail::sanitize(p.va.band_noise_low_key_track, 0.0f), 0.0f, 1.0f);
  p.va.band_noise_high_hz =
      std::clamp(patch_clamp_detail::sanitize(p.va.band_noise_high_hz, 4000.0f), 20.0f, 20000.0f);
  p.va.band_noise_high_key_track =
      std::clamp(patch_clamp_detail::sanitize(p.va.band_noise_high_key_track, 0.0f), 0.0f, 1.0f);
  p.va.velocity_exponent =
      std::clamp(patch_clamp_detail::sanitize(p.va.velocity_exponent, 2.0f), 0.0f, 4.0f);
  p.va.pulse_width = std::clamp(patch_clamp_detail::sanitize(p.va.pulse_width, 0.5f), 0.02f, 0.98f);
  p.va.unison_width = std::clamp(patch_clamp_detail::sanitize(p.va.unison_width, 0.0f), 0.0f, 1.0f);
  for (ModRoute& route : p.mod_matrix.routes) {
    route.depth = std::clamp(patch_clamp_detail::sanitize(route.depth, 0.0f), -9600.0f, 9600.0f);
  }
  p.fm.velocity_exponent =
      std::clamp(patch_clamp_detail::sanitize(p.fm.velocity_exponent, 2.0f), 0.0f, 4.0f);
  for (FmOperatorParams& op : p.fm.ops) {
    op.ratio = std::clamp(patch_clamp_detail::sanitize(op.ratio, 1.0f), 0.0f, 64.0f);
    op.detune_cents =
        std::clamp(patch_clamp_detail::sanitize(op.detune_cents, 0.0f), -1200.0f, 1200.0f);
    op.level = std::clamp(patch_clamp_detail::sanitize(op.level, 0.0f), 0.0f, 16.0f);
    op.env = patch_clamp_detail::clamp_env(op.env);
    op.vel_to_level = std::clamp(patch_clamp_detail::sanitize(op.vel_to_level, 0.0f), 0.0f, 1.0f);
    op.key_rate_scale =
        std::clamp(patch_clamp_detail::sanitize(op.key_rate_scale, 0.0f), 0.0f, 1.0f);
    op.feedback = std::clamp(patch_clamp_detail::sanitize(op.feedback, 0.0f), 0.0f, 4.0f);
  }
  p.ks.brightness = std::clamp(patch_clamp_detail::sanitize(p.ks.brightness, 0.6f), 0.0f, 1.0f);
  p.ks.decay_s = std::clamp(patch_clamp_detail::sanitize(p.ks.decay_s, 3.0f), 0.05f, 60.0f);
  p.ks.decay_stretch =
      std::clamp(patch_clamp_detail::sanitize(p.ks.decay_stretch, 0.5f), 0.0f, 1.0f);
  p.ks.pick_position =
      std::clamp(patch_clamp_detail::sanitize(p.ks.pick_position, 0.18f), 0.0f, 0.5f);
  p.ks.pick_position_per_octave =
      std::clamp(patch_clamp_detail::sanitize(p.ks.pick_position_per_octave, 0.0f), 0.0f, 4.0f);
  p.ks.exc_brightness =
      std::clamp(patch_clamp_detail::sanitize(p.ks.exc_brightness, 0.85f), 0.0f, 1.0f);
  p.ks.exc_brightness_per_octave =
      std::clamp(patch_clamp_detail::sanitize(p.ks.exc_brightness_per_octave, 0.0f), 0.0f, 4.0f);
  p.ks.vel_to_brightness =
      std::clamp(patch_clamp_detail::sanitize(p.ks.vel_to_brightness, 0.6f), 0.0f, 1.0f);
  p.ks.release_damp_s =
      std::clamp(patch_clamp_detail::sanitize(p.ks.release_damp_s, 0.08f), 0.01f, 10.0f);
  p.ks.mute_harmonic =
      std::clamp(patch_clamp_detail::sanitize(p.ks.mute_harmonic, 0.0f), 0.0f, 16.0f);
  p.ks.slap = std::clamp(patch_clamp_detail::sanitize(p.ks.slap, 0.0f), 0.0f, 1.0f);
  p.ks.polarization = std::clamp(patch_clamp_detail::sanitize(p.ks.polarization, 0.0f), 0.0f, 1.0f);
  p.ks.body_coupling = patch_clamp_detail::sanitize(p.ks.body_coupling, 0.0f);
  p.ks.pluck_style = patch_clamp_detail::sanitize(p.ks.pluck_style, 0.0f);
  p.ks.nail = patch_clamp_detail::sanitize(p.ks.nail, 0.0f);
  p.ks.pickup_pos = patch_clamp_detail::sanitize(p.ks.pickup_pos, 0.0f);
  p.ks.dispersion = patch_clamp_detail::sanitize(p.ks.dispersion, 0.0f);
  p.ks.tension_mod = patch_clamp_detail::sanitize(p.ks.tension_mod, 0.0f);
  p.ks.octave_mix = patch_clamp_detail::sanitize(p.ks.octave_mix, 0.0f);
  // Bounded here rather than only at use, so the touched node reports its real
  // range: a sweep over an unbounded divisor is a sweep over nothing.
  p.ks.harmonic_node =
      std::clamp(patch_clamp_detail::sanitize(p.ks.harmonic_node, 0.0f), 0.0f, 8.0f);
  p.ks.keyoff_noise = patch_clamp_detail::sanitize(p.ks.keyoff_noise, 0.0f);
  p.ks.pick_noise = patch_clamp_detail::sanitize(p.ks.pick_noise, 0.0f);
  // At the halo bank's own ring a damping t60 changes nothing, so that is where the range ends.
  p.ks.halo_damp_s = std::clamp(patch_clamp_detail::sanitize(p.ks.halo_damp_s, 0.0f), 0.0f,
                                PianoResonanceBank::kGuitarHaloRingT60S);
  p.ks.body_air_hz = std::clamp(patch_clamp_detail::sanitize(p.ks.body_air_hz, 0.0f), 0.0f, 400.0f);
  p.ks.body_air_t60_s =
      std::clamp(patch_clamp_detail::sanitize(p.ks.body_air_t60_s, 0.0f), 0.0f, 2.0f);
  p.ks.body_knock = std::clamp(patch_clamp_detail::sanitize(p.ks.body_knock, 0.0f), 0.0f, 4.0f);
  p.ks.hf_decay_s = std::clamp(patch_clamp_detail::sanitize(p.ks.hf_decay_s, 0.07f), 0.0f, 60.0f);
  p.ks.mid_decay_s = std::clamp(patch_clamp_detail::sanitize(p.ks.mid_decay_s, 0.0f), 0.0f, 60.0f);
  p.ks.velocity_exponent =
      std::clamp(patch_clamp_detail::sanitize(p.ks.velocity_exponent, 1.19f), 0.0f, 4.0f);
  p.modal.num_modes = std::clamp(p.modal.num_modes, 0, kMaxModalModes);
  for (ModalMode& mode : p.modal.modes) {
    mode.ratio = std::clamp(patch_clamp_detail::sanitize(mode.ratio, 1.0f), 0.01f, 64.0f);
    mode.gain = std::clamp(patch_clamp_detail::sanitize(mode.gain, 1.0f), 0.0f, 4.0f);
    mode.decay_scale =
        std::clamp(patch_clamp_detail::sanitize(mode.decay_scale, 1.0f), 0.01f, 4.0f);
  }
  p.modal.decay_s = std::clamp(patch_clamp_detail::sanitize(p.modal.decay_s, 2.0f), 0.01f, 60.0f);
  p.modal.decay_stretch =
      std::clamp(patch_clamp_detail::sanitize(p.modal.decay_stretch, 0.3f), 0.0f, 1.0f);
  p.modal.strike_brightness =
      std::clamp(patch_clamp_detail::sanitize(p.modal.strike_brightness, 0.7f), 0.0f, 1.0f);
  p.modal.vel_to_brightness =
      std::clamp(patch_clamp_detail::sanitize(p.modal.vel_to_brightness, 0.6f), 0.0f, 1.0f);
  p.modal.release_damp_s =
      std::clamp(patch_clamp_detail::sanitize(p.modal.release_damp_s, 0.15f), 0.01f, 10.0f);
  p.modal.mallet_ms =
      std::clamp(patch_clamp_detail::sanitize(p.modal.mallet_ms, 0.0f), 0.0f, 50.0f);
  p.modal.mallet_vel_exp =
      std::clamp(patch_clamp_detail::sanitize(p.modal.mallet_vel_exp, 0.0f), 0.0f, 1.0f);
  p.modal.contact = std::clamp(patch_clamp_detail::sanitize(p.modal.contact, 0.0f), 0.0f, 16.0f);
  p.modal.contact_ms =
      std::clamp(patch_clamp_detail::sanitize(p.modal.contact_ms, 0.5f), 0.02f, 20.0f);
  p.modal.velocity_exponent =
      std::clamp(patch_clamp_detail::sanitize(p.modal.velocity_exponent, 2.0f), 0.0f, 4.0f);
  for (float& level : p.additive.drawbars)
    level = std::clamp(patch_clamp_detail::sanitize(level, 0.0f), 0.0f, 8.0f);
  for (float& level : p.additive.drawbars_b)
    level = std::clamp(patch_clamp_detail::sanitize(level, 0.0f), 0.0f, 8.0f);
  p.additive.morph = std::clamp(patch_clamp_detail::sanitize(p.additive.morph, 0.0f), 0.0f, 1.0f);
  p.additive.key_click =
      std::clamp(patch_clamp_detail::sanitize(p.additive.key_click, 0.4f), 0.0f, 1.0f);
  p.additive.click_decay_ms =
      std::clamp(patch_clamp_detail::sanitize(p.additive.click_decay_ms, 6.0f), 0.5f, 100.0f);
  p.additive.percussion_harmonic = std::clamp(p.additive.percussion_harmonic, 0, 3);
  p.additive.percussion_decay_ms = std::clamp(
      patch_clamp_detail::sanitize(p.additive.percussion_decay_ms, 340.0f), 20.0f, 4000.0f);
  p.additive.percussion_level =
      std::clamp(patch_clamp_detail::sanitize(p.additive.percussion_level, 0.6f), 0.0f, 1.0f);
  p.percussion.num_modes = std::clamp(p.percussion.num_modes, 0, kMaxPercussionModes);
  for (float& ratio : p.percussion.mode_ratios) {
    ratio = std::clamp(patch_clamp_detail::sanitize(ratio, 0.0f), 0.0f, 64.0f);
  }
  p.percussion.mode_decay_s =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.mode_decay_s, 0.3f), 0.005f, 30.0f);
  p.percussion.mode_decay_exp =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.mode_decay_exp, 0.0f), 0.0f, 2.0f);
  p.percussion.tone_gain =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.tone_gain, 1.0f), 0.0f, 4.0f);
  // A share, so the interval is the whole of it: 0 and 1 are both real
  // settings and a fit has to be able to reach either end.
  p.percussion.tone_direct =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.tone_direct, 1.0f), 0.0f, 1.0f);
  p.percussion.base_freq_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.base_freq_hz, 0.0f), 0.0f, 20000.0f);
  p.percussion.pitch_drop =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.pitch_drop, 0.0f), 0.0f, 8.0f);
  p.percussion.pitch_drop_ms =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.pitch_drop_ms, 40.0f), 1.0f, 2000.0f);
  p.percussion.strike_r =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.strike_r, 0.0f), 0.0f, 1.0f);
  p.percussion.strike_theta = patch_clamp_detail::sanitize(p.percussion.strike_theta, 0.0f);
  for (float& alpha : p.percussion.mode_alpha) {
    alpha = std::clamp(patch_clamp_detail::sanitize(alpha, 0.0f), 0.0f, 64.0f);
  }
  // 50 ms is longer than any struck contact the kit models; the exponent is a
  // power of a normalized velocity, so above 1 a soft hit is brighter.
  p.percussion.mallet_ms =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.mallet_ms, 0.0f), 0.0f, 50.0f);
  p.percussion.mallet_vel_exp =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.mallet_vel_exp, 0.0f), 0.0f, 1.0f);
  // A cavity eight times the head's stiffness splits the pair by a factor of 3;
  // the two lengths bound a concert bass drum and leave a taiko room.
  p.percussion.air_spring =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.air_spring, 0.0f), 0.0f, 8.0f);
  p.percussion.head_diameter_m =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.head_diameter_m, 0.0f), 0.0f, 2.0f);
  p.percussion.shell_depth_m =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.shell_depth_m, 0.0f), 0.0f, 2.0f);
  p.percussion.noise_gain =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.noise_gain, 0.0f), 0.0f, 4.0f);
  p.percussion.noise_decay_ms =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.noise_decay_ms, 150.0f), 1.0f, 20000.0f);
  p.percussion.noise_cutoff_hz = std::clamp(
      patch_clamp_detail::sanitize(p.percussion.noise_cutoff_hz, 2500.0f), 20.0f, 20000.0f);
  p.percussion.noise_q =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.noise_q, 1.0f), 0.5f, 30.0f);
  p.percussion.noise_burst_interval_ms =
      patch_clamp_detail::sanitize(p.percussion.noise_burst_interval_ms, 10.0f);
  p.percussion.noise_burst_decay_ms =
      patch_clamp_detail::sanitize(p.percussion.noise_burst_decay_ms, 6.0f);
  p.percussion.shell_mix =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.shell_mix, 0.0f), 0.0f, 1.0f);
  p.percussion.shell_num_modes = std::clamp(p.percussion.shell_num_modes, 0, kMaxShellModes);
  for (float& freq : p.percussion.shell_freq_hz) {
    freq = std::clamp(patch_clamp_detail::sanitize(freq, 0.0f), 0.0f, 20000.0f);
  }
  for (float& t60 : p.percussion.shell_t60_s) {
    t60 = std::clamp(patch_clamp_detail::sanitize(t60, 0.05f), 0.005f, 5.0f);
  }
  for (float& weight : p.percussion.shell_weight) {
    weight = std::clamp(patch_clamp_detail::sanitize(weight, 0.0f), 0.0f, 4.0f);
  }
  // 0 stays 0 — the unbounded voicing, not a cutoff pinned to the low end.
  // That makes the accepted interval two regions rather than one: off, and a
  // real ceiling. A sweep that treats [0, 20000] as continuous spends its low
  // end bounding a cymbal below its own corner, which is not a darker cymbal
  // but a quiet one — search from a musical floor, not from zero.
  p.percussion.noise_air_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.noise_air_hz, 0.0f), 0.0f, 20000.0f);
  p.percussion.wire_buzz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.wire_buzz, 0.0f), 0.0f, 4.0f);
  // A fraction of the head's own peak swing, so the whole interval means
  // something: 0 is always in contact and 1 is never. It read [0, 4] while the
  // membrane swung around a tenth of that, which left most of the sweep range
  // in a dead zone and every other wire knob free once a fit landed there.
  p.percussion.wire_threshold =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.wire_threshold, 0.1f), 0.0f, 1.0f);
  p.percussion.wire_cutoff_hz = std::clamp(
      patch_clamp_detail::sanitize(p.percussion.wire_cutoff_hz, 4000.0f), 20.0f, 20000.0f);
  p.percussion.wire_decay_ms =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.wire_decay_ms, 0.0f), 0.0f, 2000.0f);
  p.percussion.shimmer =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.shimmer, 0.0f), 0.0f, 16.0f);
  p.percussion.shimmer_attack_ms = std::clamp(
      patch_clamp_detail::sanitize(p.percussion.shimmer_attack_ms, 40.0f), 1.0f, 2000.0f);
  p.percussion.shimmer_cutoff_hz = std::clamp(
      patch_clamp_detail::sanitize(p.percussion.shimmer_cutoff_hz, 8000.0f), 20.0f, 20000.0f);
  // Up to 16, the same ceiling `gain` carries. Four was under the plate's own
  // peak on every cymbal here, so a stick's radiation could not be the strike's
  // loudest moment - which on a ride played with the tip is most of what there
  // is to hear.
  p.percussion.contact =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.contact, 0.0f), 0.0f, 16.0f);
  // The ceiling is a felt mallet and the floor is the sample period at 96 kHz,
  // below which the pulse is a single sample whatever is asked for.
  p.percussion.contact_ms =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.contact_ms, 0.3f), 0.02f, 20.0f);
  p.percussion.plate_gain =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_gain, 0.0f), 0.0f, 4.0f);
  // A cymbal rings for tens of seconds and a gong for longer, so the upper
  // bound is the note length rather than a drum's.
  p.percussion.plate_t60_s =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_t60_s, 2.0f), 0.01f, 30.0f);
  p.percussion.plate_hf_ratio =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_hf_ratio, 0.6f), 0.01f, 1.0f);
  // The floor is the lowest partial the delay lines can still place at 96 kHz,
  // so a patch reads the same at every sample rate the library renders at. Ask
  // for less and the network scales itself to fit, which is a smaller plate
  // than the patch called for rather than a clamped number.
  p.percussion.plate_low_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_low_hz, 180.0f), 140.0f, 4000.0f);
  // 0 stays 0 — unbounded, not a ceiling pinned to the low end — for the same
  // reason `noise_air_hz` does: the accepted interval is off plus a real
  // ceiling, and a sweep that reads it as continuous spends its low end
  // squeezing a plate below its own band.
  p.percussion.plate_air_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_air_hz, 0.0f), 0.0f, 20000.0f);
  // The same ceiling as the stick's direct radiation, which is the same pulse.
  p.percussion.plate_contact =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_contact, 0.0f), 0.0f, 16.0f);
  p.percussion.plate_cascade =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_cascade, 0.0f), 0.0f, 1.0e4f);
  p.percussion.plate_cascade_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_cascade_hz, 0.0f), 0.0f, 20000.0f);
  p.percussion.plate_cascade_drop_db = std::clamp(
      patch_clamp_detail::sanitize(p.percussion.plate_cascade_drop_db, 0.0f), 0.0f, 60.0f);
  // Same floor as plate_low_hz, 0 kept as "follow plate_low_hz".
  p.percussion.plate_floor_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.plate_floor_hz, 0.0f), 0.0f, 4000.0f);
  p.percussion.phisem_beans =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_beans, 0.0f), 0.0f, 256.0f);
  p.percussion.phisem_energy_ms = std::clamp(
      patch_clamp_detail::sanitize(p.percussion.phisem_energy_ms, 100.0f), 1.0f, 20000.0f);
  p.percussion.phisem_sound_ms =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_sound_ms, 3.0f), 0.2f, 200.0f);
  p.percussion.phisem_res_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_res_hz, 0.0f), 0.0f, 20000.0f);
  p.percussion.phisem_res_q =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_res_q, 1.0f), 0.5f, 30.0f);
  p.percussion.phisem_body_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_body_hz, 0.0f), 0.0f, 20000.0f);
  p.percussion.phisem_body_q =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_body_q, 4.0f), 0.5f, 30.0f);
  p.percussion.phisem_body_gain =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_body_gain, 0.0f), 0.0f, 4.0f);
  p.percussion.phisem_scrape_hz =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_scrape_hz, 0.0f), 0.0f, 20000.0f);
  p.percussion.phisem_pitch_glide =
      std::clamp(patch_clamp_detail::sanitize(p.percussion.phisem_pitch_glide, 0.0f), -0.95f, 8.0f);
  p.piano.strings = std::clamp(p.piano.strings, 1, kMaxPianoStrings);
  p.piano.detune_cents =
      std::clamp(patch_clamp_detail::sanitize(p.piano.detune_cents, 1.6f), 0.0f, 50.0f);
  p.piano.decay_fast_s =
      std::clamp(patch_clamp_detail::sanitize(p.piano.decay_fast_s, 3.0f), 0.05f, 60.0f);
  p.piano.decay_slow_s =
      std::clamp(patch_clamp_detail::sanitize(p.piano.decay_slow_s, 12.0f), 0.05f, 120.0f);
  p.piano.decay_stretch =
      std::clamp(patch_clamp_detail::sanitize(p.piano.decay_stretch, 0.7f), 0.0f, 1.0f);
  p.piano.brightness =
      std::clamp(patch_clamp_detail::sanitize(p.piano.brightness, 0.75f), 0.0f, 1.0f);
  p.piano.dispersion =
      std::clamp(patch_clamp_detail::sanitize(p.piano.dispersion, 1.0f), 0.0f, 1.0f);
  p.piano.strike_position =
      std::clamp(patch_clamp_detail::sanitize(p.piano.strike_position, 0.085f), 0.0f, 0.5f);
  p.piano.hammer_exponent =
      std::clamp(patch_clamp_detail::sanitize(p.piano.hammer_exponent, 2.5f), 1.5f, 4.0f);
  p.piano.hammer_contact_ms =
      std::clamp(patch_clamp_detail::sanitize(p.piano.hammer_contact_ms, 1.2f), 0.2f, 10.0f);
  p.piano.hammer_dynamics =
      std::clamp(patch_clamp_detail::sanitize(p.piano.hammer_dynamics, 0.0f), 0.0f, 1.0f);
  p.piano.attack_hf_dynamics =
      std::clamp(patch_clamp_detail::sanitize(p.piano.attack_hf_dynamics, 0.0f), 0.0f, 8.0f);
  p.piano.soundboard =
      std::clamp(patch_clamp_detail::sanitize(p.piano.soundboard, 0.25f), 0.0f, 1.0f);
  p.piano.release_damp_s =
      std::clamp(patch_clamp_detail::sanitize(p.piano.release_damp_s, 0.1f), 0.01f, 10.0f);
  p.pipe_organ.brightness =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.brightness, 0.5f), 0.0f, 1.0f);
  p.pipe_organ.pipe_detune_cents =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.pipe_detune_cents, 4.0f), 0.0f, 30.0f);
  p.pipe_organ.tone_decay_s =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.tone_decay_s, 4.0f), 0.05f, 60.0f);
  p.pipe_organ.breath =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.breath, 0.35f), 0.0f, 1.0f);
  p.pipe_organ.chiff =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.chiff, 0.5f), 0.0f, 1.0f);
  p.pipe_organ.chiff_ms =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.chiff_ms, 18.0f), 0.5f, 500.0f);
  p.pipe_organ.release_damp_s =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.release_damp_s, 0.08f), 0.01f, 10.0f);
  p.pipe_organ.reed = std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.reed, 0.0f), 0.0f, 1.0f);
  p.pipe_organ.radiation =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.radiation, 0.0f), 0.0f, 1.0f);
  p.pipe_organ.keytrack = patch_clamp_detail::sanitize(p.pipe_organ.keytrack, 0.0f);
  p.pipe_organ.rank_count = std::clamp(p.pipe_organ.rank_count, 0, kMaxPipeRanks);
  for (auto& rank : p.pipe_organ.ranks) {
    rank.footage_mult =
        std::clamp(patch_clamp_detail::sanitize(rank.footage_mult, 1.0f), 0.25f, 16.0f);
    rank.brightness = std::clamp(patch_clamp_detail::sanitize(rank.brightness, 0.5f), 0.0f, 1.0f);
    rank.level = std::clamp(patch_clamp_detail::sanitize(rank.level, 1.0f), 0.0f, 1.0f);
    rank.reed = std::clamp(patch_clamp_detail::sanitize(rank.reed, 0.0f), 0.0f, 1.0f);
    rank.radiation = std::clamp(patch_clamp_detail::sanitize(rank.radiation, 0.0f), 0.0f, 1.0f);
  }
  p.pipe_organ.tremulant_rate_hz =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.tremulant_rate_hz, 0.0f), 0.0f, 12.0f);
  p.pipe_organ.tremulant_depth =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.tremulant_depth, 0.0f), 0.0f, 1.0f);
  p.pipe_organ.wind_sag =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.wind_sag, 0.0f), 0.0f, 1.0f);
  p.pipe_organ.swell =
      std::clamp(patch_clamp_detail::sanitize(p.pipe_organ.swell, 0.0f), 0.0f, 1.0f);
  p.bowed_string.bow_position =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.bow_position, 0.13f), 0.02f, 0.5f);
  p.bowed_string.bow_force =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.bow_force, 0.5f), 0.0f, 1.0f);
  p.bowed_string.bow_speed =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.bow_speed, 0.5f), 0.0f, 1.0f);
  p.bowed_string.vel_to_speed =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.vel_to_speed, 0.6f), 0.0f, 1.0f);
  p.bowed_string.brightness =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.brightness, 0.5f), 0.0f, 1.0f);
  p.bowed_string.damping =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.damping, 0.4f), 0.0f, 1.0f);
  p.bowed_string.attack_ms =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.attack_ms, 60.0f), 1.0f, 2000.0f);
  p.bowed_string.release_ms =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.release_ms, 120.0f), 1.0f, 5000.0f);
  p.bowed_string.rosin =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.rosin, 0.0f), 0.0f, 1.0f);
  p.bowed_string.stribeck = patch_clamp_detail::sanitize(p.bowed_string.stribeck, 0.5f);
  // A violin string's tension up to a contrabass's; the length a violin's up to a bass's.
  p.bowed_string.string_tension_n = std::clamp(
      patch_clamp_detail::sanitize(p.bowed_string.string_tension_n, 60.0f), 1.0f, 500.0f);
  p.bowed_string.string_length_m =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.string_length_m, 0.325f), 0.1f, 2.0f);
  // Up to a bass bow's full hair ribbon; 0 is off.
  p.bowed_string.bow_width_m =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.bow_width_m, 0.0f), 0.0f, 0.05f);
  // F_b / (Z0 v_b) well past the 3-6 a player's stroke holds; 0 is off.
  p.bowed_string.force_ratio =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.force_ratio, 0.0f), 0.0f, 12.0f);
  // The bow_position range; 0 is off.
  p.bowed_string.bow_position_ff =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.bow_position_ff, 0.0f), 0.0f, 0.5f);
  // Half the bow_position range; 0 is off.
  p.bowed_string.bow_position_spread = std::clamp(
      patch_clamp_detail::sanitize(p.bowed_string.bow_position_spread, 0.0f), 0.0f, 0.25f);
  p.bowed_string.velocity_exponent =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.velocity_exponent, 2.0f), 0.0f, 4.0f);
  p.bowed_string.sympathetic = patch_clamp_detail::sanitize(p.bowed_string.sympathetic, 0.0f);
  p.bowed_string.polarization = patch_clamp_detail::sanitize(p.bowed_string.polarization, 0.0f);
  // A wide section vibrato; 0 is off. The rate spans a slow wander to a fast vibrato.
  p.bowed_string.section_spread_cents = std::clamp(
      patch_clamp_detail::sanitize(p.bowed_string.section_spread_cents, 0.0f), 0.0f, 50.0f);
  p.bowed_string.section_rate_hz =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.section_rate_hz, 5.0f), 0.5f, 12.0f);
  p.bowed_string.attack_noise =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.attack_noise, 0.0f), 0.0f, 1.0f);
  // Shares `attack_ms`'s upper bound; the lower bound is 0 because 0 is this
  // field's "keep the one-pole ramp" sentinel rather than an instant run-up.
  p.bowed_string.bow_accel_ms =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.bow_accel_ms, 0.0f), 0.0f, 2000.0f);
  p.bowed_string.corpus_scale =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.corpus_scale, 1.0f), 0.1f, 4.0f);
  p.bowed_string.corpus_tilt_hz =
      std::clamp(patch_clamp_detail::sanitize(p.bowed_string.corpus_tilt_hz, 0.0f), 0.0f, 20000.0f);
  if (static_cast<int>(p.bowed_string.corpus_series) < 0 ||
      static_cast<int>(p.bowed_string.corpus_series) > static_cast<int>(BowedCorpus::kContrabass)) {
    p.bowed_string.corpus_series = BowedCorpus::kNone;
  }
  p.bowed_string.string_loss_harmonic = std::clamp(
      patch_clamp_detail::sanitize(p.bowed_string.string_loss_harmonic, 0.0f), 0.0f, 1000.0f);
  p.bowed_string.string_loss_per_octave = std::clamp(
      patch_clamp_detail::sanitize(p.bowed_string.string_loss_per_octave, 0.0f), 0.0f, 4.0f);
  p.reed.breath_pressure =
      std::clamp(patch_clamp_detail::sanitize(p.reed.breath_pressure, 0.6f), 0.0f, 1.0f);
  p.reed.vel_to_breath =
      std::clamp(patch_clamp_detail::sanitize(p.reed.vel_to_breath, 0.6f), 0.0f, 1.0f);
  p.reed.reed_stiffness =
      std::clamp(patch_clamp_detail::sanitize(p.reed.reed_stiffness, 0.5f), 0.0f, 1.0f);
  p.reed.reed_opening =
      std::clamp(patch_clamp_detail::sanitize(p.reed.reed_opening, 0.5f), 0.0f, 1.0f);
  p.reed.brightness = std::clamp(patch_clamp_detail::sanitize(p.reed.brightness, 0.5f), 0.0f, 1.0f);
  p.reed.damping = std::clamp(patch_clamp_detail::sanitize(p.reed.damping, 0.4f), 0.0f, 1.0f);
  p.reed.bell_radiation_hz =
      std::clamp(patch_clamp_detail::sanitize(p.reed.bell_radiation_hz, 0.0f), 0.0f, 8000.0f);
  p.reed.attack_ms =
      std::clamp(patch_clamp_detail::sanitize(p.reed.attack_ms, 40.0f), 1.0f, 2000.0f);
  p.reed.release_ms =
      std::clamp(patch_clamp_detail::sanitize(p.reed.release_ms, 80.0f), 1.0f, 5000.0f);
  p.reed.breath_noise =
      std::clamp(patch_clamp_detail::sanitize(p.reed.breath_noise, 0.12f), 0.0f, 1.0f);
  p.reed.chiff = std::clamp(patch_clamp_detail::sanitize(p.reed.chiff, 0.4f), 0.0f, 1.0f);
  p.reed.chiff_ms = std::clamp(patch_clamp_detail::sanitize(p.reed.chiff_ms, 12.0f), 1.0f, 500.0f);
  p.reed.reed_resonance =
      std::clamp(patch_clamp_detail::sanitize(p.reed.reed_resonance, 0.5f), 0.0f, 1.0f);
  p.reed.register_vent =
      std::clamp(patch_clamp_detail::sanitize(p.reed.register_vent, 0.0f), 0.0f, 1.0f);
  p.reed.growl = std::clamp(patch_clamp_detail::sanitize(p.reed.growl, 0.0f), 0.0f, 1.0f);
  p.reed.cone_growth =
      std::clamp(patch_clamp_detail::sanitize(p.reed.cone_growth, 0.0f), 0.0f, 1.0f);
  p.reed.tonehole = std::clamp(patch_clamp_detail::sanitize(p.reed.tonehole, 0.0f), 0.0f, 1.0f);
  p.reed.closing_pressure =
      std::clamp(patch_clamp_detail::sanitize(p.reed.closing_pressure, 0.0f), 0.0f, 8.0f);
  p.reed.pressure_scale =
      std::clamp(patch_clamp_detail::sanitize(p.reed.pressure_scale, 1.0f), 0.0f, 4.0f);
  p.reed.flow_gain = std::clamp(patch_clamp_detail::sanitize(p.reed.flow_gain, 0.7f), 0.0f, 1.5f);
  p.reed.velocity_exponent =
      std::clamp(patch_clamp_detail::sanitize(p.reed.velocity_exponent, 2.0f), 0.0f, 4.0f);
  p.brass.breath_pressure =
      std::clamp(patch_clamp_detail::sanitize(p.brass.breath_pressure, 0.7f), 0.0f, 1.0f);
  p.brass.vel_to_breath =
      std::clamp(patch_clamp_detail::sanitize(p.brass.vel_to_breath, 0.6f), 0.0f, 1.0f);
  p.brass.lip_tension =
      std::clamp(patch_clamp_detail::sanitize(p.brass.lip_tension, 0.5f), 0.0f, 1.0f);
  p.brass.lip_damping =
      std::clamp(patch_clamp_detail::sanitize(p.brass.lip_damping, 0.5f), 0.0f, 1.0f);
  p.brass.brightness =
      std::clamp(patch_clamp_detail::sanitize(p.brass.brightness, 0.5f), 0.0f, 1.0f);
  p.brass.damping = std::clamp(patch_clamp_detail::sanitize(p.brass.damping, 0.4f), 0.0f, 1.0f);
  p.brass.attack_ms =
      std::clamp(patch_clamp_detail::sanitize(p.brass.attack_ms, 25.0f), 1.0f, 2000.0f);
  p.brass.release_ms =
      std::clamp(patch_clamp_detail::sanitize(p.brass.release_ms, 90.0f), 1.0f, 5000.0f);
  p.brass.breath_noise =
      std::clamp(patch_clamp_detail::sanitize(p.brass.breath_noise, 0.1f), 0.0f, 1.0f);
  p.brass.chiff = std::clamp(patch_clamp_detail::sanitize(p.brass.chiff, 0.35f), 0.0f, 1.0f);
  p.brass.chiff_ms =
      std::clamp(patch_clamp_detail::sanitize(p.brass.chiff_ms, 10.0f), 1.0f, 500.0f);
  p.brass.lip_aperture =
      std::clamp(patch_clamp_detail::sanitize(p.brass.lip_aperture, 0.0f), 0.0f, 1.0f);
  p.brass.bell_radiation_hz =
      std::clamp(patch_clamp_detail::sanitize(p.brass.bell_radiation_hz, 0.0f), 0.0f, 8000.0f);
  // Wider than bell_radiation_hz above, which is a fitted radiation corner: this
  // one is the flare itself and has to reach the corner a bore with no bell
  // would have, so the fit can be asked whether a bell is there at all.
  p.brass.bell_cutoff_hz =
      std::clamp(patch_clamp_detail::sanitize(p.brass.bell_cutoff_hz, 0.0f), 0.0f, 20000.0f);
  p.brass.brassiness =
      std::clamp(patch_clamp_detail::sanitize(p.brass.brassiness, 0.0f), 0.0f, 1.0f);
  p.brass.cuivre_dynamics =
      std::clamp(patch_clamp_detail::sanitize(p.brass.cuivre_dynamics, 0.0f), 0.0f, 1.0f);
  p.brass.bore_nonlinearity =
      std::clamp(patch_clamp_detail::sanitize(p.brass.bore_nonlinearity, 0.0f), 0.0f, 1.0f);
  p.brass.mute = std::clamp(patch_clamp_detail::sanitize(p.brass.mute, 0.0f), 0.0f, 1.0f);
  p.brass.half_valve =
      std::clamp(patch_clamp_detail::sanitize(p.brass.half_valve, 0.0f), 0.0f, 1.0f);
  p.brass.dynamic_lip =
      std::clamp(patch_clamp_detail::sanitize(p.brass.dynamic_lip, 0.0f), 0.0f, 1.0f);
  p.brass.onset_breath_overshoot_db =
      std::clamp(patch_clamp_detail::sanitize(p.brass.onset_breath_overshoot_db, 0.0f), 0.0f, 8.0f);
  p.brass.onset_breath_settle_ms =
      std::clamp(patch_clamp_detail::sanitize(p.brass.onset_breath_settle_ms, 40.0f), 1.0f, 500.0f);
  p.brass.onset_breath_overshoot_vel_db = std::clamp(
      patch_clamp_detail::sanitize(p.brass.onset_breath_overshoot_vel_db, 0.0f), -3.0f, 6.0f);
  p.brass.velocity_exponent =
      std::clamp(patch_clamp_detail::sanitize(p.brass.velocity_exponent, 0.93f), 0.0f, 4.0f);
  p.flute.breath_pressure =
      std::clamp(patch_clamp_detail::sanitize(p.flute.breath_pressure, 0.55f), 0.0f, 1.0f);
  p.flute.vel_to_breath =
      std::clamp(patch_clamp_detail::sanitize(p.flute.vel_to_breath, 0.5f), 0.0f, 1.0f);
  p.flute.jet_ratio = std::clamp(patch_clamp_detail::sanitize(p.flute.jet_ratio, 0.5f), 0.1f, 0.9f);
  p.flute.jet_reflection =
      std::clamp(patch_clamp_detail::sanitize(p.flute.jet_reflection, 0.5f), 0.0f, 1.0f);
  p.flute.end_reflection =
      std::clamp(patch_clamp_detail::sanitize(p.flute.end_reflection, 0.5f), 0.0f, 1.0f);
  p.flute.brightness =
      std::clamp(patch_clamp_detail::sanitize(p.flute.brightness, 0.5f), 0.0f, 1.0f);
  p.flute.damping = std::clamp(patch_clamp_detail::sanitize(p.flute.damping, 0.35f), 0.0f, 1.0f);
  p.flute.attack_ms =
      std::clamp(patch_clamp_detail::sanitize(p.flute.attack_ms, 18.0f), 1.0f, 2000.0f);
  p.flute.release_ms =
      std::clamp(patch_clamp_detail::sanitize(p.flute.release_ms, 90.0f), 1.0f, 5000.0f);
  p.flute.breath_noise =
      std::clamp(patch_clamp_detail::sanitize(p.flute.breath_noise, 0.15f), 0.0f, 1.0f);
  p.flute.chiff = std::clamp(patch_clamp_detail::sanitize(p.flute.chiff, 0.4f), 0.0f, 1.0f);
  p.flute.chiff_ms =
      std::clamp(patch_clamp_detail::sanitize(p.flute.chiff_ms, 12.0f), 1.0f, 500.0f);
  p.flute.vibrato_rate_hz =
      std::clamp(patch_clamp_detail::sanitize(p.flute.vibrato_rate_hz, 5.0f), 0.1f, 12.0f);
  p.flute.vibrato_depth =
      std::clamp(patch_clamp_detail::sanitize(p.flute.vibrato_depth, 0.0f), 0.0f, 1.0f);
  p.flute.overblow = std::clamp(patch_clamp_detail::sanitize(p.flute.overblow, 0.0f), 0.0f, 1.0f);
  p.flute.jet_turbulence =
      std::clamp(patch_clamp_detail::sanitize(p.flute.jet_turbulence, 0.0f), 0.0f, 1.0f);
  p.flute.edge_hysteresis =
      std::clamp(patch_clamp_detail::sanitize(p.flute.edge_hysteresis, 0.0f), 0.0f, 1.0f);
  p.flute.vortex = std::clamp(patch_clamp_detail::sanitize(p.flute.vortex, 0.0f), 0.0f, 1.0f);
  p.flute.vortex_loss =
      std::clamp(patch_clamp_detail::sanitize(p.flute.vortex_loss, 0.0f), 0.0f, 400.0f);
  p.flute.jet_offset =
      std::clamp(patch_clamp_detail::sanitize(p.flute.jet_offset, 0.0f), 0.0f, 2.0f);
  p.flute.lattice_cutoff_hz =
      std::clamp(patch_clamp_detail::sanitize(p.flute.lattice_cutoff_hz, 0.0f), 0.0f, 20000.0f);
  p.flute.jet_centring_per_octave =
      std::clamp(patch_clamp_detail::sanitize(p.flute.jet_centring_per_octave, 0.0f), 0.0f, 4.0f);
  p.flute.jet_receptivity_window =
      std::clamp(patch_clamp_detail::sanitize(p.flute.jet_receptivity_window, 0.0f), 0.0f, 0.7f);
  p.flute.jet_deflection_limit =
      std::clamp(patch_clamp_detail::sanitize(p.flute.jet_deflection_limit, 0.0f), 0.0f, 8.0f);
  p.flute.breath_radiation =
      std::clamp(patch_clamp_detail::sanitize(p.flute.breath_radiation, 0.0f), 0.0f, 1.0f);
  p.flute.onset_breath_overshoot_db =
      std::clamp(patch_clamp_detail::sanitize(p.flute.onset_breath_overshoot_db, 0.0f), 0.0f, 8.0f);
  p.flute.onset_breath_settle_ms =
      std::clamp(patch_clamp_detail::sanitize(p.flute.onset_breath_settle_ms, 40.0f), 1.0f, 500.0f);
  p.flute.onset_breath_overshoot_vel_db = std::clamp(
      patch_clamp_detail::sanitize(p.flute.onset_breath_overshoot_vel_db, 0.0f), -3.0f, 6.0f);
  p.flute.velocity_exponent =
      std::clamp(patch_clamp_detail::sanitize(p.flute.velocity_exponent, 2.0f), 0.0f, 4.0f);
  p.plucked_string.brightness =
      std::clamp(patch_clamp_detail::sanitize(p.plucked_string.brightness, 0.7f), 0.0f, 1.0f);
  p.plucked_string.decay_s =
      std::clamp(patch_clamp_detail::sanitize(p.plucked_string.decay_s, 4.0f), 0.05f, 60.0f);
  p.plucked_string.decay_stretch =
      std::clamp(patch_clamp_detail::sanitize(p.plucked_string.decay_stretch, 0.5f), 0.0f, 1.0f);
  p.plucked_string.pick_position =
      std::clamp(patch_clamp_detail::sanitize(p.plucked_string.pick_position, 0.2f), 0.0f, 0.5f);
  p.plucked_string.pick_position_per_octave = std::clamp(
      patch_clamp_detail::sanitize(p.plucked_string.pick_position_per_octave, 0.0f), 0.0f, 4.0f);
  p.plucked_string.exc_brightness =
      std::clamp(patch_clamp_detail::sanitize(p.plucked_string.exc_brightness, 0.85f), 0.0f, 1.0f);
  p.plucked_string.exc_brightness_per_octave = std::clamp(
      patch_clamp_detail::sanitize(p.plucked_string.exc_brightness_per_octave, 0.0f), 0.0f, 4.0f);
  p.plucked_string.vel_to_brightness = std::clamp(
      patch_clamp_detail::sanitize(p.plucked_string.vel_to_brightness, 0.6f), 0.0f, 1.0f);
  p.plucked_string.release_damp_s = std::clamp(
      patch_clamp_detail::sanitize(p.plucked_string.release_damp_s, 0.12f), 0.01f, 10.0f);
  p.plucked_string.hf_decay_s =
      std::clamp(patch_clamp_detail::sanitize(p.plucked_string.hf_decay_s, 0.0f), 0.0f, 60.0f);
  p.plucked_string.mid_decay_s =
      std::clamp(patch_clamp_detail::sanitize(p.plucked_string.mid_decay_s, 0.0f), 0.0f, 60.0f);
  p.plucked_string.inharmonicity_b =
      std::clamp(patch_clamp_detail::sanitize(p.plucked_string.inharmonicity_b, 0.0f), 0.0f, 0.01f);
  p.plucked_string.pick_noise =
      std::clamp(patch_clamp_detail::sanitize(p.plucked_string.pick_noise, 0.0f), 0.0f, 1.0f);
  p.plucked_string.buzz =
      std::clamp(patch_clamp_detail::sanitize(p.plucked_string.buzz, 0.0f), 0.0f, 1.0f);
  p.plucked_string.velocity_exponent = std::clamp(
      patch_clamp_detail::sanitize(p.plucked_string.velocity_exponent, 2.0f), 0.0f, 4.0f);
  p.vocal.vowel = std::clamp(p.vocal.vowel, 0, kVocalVowels - 1);
  p.vocal.brightness =
      std::clamp(patch_clamp_detail::sanitize(p.vocal.brightness, 0.5f), 0.0f, 1.0f);
  p.vocal.breath_noise =
      std::clamp(patch_clamp_detail::sanitize(p.vocal.breath_noise, 0.1f), 0.0f, 1.0f);
  p.vocal.vibrato_rate_hz =
      std::clamp(patch_clamp_detail::sanitize(p.vocal.vibrato_rate_hz, 5.5f), 0.1f, 12.0f);
  p.vocal.vibrato_depth =
      std::clamp(patch_clamp_detail::sanitize(p.vocal.vibrato_depth, 0.3f), 0.0f, 1.0f);
  p.vocal.attack_ms =
      std::clamp(patch_clamp_detail::sanitize(p.vocal.attack_ms, 30.0f), 1.0f, 2000.0f);
  p.vocal.release_ms =
      std::clamp(patch_clamp_detail::sanitize(p.vocal.release_ms, 120.0f), 1.0f, 5000.0f);
  p.vocal.velocity_exponent =
      std::clamp(patch_clamp_detail::sanitize(p.vocal.velocity_exponent, 2.0f), 0.0f, 4.0f);
  p.vocal.section_spread_cents =
      std::clamp(patch_clamp_detail::sanitize(p.vocal.section_spread_cents, 0.0f), 0.0f, 50.0f);
  p.vocal.section_rate_hz =
      std::clamp(patch_clamp_detail::sanitize(p.vocal.section_rate_hz, 5.0f), 0.5f, 12.0f);
  p.free_reed.brightness =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.brightness, 0.6f), 0.0f, 1.0f);
  p.free_reed.reed_stiffness =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.reed_stiffness, 0.5f), 0.0f, 1.0f);
  p.free_reed.breath_pressure =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.breath_pressure, 0.7f), 0.0f, 1.0f);
  p.free_reed.vel_to_breath =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.vel_to_breath, 0.5f), 0.0f, 1.0f);
  p.free_reed.detune =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.detune, 0.3f), 0.0f, 1.0f);
  p.free_reed.musette_partner_level =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.musette_partner_level, 1.0f), 0.0f, 1.0f);
  p.free_reed.musette_taper =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.musette_taper, 0.0f), 0.0f, 1.0f);
  p.free_reed.attack_ms =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.attack_ms, 20.0f), 1.0f, 2000.0f);
  p.free_reed.release_ms =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.release_ms, 80.0f), 1.0f, 5000.0f);
  p.free_reed.breath_noise =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.breath_noise, 0.08f), 0.0f, 1.0f);
  p.free_reed.slot_duty =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.slot_duty, 0.0f), 0.0f, 0.9f);
  p.free_reed.slot_return =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.slot_return, 0.4f), 0.0f, 1.5f);
  p.free_reed.slot_gap =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.slot_gap, 0.14f), 0.02f, 0.5f);
  p.free_reed.slot_swing =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.slot_swing, 0.0f), 0.0f, 16.0f);
  p.free_reed.radiation =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.radiation, 1.0f), 0.0f, 1.0f);
  p.free_reed.radiation_keytrack =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.radiation_keytrack, 0.0f), 0.0f, 1.0f);
  p.free_reed.onset_breath_overshoot_db = std::clamp(
      patch_clamp_detail::sanitize(p.free_reed.onset_breath_overshoot_db, 0.0f), 0.0f, 8.0f);
  p.free_reed.onset_breath_settle_ms = std::clamp(
      patch_clamp_detail::sanitize(p.free_reed.onset_breath_settle_ms, 40.0f), 1.0f, 500.0f);
  p.free_reed.onset_breath_overshoot_vel_db = std::clamp(
      patch_clamp_detail::sanitize(p.free_reed.onset_breath_overshoot_vel_db, 0.0f), -3.0f, 6.0f);
  p.free_reed.onset_breath_overshoot_key_db = std::clamp(
      patch_clamp_detail::sanitize(p.free_reed.onset_breath_overshoot_key_db, 0.0f), -6.0f, 6.0f);
  p.free_reed.velocity_exponent =
      std::clamp(patch_clamp_detail::sanitize(p.free_reed.velocity_exponent, 2.0f), 0.0f, 4.0f);
  p.harpsichord.pluck_8a =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.pluck_8a, 0.14f), 0.0f, 0.5f);
  p.harpsichord.pluck_8b =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.pluck_8b, 0.22f), 0.0f, 0.5f);
  p.harpsichord.pluck_4 =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.pluck_4, 0.11f), 0.0f, 0.5f);
  p.harpsichord.plectrum_edge =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.plectrum_edge, 0.8f), 0.0f, 1.0f);
  p.harpsichord.end_reflection =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.end_reflection, 1.0f), 0.0f, 1.0f);
  // The instrument's own range is 3 to 6 dB; the ceiling leaves room to voice a
  // stop that is deliberately more responsive without letting a patch turn the
  // harpsichord into a piano.
  p.harpsichord.velocity_range_db =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.velocity_range_db, 5.0f), 0.0f, 24.0f);
  p.harpsichord.velocity_droop_db =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.velocity_droop_db, 0.0f), 0.0f, 12.0f);
  p.harpsichord.decay_s =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.decay_s, 3.0f), 0.05f, 60.0f);
  p.harpsichord.decay_stretch =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.decay_stretch, 0.7f), 0.0f, 2.0f);
  p.harpsichord.hf_damping =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.hf_damping, 0.45f), 0.05f, 1.0f);
  p.harpsichord.damping_ref_hz = std::clamp(
      patch_clamp_detail::sanitize(p.harpsichord.damping_ref_hz, 2000.0f), 100.0f, 20000.0f);
  p.harpsichord.unison_detune_cents = std::clamp(
      patch_clamp_detail::sanitize(p.harpsichord.unison_detune_cents, 3.5f), -50.0f, 50.0f);
  p.harpsichord.octave_detune_cents = std::clamp(
      patch_clamp_detail::sanitize(p.harpsichord.octave_detune_cents, 2.0f), -50.0f, 50.0f);
  p.harpsichord.ripple_loss_share =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.ripple_loss_share, 0.0f), 0.0f, 0.4f);
  p.harpsichord.ripple_ratio =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.ripple_ratio, 0.5f), 0.05f, 0.95f);
  p.harpsichord.pol_coupling =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.pol_coupling, 0.0f), 0.0f, 1.0f);
  p.harpsichord.pol_detune_cents =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.pol_detune_cents, 1.0f), -50.0f, 50.0f);
  p.harpsichord.pol_t60_ratio =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.pol_t60_ratio, 0.55f), 0.05f, 2.0f);
  p.harpsichord.rear_segment_mm =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.rear_segment_mm, 0.0f), 0.0f, 400.0f);
  p.harpsichord.rear_coupling =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.rear_coupling, 0.35f), 0.0f, 1.0f);
  // The floor is the inherit sentinel, not a decay; the ceiling is the longest
  // the speaking strings themselves are allowed.
  p.harpsichord.rear_decay_s =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.rear_decay_s, 0.0f), 0.0f, 60.0f);
  p.harpsichord.scale_c5_mm =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.scale_c5_mm, 280.0f), 80.0f, 800.0f);
  p.harpsichord.bass_foreshortening = std::clamp(
      patch_clamp_detail::sanitize(p.harpsichord.bass_foreshortening, 0.35f), 0.0f, 0.9f);
  p.harpsichord.pluck_noise =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.pluck_noise, 0.0f), 0.0f, 1.0f);
  p.harpsichord.jack_noise =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.jack_noise, 0.0f), 0.0f, 1.0f);
  p.harpsichord.damper_s =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.damper_s, 0.09f), 0.005f, 10.0f);
  // The floor is the off sentinel; the ceiling is the middle of the compass,
  // past which the board would not be radiating the instrument at all.
  p.harpsichord.board_radiating_from_hz = std::clamp(
      patch_clamp_detail::sanitize(p.harpsichord.board_radiating_from_hz, 0.0f), 0.0f, 500.0f);
  // The ceiling is one first-order section's whole slope: past it the board
  // would be differentiating its own drive rather than radiating it.
  p.harpsichord.board_tilt_db_oct =
      std::clamp(patch_clamp_detail::sanitize(p.harpsichord.board_tilt_db_oct, 0.0f), 0.0f, 6.0f);
  // The floor is the off sentinel and the ceiling is the strings' own level: a
  // board radiating more than what drives it is not a board.
  p.harpsichord.board_diffuse_db = std::clamp(
      patch_clamp_detail::sanitize(p.harpsichord.board_diffuse_db, -120.0f), -120.0f, 0.0f);
  // A negative set is the "no keymap" sentinel; anything else is an index the
  // bank either has or does not, which is its business rather than the clamp's.
  if (p.sample.set_index < -1) p.sample.set_index = -1;
  p.sample.level = std::clamp(patch_clamp_detail::sanitize(p.sample.level, 1.0f), 0.0f, 8.0f);
  if (p.sample.loop_override != 1 && p.sample.loop_override != 3 && p.sample.loop_override != 0) {
    p.sample.loop_override = -1;
  }
  p.sample.start_offset01 =
      std::clamp(patch_clamp_detail::sanitize(p.sample.start_offset01, 0.0f), 0.0f, 0.999f);
  // Bound by the enum's last member, not a literal: the literal was 4 and
  // outlived kVocal being added at 5, so every vocal body was reset to none.
  if (static_cast<int>(p.body) < 0 ||
      static_cast<int>(p.body) > static_cast<int>(BodyType::kVocal)) {
    p.body = BodyType::kNone;
  }
  p.body_mix = std::clamp(patch_clamp_detail::sanitize(p.body_mix, 0.0f), 0.0f, 1.0f);
  p.stereo_spread = std::clamp(patch_clamp_detail::sanitize(p.stereo_spread, 0.0f), 0.0f, 1.0f);
  return p;
}

}  // namespace sonare::midi::synth
