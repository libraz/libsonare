#pragma once

#include "c_api/sonare_c_internal.h"

namespace sonare_c_detail {

/// Canonical C subjects for the core's Mel edge refusals.
inline constexpr std::array<CName, 2> kMelRangeNames{{
    {"fmin", "fmin_hz"},
    {"fmax", "fmax_hz"},
}};

inline constexpr std::array<CName, 4> kMelInverseNames{{
    {"M", "mel"},
    {"mfcc", "coefficients"},
    kMelRangeNames[0],
    kMelRangeNames[1],
}};

}  // namespace sonare_c_detail
