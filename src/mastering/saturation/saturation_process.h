#pragma once

/// @file saturation_process.h
/// @brief Shared per-channel process loops for the memoryless saturation processors.

#include <cstddef>
#include <string>
#include <vector>

#include "rt/oversampler.h"
#include "rt/parallel_paths.h"
#include "util/exception.h"

namespace sonare::mastering::saturation::detail {

/// @brief Applies `shape(sample, channel)` in place to every sample of every channel.
template <typename Shape>
inline void process_direct(float* const* channels, int num_channels, int num_samples,
                           Shape&& shape) {
  for (int ch = 0; ch < num_channels; ++ch) {
    for (int i = 0; i < num_samples; ++i) {
      channels[ch][i] = shape(channels[ch][i], ch);
    }
  }
}

/// @brief Oversampled path: upsample, `curve(x)` at the oversampled rate, downsample, then
/// `wet_scale(wet)` and the dry/wet mix at the base rate.
/// @details @p paths holds the dry path at index 0 and the wet path at index 1. Uses only
///   preallocated scratch; blocks wider than the prepared size are rejected.
template <typename Curve, typename WetScale>
inline void process_oversampled(float* const* channels, int num_channels, int num_samples,
                                int oversample_factor, sonare::rt::Oversampler& oversampler,
                                std::vector<sonare::rt::Oversampler::StreamingState>& states,
                                sonare::rt::ParallelPaths& paths, std::vector<float>& up_scratch,
                                std::vector<float>& down_scratch, float mix, const char* name,
                                Curve&& curve, WetScale&& wet_scale) {
  const size_t os_samples =
      static_cast<size_t>(num_samples) * static_cast<size_t>(oversample_factor);
  if (os_samples > up_scratch.size() || static_cast<size_t>(num_samples) > down_scratch.size()) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        std::string("num_samples exceeds prepared ") + name + " oversampling scratch");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    const float* input = channels[ch];
    auto& state = states[static_cast<size_t>(ch)];
    oversampler.upsample_to_streaming(input, static_cast<size_t>(num_samples), up_scratch.data(),
                                      up_scratch.size(), &state);
    for (size_t i = 0; i < os_samples; ++i) {
      up_scratch[i] = curve(up_scratch[i]);
    }
    oversampler.downsample_to_streaming(up_scratch.data(), os_samples, down_scratch.data(),
                                        down_scratch.size(), &state);
    for (int i = 0; i < num_samples; ++i) {
      const float dry = paths.align(0, static_cast<size_t>(ch), input[i]);
      const float wet =
          paths.align(1, static_cast<size_t>(ch), wet_scale(down_scratch[static_cast<size_t>(i)]));
      channels[ch][i] = dry * (1.0f - mix) + wet * mix;
    }
  }
}

}  // namespace sonare::mastering::saturation::detail
