#pragma once

/// @file band_strings.h
/// @brief Accepted spellings of the equalizer band enums.
///
/// Every surface that builds an @c EqBand from text — the C ABI's band JSON,
/// the WASM band object and the Node addon's band object — accepts the same
/// spellings. Parsing lives here so a spelling added for one surface is
/// accepted by all of them; each surface keeps its own way of reporting an
/// unrecognised value, so these functions signal failure instead of throwing.

#include <optional>
#include <string_view>

#include "mastering/eq/eq_band.h"

namespace sonare::mastering::eq {

/// @brief Parses an EQ band type. Accepts the enumerator spelling in either
///        case convention plus the "Bell"/"HighCut"/"LowCut" synonyms.
std::optional<EqBandType> band_type_from_string(std::string_view value);

/// @brief Parses a biquad coefficient mode ("Rbj"/"RBJ"/"rbj", "Vicanek").
std::optional<BiquadCoeffMode> coeff_mode_from_string(std::string_view value);

/// @brief Parses a stereo placement ("Stereo", "Left", "Right", "Mid", "Side").
std::optional<StereoPlacement> placement_from_string(std::string_view value);

/// @brief Parses a phase mode ("Inherit", "ZeroLatency", "NaturalPhase",
///        "LinearPhase").
std::optional<PhaseMode> phase_mode_from_string(std::string_view value);

/// @brief Maps the C-ABI phase-mode integer onto @c PhaseMode. 0 (Inherit) is
///        not a transportable value on that surface and is rejected.
std::optional<PhaseMode> phase_mode_from_int(int mode);

}  // namespace sonare::mastering::eq
