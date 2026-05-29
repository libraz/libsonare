#include "mastering/maximizer/streaming_preview.h"

#include <cmath>
#include <stdexcept>
#include <utility>

// TODO(layer-violation): CLAUDE.md restricts `mastering/` (non-assistant) to
// `core/ + util/ + rt/`. `streaming_preview` is a loudness-target previewer
// whose core job is to report integrated LUFS and true-peak per streaming
// platform; the includes below cannot be removed without redesigning the API.
// Suggested follow-ups:
//   1. Move this helper under `mastering/assistant/` (it is functionally an
//      analysis/recommendation tool, not a DSP processor).
//   2. Or change the signature to accept pre-measured loudness + true-peak as
//      inputs and push the measurement to the C API / WASM bridge layer.
#include "metering/lufs.h"
#include "metering/true_peak.h"
#include "util/json.h"

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

std::string streaming_preview_to_json(const std::vector<StreamingPreviewResult>& results) {
  namespace json = sonare::util::json;
  json::Array platforms;
  platforms.reserve(results.size());
  for (const auto& result : results) {
    json::Object entry;
    entry["name"] = result.name;
    entry["integratedLufs"] = static_cast<double>(result.integrated_lufs);
    entry["truePeakDb"] = static_cast<double>(result.true_peak_db);
    entry["normalizationGainDb"] = static_cast<double>(result.normalization_gain_db);
    entry["ceilingRisk"] = result.ceiling_risk;
    platforms.push_back(json::Value(std::move(entry)));
  }
  json::Object root;
  root["platforms"] = json::Value(std::move(platforms));
  return json::dump(json::Value(std::move(root)));
}

}  // namespace sonare::mastering::maximizer
