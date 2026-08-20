#pragma once

/// @file coreaudio_render_utils.h
/// @brief SDK-free, deterministic helpers for the CoreAudio render trampoline.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace sonare::host::backends::detail {

struct CoreAudioCallbackFramePlan {
  int render_frames = 0;
  int64_t next_sample_time = 0;
  uint32_t xrun_delta = 0;
};

/// Convert a latency/safety-offset/buffer-size value counted in the device's
/// own hardware clock domain into samples at a different render domain (the
/// AU's negotiated stream-format rate), by ratio, instead of leaving it
/// silently mismatched against the render domain the caller reports it in.
/// Falls back to an unconverted pass-through when either rate is non-finite
/// or non-positive (the "unknown" case, e.g. a device property was
/// unavailable), so a property-read failure degrades to leaving the value as-
/// is rather than corrupting it with a division by an invalid rate.
inline int64_t scale_coreaudio_device_domain_samples(int64_t device_domain_samples,
                                                     double device_domain_rate,
                                                     double render_domain_rate) noexcept {
  if (!(device_domain_rate > 0.0) || !std::isfinite(device_domain_rate) ||
      !(render_domain_rate > 0.0) || !std::isfinite(render_domain_rate)) {
    return device_domain_samples;
  }
  const double scaled =
      static_cast<double>(device_domain_samples) * (render_domain_rate / device_domain_rate);
  if (!std::isfinite(scaled)) return device_domain_samples;
  return static_cast<int64_t>(std::llround(scaled));
}

/// True when the device's own hardware clock domain and the AU's negotiated
/// render-callback domain are the same rate, so a quantity spanning both
/// (e.g. comparing a HAL sample-clock timestamp against an accumulated
/// render-domain frame count to detect an xrun) stays meaningful. The
/// comparison is exact: a rate that only nearly matches still accumulates
/// drift, and reporting a mismatch there is the safe direction. An
/// "unknown" device rate (<= 0, the property was unavailable) is treated as a
/// match: the caller has no evidence of a mismatch, so it keeps whatever
/// behaviour it already has rather than newly disabling the check on missing
/// telemetry.
inline bool coreaudio_sample_clock_domains_match(double device_domain_rate,
                                                 double render_domain_rate) noexcept {
  if (!(device_domain_rate > 0.0)) return true;
  return device_domain_rate == render_domain_rate;
}

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
