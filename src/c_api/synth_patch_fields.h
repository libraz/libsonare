#pragma once

/// @file synth_patch_fields.h
/// @brief Shared catalog names and member mappings for SonareSynthPatch fields.

#include <sonare/sonare_c_project_instruments.h>

#include <cstdint>
#include <variant>

#include "c_api/synth_enum_names.h"

namespace sonare_c_detail {

struct SynthPatchEnumField {
  const char* name;
  int SonareSynthPatch::*member;
  sonare::c_api::synth_names::NameTable values;
};

inline constexpr SynthPatchEnumField kSynthPatchEnumFields[] = {
    {"waveform", &SonareSynthPatch::waveform,
     sonare::c_api::synth_names::name_table(sonare::c_api::synth_names::kWaveforms)},
    {"filterModel", &SonareSynthPatch::filter_model,
     sonare::c_api::synth_names::name_table(sonare::c_api::synth_names::kFilterModels)},
    {"filterOutput", &SonareSynthPatch::filter_output,
     sonare::c_api::synth_names::name_table(sonare::c_api::synth_names::kFilterOutputs)},
    {"body", &SonareSynthPatch::body,
     sonare::c_api::synth_names::name_table(sonare::c_api::synth_names::kBodyTypes)},
    {"retrigger", &SonareSynthPatch::retrigger,
     sonare::c_api::synth_names::name_table(sonare::c_api::synth_names::kRetriggers)},
    {"lfoShape", &SonareSynthPatch::lfo_shape,
     sonare::c_api::synth_names::name_table(sonare::c_api::synth_names::kLfoShapes)},
    {"sampleLoop", &SonareSynthPatch::sample_loop,
     sonare::c_api::synth_names::name_table(sonare::c_api::synth_names::kSampleLoopModes)},
    {"sampleKeyTrack", &SonareSynthPatch::sample_key_track,
     sonare::c_api::synth_names::name_table(sonare::c_api::synth_names::kSampleKeyTracks)},
};

struct SynthPatchNumericField {
  const char* name;
  std::variant<float SonareSynthPatch::*, int SonareSynthPatch::*> member;
  uint32_t presence_bit;
  bool presence_fields2;
};

inline constexpr SynthPatchNumericField kSynthPatchNumericFields[] = {
    {"unison", &SonareSynthPatch::unison, SONARE_SYNTH_FIELD_UNISON, false},
    {"detuneCents", &SonareSynthPatch::detune_cents, SONARE_SYNTH_FIELD_DETUNE_CENTS, false},
    {"driftCents", &SonareSynthPatch::drift_cents, SONARE_SYNTH_FIELD_DRIFT_CENTS, false},
    {"drive", &SonareSynthPatch::drive, SONARE_SYNTH_FIELD_DRIVE, false},
    {"cutoffHz", &SonareSynthPatch::cutoff_hz, SONARE_SYNTH_FIELD_CUTOFF_HZ, false},
    {"resonanceQ", &SonareSynthPatch::resonance_q, SONARE_SYNTH_FIELD_RESONANCE_Q, false},
    {"keyTrack", &SonareSynthPatch::key_track, SONARE_SYNTH_FIELD_KEY_TRACK, false},
    {"envToCutoffCents", &SonareSynthPatch::env_to_cutoff_cents,
     SONARE_SYNTH_FIELD_ENV_TO_CUTOFF_CENTS, false},
    {"velToCutoffCents", &SonareSynthPatch::vel_to_cutoff_cents,
     SONARE_SYNTH_FIELD_VEL_TO_CUTOFF_CENTS, false},
    {"ampAttackMs", &SonareSynthPatch::amp_attack_ms, SONARE_SYNTH_FIELD_AMP_ATTACK_MS, false},
    {"ampDecayMs", &SonareSynthPatch::amp_decay_ms, SONARE_SYNTH_FIELD_AMP_DECAY_MS, false},
    {"ampSustain", &SonareSynthPatch::amp_sustain, SONARE_SYNTH_FIELD_AMP_SUSTAIN, false},
    {"ampReleaseMs", &SonareSynthPatch::amp_release_ms, SONARE_SYNTH_FIELD_AMP_RELEASE_MS, false},
    {"filterAttackMs", &SonareSynthPatch::filter_attack_ms,
     SONARE_SYNTH_FIELD_FILTER_ATTACK_MS, false},
    {"filterDecayMs", &SonareSynthPatch::filter_decay_ms,
     SONARE_SYNTH_FIELD_FILTER_DECAY_MS, false},
    {"filterSustain", &SonareSynthPatch::filter_sustain, SONARE_SYNTH_FIELD_FILTER_SUSTAIN, false},
    {"filterReleaseMs", &SonareSynthPatch::filter_release_ms,
     SONARE_SYNTH_FIELD_FILTER_RELEASE_MS, false},
    {"lfoRateHz", &SonareSynthPatch::lfo_rate_hz, SONARE_SYNTH_FIELD_LFO_RATE_HZ, false},
    {"lfoToPitchCents", &SonareSynthPatch::lfo_to_pitch_cents,
     SONARE_SYNTH_FIELD_LFO_TO_PITCH_CENTS, false},
    {"lfo2RateHz", &SonareSynthPatch::lfo2_rate_hz, SONARE_SYNTH_FIELD_LFO2_RATE_HZ, false},
    {"glideMs", &SonareSynthPatch::glide_ms, SONARE_SYNTH_FIELD_GLIDE_MS, false},
    {"bodyMix", &SonareSynthPatch::body_mix, SONARE_SYNTH_FIELD_BODY_MIX, false},
    {"stereoSpread", &SonareSynthPatch::stereo_spread, SONARE_SYNTH_FIELD_STEREO_SPREAD, false},
    {"gain", &SonareSynthPatch::gain, SONARE_SYNTH_FIELD_GAIN, false},
    {"polyphony", &SonareSynthPatch::polyphony, SONARE_SYNTH_FIELD_POLYPHONY, false},
    {"busDrive", &SonareSynthPatch::bus_drive, SONARE_SYNTH_FIELD_BUS_DRIVE, false},
    {"sampleSet", &SonareSynthPatch::sample_set, 0, false},
    {"sampleLevel", &SonareSynthPatch::sample_level, 0, false},
    {"sampleStartOffset", &SonareSynthPatch::sample_start_offset, 0, false},
    {"hpCutoffHz", &SonareSynthPatch::hp_cutoff_hz, SONARE_SYNTH_FIELD_HP_CUTOFF_HZ, false},
    {"sampleHoldHz", &SonareSynthPatch::sample_hold_hz, SONARE_SYNTH_FIELD_SAMPLE_HOLD_HZ, false},
    {"bitDepth", &SonareSynthPatch::bit_depth, SONARE_SYNTH_FIELD_BIT_DEPTH, false},
    {"pitchOffsetCents", &SonareSynthPatch::pitch_offset_cents,
     SONARE_SYNTH_FIELD_PITCH_OFFSET_CENTS, false},
    {"driftRateHz", &SonareSynthPatch::drift_rate_hz, SONARE_SYNTH_FIELD2_DRIFT_RATE_HZ, true},
    {"ampDelayMs", &SonareSynthPatch::amp_delay_ms, SONARE_SYNTH_FIELD2_AMP_DELAY_MS, true},
    {"ampHoldMs", &SonareSynthPatch::amp_hold_ms, SONARE_SYNTH_FIELD2_AMP_HOLD_MS, true},
    {"filterDelayMs", &SonareSynthPatch::filter_delay_ms,
     SONARE_SYNTH_FIELD2_FILTER_DELAY_MS, true},
    {"filterHoldMs", &SonareSynthPatch::filter_hold_ms, SONARE_SYNTH_FIELD2_FILTER_HOLD_MS, true},
    {"oneShot", &SonareSynthPatch::one_shot, SONARE_SYNTH_FIELD2_ONE_SHOT, true},
};

}  // namespace sonare_c_detail
