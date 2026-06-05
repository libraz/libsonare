#include "mastering/final/bit_depth.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "util/exception.h"

namespace sonare::mastering::final {
namespace {

float sanitize_sample(float sample) noexcept {
  if (std::isnan(sample)) return 0.0f;
  if (sample == std::numeric_limits<float>::infinity()) return 1.0f;
  if (sample == -std::numeric_limits<float>::infinity()) return -1.0f;
  return sample;
}

}  // namespace

Audio bit_depth(const Audio& audio, const BitDepthConfig& config) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  if (config.target_bits < 2 || config.target_bits > 32) {
    throw SonareException(ErrorCode::InvalidParameter, "target_bits must be in [2, 32]");
  }
  const float scale = static_cast<float>(int64_t{1} << (config.target_bits - 1));
  const float min_code = -scale;
  const float max_code = scale - 1.0f;
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  for (auto& sample : samples) {
    sample = sanitize_sample(sample);
    if (config.clamp) sample = std::clamp(sample, -1.0f, 1.0f);
    sample = std::clamp(std::round(sample * scale), min_code, max_code) / scale;
    if (config.clamp) sample = std::clamp(sample, -1.0f, 1.0f);
  }
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace sonare::mastering::final
