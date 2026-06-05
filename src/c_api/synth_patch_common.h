#pragma once

/// @file synth_patch_common.h
/// @brief Shared SonareSynthPatch -> NativeSynthConfig conversion used by the
///        project bounce surface and the realtime engine surface (the 2-host
///        principle: one patch struct drives both).

#include <algorithm>
#include <cstring>

#include "midi/synth/native_synth.h"
#include "midi/synth/synth_presets.h"
#include "sonare_c_types.h"

namespace sonare_c_detail {

static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kSubtractive) + 1 ==
              SONARE_SYNTH_ENGINE_SUBTRACTIVE);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kFm) + 1 ==
              SONARE_SYNTH_ENGINE_FM);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kKarplusStrong) + 1 ==
              SONARE_SYNTH_ENGINE_KARPLUS_STRONG);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kModal) + 1 ==
              SONARE_SYNTH_ENGINE_MODAL);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kAdditive) + 1 ==
              SONARE_SYNTH_ENGINE_ADDITIVE);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kPercussion) + 1 ==
              SONARE_SYNTH_ENGINE_PERCUSSION);
static_assert(static_cast<int>(sonare::midi::synth::SynthEngineMode::kPiano) + 1 ==
              SONARE_SYNTH_ENGINE_PIANO);

static_assert(static_cast<int>(sonare::midi::synth::VaWaveform::kSine) + 1 ==
              SONARE_SYNTH_OSC_SINE);
static_assert(static_cast<int>(sonare::midi::synth::VaWaveform::kSaw) + 1 == SONARE_SYNTH_OSC_SAW);
static_assert(static_cast<int>(sonare::midi::synth::VaWaveform::kSquare) + 1 ==
              SONARE_SYNTH_OSC_SQUARE);
static_assert(static_cast<int>(sonare::midi::synth::VaWaveform::kTriangle) + 1 ==
              SONARE_SYNTH_OSC_TRIANGLE);
static_assert(static_cast<int>(sonare::midi::synth::VaWaveform::kNoise) + 1 ==
              SONARE_SYNTH_OSC_NOISE);

static_assert(static_cast<int>(sonare::midi::synth::SynthFilterModel::kSvf) + 1 ==
              SONARE_SYNTH_FILTER_SVF);
static_assert(static_cast<int>(sonare::midi::synth::SynthFilterModel::kMoogLadder) + 1 ==
              SONARE_SYNTH_FILTER_MOOG_LADDER);
static_assert(static_cast<int>(sonare::midi::synth::SynthFilterModel::kDiodeLadder) + 1 ==
              SONARE_SYNTH_FILTER_DIODE_LADDER);
static_assert(static_cast<int>(sonare::midi::synth::SynthFilterModel::kSallenKey) + 1 ==
              SONARE_SYNTH_FILTER_SALLEN_KEY);

static_assert(static_cast<int>(sonare::midi::synth::SynthFilterOutput::kLowpass) + 1 ==
              SONARE_SYNTH_FILTER_OUT_LOWPASS);
static_assert(static_cast<int>(sonare::midi::synth::SynthFilterOutput::kBandpass) + 1 ==
              SONARE_SYNTH_FILTER_OUT_BANDPASS);
static_assert(static_cast<int>(sonare::midi::synth::SynthFilterOutput::kHighpass) + 1 ==
              SONARE_SYNTH_FILTER_OUT_HIGHPASS);

static_assert(static_cast<int>(sonare::midi::synth::BodyType::kNone) + 1 == SONARE_SYNTH_BODY_NONE);
static_assert(static_cast<int>(sonare::midi::synth::BodyType::kGuitar) + 1 ==
              SONARE_SYNTH_BODY_GUITAR);
static_assert(static_cast<int>(sonare::midi::synth::BodyType::kViolin) + 1 ==
              SONARE_SYNTH_BODY_VIOLIN);
static_assert(static_cast<int>(sonare::midi::synth::BodyType::kWoodTube) + 1 ==
              SONARE_SYNTH_BODY_WOOD_TUBE);

static_assert(static_cast<int>(sonare::midi::synth::ModSource::kRandom) + 1 ==
              SONARE_SYNTH_MOD_SOURCE_COUNT);
static_assert(static_cast<int>(sonare::midi::synth::ModDestination::kPanUnits) + 1 ==
              SONARE_SYNTH_MOD_DESTINATION_COUNT);

inline sonare::midi::synth::ModSource mod_source_from_c(int value) noexcept {
  using sonare::midi::synth::ModSource;
  if (value < static_cast<int>(ModSource::kNone) || value > static_cast<int>(ModSource::kRandom)) {
    return ModSource::kNone;
  }
  return static_cast<ModSource>(value);
}

