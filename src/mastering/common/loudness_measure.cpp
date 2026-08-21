/// @file loudness_measure.cpp
/// @brief Stateless LUFS / true-peak helpers. The only `mastering/common/`
///        translation unit allowed to depend on `metering/`.

#include "mastering/common/loudness_measure.h"

#include <algorithm>
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
  float true_peak = 0.0f;
  std::vector<float> channel(frames);
  for (int index = 0; index < channels; ++index) {
    for (std::size_t frame = 0; frame < frames; ++frame) {
      channel[frame] =
          samples[frame * static_cast<std::size_t>(channels) + static_cast<std::size_t>(index)];
    }
    // The buffer overload measures the de-interleaved channel in place. Wrapping
    // it in an Audio first would deep-copy a second track-length buffer for
    // nothing, which on an album-length master is hundreds of megabytes held
    // only to be read once.
    true_peak = std::max(true_peak,
                         metering::true_peak(channel.data(), channel.size(), true_peak_oversample));
  }
  return {lufs.integrated_lufs, lufs.max_momentary_lufs, lufs.max_short_term_lufs,
          true_peak_to_dbtp(true_peak), lufs.loudness_range};
}

LoudnessSummary measure_loudness_summary_stereo_planar(const float* left, const float* right,
                                                       std::size_t frames, int sample_rate,
                                                       int true_peak_oversample) {
  if ((left == nullptr || right == nullptr) && frames != 0) {
    throw SonareException(
        ErrorCode::InvalidParameter,
        "measure_loudness_summary_stereo_planar: channel pointer is null with non-zero frames");
  }
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
    lufs = metering::lufs_interleaved(interleaved.data(), frames, 2, sample_rate);
  }
  const float true_peak = std::max(metering::true_peak(left, frames, true_peak_oversample),
                                   metering::true_peak(right, frames, true_peak_oversample));
  return {lufs.integrated_lufs, lufs.max_momentary_lufs, lufs.max_short_term_lufs,
          true_peak_to_dbtp(true_peak), lufs.loudness_range};
}

}  // namespace sonare::mastering::common
