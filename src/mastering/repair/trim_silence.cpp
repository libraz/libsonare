#include "mastering/repair/trim_silence.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/stereo_pair.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/dsp_primitives.h"
#include "util/exception.h"
#include "util/validated.h"

namespace sonare::mastering::repair {
namespace {

bool is_active_sample(const float* samples, size_t index, const TrimSilenceConfig& config,
                      CenteredWindowEnergy& energy) {
  if (config.mode == TrimSilenceMode::Peak) {
    return std::abs(samples[index]) > config.threshold;
  }
  const float window_rms = static_cast<float>(std::sqrt(energy.mean_square_at(index)));
  const float loudness_db =
      window_rms <= 1.0e-12f ? sonare::constants::kFloorDb : linear_to_db(window_rms);
  return loudness_db > config.gate_lufs;
}

bool is_empty_range(const TrimRange& range) { return range.first >= range.last_exclusive; }

TrimReport to_report(const TrimRange& range, size_t size) {
  TrimReport report;
  report.range = range;
  report.removed_head_samples = std::min(size, range.first);
  report.removed_tail_samples = size - std::min(size, range.last_exclusive);
  return report;
}

TrimRange scan(const float* samples, size_t size, int sample_rate,
               const TrimSilenceConfig& config) {
  TrimRange range;
  if (samples == nullptr || size == 0) return range;

  const size_t loudness_radius =
      std::max<size_t>(1, static_cast<size_t>(sample_rate * config.window_ms * 0.0005f));
  CenteredWindowEnergy energy(samples, size, loudness_radius);
  size_t first = 0;
  while (first < size && !is_active_sample(samples, first, config, energy)) ++first;
  if (first == size) {
    range.first = size;
    range.last_exclusive = size;
    return range;
  }

  size_t last = size - 1;
  while (last > first && !is_active_sample(samples, last, config, energy)) --last;
  first = first > config.padding_samples ? first - config.padding_samples : 0;
  last = std::min(size - 1, last + config.padding_samples);
  range.first = first;
  range.last_exclusive = last + 1;
  return range;
}

TrimRange union_range(const TrimRange& left, const TrimRange& right) {
  if (is_empty_range(left)) return right;
  if (is_empty_range(right)) return left;
  return {std::min(left.first, right.first), std::max(left.last_exclusive, right.last_exclusive)};
}

}  // namespace

void validate_config(const TrimSilenceConfig& config) {
  if (!std::isfinite(config.threshold) || config.threshold < 0.0f || config.threshold > 1.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "threshold must be in [0, 1]");
  }
  if (config.padding_samples > kMaxTrimPaddingSamples) {
    throw SonareException(ErrorCode::InvalidParameter, "padding_samples is out of range");
  }
  if (config.mode != TrimSilenceMode::Peak && config.mode != TrimSilenceMode::LufsGated) {
    throw SonareException(ErrorCode::InvalidParameter, "invalid trim silence mode");
  }
  if (!std::isfinite(config.gate_lufs) || config.gate_lufs < kMinTrimGateLufs ||
      config.gate_lufs > kMaxTrimGateLufs) {
    throw SonareException(ErrorCode::InvalidParameter, "gate_lufs must be in [-144, 0]");
  }
  if (!std::isfinite(config.window_ms) || config.window_ms <= 0.0f ||
      config.window_ms > kMaxTrimWindowMs) {
    throw SonareException(ErrorCode::InvalidParameter, "window_ms must be in (0, 10000]");
  }
}

TrimRange detect_trim_range(const float* samples, size_t size, int sample_rate,
                            const TrimSilenceConfig& config) {
  const auto validated = Validated<TrimSilenceConfig>::make(config);
  if (sample_rate <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  return scan(samples, size, sample_rate, validated.get());
}

TrimRange detect_trim_range_stereo(const float* left, const float* right, size_t size,
                                   int sample_rate, const TrimSilenceConfig& config) {
  const auto validated = Validated<TrimSilenceConfig>::make(config);
  if (sample_rate <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "sample_rate must be positive");
  }
  if (left == nullptr || right == nullptr || size == 0) return {};
  return union_range(scan(left, size, sample_rate, validated.get()),
                     scan(right, size, sample_rate, validated.get()));
}

Audio trim_silence(const Audio& audio, const TrimSilenceConfig& config) {
  return trim_silence(audio, config, nullptr);
}

Audio trim_silence(const Audio& audio, const TrimSilenceConfig& config, TrimReport* report) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  const auto validated = Validated<TrimSilenceConfig>::make(config);

  const TrimRange range =
      detect_trim_range(audio.data(), audio.size(), audio.sample_rate(), validated.get());
  if (report != nullptr) *report = to_report(range, audio.size());
  if (range.first == audio.size()) return Audio::from_vector({}, audio.sample_rate());
  return audio.slice_samples(range.first, range.last_exclusive);
}

TrimSilenceStereoResult trim_silence_stereo(const Audio& left, const Audio& right,
                                            const TrimSilenceConfig& config) {
  require_stereo_pair(left, right);
  const auto validated = Validated<TrimSilenceConfig>::make(config);
  const int sample_rate = left.sample_rate();
  const size_t size = left.size();

  TrimSilenceStereoResult result;
  result.left_range = detect_trim_range(left.data(), size, sample_rate, validated.get());
  result.right_range = detect_trim_range(right.data(), size, sample_rate, validated.get());
  const TrimRange shared = union_range(result.left_range, result.right_range);
  result.report = to_report(shared, size);
  if (is_empty_range(shared)) {
    result.left = Audio::from_vector({}, sample_rate);
    result.right = Audio::from_vector({}, sample_rate);
    return result;
  }
  result.left = left.slice_samples(shared.first, shared.last_exclusive);
  result.right = right.slice_samples(shared.first, shared.last_exclusive);
  return result;
}

}  // namespace sonare::mastering::repair
