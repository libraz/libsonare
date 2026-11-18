#pragma once

/// @file band_strings.h
/// @brief Accepted spellings of the equalizer band enums.
///
/// Every surface that builds an @c EqBand from text — the C ABI's band JSON,
/// the WASM band object and the Node addon's band object — accepts exactly one
/// (lowerCamel) spelling per enumerator, and every writer emits it. Parsing and
/// writing live here; each surface keeps its own way of reporting an
/// unrecognised value, so the parsers signal failure instead of throwing.

#include <optional>
#include <string_view>

#include "mastering/eq/eq_band.h"

namespace sonare::mastering::eq {

/// @brief Parses an EQ band type: peak, lowShelf, highShelf, lowPass, highPass,
///        bandPass, notch, tiltShelf, flatTilt or allPass. Any other spelling
///        (other case, synonyms) is refused.
std::optional<EqBandType> band_type_from_string(std::string_view value);

/// @brief Parses a biquad coefficient mode: rbj or vicanek.
std::optional<BiquadCoeffMode> coeff_mode_from_string(std::string_view value);

/// @brief Parses a stereo placement: stereo, left, right, mid or side.
std::optional<StereoPlacement> placement_from_string(std::string_view value);

/// @brief Parses a phase mode: inherit, zeroLatency, naturalPhase or linearPhase.
std::optional<PhaseMode> phase_mode_from_string(std::string_view value);

/// @brief The token each enumerator is written as; the inverse of the parsers above.
const char* band_type_to_string(EqBandType type);
const char* coeff_mode_to_string(BiquadCoeffMode mode);
const char* placement_to_string(StereoPlacement placement);
const char* phase_mode_to_string(PhaseMode phase);

/// @brief The accepted tokens of each closed set, comma-separated, for error messages.
const char* band_type_tokens();
const char* coeff_mode_tokens();
const char* placement_tokens();
const char* phase_mode_tokens();

/// @brief Maps the C-ABI phase-mode integer onto @c PhaseMode. 0 (Inherit) is
///        not a transportable value on that surface and is rejected.
std::optional<PhaseMode> phase_mode_from_int(int mode);

/// @brief Parses a placement ordinal. 0 is Stereo — a real value here, unlike
///        the phase mode above, where 0 means "unset".
std::optional<StereoPlacement> placement_from_int(int placement);

}  // namespace sonare::mastering::eq
