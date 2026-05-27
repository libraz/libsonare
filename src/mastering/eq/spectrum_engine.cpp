#include "mastering/eq/spectrum_engine.h"

#include <cmath>
#include <limits>

namespace sonare::mastering::eq {

SpectrumGrabResult spectrum_grab_band(float frequency_hz, const EqBand* bands, size_t num_bands,
                                      size_t max_bands) noexcept {
  SpectrumGrabResult result{};
  if (bands == nullptr || num_bands == 0) {
    return result;
  }

  float best_octaves = std::numeric_limits<float>::infinity();
  size_t first_disabled = max_bands;
  const size_t count = num_bands < max_bands ? num_bands : max_bands;
  for (size_t i = 0; i < count; ++i) {
    if (!bands[i].enabled && first_disabled == max_bands) {
      first_disabled = i;
      continue;
    }
    if (!bands[i].enabled || !(bands[i].frequency_hz > 0.0f) || !(frequency_hz > 0.0f)) {
      continue;
    }
    const float distance = std::abs(std::log2(frequency_hz / bands[i].frequency_hz));
    if (distance < best_octaves) {
      best_octaves = distance;
      result.index = i;
      result.use_existing = true;
    }
  }

  if (!result.use_existing && first_disabled < max_bands) {
    result.index = first_disabled;
  }
  return result;
}

}  // namespace sonare::mastering::eq
