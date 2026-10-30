#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <vector>

#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/note_mask.h"
#include "util/exception.h"
#include "util/insertion_sort.h"
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

/// The masks that cover a frame, ascending in note index, advanced one frame at a time from
/// frame 0 so a sparse score is not rescanned in full at every frame.
class ActiveMasks {
 public:
  explicit ActiveMasks(const std::vector<NoteMask>& notes)
      : notes_(notes), by_start_(notes.size()) {
    std::iota(by_start_.begin(), by_start_.end(), size_t{0});
    sonare::insertion_sort(by_start_.begin(), by_start_.end(), [&](size_t lhs, size_t rhs) {
      return notes_[lhs].frame_start < notes_[rhs].frame_start;
    });
  }

  /// The masks live at @p frame; @p frame must not decrease between calls.
  const std::vector<size_t>& advance(int frame) {
    const auto live = [&](size_t note) { return frame < notes_[note].frame_end(); };
    active_.erase(
        std::remove_if(active_.begin(), active_.end(), [&](size_t note) { return !live(note); }),
        active_.end());
    bool joined = false;
    while (next_ < by_start_.size() && notes_[by_start_[next_]].frame_start <= frame) {
      if (live(by_start_[next_])) {
        active_.push_back(by_start_[next_]);
        joined = true;
      }
      ++next_;
    }
    if (joined) std::sort(active_.begin(), active_.end());
    return active_;
  }

 private:
  const std::vector<NoteMask>& notes_;
  std::vector<size_t> by_start_;
  std::vector<size_t> active_;
  size_t next_ = 0;
};

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
