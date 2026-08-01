/// @file loudness_measure.cpp
/// @brief Stateless LUFS / true-peak helpers. The only `mastering/common/`
///        translation unit allowed to depend on `metering/`.

#include "mastering/common/loudness_measure.h"

#include <algorithm>
#include <limits>
#include <vector>

#include "metering/lufs.h"
#include "metering/true_peak.h"
#include "util/exception.h"

namespace sonare::mastering::common {

float measure_lufs(const Audio& audio) { return metering::lufs(audio).integrated_lufs; }

float measure_lufs(const float* samples, std::size_t length, int sample_rate) {
  if (length == 0) {
    // Build a zero-length Audio so callers get the same gating behaviour as
    // the Audio overload for empty inputs.
    Audio audio = Audio::from_buffer(nullptr, 0, sample_rate);
    return metering::lufs(audio).integrated_lufs;
  }
  if (samples == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "measure_lufs: samples pointer is null with non-zero length");
  }
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  return metering::lufs(audio).integrated_lufs;
}

float measure_lufs_interleaved(const float* samples, std::size_t frames, int channels,
                               int sample_rate) {
  if (frames == 0) {
    return metering::lufs_interleaved(nullptr, 0, channels, sample_rate).integrated_lufs;
  }
  if (samples == nullptr) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "measure_lufs_interleaved: samples pointer is null with non-zero frame count");
  }
  return metering::lufs_interleaved(samples, frames, channels, sample_rate).integrated_lufs;
}

float measure_lra(const Audio& audio) { return metering::lufs(audio).loudness_range; }

float measure_lra_interleaved(const float* samples, std::size_t frames, int channels,
                              int sample_rate) {
  if (frames == 0) {
    return metering::lufs_interleaved(nullptr, 0, channels, sample_rate).loudness_range;
  }
  if (samples == nullptr) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "measure_lra_interleaved: samples pointer is null with non-zero frame count");
  }
  return metering::lufs_interleaved(samples, frames, channels, sample_rate).loudness_range;
}

float measure_true_peak_dbtp(const Audio& audio, int oversample_factor) {
  return metering::true_peak_db(audio, oversample_factor);
}

LufsAndTruePeak measure_lufs_and_true_peak(const Audio& audio, int true_peak_oversample) {
  LufsAndTruePeak result;
  result.integrated_lufs = metering::lufs(audio).integrated_lufs;
  result.true_peak_dbtp = metering::true_peak_db(audio, true_peak_oversample);
  return result;
}

LoudnessSummary measure_loudness_summary(const Audio& audio, int true_peak_oversample) {
  const metering::LufsResult lufs = metering::lufs(audio);
  return {lufs.integrated_lufs, lufs.max_momentary_lufs, lufs.max_short_term_lufs,
          metering::true_peak_db(audio, true_peak_oversample), lufs.loudness_range};
}

LoudnessSummary measure_loudness_summary_interleaved(const float* samples, std::size_t frames,
                                                     int channels, int sample_rate,
                                                     int true_peak_oversample) {
  const metering::LufsResult lufs =
      metering::lufs_interleaved(samples, frames, channels, sample_rate);
  float true_peak_dbtp = -std::numeric_limits<float>::infinity();
  std::vector<float> channel(frames);
  for (int index = 0; index < channels; ++index) {
    for (std::size_t frame = 0; frame < frames; ++frame) {
      channel[frame] =
          samples[frame * static_cast<std::size_t>(channels) + static_cast<std::size_t>(index)];
    }
    const Audio audio = Audio::from_buffer(channel.data(), channel.size(), sample_rate);
    true_peak_dbtp = std::max(true_peak_dbtp, metering::true_peak_db(audio, true_peak_oversample));
  }
  return {lufs.integrated_lufs, lufs.max_momentary_lufs, lufs.max_short_term_lufs, true_peak_dbtp,
          lufs.loudness_range};
}

}  // namespace sonare::mastering::common
