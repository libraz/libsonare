#pragma once

/// @file mel_range_names.h
/// @brief The C spelling of the core's mel-range subjects, so a refusal from a mel entry names the
///        fminHz / fmaxHz keys the caller wrote.

#ifdef __EMSCRIPTEN__

#include <array>

#include "c_api/sonare_c_internal.h"

/// The core spells the mel range fmin / fmax; every facade key is fminHz / fmaxHz.
inline const std::array<sonare_c_detail::CName, 2> kMelRangeNames{
    {{"fmin", "fmin_hz"}, {"fmax", "fmax_hz"}}};

#endif  // __EMSCRIPTEN__
