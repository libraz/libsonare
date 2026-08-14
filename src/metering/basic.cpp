#include "metering/basic.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "metering/clipping.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"

namespace sonare::metering {

using sonare::constants::kEpsilon;
using sonare::constants::kFloorDb;

namespace {

float frame_rms_db(const float* data, size_t start, size_t end) {
  if (start >= end) return -std::numeric_limits<float>::infinity();

  const float frame_rms = rms(data + start, end - start);
  if (frame_rms < kEpsilon) return -std::numeric_limits<float>::infinity();
  return linear_to_db(frame_rms);
}

}  // namespace

float peak_db(const Audio& audio) {
  // Silence/empty reports the shared finite dB floor (kFloorDb, -120 dB) rather
  // than -inf, matching the true-peak / spectrum / dynamic-range meters. A finite
  // floor is JSON-safe (-inf serializes to null and silently drops the field).
  if (audio.empty()) return kFloorDb;

  const float* data = audio.data();
  const float peak = peak_abs(data, audio.size());

  if (peak < kEpsilon) return kFloorDb;
  return linear_to_db(peak);
}

float rms_db(const Audio& audio) {
  // Silence/empty reports kFloorDb (see peak_db) instead of a JSON-unsafe -inf.
  if (audio.empty()) return kFloorDb;

  const float* data = audio.data();
  const float audio_rms = rms(data, audio.size());
  if (audio_rms < kEpsilon) return kFloorDb;
  return linear_to_db(audio_rms);
}

float crest_factor_db(const Audio& audio) {
  const float peak = peak_db(audio);
  const float rms = rms_db(audio);
  if (!std::isfinite(peak) || !std::isfinite(rms)) {
    // Silent/empty input has no meaningful peak-to-RMS ratio. Return 0 dB (a
    // usable neutral) rather than +inf, which callers cannot average or display.
    return 0.0f;
  }
  return peak - rms;
}

float crest_factor_db_interleaved(const float* samples, std::size_t frames, int channels) {
  if (samples == nullptr || frames == 0 || channels <= 0) return 0.0f;
  const std::size_t total = frames * static_cast<std::size_t>(channels);
  const float peak = peak_abs(samples, total);
  const float level = rms(samples, total);
  // Silent input has no meaningful peak-to-RMS ratio; return the same 0 dB
  // neutral the mono meter reports rather than a value callers cannot display.
  if (peak < kEpsilon || level < kEpsilon) return 0.0f;
  return linear_to_db(peak) - linear_to_db(level);
}

float clipping_ratio(const Audio& audio, float threshold) {
  return detect_clipping(audio, threshold).clipping_ratio;
}

float silence_ratio(const Audio& audio, float threshold_db, int frame_length, int hop_length) {
  SONARE_CHECK(frame_length > 0 && hop_length > 0, ErrorCode::InvalidParameter);
  if (audio.empty()) return 0.0f;

  size_t silent_frames = 0;
  size_t total_frames = 0;
  const float* data = audio.data();
  for (size_t start = 0; start < audio.size(); start += static_cast<size_t>(hop_length)) {
    const size_t end = std::min(audio.size(), start + static_cast<size_t>(frame_length));
    if (frame_rms_db(data, start, end) < threshold_db) ++silent_frames;
    ++total_frames;
    if (end == audio.size()) break;
  }

  if (total_frames == 0) return 0.0f;
  return static_cast<float>(silent_frames) / static_cast<float>(total_frames);
}

float dc_offset(const Audio& audio) {
  if (audio.empty()) return 0.0f;

  double sum = 0.0;
  const float* data = audio.data();
  for (size_t i = 0; i < audio.size(); ++i) {
    sum += static_cast<double>(data[i]);
  }
  return static_cast<float>(sum / static_cast<double>(audio.size()));
}

}  // namespace sonare::metering
