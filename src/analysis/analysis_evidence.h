#pragma once

/// @file analysis_evidence.h
/// @brief The one test analyzers apply before turning a measurement into a verdict.
/// @details Silence and absent events leave every derived ratio at a degenerate value
///          (flatness 1, crest 0, a zero level read as "quiet"), so each analyzer gates its
///          verdict on this predicate and returns its documented no-result value when it fails.

#include "util/constants.h"

namespace sonare {

/// @brief True when @p magnitude (a non-negative level, energy, norm or duration) carries
///        evidence, i.e. exceeds @ref constants::kEpsilon. NaN carries none.
inline bool has_evidence(double magnitude) { return magnitude > constants::kEpsilon; }

}  // namespace sonare
