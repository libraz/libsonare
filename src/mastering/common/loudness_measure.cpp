/// @file loudness_measure.cpp
/// @brief Stateless LUFS / true-peak helpers. The only `mastering/common/`
///        translation unit allowed to depend on `metering/`.

#include "mastering/common/loudness_measure.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "metering/lufs.h"
#include "metering/true_peak.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

namespace sonare::mastering::common {
namespace {

// Same silence handling metering::true_peak_db() applies to an Audio: below the
// shared epsilon report the finite dB floor rather than -inf, so every meter
// spells silence the same way. Taking the maximum in the linear domain first
// and converting once is equivalent, because the conversion is monotonic.
float true_peak_to_dbtp(float peak) noexcept {
  if (peak < sonare::constants::kEpsilon) return sonare::constants::kFloorDb;
  return linear_to_db(peak);
}

// The series carry the library defaults; naming the config here also pins the
// four mirrors in loudness_measure.h against metering's own values.
constexpr metering::LufsConfig kSeriesConfig{};
static_assert(kMomentaryWindowSeconds == kSeriesConfig.momentary_duration_sec,
              "momentary window mirror drifted from metering::LufsConfig");
static_assert(kShortTermWindowSeconds == kSeriesConfig.short_term_duration_sec,
              "short-term window mirror drifted from metering::LufsConfig");
static_assert(kMomentaryWindowSeconds * (1.0f - metering::kLufsMomentaryOverlap) ==
                  kLoudnessSeriesHopSeconds,
              "momentary hop mirror drifted from metering::kLufsMomentaryOverlap");
static_assert(kLoudnessSeriesHopSeconds == metering::kLufsShortTermHopSec,
              "short-term hop mirror drifted from metering::kLufsShortTermHopSec");

// Linear-domain peak across de-interleaved channels. The buffer overload
// measures the channel in place; wrapping it in an Audio would deep-copy a
// second track-length buffer for nothing, which on an album-length master is
// hundreds of megabytes held only to be read once.
float interleaved_true_peak(const float* samples, std::size_t frames, int channels,
                            int oversample_factor) {
  float peak = 0.0f;
  std::vector<float> channel(frames);
  for (int index = 0; index < channels; ++index) {
    for (std::size_t frame = 0; frame < frames; ++frame) {
      channel[frame] =
          samples[frame * static_cast<std::size_t>(channels) + static_cast<std::size_t>(index)];
    }
    peak = std::max(peak, metering::true_peak(channel.data(), channel.size(), oversample_factor));
  }
  return peak;
}

// Shared body behind both interleaved summary overloads; @p series may be null.
LoudnessSummary interleaved_summary(const float* samples, std::size_t frames, int channels,
                                    int sample_rate, int true_peak_oversample,
                                    LoudnessSeries* series) {
  const metering::LufsResult lufs =
      metering::lufs_interleaved(samples, frames, channels, sample_rate, kSeriesConfig,
                                 series != nullptr ? &series->momentary_lufs : nullptr,
                                 series != nullptr ? &series->short_term_lufs : nullptr);
  return {lufs.integrated_lufs, lufs.max_momentary_lufs, lufs.max_short_term_lufs,
          true_peak_to_dbtp(interleaved_true_peak(samples, frames, channels, true_peak_oversample)),
          lufs.loudness_range};
}

// Shared body behind both stereo planar summary overloads; @p series may be null.
LoudnessSummary stereo_planar_summary(const float* left, const float* right, std::size_t frames,
                                      int sample_rate, int true_peak_oversample,
                                      LoudnessSeries* series) {
  // BS.1770 channel summing is only exposed on an interleaved buffer, so build
  // one, measure, and release it before the per-channel true peak — which reads
  // the caller's planar buffers directly. The interleaved copy is then the only
  // track-length temporary this call ever holds, and it is gone before the
  // second measurement starts.
  metering::LufsResult lufs;
  {
    std::vector<float> interleaved(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
      interleaved[2 * frame] = left[frame];
      interleaved[2 * frame + 1] = right[frame];
    }
    lufs = metering::lufs_interleaved(interleaved.data(), frames, 2, sample_rate, kSeriesConfig,
                                      series != nullptr ? &series->momentary_lufs : nullptr,
                                      series != nullptr ? &series->short_term_lufs : nullptr);
  }
  const float true_peak = std::max(metering::true_peak(left, frames, true_peak_oversample),
                                   metering::true_peak(right, frames, true_peak_oversample));
  return {lufs.integrated_lufs, lufs.max_momentary_lufs, lufs.max_short_term_lufs,
          true_peak_to_dbtp(true_peak), lufs.loudness_range};
}

// Silence is -inf in a loudness series. Floor it the way true_peak_to_dbtp
// floors a silent peak, so differencing two silent blocks yields 0, not NaN.
float floored_lufs(float lufs) noexcept {
  return std::isfinite(lufs) ? lufs : sonare::constants::kFloorDb;
}

void difference_series(const std::vector<float>& before, const std::vector<float>& after,
                       std::vector<float>* out) {
  if (out == nullptr) return;
  out->resize(before.size());
  for (std::size_t index = 0; index < before.size(); ++index) {
    (*out)[index] = floored_lufs(after[index]) - floored_lufs(before[index]);
  }
}

}  // namespace

float measure_lufs(const Audio& audio) { return metering::lufs(audio).integrated_lufs; }

float measure_lufs(const float* samples, std::size_t length, int sample_rate) {
  if (samples == nullptr && length != 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "measure_lufs: samples pointer is null with non-zero length");
  }
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  return metering::lufs(audio).integrated_lufs;
}

