#pragma once

/// @file legacy_insert_params.h
/// @brief Folds a legacy amp-preset spelling inside an insert's params JSON onto
///        its current one. Kept apart from legacy_ids.h because it catches, so only
///        units compiled with exception support include it.

#include <string>

#include "mastering/api/legacy_ids.h"
#include "util/json.h"

namespace sonare::mastering::api {

/// @brief True when @p text contains a legacy amp-preset spelling, so a caller
///        can skip re-parsing a params document that has none.
inline bool mentions_legacy_amp_preset(const std::string& text) {
  for (const legacy_detail::Rename& entry : legacy_detail::kAmpPresets) {
    if (text.find(entry.legacy) != std::string::npos) return true;
  }
  return false;
}

/// @brief @p params_json with a legacy amp-preset spelling in its `preset` value
///        replaced by the current one; any other document is returned unchanged.
inline std::string canonical_insert_params_text(const std::string& processor,
                                                const std::string& params_json) {
  if (processor != "saturation.ampSim" || !mentions_legacy_amp_preset(params_json)) {
    return params_json;
  }
  try {
    util::json::Value document = util::json::parse(params_json);
    if (!document.is_object()) return params_json;
    auto& members = document.as_object();
    const auto preset = members.find("preset");
    if (preset == members.end() || !preset->second.is_string()) return params_json;
    const std::string current = canonical_amp_preset_name(preset->second.as_string());
    if (current == preset->second.as_string()) return params_json;
    preset->second = util::json::Value(current);
    return util::json::dump(document);
  } catch (const util::json::JsonError&) {
    return params_json;
  }
}

}  // namespace sonare::mastering::api
