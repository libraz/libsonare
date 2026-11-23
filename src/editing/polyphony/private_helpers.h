#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <string>
#include <vector>

#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/note_mask.h"
#include "util/insertion_sort.h"
#include "util/math_utils.h"
#include "util/parameter_checks.h"

namespace sonare::editing::polyphony::detail {

/// Every bin and frame a mask names lands inside the spectrum, and its own
/// sparse indexing is consistent.
inline void check_mask_shape(const NoteMask& mask, int n_bins, int n_frames,
                             const std::string& subject, const std::string& frame_count_subject) {
  const std::string mask_n_frames = subject + ".n_frames";
  const std::string mask_frame_start = subject + ".frame_start";
  util::check::integer_at_least(mask.n_frames, 0, mask_n_frames.c_str());
  util::check::integer_at_least(mask.frame_start, 0, mask_frame_start.c_str());
  if (mask.frame_end() > n_frames) {
    util::check::refuse_relation({mask_frame_start, mask_n_frames, frame_count_subject},
                                 "{0} plus the number of {1} must be at most {2}");
  }
  if (mask.frame_offset.size() != static_cast<size_t>(mask.n_frames) + 1) {
    util::check::refuse_relation({subject + ".frame_offset", mask_n_frames},
                                 "{0} must have {1} + 1 entries");
  }
  if (mask.weights.size() != mask.bins.size()) {
    util::check::refuse_relation({subject + ".weights", subject + ".bins"},
                                 "{0} and {1} must have the same number of entries");
  }
  if (mask.frame_offset.front() != 0) {
    util::check::integer_in_range(mask.frame_offset.front(), 0, 0,
                                  (subject + ".frame_offset[0]").c_str());
  }
  if (static_cast<size_t>(mask.frame_offset.back()) != mask.bins.size()) {
    util::check::refuse_relation({subject + ".frame_offset", subject + ".bins"},
                                 "{0} must end at the size of {1}");
  }
  for (size_t i = 1; i < mask.frame_offset.size(); ++i) {
    if (mask.frame_offset[i] < mask.frame_offset[i - 1]) {
      util::check::refuse_relation({subject + ".frame_offset[" + std::to_string(i) + "]",
                                    subject + ".frame_offset[" + std::to_string(i - 1) + "]"},
                                   "{0} must be at least {1}");
    }
  }
  for (size_t i = 0; i < mask.bins.size(); ++i) {
    const int32_t bin = mask.bins[i];
    if (!(bin >= 0 && bin < n_bins)) {
      util::check::integer_in_range(bin, 0, static_cast<long long>(n_bins) - 1,
                                    (subject + ".bins[" + std::to_string(i) + "]").c_str());
    }
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
