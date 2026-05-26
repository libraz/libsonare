#include "mastering/maximizer/streaming_preview.h"

#include <cmath>
#include <stdexcept>

#include "metering/lufs.h"
#include "metering/true_peak.h"

namespace sonare::mastering::maximizer {

std::vector<StreamingPreviewResult> streaming_preview(
    const Audio& audio, const std::vector<StreamingPlatform>& platforms) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (platforms.empty()) throw std::invalid_argument("platform list must not be empty");

  const float integrated = metering::lufs(audio).integrated_lufs;
  const float true_peak = metering::true_peak_db(audio, 4);
  std::vector<StreamingPreviewResult> results;
  results.reserve(platforms.size());
  for (const auto& platform : platforms) {
    if (platform.name.empty()) throw std::invalid_argument("platform name must not be empty");
    const float gain = std::isfinite(integrated) ? platform.target_lufs - integrated : 0.0f;
    results.push_back({platform.name, integrated, true_peak, gain,
                       std::isfinite(true_peak) && true_peak + gain > platform.ceiling_db});
  }
  return results;
}

}  // namespace sonare::mastering::maximizer