inline sonare::midi::synth::ModDestination mod_destination_from_c(int value) noexcept {
  using sonare::midi::synth::ModDestination;
  if (value < static_cast<int>(ModDestination::kNone) ||
      value > static_cast<int>(ModDestination::kPanUnits)) {
    return ModDestination::kNone;
  }
  return static_cast<ModDestination>(value);
}

/// Resolves a versioned C synth patch onto a NativeSynthConfig: the base is
/// the named preset (or the default subtractive patch when @p c.preset is
/// empty), then every non-zero struct field overrides the base ("0 => keep").
/// Struct version 1 has no per-field presence bits, so explicit zero numeric
/// overrides are intentionally indistinguishable from "keep".
/// Returns false (and sets @p out_error) for an unsupported struct_version or
/// an unknown preset name. The result still passes through NativeSynth's own
/// constructor clamping.
inline bool synth_config_from_patch_c(const SonareSynthPatch& c,
                                      sonare::midi::synth::NativeSynthConfig* out,
                                      const char** out_error) {
  using sonare::midi::synth::BodyType;
  using sonare::midi::synth::find_synth_preset;
  using sonare::midi::synth::kMaxModRoutes;
  using sonare::midi::synth::ModDestination;
  using sonare::midi::synth::ModSource;
  using sonare::midi::synth::NativeSynthConfig;
  using sonare::midi::synth::SynthEngineMode;
  using sonare::midi::synth::SynthFilterModel;
  using sonare::midi::synth::SynthFilterOutput;
  using sonare::midi::synth::VaWaveform;

  if (out_error) *out_error = nullptr;
  if (c.struct_version > 1) {
    if (out_error) *out_error = "unsupported SonareSynthPatch struct_version";
    return false;
  }
  NativeSynthConfig cfg;
  if (c.preset[0] != '\0') {
    // Defensive copy: the fixed field may legally lack a terminator.
    char name[SONARE_SYNTH_PRESET_NAME_MAX + 1] = {};
    std::memcpy(name, c.preset, SONARE_SYNTH_PRESET_NAME_MAX);
    const sonare::midi::synth::SynthPreset* preset = find_synth_preset(name);
    if (preset == nullptr) {
      if (out_error) *out_error = "unknown synth preset name";
      return false;
    }
    cfg = preset->config;
  }
  sonare::midi::synth::NativeSynthPatch& p = cfg.patch;

  if (c.engine_mode > 0) p.mode = static_cast<SynthEngineMode>(c.engine_mode - 1);
  // Oscillator section.
  if (c.waveform > 0) p.waveform = static_cast<VaWaveform>(c.waveform - 1);
  if (c.unison != 0) p.unison = c.unison;
  if (c.detune_cents != 0.0f) p.detune_cents = c.detune_cents;
  if (c.drift_cents != 0.0f) p.drift_cents = c.drift_cents;
  if (c.drive != 0.0f) p.drive = c.drive;
  // Filter section.
  if (c.filter_model > 0) p.filter_model = static_cast<SynthFilterModel>(c.filter_model - 1);
  if (c.filter_output > 0) p.filter_output = static_cast<SynthFilterOutput>(c.filter_output - 1);
  if (c.cutoff_hz != 0.0f) p.cutoff_hz = c.cutoff_hz;
  if (c.resonance_q != 0.0f) p.resonance_q = c.resonance_q;
  if (c.key_track != 0.0f) p.key_track = c.key_track;
  if (c.env_to_cutoff_cents != 0.0f) p.env_to_cutoff_cents = c.env_to_cutoff_cents;
  if (c.vel_to_cutoff_cents != 0.0f) p.vel_to_cutoff_cents = c.vel_to_cutoff_cents;
  // Envelopes.
  if (c.amp_attack_ms != 0.0f) p.amp_env.attack_ms = c.amp_attack_ms;
  if (c.amp_decay_ms != 0.0f) p.amp_env.decay_ms = c.amp_decay_ms;
  if (c.amp_sustain != 0.0f) p.amp_env.sustain = c.amp_sustain;
  if (c.amp_release_ms != 0.0f) p.amp_env.release_ms = c.amp_release_ms;
  if (c.filter_attack_ms != 0.0f) p.filter_env.attack_ms = c.filter_attack_ms;
  if (c.filter_decay_ms != 0.0f) p.filter_env.decay_ms = c.filter_decay_ms;
  if (c.filter_sustain != 0.0f) p.filter_env.sustain = c.filter_sustain;
  if (c.filter_release_ms != 0.0f) p.filter_env.release_ms = c.filter_release_ms;
  // LFOs / glide.
  if (c.lfo_rate_hz != 0.0f) p.lfo_rate_hz = c.lfo_rate_hz;
  if (c.lfo_to_pitch_cents != 0.0f) p.lfo_to_pitch_cents = c.lfo_to_pitch_cents;
  if (c.lfo2_rate_hz != 0.0f) p.lfo2_rate_hz = c.lfo2_rate_hz;
  if (c.glide_ms != 0.0f) p.glide_ms = c.glide_ms;
  // Realism polish.
  if (c.body > 0) p.body = static_cast<BodyType>(c.body - 1);
  if (c.body_mix != 0.0f) p.body_mix = c.body_mix;
  if (c.stereo_spread != 0.0f) p.stereo_spread = c.stereo_spread;
  // Mod matrix: a non-empty C table replaces the base matrix.
  if (c.num_mod_routings > 0) {
    p.mod_matrix = {};
    const int count = std::min(c.num_mod_routings, kMaxModRoutes);
    for (int i = 0; i < count; ++i) {
      const SonareSynthModRouting& r = c.mod_routings[i];
      p.mod_matrix.routes[static_cast<size_t>(i)] = {
          mod_source_from_c(r.source), mod_destination_from_c(r.destination), r.depth};
    }
  }
  // Voice pool / bus.
  if (c.gain != 0.0f) cfg.gain = c.gain;
  if (c.polyphony != 0) cfg.polyphony = c.polyphony;
  if (c.bus_drive != 0.0f) cfg.bus_drive = c.bus_drive;

  *out = cfg;
  return true;
}

