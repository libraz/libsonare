#pragma once

#include <algorithm>
#include <cstddef>
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
constexpr uint32_t kEngineParamLaneMask = 0x0000FF00u;
constexpr uint32_t kEngineParamKindMask = 0x000000FFu;
constexpr uint32_t kEngineParamLaneShift = 8u;

// Arrangement typed track automation is resolved to this engine namespace on
// the control thread.  Keep the encoding in one header so the compiler and the
// realtime router cannot drift while the public project ABI remains opaque.
inline constexpr uint32_t make_track_lane_param_id(size_t lane_index,
                                                   uint32_t target_kind) noexcept {
  return kEngineParamNamespace |
         ((static_cast<uint32_t>(lane_index) << kEngineParamLaneShift) & kEngineParamLaneMask) |
         (target_kind & kEngineParamKindMask);
}

}  // namespace sonare::engine
