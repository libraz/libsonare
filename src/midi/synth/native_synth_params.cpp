/// @file native_synth_params.cpp
/// @brief Continuous-parameter automation surface for NativeSynth: the
///        JSON-key -> param-id table and the audio-thread apply.

#include <algorithm>
#include <array>
#include <cmath>

#include "midi/synth/native_synth.h"

namespace sonare::midi::synth {

namespace {

struct ParamEntry {
  const char* key;
  NativeSynthParamId id;
  // The one range apply_parameter's clamp reads and describe_parameter
  // reports -- kept beside the id so the two can never disagree.
  float min;
  float max;
  const char* unit;
};

/// JSON-key names mirror the bindings' SynthPatch fields exactly, so a host
/// resolves the same string it would put in a patch object. Ids are stable and
/// append-only: a saved automation lane carries the id, not the name.
///
/// Structural patch fields (preset / engineMode / waveform / filterModel /
/// filterOutput / unison / polyphony / body / modRoutings) are deliberately
/// absent. Changing them mid-flight would resize the voice pool, reallocate a
/// waveguide slab or swap a filter topology, none of which is audio-thread
/// safe; they stay patch edits applied through the instrument sync path.
constexpr std::array<ParamEntry, 28> kParams{{
    {"gain", NativeSynthParamId::kGain, 0.0f, 4.0f, ""},
    {"busDrive", NativeSynthParamId::kBusDrive, 0.0f, 1.0f, ""},
    {"cutoffHz", NativeSynthParamId::kCutoffHz, 10.0f, 22000.0f, "Hz"},
    {"resonanceQ", NativeSynthParamId::kResonanceQ, 0.5f, 30.0f, "Q"},
    {"drive", NativeSynthParamId::kDrive, 0.0f, 1.0f, ""},
    {"keyTrack", NativeSynthParamId::kKeyTrack, 0.0f, 1.0f, ""},
    {"envToCutoffCents", NativeSynthParamId::kEnvToCutoffCents, -9600.0f, 9600.0f, "cents"},
    {"velToCutoffCents", NativeSynthParamId::kVelToCutoffCents, -9600.0f, 9600.0f, "cents"},
    {"ampAttackMs", NativeSynthParamId::kAmpAttackMs, 0.0f, 20000.0f, "ms"},
    {"ampDecayMs", NativeSynthParamId::kAmpDecayMs, 0.0f, 20000.0f, "ms"},
    {"ampSustain", NativeSynthParamId::kAmpSustain, 0.0f, 1.0f, ""},
    {"ampReleaseMs", NativeSynthParamId::kAmpReleaseMs, 1.0f, 20000.0f, "ms"},
    {"filterAttackMs", NativeSynthParamId::kFilterAttackMs, 0.0f, 20000.0f, "ms"},
    {"filterDecayMs", NativeSynthParamId::kFilterDecayMs, 0.0f, 20000.0f, "ms"},
    {"filterSustain", NativeSynthParamId::kFilterSustain, 0.0f, 1.0f, ""},
    {"filterReleaseMs", NativeSynthParamId::kFilterReleaseMs, 1.0f, 20000.0f, "ms"},
    {"lfoRateHz", NativeSynthParamId::kLfoRateHz, 0.0f, 40.0f, "Hz"},
    {"lfoToPitchCents", NativeSynthParamId::kLfoToPitchCents, 0.0f, 1200.0f, "cents"},
    {"lfo2RateHz", NativeSynthParamId::kLfo2RateHz, 0.0f, 40.0f, "Hz"},
    {"glideMs", NativeSynthParamId::kGlideMs, 0.0f, 5000.0f, "ms"},
    {"bodyMix", NativeSynthParamId::kBodyMix, 0.0f, 1.0f, ""},
    {"stereoSpread", NativeSynthParamId::kStereoSpread, 0.0f, 1.0f, ""},
    {"detuneCents", NativeSynthParamId::kDetuneCents, 0.0f, 200.0f, "cents"},
    {"driftCents", NativeSynthParamId::kDriftCents, 0.0f, 100.0f, "cents"},
    {"pitchOffsetCents", NativeSynthParamId::kPitchOffsetCents, -4800.0f, 4800.0f, "cents"},
    {"hpCutoffHz", NativeSynthParamId::kHpCutoffHz, 0.0f, 22000.0f, "Hz"},
    {"sampleHoldHz", NativeSynthParamId::kSampleHoldHz, 0.0f, 192000.0f, "Hz"},
    {"bitDepth", NativeSynthParamId::kBitDepth, 0.0f, 24.0f, "bits"},
}};

const ParamEntry* find_param_entry(NativeSynthParamId id) noexcept {
  for (const ParamEntry& entry : kParams) {
    if (entry.id == id) return &entry;
  }
  return nullptr;
}

float clamp_finite(float value, float lo, float hi, float fallback) noexcept {
  if (!std::isfinite(value)) return fallback;
  return std::clamp(value, lo, hi);
}

/// The value @p id reports before any patch load or live automation: the
/// engine's own compiled-in defaults, read from a fresh NativeSynthConfig
/// rather than this instance's config_ -- describe_parameter runs on the
/// control thread and config_.patch is audio-thread state a live voice may be
/// reading concurrently.
float default_value_for(NativeSynthParamId id) noexcept {
  static const NativeSynthConfig kDefaults{};
  const NativeSynthPatch& p = kDefaults.patch;
  switch (id) {
    case NativeSynthParamId::kGain:
      return kDefaults.gain;
    case NativeSynthParamId::kBusDrive:
      return kDefaults.bus_drive;
    case NativeSynthParamId::kCutoffHz:
      return p.cutoff_hz;
    case NativeSynthParamId::kResonanceQ:
      return p.resonance_q;
    case NativeSynthParamId::kDrive:
      return p.drive;
    case NativeSynthParamId::kKeyTrack:
      return p.key_track;
    case NativeSynthParamId::kEnvToCutoffCents:
      return p.env_to_cutoff_cents;
    case NativeSynthParamId::kVelToCutoffCents:
      return p.vel_to_cutoff_cents;
    case NativeSynthParamId::kAmpAttackMs:
      return p.amp_env.attack_ms;
    case NativeSynthParamId::kAmpDecayMs:
      return p.amp_env.decay_ms;
    case NativeSynthParamId::kAmpSustain:
      return p.amp_env.sustain;
    case NativeSynthParamId::kAmpReleaseMs:
      return p.amp_env.release_ms;
    case NativeSynthParamId::kFilterAttackMs:
      return p.filter_env.attack_ms;
    case NativeSynthParamId::kFilterDecayMs:
      return p.filter_env.decay_ms;
    case NativeSynthParamId::kFilterSustain:
      return p.filter_env.sustain;
    case NativeSynthParamId::kFilterReleaseMs:
      return p.filter_env.release_ms;
    case NativeSynthParamId::kLfoRateHz:
      return p.lfo_rate_hz;
    case NativeSynthParamId::kLfoToPitchCents:
      return p.lfo_to_pitch_cents;
    case NativeSynthParamId::kLfo2RateHz:
      return p.lfo2_rate_hz;
    case NativeSynthParamId::kGlideMs:
      return p.glide_ms;
    case NativeSynthParamId::kBodyMix:
      return p.body_mix;
    case NativeSynthParamId::kStereoSpread:
      return p.stereo_spread;
    case NativeSynthParamId::kDetuneCents:
      return p.detune_cents;
    case NativeSynthParamId::kDriftCents:
      return p.drift_cents;
    case NativeSynthParamId::kPitchOffsetCents:
      return p.pitch_offset_cents;
    case NativeSynthParamId::kHpCutoffHz:
      return p.hp_cutoff_hz;
    case NativeSynthParamId::kSampleHoldHz:
      return p.sample_hold_hz;
    case NativeSynthParamId::kBitDepth:
      return p.bit_depth;
  }
  return 0.0f;
}

}  // namespace

const char* native_synth_param_name(NativeSynthParamId id) noexcept {
  for (const ParamEntry& entry : kParams) {
    if (entry.id == id) return entry.key;
  }
  return nullptr;
}

size_t native_synth_param_count() noexcept { return kParams.size(); }

const char* native_synth_param_name_at(size_t index) noexcept {
  return index < kParams.size() ? kParams[index].key : nullptr;
}

int NativeSynth::parameter_id_for_key(const std::string& key) const noexcept {
  for (const ParamEntry& entry : kParams) {
    if (key == entry.key) return static_cast<int>(entry.id);
  }
  return -1;
}

bool NativeSynth::apply_parameter(unsigned int param_id, float value) noexcept {
  const auto id = static_cast<NativeSynthParamId>(param_id);
  const ParamEntry* entry = find_param_entry(id);
  if (entry == nullptr) return false;
  NativeSynthPatch& p = config_.patch;
  switch (id) {
    case NativeSynthParamId::kGain:
      // The instrument master gain, not the per-voice patch gain: it multiplies
      // the summed mix every sample, so it is the one a fader-style lane wants.
      config_.gain = clamp_finite(value, entry->min, entry->max, config_.gain);
      return true;
    case NativeSynthParamId::kBusDrive:
      config_.bus_drive = clamp_finite(value, entry->min, entry->max, config_.bus_drive);
      // Mirror prepare()'s derivation so the change is audible this block.
      bus_drive_gain_ = config_.bus_drive > 0.0f ? 1.0f + 3.0f * config_.bus_drive : 0.0f;
      return true;
    case NativeSynthParamId::kCutoffHz:
      p.cutoff_hz = clamp_finite(value, entry->min, entry->max, p.cutoff_hz);
      return true;
    case NativeSynthParamId::kResonanceQ:
      p.resonance_q = clamp_finite(value, entry->min, entry->max, p.resonance_q);
      return true;
    case NativeSynthParamId::kDrive:
      p.drive = clamp_finite(value, entry->min, entry->max, p.drive);
      return true;
    case NativeSynthParamId::kKeyTrack:
      p.key_track = clamp_finite(value, entry->min, entry->max, p.key_track);
      return true;
    case NativeSynthParamId::kEnvToCutoffCents:
      p.env_to_cutoff_cents = clamp_finite(value, entry->min, entry->max, p.env_to_cutoff_cents);
      return true;
    case NativeSynthParamId::kVelToCutoffCents:
      p.vel_to_cutoff_cents = clamp_finite(value, entry->min, entry->max, p.vel_to_cutoff_cents);
      return true;
    case NativeSynthParamId::kAmpAttackMs:
      p.amp_env.attack_ms = clamp_finite(value, entry->min, entry->max, p.amp_env.attack_ms);
      return true;
    case NativeSynthParamId::kAmpDecayMs:
      p.amp_env.decay_ms = clamp_finite(value, entry->min, entry->max, p.amp_env.decay_ms);
      return true;
    case NativeSynthParamId::kAmpSustain:
      p.amp_env.sustain = clamp_finite(value, entry->min, entry->max, p.amp_env.sustain);
      return true;
    case NativeSynthParamId::kAmpReleaseMs:
      p.amp_env.release_ms = clamp_finite(value, entry->min, entry->max, p.amp_env.release_ms);
      return true;
    case NativeSynthParamId::kFilterAttackMs:
      p.filter_env.attack_ms = clamp_finite(value, entry->min, entry->max, p.filter_env.attack_ms);
      return true;
    case NativeSynthParamId::kFilterDecayMs:
      p.filter_env.decay_ms = clamp_finite(value, entry->min, entry->max, p.filter_env.decay_ms);
      return true;
    case NativeSynthParamId::kFilterSustain:
      p.filter_env.sustain = clamp_finite(value, entry->min, entry->max, p.filter_env.sustain);
      return true;
    case NativeSynthParamId::kFilterReleaseMs:
      p.filter_env.release_ms =
          clamp_finite(value, entry->min, entry->max, p.filter_env.release_ms);
      return true;
    case NativeSynthParamId::kLfoRateHz:
      p.lfo_rate_hz = clamp_finite(value, entry->min, entry->max, p.lfo_rate_hz);
      return true;
    case NativeSynthParamId::kLfoToPitchCents:
      p.lfo_to_pitch_cents = clamp_finite(value, entry->min, entry->max, p.lfo_to_pitch_cents);
      return true;
    case NativeSynthParamId::kLfo2RateHz:
      p.lfo2_rate_hz = clamp_finite(value, entry->min, entry->max, p.lfo2_rate_hz);
      return true;
    case NativeSynthParamId::kGlideMs:
      p.glide_ms = clamp_finite(value, entry->min, entry->max, p.glide_ms);
      return true;
    case NativeSynthParamId::kBodyMix:
      p.body_mix = clamp_finite(value, entry->min, entry->max, p.body_mix);
      return true;
    case NativeSynthParamId::kStereoSpread:
      p.stereo_spread = clamp_finite(value, entry->min, entry->max, p.stereo_spread);
      return true;
    case NativeSynthParamId::kDetuneCents:
      p.detune_cents = clamp_finite(value, entry->min, entry->max, p.detune_cents);
      return true;
    case NativeSynthParamId::kDriftCents:
      p.drift_cents = clamp_finite(value, entry->min, entry->max, p.drift_cents);
      return true;
    case NativeSynthParamId::kPitchOffsetCents:
      p.pitch_offset_cents = clamp_finite(value, entry->min, entry->max, p.pitch_offset_cents);
      return true;
    case NativeSynthParamId::kHpCutoffHz:
      p.hp_cutoff_hz = clamp_finite(value, entry->min, entry->max, p.hp_cutoff_hz);
      return true;
    // The two converter halves take the same floor the patch clamp gives
    // them, so a lane and a patch edit reaching the same value land on the
    // same sound: zero is off and anything under the floor is raised to it.
    case NativeSynthParamId::kSampleHoldHz:
      p.sample_hold_hz = clamp_finite(value, entry->min, entry->max, p.sample_hold_hz);
      if (p.sample_hold_hz > 0.0f) p.sample_hold_hz = std::max(p.sample_hold_hz, 100.0f);
      return true;
    case NativeSynthParamId::kBitDepth:
      p.bit_depth = clamp_finite(value, entry->min, entry->max, p.bit_depth);
      if (p.bit_depth > 0.0f) p.bit_depth = std::max(p.bit_depth, 1.0f);
      return true;
  }
  return false;
}

bool NativeSynth::describe_parameter(unsigned int param_id,
                                     automation::ParameterDescription* out) const {
  if (out == nullptr) return false;
  const auto id = static_cast<NativeSynthParamId>(param_id);
  const ParamEntry* entry = find_param_entry(id);
  if (entry == nullptr) return false;
  out->name = entry->key;
  out->unit = entry->unit;
  out->min_value = entry->min;
  out->max_value = entry->max;
  out->default_value = default_value_for(id);
  out->rt_safe = true;
  out->default_curve = automation::CurveType::Linear;
  return true;
}

}  // namespace sonare::midi::synth
