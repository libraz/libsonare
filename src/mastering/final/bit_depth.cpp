#include "mastering/final/bit_depth.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "util/exception.h"
#include "util/non_finite_sample.h"

namespace sonare::mastering::final {

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
    // Last stage before the samples leave for a file or a device, so a
    // non-finite one is replaced here rather than propagated.
    if (resolve_non_finite(SampleDestination::kIrreversibleOutput, sample)) ++non_finite;
    if (config.clamp) sample = std::clamp(sample, -1.0f, 1.0f);
    sample = std::clamp(std::round(sample * scale), min_code, max_code) / scale;
    if (config.clamp) sample = std::clamp(sample, -1.0f, 1.0f);
  }
  if (non_finite_samples != nullptr) *non_finite_samples = non_finite;
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace sonare::mastering::final
