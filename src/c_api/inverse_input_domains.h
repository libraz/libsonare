#pragma once

/// @file inverse_input_domains.h
/// @brief Numeric domains of the Griffin-Lim and CQT/VQT inverse C-ABI arguments that no core
///        validator states, held once for the checks that refuse them and for
///        numeric_domain_rows.h.

#include "util/parameter_checks.h"
#include "util/resource_limits.h"

namespace sonare_c_detail {

/// @brief What the Griffin-Lim iteration count of an inverse transform accepts.
inline constexpr sonare::util::check::NumericDomain kGriffinLimIterationsDomain =
    sonare::util::check::integer_domain(1, sonare::resource::kMaxGriffinLimIterations);
/// @brief What a magnitude spectrogram's bin and frame counts, and Griffin-Lim's FFT size,
///        accept.
inline constexpr sonare::util::check::NumericDomain kInverseSpectrogramExtentDomain =
    sonare::util::check::integer_domain_of<int>(1);

}  // namespace sonare_c_detail
