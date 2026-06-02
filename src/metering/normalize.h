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
///          (`db_to_linear(target - measured)`) to every sample. When the
///          measurement is non-finite (e.g. silence below the absolute gate)
///          the buffer is left unchanged. @p target_lufs must already be the
///          resolved target (callers handle any "use default" sentinel before
///          calling). Shared by the C-ABI and WASM offline bounce so the two
///          paths stay byte-for-byte identical.
inline void normalize_interleaved_to_lufs(std::vector<float>& interleaved, std::size_t frames,
                                          int channels, int sample_rate, float target_lufs) {
  const LufsResult loudness = lufs_interleaved(interleaved.data(), frames, channels, sample_rate);
  if (!std::isfinite(loudness.integrated_lufs)) {
    return;
  }
  const float gain = db_to_linear(target_lufs - loudness.integrated_lufs);
  for (float& sample : interleaved) {
    sample *= gain;
  }
}

}  // namespace sonare::metering
