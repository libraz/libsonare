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
constexpr std::array<ParamEntry, 25> kParams{{
    {"gain", NativeSynthParamId::kGain},
    {"busDrive", NativeSynthParamId::kBusDrive},
    {"cutoffHz", NativeSynthParamId::kCutoffHz},
    {"resonanceQ", NativeSynthParamId::kResonanceQ},
    {"drive", NativeSynthParamId::kDrive},
    {"keyTrack", NativeSynthParamId::kKeyTrack},
    {"envToCutoffCents", NativeSynthParamId::kEnvToCutoffCents},
    {"velToCutoffCents", NativeSynthParamId::kVelToCutoffCents},
    {"ampAttackMs", NativeSynthParamId::kAmpAttackMs},
    {"ampDecayMs", NativeSynthParamId::kAmpDecayMs},
    {"ampSustain", NativeSynthParamId::kAmpSustain},
    {"ampReleaseMs", NativeSynthParamId::kAmpReleaseMs},
    {"filterAttackMs", NativeSynthParamId::kFilterAttackMs},
    {"filterDecayMs", NativeSynthParamId::kFilterDecayMs},
    {"filterSustain", NativeSynthParamId::kFilterSustain},
    {"filterReleaseMs", NativeSynthParamId::kFilterReleaseMs},
    {"lfoRateHz", NativeSynthParamId::kLfoRateHz},
    {"lfoToPitchCents", NativeSynthParamId::kLfoToPitchCents},
    {"lfo2RateHz", NativeSynthParamId::kLfo2RateHz},
    {"glideMs", NativeSynthParamId::kGlideMs},
    {"bodyMix", NativeSynthParamId::kBodyMix},
    {"stereoSpread", NativeSynthParamId::kStereoSpread},
    {"detuneCents", NativeSynthParamId::kDetuneCents},
    {"driftCents", NativeSynthParamId::kDriftCents},
    {"pitchOffsetCents", NativeSynthParamId::kPitchOffsetCents},
}};

float clamp_finite(float value, float lo, float hi, float fallback) noexcept {
  if (!std::isfinite(value)) return fallback;
  return std::clamp(value, lo, hi);
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
  NativeSynthPatch& p = config_.patch;
  switch (static_cast<NativeSynthParamId>(param_id)) {
    case NativeSynthParamId::kGain:
      // The instrument master gain, not the per-voice patch gain: it multiplies
      // the summed mix every sample, so it is the one a fader-style lane wants.
      config_.gain = clamp_finite(value, 0.0f, 4.0f, config_.gain);
      return true;
    case NativeSynthParamId::kBusDrive:
      config_.bus_drive = clamp_finite(value, 0.0f, 1.0f, config_.bus_drive);
      // Mirror prepare()'s derivation so the change is audible this block.
      bus_drive_gain_ = config_.bus_drive > 0.0f ? 1.0f + 3.0f * config_.bus_drive : 0.0f;
      return true;
    case NativeSynthParamId::kCutoffHz:
      p.cutoff_hz = clamp_finite(value, 10.0f, 22000.0f, p.cutoff_hz);
      return true;
    case NativeSynthParamId::kResonanceQ:
      p.resonance_q = clamp_finite(value, 0.5f, 30.0f, p.resonance_q);
      return true;
    case NativeSynthParamId::kDrive:
      p.drive = clamp_finite(value, 0.0f, 1.0f, p.drive);
      return true;
    case NativeSynthParamId::kKeyTrack:
      p.key_track = clamp_finite(value, 0.0f, 1.0f, p.key_track);
      return true;
    case NativeSynthParamId::kEnvToCutoffCents:
      p.env_to_cutoff_cents = clamp_finite(value, -9600.0f, 9600.0f, p.env_to_cutoff_cents);
      return true;
    case NativeSynthParamId::kVelToCutoffCents:
      p.vel_to_cutoff_cents = clamp_finite(value, -9600.0f, 9600.0f, p.vel_to_cutoff_cents);
      return true;
    case NativeSynthParamId::kAmpAttackMs:
      p.amp_env.attack_ms = clamp_finite(value, 0.0f, 20000.0f, p.amp_env.attack_ms);
      return true;
    case NativeSynthParamId::kAmpDecayMs:
      p.amp_env.decay_ms = clamp_finite(value, 0.0f, 20000.0f, p.amp_env.decay_ms);
      return true;
    case NativeSynthParamId::kAmpSustain:
      p.amp_env.sustain = clamp_finite(value, 0.0f, 1.0f, p.amp_env.sustain);
      return true;
    case NativeSynthParamId::kAmpReleaseMs:
      p.amp_env.release_ms = clamp_finite(value, 1.0f, 20000.0f, p.amp_env.release_ms);
      return true;
    case NativeSynthParamId::kFilterAttackMs:
      p.filter_env.attack_ms = clamp_finite(value, 0.0f, 20000.0f, p.filter_env.attack_ms);
      return true;
    case NativeSynthParamId::kFilterDecayMs:
      p.filter_env.decay_ms = clamp_finite(value, 0.0f, 20000.0f, p.filter_env.decay_ms);
      return true;
    case NativeSynthParamId::kFilterSustain:
      p.filter_env.sustain = clamp_finite(value, 0.0f, 1.0f, p.filter_env.sustain);
      return true;
    case NativeSynthParamId::kFilterReleaseMs:
      p.filter_env.release_ms = clamp_finite(value, 1.0f, 20000.0f, p.filter_env.release_ms);
      return true;
    case NativeSynthParamId::kLfoRateHz:
      p.lfo_rate_hz = clamp_finite(value, 0.0f, 40.0f, p.lfo_rate_hz);
      return true;
    case NativeSynthParamId::kLfoToPitchCents:
      p.lfo_to_pitch_cents = clamp_finite(value, 0.0f, 1200.0f, p.lfo_to_pitch_cents);
      return true;
    case NativeSynthParamId::kLfo2RateHz:
      p.lfo2_rate_hz = clamp_finite(value, 0.0f, 40.0f, p.lfo2_rate_hz);
      return true;
    case NativeSynthParamId::kGlideMs:
      p.glide_ms = clamp_finite(value, 0.0f, 5000.0f, p.glide_ms);
      return true;
    case NativeSynthParamId::kBodyMix:
      p.body_mix = clamp_finite(value, 0.0f, 1.0f, p.body_mix);
      return true;
    case NativeSynthParamId::kStereoSpread:
      p.stereo_spread = clamp_finite(value, 0.0f, 1.0f, p.stereo_spread);
      return true;
    case NativeSynthParamId::kDetuneCents:
      p.detune_cents = clamp_finite(value, 0.0f, 200.0f, p.detune_cents);
      return true;
    case NativeSynthParamId::kDriftCents:
      p.drift_cents = clamp_finite(value, 0.0f, 100.0f, p.drift_cents);
      return true;
    case NativeSynthParamId::kPitchOffsetCents:
      p.pitch_offset_cents = clamp_finite(value, -4800.0f, 4800.0f, p.pitch_offset_cents);
      return true;
  }
  return false;
}

}  // namespace sonare::midi::synth
