#include "metering/basic.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "metering/clipping.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

namespace sonare::metering {

namespace {

// A non-finite sample has no level, and absorbing one would publish a level for audio that was
// never measured; refusing beats inventing one.
void require_measurable(const float* data, size_t n) {
  if (!numeric::all_finite(data, n)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, "audio",
                                            "contains a non-finite sample");
  }
}

float frame_rms_db(const float* data, size_t start, size_t end) {
  if (start >= end) return kNoLevelDb;

  return amplitude_to_level_db(rms(data + start, end - start));
}

}  // namespace

float peak_db(const Audio& audio) {
  // An empty buffer is not measured; silence has no measurable level (-inf).
  if (audio.empty()) return kNotMeasured;

  const float* data = audio.data();
  require_measurable(data, audio.size());
  return amplitude_to_level_db(peak_abs(data, audio.size()));
}

float rms_db(const Audio& audio) {
  if (audio.empty()) return kNotMeasured;

  const float* data = audio.data();
  require_measurable(data, audio.size());
  return amplitude_to_level_db(rms(data, audio.size()));
}

namespace {

// Silent input has no peak-to-RMS ratio, so both crest meters report it as not measured.
float crest_from_samples(const float* samples, std::size_t total) {
  require_measurable(samples, total);
  return level_difference_db(amplitude_to_level_db(peak_abs(samples, total)),
                             amplitude_to_level_db(rms(samples, total)));
}

}  // namespace

float crest_factor_db(const Audio& audio) {
  if (audio.empty()) return kNotMeasured;
  return crest_from_samples(audio.data(), audio.size());
}

float crest_factor_db_interleaved(const float* samples, std::size_t frames, int channels) {
  if (samples == nullptr || frames == 0 || channels <= 0) return kNotMeasured;
  const std::size_t total = frames * static_cast<std::size_t>(channels);
  return crest_from_samples(samples, total);
}

float clipping_ratio(const Audio& audio, float threshold) {
  return detect_clipping(audio, threshold).clipping_ratio;
}

float silence_ratio(const Audio& audio, float threshold_db, int frame_length, int hop_length) {
  util::check::integer_within(frame_length, kSilenceRatioFrameLengthDomain, "frame_length");
  util::check::integer_within(hop_length, kSilenceRatioHopLengthDomain, "hop_length");
  // A NaN threshold makes every comparison below false, so the function reports
  // "no silence at all" for a request it could not evaluate. The check belongs
  // here rather than in the C-ABI wrapper: WASM calls this core function
  // directly, the way detect_clipping's sibling guard is already inherited.
  util::check::finite_float(threshold_db, "threshold_db");
  if (audio.empty()) return kNotMeasured;

  size_t silent_frames = 0;
  size_t total_frames = 0;
  const float* data = audio.data();
  for (size_t start = 0; start < audio.size(); start += static_cast<size_t>(hop_length)) {
    const size_t end = std::min(audio.size(), start + static_cast<size_t>(frame_length));
    if (frame_rms_db(data, start, end) < threshold_db) ++silent_frames;
    ++total_frames;
    if (end == audio.size()) break;
  }

  return static_cast<float>(silent_frames) / static_cast<float>(total_frames);
}

float dc_offset(const Audio& audio) {
  if (audio.empty()) return kNotMeasured;

  const float* data = audio.data();
  require_measurable(data, audio.size());
  double sum = 0.0;
  for (size_t i = 0; i < audio.size(); ++i) {
    sum += static_cast<double>(data[i]);
  }
  return static_cast<float>(sum / static_cast<double>(audio.size()));
}

}  // namespace sonare::metering
