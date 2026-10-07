#pragma once

/// @file normalize.h
/// @brief Shared interleaved-buffer LUFS normalization used by the offline
///        bounce paths (C-ABI engine, WASM engine).

#include <cmath>
#include <cstddef>
#include <vector>

#include "metering/lufs.h"
#include "util/db.h"

namespace sonare::metering {

/// @brief Scales an interleaved buffer so its integrated loudness matches
///        @p target_lufs.
/// @details Measures the integrated LUFS of @p interleaved (frames * channels
///          values) and applies a single static make-up gain
///          to every sample, solved so the remeasured result reaches the target
///          even when the gain moves blocks across the absolute gate. When the
///          measurement is non-finite (e.g. silence below the absolute gate)
///          the buffer is left unchanged. @p target_lufs must already be the
///          resolved target (callers handle any "use default" sentinel before
///          calling). Shared by the C-ABI and WASM offline bounce so the two
///          paths stay byte-for-byte identical.
inline void normalize_interleaved_to_lufs(std::vector<float>& interleaved, std::size_t frames,
                                          int channels, int sample_rate, float target_lufs) {
  const LufsGainToTarget solved =
      gain_to_integrated_lufs(interleaved.data(), frames, channels, sample_rate, target_lufs);
  if (!std::isfinite(solved.measured_lufs)) {
    return;
  }
  const float gain = db_to_linear(solved.gain_db);
  for (float& sample : interleaved) {
    sample *= gain;
  }
}

}  // namespace sonare::metering
