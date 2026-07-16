#include "mastering/repair/trim_silence.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"

namespace sonare::mastering::repair {
namespace {

float sample_loudness_db(const float* samples, size_t size, size_t center, size_t radius) {
  const size_t begin = center > radius ? center - radius : 0;
  const size_t end = std::min(size, center + radius + 1);
  const float window_rms = rms(samples + begin, std::max<size_t>(1, end - begin));
  return window_rms <= 1.0e-12f ? sonare::constants::kFloorDb : linear_to_db(window_rms);
}

bool is_active_sample(const float* samples, size_t size, size_t index,
                      const TrimSilenceConfig& config, size_t loudness_radius) {
  if (config.mode == TrimSilenceMode::Peak) {
    return std::abs(samples[index]) > config.threshold;
  }
  return sample_loudness_db(samples, size, index, loudness_radius) > config.gate_lufs;
}

}  // namespace

void validate_config(const TrimSilenceConfig& config) {
  if (!std::isfinite(config.threshold) || config.threshold < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "threshold must be finite and non-negative");
  }
  if (config.mode != TrimSilenceMode::Peak && config.mode != TrimSilenceMode::LufsGated) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid trim silence mode");
  }
  if (!std::isfinite(config.gate_lufs)) {
    throw SonareException(ErrorCode::InvalidParameter, "gate_lufs must be finite");
  }
  if (!std::isfinite(config.window_ms) || config.window_ms <= 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "window_ms must be finite and positive");
  }
}

TrimRange detect_trim_range(const float* samples, size_t size, int sample_rate,
                            const TrimSilenceConfig& config) {
  validate_config(config);
  if (sample_rate <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  TrimRange range;
  if (samples == nullptr || size == 0) return range;

  const size_t loudness_radius =
      std::max<size_t>(1, static_cast<size_t>(sample_rate * config.window_ms * 0.0005f));
  size_t first = 0;
  while (first < size && !is_active_sample(samples, size, first, config, loudness_radius)) ++first;
  if (first == size) {
    range.first = size;
    range.last_exclusive = size;
    return range;
  }

  size_t last = size - 1;
  while (last > first && !is_active_sample(samples, size, last, config, loudness_radius)) --last;
  first = first > config.padding_samples ? first - config.padding_samples : 0;
  last = std::min(size - 1, last + config.padding_samples);
  range.first = first;
  range.last_exclusive = last + 1;
  return range;
}

Audio trim_silence(const Audio& audio, const TrimSilenceConfig& config) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  validate_config(config);

  const TrimRange range =
      detect_trim_range(audio.data(), audio.size(), audio.sample_rate(), config);
  if (range.first == audio.size()) return Audio::from_vector({}, audio.sample_rate());
  return audio.slice_samples(range.first, range.last_exclusive);
}

}  // namespace sonare::mastering::repair