float measure_lufs_interleaved(const float* samples, std::size_t frames, int channels,
                               int sample_rate) {
  if (samples == nullptr && frames != 0) {
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

float measure_true_peak_dbtp_stereo_planar(const float* left, const float* right,
                                           std::size_t frames, int oversample_factor) {
  if ((left == nullptr || right == nullptr) && frames != 0) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "measure_true_peak_dbtp_stereo_planar: channel pointer is null with non-zero frames");
  }
  return true_peak_to_dbtp(std::max(metering::true_peak(left, frames, oversample_factor),
                                    metering::true_peak(right, frames, oversample_factor)));
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
  return interleaved_summary(samples, frames, channels, sample_rate, true_peak_oversample, nullptr);
}

LoudnessSummary measure_loudness_summary_stereo_planar(const float* left, const float* right,
                                                       std::size_t frames, int sample_rate,
                                                       int true_peak_oversample) {
  if ((left == nullptr || right == nullptr) && frames != 0) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "measure_loudness_summary_stereo_planar: channel pointer is null with non-zero frames");
  }
  return stereo_planar_summary(left, right, frames, sample_rate, true_peak_oversample, nullptr);
}

LoudnessSummary measure_loudness_summary_interleaved(const float* samples, std::size_t frames,
                                                     int channels, int sample_rate,
                                                     int true_peak_oversample,
                                                     LoudnessSeries* series) {
  if (series == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "measure_loudness_summary_interleaved: series pointer is null");
  }
  if (samples == nullptr && frames != 0) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "measure_loudness_summary_interleaved: samples pointer is null with non-zero frame count");
  }
  return interleaved_summary(samples, frames, channels, sample_rate, true_peak_oversample, series);
}

LoudnessSummary measure_loudness_summary_stereo_planar(const float* left, const float* right,
                                                       std::size_t frames, int sample_rate,
                                                       int true_peak_oversample,
                                                       LoudnessSeries* series) {
  if (series == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "measure_loudness_summary_stereo_planar: series pointer is null");
  }
  if ((left == nullptr || right == nullptr) && frames != 0) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "measure_loudness_summary_stereo_planar: channel pointer is null with non-zero frames");
  }
  return stereo_planar_summary(left, right, frames, sample_rate, true_peak_oversample, series);
}

void measure_loudness_series_interleaved(const float* samples, std::size_t frames, int channels,
                                         int sample_rate, LoudnessSeries* series) {
  if (series == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "measure_loudness_series_interleaved: series pointer is null");
  }
  if (samples == nullptr && frames != 0) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "measure_loudness_series_interleaved: samples pointer is null with non-zero frame count");
  }
  metering::lufs_interleaved(samples, frames, channels, sample_rate, kSeriesConfig,
                             &series->momentary_lufs, &series->short_term_lufs);
}

void measure_loudness_series_stereo_planar(const float* left, const float* right,
                                           std::size_t frames, int sample_rate,
                                           LoudnessSeries* series) {
  if (series == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "measure_loudness_series_stereo_planar: series pointer is null");
  }
  if ((left == nullptr || right == nullptr) && frames != 0) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "measure_loudness_series_stereo_planar: channel pointer is null with non-zero frames");
  }
  std::vector<float> interleaved(frames * 2);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    interleaved[2 * frame] = left[frame];
    interleaved[2 * frame + 1] = right[frame];
  }
  metering::lufs_interleaved(interleaved.data(), frames, 2, sample_rate, kSeriesConfig,
                             &series->momentary_lufs, &series->short_term_lufs);
}

void stage_level_delta_lu(const LoudnessSeries& before, const LoudnessSeries& after,
                          std::vector<float>* momentary_delta,
                          std::vector<float>* short_term_delta) {
  if (before.momentary_lufs.size() != after.momentary_lufs.size() ||
      before.short_term_lufs.size() != after.short_term_lufs.size()) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "stage_level_delta_lu: series lengths differ, so the stage changed the frame count");
  }
  difference_series(before.momentary_lufs, after.momentary_lufs, momentary_delta);
  difference_series(before.short_term_lufs, after.short_term_lufs, short_term_delta);
}

LoudnessSummary measure_residual_loudness_summary(const float* before, const float* after,
                                                  std::size_t frames, int sample_rate,
                                                  int true_peak_oversample) {
  if ((before == nullptr || after == nullptr) && frames != 0) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "measure_residual_loudness_summary: input pointer is null with non-zero frames");
  }
  std::vector<float> residual(frames);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    residual[frame] = before[frame] - after[frame];
  }
  const metering::LufsResult lufs =
      metering::lufs_interleaved(residual.data(), frames, 1, sample_rate, kSeriesConfig);
  return {lufs.integrated_lufs, lufs.max_momentary_lufs, lufs.max_short_term_lufs,
          true_peak_to_dbtp(metering::true_peak(residual.data(), frames, true_peak_oversample)),
          lufs.loudness_range};
}

LoudnessSummary measure_residual_loudness_summary_stereo_planar(
    const float* before_left, const float* before_right, const float* after_left,
    const float* after_right, std::size_t frames, int sample_rate, int true_peak_oversample) {
  if ((before_left == nullptr || before_right == nullptr || after_left == nullptr ||
       after_right == nullptr) &&
      frames != 0) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "measure_residual_loudness_summary_stereo_planar: channel pointer is null with non-zero "
        "frames");
  }
  std::vector<float> left(frames);
  std::vector<float> right(frames);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    left[frame] = before_left[frame] - after_left[frame];
    right[frame] = before_right[frame] - after_right[frame];
  }
  return stereo_planar_summary(left.data(), right.data(), frames, sample_rate, true_peak_oversample,
                               nullptr);
}

}  // namespace sonare::mastering::common
