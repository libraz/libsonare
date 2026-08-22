#pragma once

/// @file config_from_params.h
/// @brief Shared param-list parsers for the mastering-assistant config structs.
/// @details Maps flat string-keyed mastering params (accepting both camelCase
///          and snake_case aliases) into AssistantConfig / AudioProfileConfig.
///          Used by both the C-ABI helper layer and the WASM binding so the JS
///          and C/Python paths stay in lock-step.

#include <cmath>
#include <cstddef>
#include <string>

#include "mastering/api/named_processor.h"
#include "mastering/assistant/audio_profile.h"
#include "mastering/assistant/platform_targets.h"
#include "mastering/assistant/suggester.h"
#include "util/exception.h"

namespace sonare::mastering::assistant {

/// @brief Build an AssistantConfig from a flat param list.
inline AssistantConfig assistant_config_from_params(const api::Param* params, std::size_t count) {
  api::validate_params(params, count);
  AssistantConfig config;
  for (std::size_t index = 0; index < count; ++index) {
    const std::string& key = params[index].key;
    const double value = params[index].value;
    if (key == "targetLufs" || key == "target_lufs") {
      config.target_lufs = static_cast<float>(value);
      // Record that the caller named it, so a delivery target cannot claim the
      // one slider position that happens to equal the default.
      config.target_lufs_explicit = true;
    } else if (key == "ceilingDb" || key == "ceiling_db") {
      config.ceiling_db = static_cast<float>(value);
      config.ceiling_db_explicit = true;
    } else if (key == "enableRepair" || key == "enable_repair") {
      config.enable_repair = value != 0.0;
    } else if (key == "preferStreamingSafe" || key == "prefer_streaming_safe") {
      config.prefer_streaming_safe = value != 0.0;
    } else if (key == "speechMonoAmount" || key == "speech_mono_amount") {
      config.speech_mono_amount = static_cast<float>(value);
    } else if (key == "targetPlatform" || key == "target_platform") {
      // A param value is a number, so the delivery target arrives as the index
      // platform_index_from_name() resolved. Anything that is not exactly one of
      // those indices is rejected rather than truncated toward a neighbouring
      // target: a caller who meant a name and sent a number is wrong in a way
      // that must be visible. The range is checked before the cast, because
      // narrowing an out-of-range double to int is undefined.
      double index = 0.0;
      const bool integral = std::modf(value, &index) == 0.0;
      const bool in_range = index >= 0.0 && index < static_cast<double>(kPlatformTargets.size());
      SONARE_CHECK_MSG(integral && in_range, ErrorCode::InvalidParameter,
                       "targetPlatform must be a delivery-target index; expected one of: " +
                           platform_names_joined());
      config.target_platform = platform_name_at(static_cast<int>(index));
    }
  }
  return config;
}

/// @brief Build an AudioProfileConfig from a flat param list.
inline AudioProfileConfig audio_profile_config_from_params(const api::Param* params,
                                                           std::size_t count) {
  api::validate_params(params, count);
  AudioProfileConfig config;
  for (std::size_t index = 0; index < count; ++index) {
    const std::string& key = params[index].key;
    const double value = params[index].value;
    if (key == "nFft" || key == "n_fft") {
      config.n_fft = static_cast<int>(value);
    } else if (key == "hopLength" || key == "hop_length") {
      config.hop_length = static_cast<int>(value);
    } else if (key == "truePeakOversample" || key == "true_peak_oversample") {
      config.true_peak_oversample = static_cast<int>(value);
    }
  }
  return config;
}

}  // namespace sonare::mastering::assistant
