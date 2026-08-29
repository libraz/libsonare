#pragma once

/// @file patch_sections.h
/// @brief Blanks the engine sections a patch does not voice.
/// @details A `NativeSynthPatch` carries all sixteen engines' parameters side
/// by side and voices exactly one, chosen by `mode`. The other fifteen sit at
/// defaults that are mostly non-zero, and `clamp_synth_patch` raises several of
/// them to a non-zero lower bound, so every one of the bank's ~290
/// constant-initialised patches writes fifteen unread sections into the
/// WebAssembly data section. Blanking them where the table is stored deletes
/// that: each section is read only under its own `mode`, so no rendered sample
/// moves.
///
/// Applied after the clamp and after the tuning override layer, so
/// `clamp_synth_patch` still measures every field's bounds for the
/// `SONARE_TUNING_DUMP` catalogue and a fitted override still reaches the patch
/// it belongs to.
///
/// Each blank names every field of its struct rather than only the fields whose
/// default is non-zero, and `patch_sections_test` asserts the result is
/// byte-zero — a field added to an engine and not added here fails as a red test
/// rather than as bytes nobody counts.

#include <cstdint>

#include "midi/synth/native_synth.h"

namespace sonare::midi::synth {

constexpr FmOperatorParams blank_fm_operator() noexcept {
  FmOperatorParams z{};
  z.ratio = 0.0f;
  z.detune_cents = 0.0f;
  z.level = 0.0f;
  z.env = DahdsrConfig{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  z.vel_to_level = 0.0f;
  z.key_rate_scale = 0.0f;
  z.feedback = 0.0f;
  return z;
}

constexpr ModalMode blank_modal_mode() noexcept {
  ModalMode z{};
  z.ratio = 0.0f;
  z.gain = 0.0f;
  z.decay_scale = 0.0f;
  return z;
}

constexpr PipeOrganRank blank_pipe_organ_rank() noexcept {
  PipeOrganRank z{};
  z.footage_mult = 0.0f;
  z.stopped = false;
  z.brightness = 0.0f;
  z.level = 0.0f;
  z.reed = 0.0f;
  z.radiation = 0.0f;
  return z;
}

constexpr FmPatchParams blank_fm() noexcept {
  FmPatchParams z{};
  z.algorithm = static_cast<FmAlgorithm>(0);
  for (FmOperatorParams& v : z.ops) v = blank_fm_operator();
  return z;
}

constexpr KsPatchParams blank_ks() noexcept {
  KsPatchParams z{};
  z.brightness = 0.0f;
  z.decay_s = 0.0f;
  z.decay_stretch = 0.0f;
  z.pick_position = 0.0f;
  z.exc_brightness = 0.0f;
  z.vel_to_brightness = 0.0f;
  z.release_damp_s = 0.0f;
  z.slap = 0.0f;
  z.polarization = 0.0f;
  z.body_coupling = 0.0f;
  z.pluck_style = 0.0f;
  z.nail = 0.0f;
  z.sympathetic = false;
  z.pickup_pos = 0.0f;
  z.dispersion = 0.0f;
  z.tension_mod = 0.0f;
  z.octave_mix = 0.0f;
  z.harmonic_node = 0.0f;
  z.keyoff_noise = 0.0f;
  return z;
}

constexpr ModalPatchParams blank_modal() noexcept {
  ModalPatchParams z{};
  z.num_modes = 0;
  for (ModalMode& v : z.modes) v = blank_modal_mode();
  z.decay_s = 0.0f;
  z.decay_stretch = 0.0f;
  z.strike_brightness = 0.0f;
  z.vel_to_brightness = 0.0f;
  z.release_damp_s = 0.0f;
  return z;
}

constexpr AdditivePatchParams blank_additive() noexcept {
  AdditivePatchParams z{};
  for (float& v : z.drawbars) v = 0.0f;
  z.key_click = 0.0f;
  z.click_decay_ms = 0.0f;
  z.percussion_harmonic = 0;
  z.percussion_decay_ms = 0.0f;
  z.percussion_level = 0.0f;
  return z;
}

constexpr PercussionPatchParams blank_percussion() noexcept {
  PercussionPatchParams z{};
  z.gm_kit = false;
  z.exclusive_class = 0;
  z.num_modes = 0;
  for (float& v : z.mode_ratios) v = 0.0f;
  z.mode_decay_s = 0.0f;
  z.tone_gain = 0.0f;
  z.tone_direct = 0.0f;
  z.base_freq_hz = 0.0f;
  z.pitch_drop = 0.0f;
  z.pitch_drop_ms = 0.0f;
  z.strike_r = 0.0f;
  z.strike_theta = 0.0f;
  for (uint8_t& v : z.mode_m) v = 0;
  for (float& v : z.mode_alpha) v = 0.0f;
  z.noise_gain = 0.0f;
  z.noise_decay_ms = 0.0f;
  z.noise_cutoff_hz = 0.0f;
  z.noise_q = 0.0f;
  z.noise_output = static_cast<SynthFilterOutput>(0);
  z.noise_air_hz = 0.0f;
  z.shell_mix = 0.0f;
  z.shell_num_modes = 0;
  for (float& v : z.shell_freq_hz) v = 0.0f;
  for (float& v : z.shell_t60_s) v = 0.0f;
  for (float& v : z.shell_weight) v = 0.0f;
  z.wire_buzz = 0.0f;
  z.wire_threshold = 0.0f;
  z.wire_cutoff_hz = 0.0f;
  z.shimmer = 0.0f;
  z.shimmer_attack_ms = 0.0f;
  z.shimmer_cutoff_hz = 0.0f;
  z.contact = 0.0f;
  z.contact_ms = 0.0f;
  z.plate_gain = 0.0f;
  z.plate_t60_s = 0.0f;
  z.plate_hf_ratio = 0.0f;
  z.plate_low_hz = 0.0f;
  z.plate_air_hz = 0.0f;
  z.phisem_beans = 0.0f;
  z.phisem_energy_ms = 0.0f;
  z.phisem_sound_ms = 0.0f;
  z.phisem_res_hz = 0.0f;
  z.phisem_res_q = 0.0f;
  z.phisem_body_hz = 0.0f;
  z.phisem_body_q = 0.0f;
  z.phisem_body_gain = 0.0f;
  z.phisem_scrape_hz = 0.0f;
  z.phisem_pitch_glide = 0.0f;
  return z;
}

constexpr PianoPatchParams blank_piano() noexcept {
  PianoPatchParams z{};
  z.strings = 0;
  z.detune_cents = 0.0f;
  z.decay_fast_s = 0.0f;
  z.decay_slow_s = 0.0f;
  z.decay_stretch = 0.0f;
  z.brightness = 0.0f;
  z.dispersion = 0.0f;
  z.strike_position = 0.0f;
  z.hammer_exponent = 0.0f;
  z.hammer_contact_ms = 0.0f;
  z.hammer_dynamics = 0.0f;
  z.soundboard = 0.0f;
  z.release_damp_s = 0.0f;
  return z;
}

constexpr PipeOrganPatchParams blank_pipe_organ() noexcept {
  PipeOrganPatchParams z{};
  z.stopped = false;
  z.brightness = 0.0f;
  z.tone_decay_s = 0.0f;
  z.breath = 0.0f;
  z.chiff = 0.0f;
  z.chiff_ms = 0.0f;
  z.release_damp_s = 0.0f;
  z.reed = 0.0f;
  z.radiation = 0.0f;
  z.keytrack = 0.0f;
  z.rank_count = 0;
  for (PipeOrganRank& v : z.ranks) v = blank_pipe_organ_rank();
  z.tremulant_rate_hz = 0.0f;
  z.tremulant_depth = 0.0f;
  z.wind_sag = 0.0f;
  z.swell = 0.0f;
  return z;
}

constexpr BowedStringPatchParams blank_bowed_string() noexcept {
  BowedStringPatchParams z{};
  z.bow_position = 0.0f;
  z.bow_force = 0.0f;
  z.bow_speed = 0.0f;
  z.vel_to_speed = 0.0f;
  z.brightness = 0.0f;
  z.damping = 0.0f;
  z.attack_ms = 0.0f;
  z.release_ms = 0.0f;
  z.rosin = 0.0f;
  z.elasto_plastic = false;
  z.stribeck = 0.0f;
  z.sympathetic = 0.0f;
  z.polarization = 0.0f;
  return z;
}

constexpr ReedPatchParams blank_reed() noexcept {
  ReedPatchParams z{};
  z.breath_pressure = 0.0f;
  z.vel_to_breath = 0.0f;
  z.reed_stiffness = 0.0f;
  z.reed_opening = 0.0f;
  z.conical = false;
  z.brightness = 0.0f;
  z.damping = 0.0f;
  z.attack_ms = 0.0f;
  z.release_ms = 0.0f;
  z.breath_noise = 0.0f;
  z.chiff = 0.0f;
  z.chiff_ms = 0.0f;
  z.dynamic_reed = false;
  z.reed_resonance = 0.0f;
  z.register_vent = 0.0f;
  z.growl = 0.0f;
  z.cone_growth = 0.0f;
  z.tonehole = 0.0f;
  return z;
}

constexpr BrassPatchParams blank_brass() noexcept {
  BrassPatchParams z{};
  z.breath_pressure = 0.0f;
  z.vel_to_breath = 0.0f;
  z.lip_tension = 0.0f;
  z.lip_damping = 0.0f;
  z.conical = false;
  z.brightness = 0.0f;
  z.damping = 0.0f;
  z.attack_ms = 0.0f;
  z.release_ms = 0.0f;
  z.breath_noise = 0.0f;
  z.chiff = 0.0f;
  z.chiff_ms = 0.0f;
  z.brassiness = 0.0f;
  z.cuivre_dynamics = 0.0f;
  z.mute = 0.0f;
  z.half_valve = 0.0f;
  z.dynamic_lip = 0.0f;
  return z;
}

constexpr FlutePatchParams blank_flute() noexcept {
  FlutePatchParams z{};
  z.breath_pressure = 0.0f;
  z.vel_to_breath = 0.0f;
  z.jet_ratio = 0.0f;
  z.jet_reflection = 0.0f;
  z.end_reflection = 0.0f;
  z.brightness = 0.0f;
  z.damping = 0.0f;
  z.attack_ms = 0.0f;
  z.release_ms = 0.0f;
  z.breath_noise = 0.0f;
  z.chiff = 0.0f;
  z.chiff_ms = 0.0f;
  z.vibrato_rate_hz = 0.0f;
  z.vibrato_depth = 0.0f;
  z.overblow = 0.0f;
  z.jet_turbulence = 0.0f;
  z.edge_hysteresis = 0.0f;
  z.vortex = 0.0f;
  return z;
}

constexpr PluckedStringPatchParams blank_plucked_string() noexcept {
  PluckedStringPatchParams z{};
  z.brightness = 0.0f;
  z.decay_s = 0.0f;
  z.decay_stretch = 0.0f;
  z.pick_position = 0.0f;
  z.exc_brightness = 0.0f;
  z.vel_to_brightness = 0.0f;
  z.release_damp_s = 0.0f;
  z.buzz = 0.0f;
  return z;
}

constexpr VocalPatchParams blank_vocal() noexcept {
  VocalPatchParams z{};
  z.vowel = 0;
  z.brightness = 0.0f;
  z.breath_noise = 0.0f;
  z.vibrato_rate_hz = 0.0f;
  z.vibrato_depth = 0.0f;
  z.attack_ms = 0.0f;
  z.release_ms = 0.0f;
  return z;
}

constexpr FreeReedPatchParams blank_free_reed() noexcept {
  FreeReedPatchParams z{};
  z.brightness = 0.0f;
  z.reed_stiffness = 0.0f;
  z.breath_pressure = 0.0f;
  z.vel_to_breath = 0.0f;
  z.detune = 0.0f;
  z.attack_ms = 0.0f;
  z.release_ms = 0.0f;
  z.breath_noise = 0.0f;
  return z;
}

constexpr HarpsichordPatchParams blank_harpsichord() noexcept {
  HarpsichordPatchParams z{};
  z.eight_a = false;
  z.eight_b = false;
  z.four = false;
  z.pluck_8a = 0.0f;
  z.pluck_8b = 0.0f;
  z.pluck_4 = 0.0f;
  z.plectrum_edge = 0.0f;
  z.end_reflection = 0.0f;
  z.velocity_range_db = 0.0f;
  z.peak_velocity = 0;
  z.velocity_droop_db = 0.0f;
  z.decay_s = 0.0f;
  z.decay_stretch = 0.0f;
  z.hf_damping = 0.0f;
  z.damping_ref_hz = 0.0f;
  z.unison_detune_cents = 0.0f;
  z.octave_detune_cents = 0.0f;
  z.rear_segment_mm = 0.0f;
  z.rear_coupling = 0.0f;
  z.rear_decay_s = 0.0f;
  z.scale_c5_mm = 0.0f;
  z.bass_foreshortening = 0.0f;
  z.pluck_noise = 0.0f;
  z.jack_noise = 0.0f;
  z.damper_s = 0.0f;
  z.board_radiating_from_hz = 0.0f;
  z.board_tilt_db_oct = 0.0f;
  z.board_diffuse_db = 0.0f;
  z.undamped_from_note = 0;
  return z;
}

/// Returns @p patch with every engine section other than the one `mode` selects
/// replaced by zeroes. `kSubtractive` voices none of them, so it blanks all
/// fifteen.
constexpr NativeSynthPatch strip_unvoiced_sections(const NativeSynthPatch& patch) noexcept {
  NativeSynthPatch p = patch;
  if (p.mode != SynthEngineMode::kFm) p.fm = blank_fm();
  if (p.mode != SynthEngineMode::kKarplusStrong) p.ks = blank_ks();
  if (p.mode != SynthEngineMode::kModal) p.modal = blank_modal();
  if (p.mode != SynthEngineMode::kAdditive) p.additive = blank_additive();
  if (p.mode != SynthEngineMode::kPercussion) p.percussion = blank_percussion();
  if (p.mode != SynthEngineMode::kPiano) p.piano = blank_piano();
  if (p.mode != SynthEngineMode::kPipeOrgan) p.pipe_organ = blank_pipe_organ();
  if (p.mode != SynthEngineMode::kBowedString) p.bowed_string = blank_bowed_string();
  if (p.mode != SynthEngineMode::kReed) p.reed = blank_reed();
  if (p.mode != SynthEngineMode::kBrass) p.brass = blank_brass();
  if (p.mode != SynthEngineMode::kFlute) p.flute = blank_flute();
  if (p.mode != SynthEngineMode::kPluckedString) p.plucked_string = blank_plucked_string();
  if (p.mode != SynthEngineMode::kVocal) p.vocal = blank_vocal();
  if (p.mode != SynthEngineMode::kFreeReed) p.free_reed = blank_free_reed();
  if (p.mode != SynthEngineMode::kHarpsichord) p.harpsichord = blank_harpsichord();
  return p;
}

}  // namespace sonare::midi::synth
