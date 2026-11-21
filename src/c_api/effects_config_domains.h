#pragma once

/// @file effects_config_domains.h
/// @brief Numeric domains of the effects C-ABI arguments and configs that no core validator
///        states, held once for the checks that refuse them and for numeric_domain_rows.h.

#include "effects/decompose.h"
#include "effects/spectral_edit.h"
#include "sonare/sonare_c.h"
#include "util/parameter_checks.h"

namespace sonare_c_detail {

/// @brief What the biquad filter's type accepts.
inline constexpr sonare::util::check::NumericDomain kBiquadFilterTypeDomain =
    sonare::util::check::integer_domain(SONARE_BIQUAD_FILTER_HIGH_PASS, SONARE_BIQUAD_FILTER_NOTCH);
/// @brief What the number of channels of a linked stem separation accepts.
inline constexpr sonare::util::check::NumericDomain kDecomposeStemsLinkedChannelsDomain =
    sonare::util::check::integer_domain(
        1, static_cast<long long>(sonare::kMaxDecomposeStemsLinkedChannels));
/// @brief What the spectral edit config's heal radius in frames accepts.
inline constexpr sonare::util::check::NumericDomain kSpectralEditHealRadiusDomain =
    sonare::util::check::integer_domain_of<int>(1);
/// @brief What the spectral edit config's window accepts.
inline constexpr sonare::util::check::NumericDomain kSpectralEditWindowDomain =
    sonare::util::check::integer_domain(SONARE_WINDOW_HANN, SONARE_WINDOW_RECTANGULAR);
/// @brief What a spectral region op's mode accepts.
inline constexpr sonare::util::check::NumericDomain kSpectralEditModeDomain =
    sonare::util::check::integer_domain(SONARE_SPECTRAL_EDIT_MODE_GAIN,
                                        SONARE_SPECTRAL_EDIT_MODE_HEAL);

}  // namespace sonare_c_detail
