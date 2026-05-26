#include "metering/clipping.h"

#include <algorithm>
#include <cmath>

#include "util/exception.h"

namespace sonare::metering {

ClippingResult detect_clipping(const Audio& audio, float threshold, size_t min_region_samples) {
  SONARE_CHECK(threshold >= 0.0f && threshold <= 1.0f, ErrorCode::InvalidParameter);

  ClippingResult result;
  if (audio.empty()) return result;

  const float* data = audio.data();
  size_t i = 0;
  while (i < audio.size()) {
    if (std::abs(data[i]) < threshold) {
      ++i;
      continue;
    }

    const size_t start = i;
    float region_peak = 0.0f;
    while (i < audio.size() && std::abs(data[i]) >= threshold) {
      region_peak = std::max(region_peak, std::abs(data[i]));
      ++i;
    }

    const size_t length = i - start;
    result.clipped_samples += length;
    result.max_clipped_peak = std::max(result.max_clipped_peak, region_peak);
    if (length >= min_region_samples) {
      result.regions.push_back({start, i, length, region_peak});
    }
  }

  result.clipping_ratio =
      static_cast<float>(result.clipped_samples) / static_cast<float>(audio.size());
  return result;
}

}  // namespace sonare::metering
