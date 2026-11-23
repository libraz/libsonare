#pragma once

/// @file numeric_domain_rows_instruments.h
/// @brief Numeric-domain rows of the instrument and metronome configurations an engine or a
///        bounce takes.

#include "c_api/instrument_config_domains.h"
#include "c_api/numeric_domain_row.h"

namespace sonare_c_detail {

/// @brief The rows, sorted by (entry, param).
inline const NumericDomainRow kInstrumentDomainRows[] = {
    {"engine_set_builtin_instrument", "config.attack_ms", kBuiltinSynthStageMsDomain, nullptr},
    {"engine_set_builtin_instrument", "config.decay_ms", kBuiltinSynthStageMsDomain, nullptr},
    {"engine_set_builtin_instrument", "config.gain", kBuiltinSynthGainDomain, nullptr},
    {"engine_set_builtin_instrument", "config.polyphony", kSynthPolyphonyDomain, nullptr},
    {"engine_set_builtin_instrument", "config.release_ms", kBuiltinSynthStageMsDomain, nullptr},
    {"engine_set_builtin_instrument", "config.sustain", kBuiltinSynthSustainDomain, nullptr},
    {"engine_set_metronome", "config.accent_gain", kMetronomeGainDomain, nullptr},
    {"engine_set_metronome", "config.beat_gain", kMetronomeGainDomain, nullptr},
    {"engine_set_metronome", "config.click_samples", kMetronomeClickSamplesDomain, nullptr},
    {"engine_set_metronome", "config.click_sec", kMetronomeClickSecDomain, nullptr},
    {"engine_set_sf2_instrument", "config.clear_bank_rig", kSf2PlayerFlagDomain, nullptr},
    {"engine_set_sf2_instrument", "config.gain", kSf2PlayerGainDomain, nullptr},
    {"engine_set_sf2_instrument", "config.gs_efx_realization", kGsEfxRealizationDomain, nullptr},
    {"engine_set_sf2_instrument", "config.polyphony", kSynthPolyphonyDomain, nullptr},
    {"engine_set_sf2_instrument", "config.prefer_model_for_modeled_families", kSf2PlayerFlagDomain,
     nullptr},
    {"engine_set_synth_instrument", "patch.num_mod_routings", kSynthModRoutingCountDomain, nullptr},
    {"engine_set_synth_instrument", "patch.polyphony", kSynthPolyphonyDomain, nullptr},
    {"engine_set_synth_instrument", "patch.unison", kSynthUnisonDomain, nullptr},
    {"project_bounce", "options.builtin_instruments[].config.attack_ms", kBuiltinSynthStageMsDomain,
     nullptr},
    {"project_bounce", "options.builtin_instruments[].config.decay_ms", kBuiltinSynthStageMsDomain,
     nullptr},
    {"project_bounce", "options.builtin_instruments[].config.gain", kBuiltinSynthGainDomain,
     nullptr},
    {"project_bounce", "options.builtin_instruments[].config.polyphony", kSynthPolyphonyDomain,
     nullptr},
    {"project_bounce", "options.builtin_instruments[].config.release_ms",
     kBuiltinSynthStageMsDomain, nullptr},
    {"project_bounce", "options.builtin_instruments[].config.sustain", kBuiltinSynthSustainDomain,
     nullptr},
    {"project_bounce", "options.metronome.accent_gain", kMetronomeGainDomain, nullptr},
    {"project_bounce", "options.metronome.beat_gain", kMetronomeGainDomain, nullptr},
    {"project_bounce", "options.metronome.click_samples", kMetronomeClickSamplesDomain, nullptr},
    {"project_bounce", "options.metronome.click_sec", kMetronomeClickSecDomain, nullptr},
    {"project_bounce", "options.sf2_instruments[].config.clear_bank_rig", kSf2PlayerFlagDomain,
     nullptr},
    {"project_bounce", "options.sf2_instruments[].config.gain", kSf2PlayerGainDomain, nullptr},
    {"project_bounce", "options.sf2_instruments[].config.gs_efx_realization",
     kGsEfxRealizationDomain, nullptr},
    {"project_bounce", "options.sf2_instruments[].config.polyphony", kSynthPolyphonyDomain,
     nullptr},
    {"project_bounce", "options.sf2_instruments[].config.prefer_model_for_modeled_families",
     kSf2PlayerFlagDomain, nullptr},
    {"project_bounce", "options.synth_instruments[].patch.num_mod_routings",
     kSynthModRoutingCountDomain, nullptr},
    {"project_bounce", "options.synth_instruments[].patch.polyphony", kSynthPolyphonyDomain,
     nullptr},
    {"project_bounce", "options.synth_instruments[].patch.unison", kSynthUnisonDomain, nullptr},
};

}  // namespace sonare_c_detail
