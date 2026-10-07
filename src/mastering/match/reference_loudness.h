#pragma once

/// @file reference_loudness.h
/// @brief Loudness comparison against a reference master.

#include "core/audio.h"

namespace sonare::mastering::match {

struct ReferenceLoudness {
  float source_lufs = 0.0f;
  float reference_lufs = 0.0f;
  /// Uniform gain that lands the remeasured source on `reference_lufs`,
  /// accounting for blocks it moves across the absolute gate; 0 when either
  /// loudness is non-finite.
  float gain_to_match_db = 0.0f;
};

ReferenceLoudness reference_loudness(const Audio& source, const Audio& reference);

}  // namespace sonare::mastering::match
