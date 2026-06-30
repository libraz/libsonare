#pragma once

#include <algorithm>
#include <cstdint>

namespace sonare::engine {

inline int64_t block_end_frame(int64_t block_start, int num_frames) noexcept {
  return block_start + static_cast<int64_t>(std::max(num_frames, 0));
}

inline bool command_belongs_to_block(int64_t sample_time, int64_t block_start,
                                     int num_frames) noexcept {
  return sample_time >= block_start && sample_time < block_end_frame(block_start, num_frames);
}

constexpr uint32_t kEngineParamNamespace = 0x4D580000u;
constexpr uint32_t kEngineParamNamespaceMask = 0xFFFF0000u;

}  // namespace sonare::engine
