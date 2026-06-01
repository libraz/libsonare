#include "mastering/final/bit_depth.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "util/exception.h"

namespace sonare::mastering::final {

Audio bit_depth(const Audio& audio, const BitDepthConfig& config) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  if (config.target_bits < 2 || config.target_bits > 32) {
    throw SonareException(ErrorCode::InvalidParameter, "target_bits must be in [2, 32]");
  }
  const float scale = static_cast<float>(int64_t{1} << (config.target_bits - 1));
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  for (auto& sample : samples) {
    if (config.clamp) sample = std::clamp(sample, -1.0f, 1.0f);
    sample = std::round(sample * scale) / scale;
    if (config.clamp) sample = std::clamp(sample, -1.0f, 1.0f);
  }
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace sonare::mastering::final
