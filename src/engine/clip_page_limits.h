#pragma once

/// @file clip_page_limits.h
/// @brief Shared resource limits for paged clip providers on every surface.

#include <cstddef>
#include <cstdint>

namespace sonare::engine {

inline constexpr int kMaxClipPageChannels = 64;
inline constexpr int64_t kMaxClipPageFrames = int64_t{1} << 24;
inline constexpr int64_t kMaxClipPageCount = int64_t{1} << 20;

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
