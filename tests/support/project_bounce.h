/// @file project_bounce.h
/// @brief sonare_project_bounce reporting the interleaved sample count.
/// @details The C entry reports sample frames; most cases size and compare the
///          interleaved buffer, so this multiplies by the channel count on success
///          and forwards every pointer, NULL included, unchanged.

#pragma once

#include <sonare/sonare_c.h>

#include <cstddef>

namespace sonare_test {

/// Bounces @p project and writes the interleaved length (frames * channels) to @p out_len.
inline SonareError bounce_interleaved(SonareProject* project,
                                      const SonareProjectBounceOptions* options,
                                      float** out_interleaved, size_t* out_len, int* out_channels) {
  const SonareError err =
      sonare_project_bounce(project, options, out_interleaved, out_len, out_channels);
  if (err == SONARE_OK && out_len != nullptr && out_channels != nullptr) {
    *out_len *= static_cast<size_t>(*out_channels);
  }
  return err;
}

}  // namespace sonare_test
