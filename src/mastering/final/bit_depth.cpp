#include "mastering/final/bit_depth.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "util/exception.h"

namespace sonare::mastering::final {
namespace {

/// Replaces a non-finite sample with a finite in-domain one and counts it in
/// @p non_finite, which is the only thing that separates the replacement from a
/// sample the caller delivered.
float sanitize_sample(float sample, size_t& non_finite) noexcept {
  if (std::isnan(sample)) {
    ++non_finite;
    return 0.0f;
  }
  if (sample == std::numeric_limits<float>::infinity()) {
    ++non_finite;
    return 1.0f;
  }
  if (sample == -std::numeric_limits<float>::infinity()) {
    ++non_finite;
    return -1.0f;
  }
  return sample;
}

}  // namespace

Audio bit_depth(const Audio& audio, const BitDepthConfig& config, size_t* non_finite_samples) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  if (config.target_bits < 2 || config.target_bits > 32) {
    throw SonareException(ErrorCode::InvalidParameter, "target_bits must be in [2, 32]");
  }
  const float scale = static_cast<float>(int64_t{1} << (config.target_bits - 1));
  const float min_code = -scale;
  const float max_code = scale - 1.0f;
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  size_t non_finite = 0;
  for (auto& sample : samples) {
    sample = sanitize_sample(sample, non_finite);
    if (config.clamp) sample = std::clamp(sample, -1.0f, 1.0f);
    sample = std::clamp(std::round(sample * scale), min_code, max_code) / scale;
    if (config.clamp) sample = std::clamp(sample, -1.0f, 1.0f);
  }
  if (non_finite_samples != nullptr) *non_finite_samples = non_finite;
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace sonare::mastering::final
