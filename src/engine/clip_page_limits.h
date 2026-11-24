#pragma once

/// @file clip_page_limits.h
/// @brief Shared resource limits for paged clip providers on every surface.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "util/parameter_checks.h"

namespace sonare::engine {

inline constexpr int kMaxClipPageChannels = 64;
inline constexpr int64_t kMaxClipPageFrames = int64_t{1} << 24;
inline constexpr int64_t kMaxClipPageCount = int64_t{1} << 20;

/// The exact frame count accepted for a page after its index and dimensions have been validated.
inline int64_t clip_page_expected_frame_count(int64_t page_index, int64_t samples,
                                              int64_t page_frames) noexcept {
  const int64_t page_start = page_index * page_frames;
  return std::min(page_frames, samples - page_start);
}

/// Refuses a page whose frame count differs from its final-page-aware expected length.
inline void admit_clip_page_frame_count(int64_t page_index, int64_t samples, int64_t page_frames,
                                        int64_t frames, const char* subject) {
  const std::array<int64_t, 1> accepted = {
      clip_page_expected_frame_count(page_index, samples, page_frames)};
  sonare::util::check::one_of(frames, accepted, subject);
}

/// Validates dimensions before either the ownership or raw-pointer metadata
/// arrays are allocated. The subtraction form avoids addition overflow.
inline bool validate_clip_page_dimensions(int channels, int64_t samples, int64_t page_frames,
                                          int64_t* out_page_count = nullptr) noexcept {
  if (channels <= 0 || channels > kMaxClipPageChannels || samples <= 0 || page_frames <= 0 ||
      page_frames > kMaxClipPageFrames) {
    return false;
  }
  const int64_t page_count = 1 + (samples - 1) / page_frames;
  if (page_count <= 0 || page_count > kMaxClipPageCount) return false;
  if (out_page_count != nullptr) *out_page_count = page_count;
  return true;
}

}  // namespace sonare::engine
