#pragma once

/// @file true_peak_fir.h
/// @brief Shared polyphase FIR designs for true-peak interpolation.

#include "rt/polyphase_fir.h"

namespace sonare::rt {

/// Returns whether @p factor has a supported standard polyphase design.
bool is_supported_polyphase_oversample_factor(int factor) noexcept;

/// Returns the canonical true-peak interpolation FIR for @p factor.
/// @throws SonareException if @p factor is unsupported.
const PolyphaseFir& true_peak_fir_for(int factor);

}  // namespace sonare::rt