/// Fills a versioned C synth patch from a catalog preset (the read direction:
/// preset name + the wrapper-section values, so hosts can inspect and tweak).
inline void synth_patch_to_c(const sonare::midi::synth::SynthPreset& preset,
                             SonareSynthPatch* out) {
  const sonare::midi::synth::NativeSynthPatch& p = preset.config.patch;
  *out = SonareSynthPatch{};
  out->struct_version = 1;
  std::strncpy(out->preset, preset.name, SONARE_SYNTH_PRESET_NAME_MAX - 1);
  out->engine_mode = static_cast<int>(p.mode) + 1;
  out->waveform = static_cast<int>(p.waveform) + 1;
  out->unison = p.unison;
  out->detune_cents = p.detune_cents;
  out->drift_cents = p.drift_cents;
  out->drive = p.drive;
  out->filter_model = static_cast<int>(p.filter_model) + 1;
  out->filter_output = static_cast<int>(p.filter_output) + 1;
  out->cutoff_hz = p.cutoff_hz;
  out->resonance_q = p.resonance_q;
  out->key_track = p.key_track;
  out->env_to_cutoff_cents = p.env_to_cutoff_cents;
  out->vel_to_cutoff_cents = p.vel_to_cutoff_cents;
  out->amp_attack_ms = p.amp_env.attack_ms;
  out->amp_decay_ms = p.amp_env.decay_ms;
  out->amp_sustain = p.amp_env.sustain;
  out->amp_release_ms = p.amp_env.release_ms;
  out->filter_attack_ms = p.filter_env.attack_ms;
  out->filter_decay_ms = p.filter_env.decay_ms;
  out->filter_sustain = p.filter_env.sustain;
  out->filter_release_ms = p.filter_env.release_ms;
  out->lfo_rate_hz = p.lfo_rate_hz;
  out->lfo_to_pitch_cents = p.lfo_to_pitch_cents;
  out->lfo2_rate_hz = p.lfo2_rate_hz;
  out->glide_ms = p.glide_ms;
  out->body = static_cast<int>(p.body) + 1;
  out->body_mix = p.body_mix;
  out->stereo_spread = p.stereo_spread;
  int n = 0;
  for (const sonare::midi::synth::ModRoute& r : p.mod_matrix.routes) {
    if (r.source == sonare::midi::synth::ModSource::kNone ||
        r.destination == sonare::midi::synth::ModDestination::kNone || r.depth == 0.0f) {
      continue;
    }
    out->mod_routings[n] = {static_cast<int>(r.source), static_cast<int>(r.destination), r.depth};
    ++n;
  }
  out->num_mod_routings = n;
  out->gain = preset.config.gain;
  out->polyphony = preset.config.polyphony;
  out->bus_drive = preset.config.bus_drive;
}

}  // namespace sonare_c_detail
