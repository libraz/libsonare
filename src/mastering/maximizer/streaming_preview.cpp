#include "mastering/maximizer/streaming_preview.h"

#include <cmath>
#include <utility>

#include "mastering/common/loudness_measure.h"
#include "util/exception.h"
#include "util/json.h"

namespace sonare::mastering::maximizer {

std::vector<StreamingPreviewResult> streaming_preview(
    const Audio& audio, const std::vector<StreamingPlatform>& platforms) {
  if (audio.empty()) throw SonareException(ErrorCode::InvalidParameter, "audio must not be empty");
  if (platforms.empty())
    throw SonareException(ErrorCode::InvalidParameter, "platform list must not be empty");

  const auto measured = common::measure_lufs_and_true_peak(audio);
  const float integrated = measured.integrated_lufs;
  const float true_peak = measured.true_peak_dbtp;
  std::vector<StreamingPreviewResult> results;
  results.reserve(platforms.size());
  for (const auto& platform : platforms) {
    if (platform.name.empty())
      throw SonareException(ErrorCode::InvalidParameter, "platform name must not be empty");
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
