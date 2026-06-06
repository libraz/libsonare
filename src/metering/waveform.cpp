#include "metering/waveform.h"

#include <algorithm>
#include <limits>

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#elif defined(__SSE__)
#include <xmmintrin.h>
#endif

#include "util/exception.h"

namespace sonare::metering {
namespace {

struct MinMax {
  float min;
  float max;
};

[[maybe_unused]] MinMax scalar_minmax_contiguous(const float* samples, size_t count) {
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

MinMax minmax_contiguous(const float* samples, size_t count) {
#if defined(__wasm_simd128__)
  size_t index = 0;
  v128_t min_v = wasm_f32x4_splat(std::numeric_limits<float>::infinity());
  v128_t max_v = wasm_f32x4_splat(-std::numeric_limits<float>::infinity());
  for (; index + 4 <= count; index += 4) {
    const v128_t values = wasm_v128_load(samples + index);
    min_v = wasm_f32x4_pmin(min_v, values);
    max_v = wasm_f32x4_pmax(max_v, values);
  }
  alignas(16) float min_lane[4];
  alignas(16) float max_lane[4];
  wasm_v128_store(min_lane, min_v);
  wasm_v128_store(max_lane, max_v);
  float mn = std::min(std::min(min_lane[0], min_lane[1]), std::min(min_lane[2], min_lane[3]));
  float mx = std::max(std::max(max_lane[0], max_lane[1]), std::max(max_lane[2], max_lane[3]));
  for (; index < count; ++index) {
    const float v = samples[index];
    mn = std::min(mn, v);
    mx = std::max(mx, v);
  }
  return {mn, mx};
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  size_t index = 0;
  float32x4_t min_v = vdupq_n_f32(std::numeric_limits<float>::infinity());
  float32x4_t max_v = vdupq_n_f32(-std::numeric_limits<float>::infinity());
  for (; index + 4 <= count; index += 4) {
    const float32x4_t values = vld1q_f32(samples + index);
    min_v = vminq_f32(min_v, values);
    max_v = vmaxq_f32(max_v, values);
  }
#if defined(__aarch64__)
  float mn = vminvq_f32(min_v);
  float mx = vmaxvq_f32(max_v);
#else
  const float32x2_t min_pair = vpmin_f32(vget_low_f32(min_v), vget_high_f32(min_v));
  const float32x2_t max_pair = vpmax_f32(vget_low_f32(max_v), vget_high_f32(max_v));
  const float32x2_t min_reduced = vpmin_f32(min_pair, min_pair);
  const float32x2_t max_reduced = vpmax_f32(max_pair, max_pair);
  float mn = vget_lane_f32(min_reduced, 0);
  float mx = vget_lane_f32(max_reduced, 0);
#endif
  for (; index < count; ++index) {
    const float v = samples[index];
    mn = std::min(mn, v);
    mx = std::max(mx, v);
  }
  return {mn, mx};
#elif defined(__SSE__)
  size_t index = 0;
  __m128 min_v = _mm_set1_ps(std::numeric_limits<float>::infinity());
  __m128 max_v = _mm_set1_ps(-std::numeric_limits<float>::infinity());
  for (; index + 4 <= count; index += 4) {
    const __m128 values = _mm_loadu_ps(samples + index);
    min_v = _mm_min_ps(min_v, values);
    max_v = _mm_max_ps(max_v, values);
  }
  alignas(16) float min_lane[4];
  alignas(16) float max_lane[4];
  _mm_store_ps(min_lane, min_v);
  _mm_store_ps(max_lane, max_v);
  float mn = std::min(std::min(min_lane[0], min_lane[1]), std::min(min_lane[2], min_lane[3]));
  float mx = std::max(std::max(max_lane[0], max_lane[1]), std::max(max_lane[2], max_lane[3]));
  for (; index < count; ++index) {
    const float v = samples[index];
    mn = std::min(mn, v);
    mx = std::max(mx, v);
  }
  return {mn, mx};
#else
  return scalar_minmax_contiguous(samples, count);
#endif
}

}  // namespace

WaveformPeaksResult waveform_peaks(const float* samples, size_t frames, int channels,
                                   size_t samples_per_bucket) {
  SONARE_CHECK(samples != nullptr || frames == 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(channels > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(samples_per_bucket > 0, ErrorCode::InvalidParameter);

  WaveformPeaksResult result;
  result.channels = channels;
  result.samples_per_bucket = samples_per_bucket;
  result.bucket_count = frames == 0 ? 0 : (frames + samples_per_bucket - 1) / samples_per_bucket;

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
