#pragma once

/// @file editing_input_domains.h
/// @brief Numeric domains of the C-ABI editing entries' arguments that no core validator sees,
///        stated once for the checks that refuse them and for numeric_domain_rows.h.

#include <cstdint>

#include "util/parameter_checks.h"
#include "util/resource_limits.h"

namespace sonare_c_detail {

/// @brief What the root pitch class of a mode's scale mask accepts.
inline constexpr sonare::util::check::NumericDomain kScaleMaskRootDomain =
    sonare::util::check::integer_domain(0, 11);
/// @brief What the pitch class queried of a scale accepts.
inline constexpr sonare::util::check::NumericDomain kScalePitchClassDomain =
    sonare::util::check::integer_domain(0, 11);
/// @brief What the pitch analysis window length of a vocal session's create options accepts.
inline constexpr sonare::util::check::NumericDomain kVocalCreateFrameLengthDomain =
    sonare::util::check::integer_domain_of<uint32_t>(1);
/// @brief What the pitch analysis hop of a vocal session's create options accepts.
inline constexpr sonare::util::check::NumericDomain kVocalCreateHopLengthDomain =
    sonare::util::check::integer_domain_of<uint32_t>(1);
/// @brief What a given spectrum-meter FFT size accepts; 0 selects the library default. The
///        ceiling is the spectrum meter's FFT resource limit.
inline constexpr sonare::util::check::NumericDomain kSpectrumNFftDomain =
    sonare::util::check::integer_domain(
        2, static_cast<long long>(sonare::resource::kDefaultSpectrumResourceLimits.max_fft_size));
/// @brief What a given spectrum-meter octave-smoothing fraction accepts; 0 selects the default.
inline constexpr sonare::util::check::NumericDomain kSpectrumOctaveFractionDomain =
    sonare::util::check::integer_domain_of<int>(1);

}  // namespace sonare_c_detail
