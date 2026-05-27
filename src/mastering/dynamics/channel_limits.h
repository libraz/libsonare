#pragma once

/// @file channel_limits.h
/// @brief Shared channel-state sizing for realtime-safe dynamics processors.

#include <cstddef>

namespace sonare::mastering::dynamics {

/// Dynamics processors preallocate per-channel state during prepare() so their
/// audio-thread process() path does not resize vectors.
constexpr std::size_t kRealtimePreparedChannels = 64;

}  // namespace sonare::mastering::dynamics
