#include "metering/waveform.h"

#include <algorithm>
#include <limits>

#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::metering {
namespace {

struct MinMax {
  float min;
  float max;
};

MinMax minmax_contiguous(const float* samples, size_t count) {
  if (count == 0) return {0.0f, 0.0f};
  float mn = std::numeric_limits<float>::infinity();
  float mx = -std::numeric_limits<float>::infinity();
  for (size_t index = 0; index < count; ++index) {
    const float v = samples[index];
    mn = std::min(mn, v);
    mx = std::max(mx, v);
  }
  return {mn, mx};
}

MinMax scalar_minmax_interleaved_channel(const float* samples, size_t start, size_t end,
                                         int channels, int channel) {
  if (start >= end) return {0.0f, 0.0f};
  float mn = std::numeric_limits<float>::infinity();
  float mx = -std::numeric_limits<float>::infinity();
  const size_t channel_count = static_cast<size_t>(channels);
  const size_t ch = static_cast<size_t>(channel);
  for (size_t frame = start; frame < end; ++frame) {
    const float v = samples[frame * channel_count + ch];
    mn = std::min(mn, v);
    mx = std::max(mx, v);
  }
  return {mn, mx};
}

}  // namespace

WaveformPeaksResult waveform_peaks(const float* samples, size_t frames, int channels,
                                   size_t samples_per_bucket) {
  SONARE_CHECK(samples != nullptr || frames == 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(channels > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(samples_per_bucket > 0, ErrorCode::InvalidParameter);
  const size_t channel_count = static_cast<size_t>(channels);
  SONARE_CHECK(frames <= std::numeric_limits<size_t>::max() / channel_count,
               ErrorCode::InvalidParameter);
  // A bucket whose samples are not finite has no min/max to report, and the 0/0
  // it would otherwise carry is what a display draws as silence.
  SONARE_CHECK_MSG(numeric::all_finite(samples, frames * channel_count),
                   ErrorCode::InvalidParameter,
                   "waveform_peaks: audio contains a non-finite sample");

  WaveformPeaksResult result;
  result.channels = channels;
  result.samples_per_bucket = samples_per_bucket;
  result.bucket_count = frames == 0 ? 0 : (frames - 1) / samples_per_bucket + 1;

  const size_t total = static_cast<size_t>(channels) * result.bucket_count;
  result.min.assign(total, 0.0f);
  result.max.assign(total, 0.0f);

  for (size_t bucket = 0; bucket < result.bucket_count; ++bucket) {
    const size_t start = bucket * samples_per_bucket;
    const size_t end = std::min(frames, start + samples_per_bucket);
    for (int ch = 0; ch < channels; ++ch) {
      const MinMax bucket_minmax =
          channels == 1 ? minmax_contiguous(samples + start, end - start)
                        : scalar_minmax_interleaved_channel(samples, start, end, channels, ch);
      const size_t out_index = static_cast<size_t>(ch) * result.bucket_count + bucket;
      result.min[out_index] = bucket_minmax.min;
      result.max[out_index] = bucket_minmax.max;
    }
  }

  return result;
}

std::vector<WaveformPeaksResult> waveform_peak_pyramid(
    const float* samples, size_t frames, int channels,
    const std::vector<size_t>& samples_per_bucket_levels) {
  SONARE_CHECK(!samples_per_bucket_levels.empty(), ErrorCode::InvalidParameter);
  std::vector<WaveformPeaksResult> result;
  result.reserve(samples_per_bucket_levels.size());
  for (size_t level : samples_per_bucket_levels) {
    result.push_back(waveform_peaks(samples, frames, channels, level));
  }
  return result;
}

}  // namespace sonare::metering
