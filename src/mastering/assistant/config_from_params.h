#pragma once

/// @file config_from_params.h
/// @brief Shared param-list parsers for the mastering-assistant config structs.
/// @details Maps flat string-keyed mastering params (accepting both camelCase
///          and snake_case aliases) into AssistantConfig / AudioProfileConfig.
///          Used by both the C-ABI helper layer and the WASM binding so the JS
///          and C/Python paths stay in lock-step.

#include <cstddef>
#include <string>

#include "mastering/api/named_processor.h"
#include "mastering/assistant/audio_profile.h"
#include "mastering/assistant/suggester.h"

namespace sonare::mastering::assistant {

/// @brief Build an AssistantConfig from a flat param list.
inline AssistantConfig assistant_config_from_params(const api::Param* params, std::size_t count) {
  AssistantConfig config;
  for (std::size_t index = 0; index < count; ++index) {
    const std::string& key = params[index].key;
    const double value = params[index].value;
    if (key == "targetLufs" || key == "target_lufs") {
      config.target_lufs = static_cast<float>(value);
    } else if (key == "ceilingDb" || key == "ceiling_db") {
      config.ceiling_db = static_cast<float>(value);
    } else if (key == "enableRepair" || key == "enable_repair") {
      config.enable_repair = value != 0.0;
    } else if (key == "preferStreamingSafe" || key == "prefer_streaming_safe") {
      config.prefer_streaming_safe = value != 0.0;
    } else if (key == "speechMonoAmount" || key == "speech_mono_amount") {
      config.speech_mono_amount = static_cast<float>(value);
    }
  }
  return config;
}

/// @brief Build an AudioProfileConfig from a flat param list.
inline AudioProfileConfig audio_profile_config_from_params(const api::Param* params,
                                                           std::size_t count) {
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
