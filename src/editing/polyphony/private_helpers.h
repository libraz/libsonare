#pragma once

#include <cstddef>

#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/note_mask.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare::editing::polyphony::detail {

/// Every bin and frame a mask names lands inside the spectrum, and its own
/// sparse indexing is consistent.
inline void check_mask_shape(const NoteMask& mask, int n_bins, int n_frames) {
  SONARE_CHECK(mask.n_frames >= 0 && mask.frame_start >= 0 && mask.frame_end() <= n_frames,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(mask.frame_offset.size() == static_cast<size_t>(mask.n_frames) + 1,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(mask.weights.size() == mask.bins.size(), ErrorCode::InvalidParameter);
  SONARE_CHECK(mask.frame_offset.front() == 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(static_cast<size_t>(mask.frame_offset.back()) == mask.bins.size(),
               ErrorCode::InvalidParameter);
  for (size_t i = 1; i < mask.frame_offset.size(); ++i) {
    SONARE_CHECK(mask.frame_offset[i] >= mask.frame_offset[i - 1], ErrorCode::InvalidParameter);
  }
  for (const int32_t bin : mask.bins) {
    SONARE_CHECK(bin >= 0 && bin < n_bins, ErrorCode::InvalidParameter);
  }
}

/// The ridge's f0 at @p frame, falling back to its median where the requested
/// frame is outside the ridge span.
inline double ridge_f0_at(const F0Ridge& ridge, int frame) {
  const int index = frame - ridge.frame_start;
  if (index >= 0 && index < static_cast<int>(ridge.f0_hz.size())) {
    return ridge.f0_hz[static_cast<size_t>(index)];
  }
  return sonare::median(ridge.f0_hz.data(), ridge.f0_hz.size());
}

}  // namespace sonare::editing::polyphony::detail
