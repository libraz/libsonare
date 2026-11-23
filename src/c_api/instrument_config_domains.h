#pragma once

/// @file instrument_config_domains.h
/// @brief Numeric domains of the built-in synth, SF2 player and metronome C-ABI configs, stated
///        once for the checks that refuse them and for the table in numeric_domain_rows.h.

#include "engine/metronome.h"
#include "midi/builtin_synth.h"
#include "midi/synth/mod_matrix.h"
#include "midi/synth/native_synth_patch.h"
#include "util/parameter_checks.h"

namespace sonare_c_detail {

/// @brief What the built-in synth's gain accepts.
inline constexpr sonare::util::check::NumericDomain kBuiltinSynthGainDomain =
    sonare::util::check::real_domain(0.0, false, sonare::midi::kMaxSynthGain, true);
/// @brief What the built-in synth's attack_ms, decay_ms and release_ms accept.
inline constexpr sonare::util::check::NumericDomain kBuiltinSynthStageMsDomain =
    sonare::util::check::real_domain(0.0, false, sonare::midi::kMaxSynthStageMs, true);
/// @brief What the built-in synth's sustain level accepts.
inline constexpr sonare::util::check::NumericDomain kBuiltinSynthSustainDomain =
    sonare::util::check::real_domain(0.0, false, 1.0, true);
/// @brief What the built-in synth's and the SF2 player's polyphony accept.
inline constexpr sonare::util::check::NumericDomain kSynthPolyphonyDomain =
    sonare::util::check::integer_domain(1, sonare::midi::kMaxSynthVoices);
/// @brief What a synth patch's unison oscillator count accepts.
inline constexpr sonare::util::check::NumericDomain kSynthUnisonDomain =
    sonare::util::check::integer_domain(1, sonare::midi::synth::kMaxUnisonOscs);
/// @brief What a synth patch's mod routing count accepts.
inline constexpr sonare::util::check::NumericDomain kSynthModRoutingCountDomain =
    sonare::util::check::integer_domain(0, sonare::midi::synth::kMaxModRoutes);
/// @brief What the byte size of a SoundFont image accepts, on the engine and the project loader.
inline constexpr sonare::util::check::NumericDomain kSoundFontSizeDomain =
    sonare::util::check::integer_domain_of<size_t>(1);
/// @brief What the SF2 player's gain accepts.
inline constexpr sonare::util::check::NumericDomain kSf2PlayerGainDomain =
    sonare::util::check::real_domain(0.0, sonare::midi::kMaxSynthGain);
/// @brief What the SF2 player's two on/off flags accept.
inline constexpr sonare::util::check::NumericDomain kSf2PlayerFlagDomain =
    sonare::util::check::integer_domain_of<int>(0);
/// @brief What the SF2 player's gs_efx_realization accepts.
inline constexpr sonare::util::check::NumericDomain kGsEfxRealizationDomain =
    sonare::util::check::integer_domain(0, 1);

/// @brief What the metronome's beat_gain and accent_gain accept.
inline constexpr sonare::util::check::NumericDomain kMetronomeGainDomain =
    sonare::util::check::real_domain_above(0.0, true);
/// @brief What the metronome's click_samples accepts when it is not the click_sec sentinel.
inline constexpr sonare::util::check::NumericDomain kMetronomeClickSamplesDomain =
    sonare::util::check::integer_domain(1, sonare::engine::kMaxMetronomeClickSamples);
/// @brief What the metronome's click_sec accepts.
inline constexpr sonare::util::check::NumericDomain kMetronomeClickSecDomain =
    sonare::util::check::real_domain(0.0, false, sonare::engine::kMaxMetronomeClickSeconds, true);

}  // namespace sonare_c_detail
