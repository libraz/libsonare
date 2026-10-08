#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "core/audio.h"

namespace sonare::mastering::repair {

/// @brief Zero-pads @p audio up to one analysis frame of @p n_fft samples.
/// @details A short block takes the same STFT path as a full clip; the caller trims the
/// reconstruction back to the original length. Audio already one frame long is returned as is.
inline Audio padded_for_analysis(const Audio& audio, int n_fft) {
  if (static_cast<int>(audio.size()) >= n_fft) return audio;
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  samples.resize(static_cast<std::size_t>(n_fft), 0.0f);
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace sonare::mastering::repair
