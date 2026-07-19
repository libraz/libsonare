#pragma once

/// @file coreaudio_render_utils.h
/// @brief SDK-free, deterministic helpers for the CoreAudio render trampoline.

#include <cstddef>
#include <cstdint>
#include <limits>

namespace sonare::host::backends::detail {

struct CoreAudioCallbackFramePlan {
  int render_frames = 0;
  int64_t next_sample_time = 0;
  uint32_t xrun_delta = 0;
};

/// Bound the engine request to its negotiated scratch while advancing the
/// device clock by every frame the hardware actually requested. An oversize
/// hardware callback is one xrun, even though its safe prefix can still render.
inline CoreAudioCallbackFramePlan plan_coreaudio_callback(uint32_t device_frames,
                                                          int negotiated_max_frames,
                                                          int64_t sample_time) noexcept {
  CoreAudioCallbackFramePlan plan;
  if (negotiated_max_frames > 0) {
    const uint32_t maximum = static_cast<uint32_t>(negotiated_max_frames);
    plan.render_frames = static_cast<int>(device_frames < maximum ? device_frames : maximum);
    plan.xrun_delta = device_frames > maximum ? 1u : 0u;
  }

  const int64_t increment = static_cast<int64_t>(device_frames);
  plan.next_sample_time = sample_time > std::numeric_limits<int64_t>::max() - increment
                              ? std::numeric_limits<int64_t>::max()
                              : sample_time + increment;
  return plan;
}

/// Copy a bounded planar engine prefix into an interleaved device buffer and
/// zero everything else, including every frame beyond the negotiated maximum.
inline void copy_coreaudio_interleaved(float* destination, size_t destination_frames,
                                       int destination_channels, const float* const* source,
                                       int source_channels, int render_frames) noexcept {
  if (destination == nullptr || destination_channels <= 0) return;
  for (size_t frame = 0; frame < destination_frames; ++frame) {
    const bool rendered = render_frames > 0 && frame < static_cast<size_t>(render_frames);
    for (int channel = 0; channel < destination_channels; ++channel) {
      destination[frame * static_cast<size_t>(destination_channels) +
                  static_cast<size_t>(channel)] =
          rendered && source != nullptr && channel < source_channels && source[channel] != nullptr
              ? source[channel][frame]
              : 0.0f;
    }
  }
}

/// Non-interleaved equivalent of copy_coreaudio_interleaved().
inline void copy_coreaudio_planar_channel(float* destination, size_t destination_frames,
                                          const float* source, int render_frames) noexcept {
  if (destination == nullptr) return;
  for (size_t frame = 0; frame < destination_frames; ++frame) {
    destination[frame] =
        source != nullptr && render_frames > 0 && frame < static_cast<size_t>(render_frames)
            ? source[frame]
            : 0.0f;
  }
}

}  // namespace sonare::host::backends::detail
