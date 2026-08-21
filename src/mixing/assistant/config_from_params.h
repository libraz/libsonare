#pragma once

/// @file config_from_params.h
/// @brief Shared param-list parsers for the mixing-assistant config structs.
/// @details Maps flat string-keyed params (accepting both camelCase and
///          snake_case aliases) into MixAssistantConfig / TrackProfileConfig.
///          Used by both the C-ABI helper layer and the WASM binding so the JS
///          and C/Python paths stay in lock-step.

#include <cstddef>
#include <string>

#include "mastering/api/named_processor.h"
#include "mixing/assistant/suggester.h"
#include "mixing/assistant/track_profile.h"

namespace sonare::mixing::assistant {

/// @brief Build a MixAssistantConfig from a flat param list.
/// @details Unknown keys are ignored, matching the mastering assistant.
inline MixAssistantConfig mix_assistant_config_from_params(const mastering::api::Param* params,
                                                           std::size_t count) {
  mastering::api::validate_params(params, count);
  MixAssistantConfig config;
  for (std::size_t index = 0; index < count; ++index) {
    const std::string& key = params[index].key;
    const double value = params[index].value;
    if (key == "targetTrackLufs" || key == "target_track_lufs") {
      config.target_track_lufs = static_cast<float>(value);
    } else if (key == "suggestionStrength" || key == "suggestion_strength") {
      config.suggestion_strength = static_cast<float>(value);
    } else if (key == "eqMaxCutDb" || key == "eq_max_cut_db") {
      config.eq_max_cut_db = static_cast<float>(value);
    } else if (key == "mixBusHeadroomDbtp" || key == "mix_bus_headroom_dbtp") {
      config.mix_bus_headroom_dbtp = static_cast<float>(value);
    } else if (key == "enableStructure" || key == "enable_structure") {
      config.enable_structure = value != 0.0;
    } else if (key == "enableGain" || key == "enable_gain") {
      config.enable_gain = value != 0.0;
    } else if (key == "enableBalance" || key == "enable_balance") {
      config.enable_balance = value != 0.0;
    } else if (key == "enableEq" || key == "enable_eq") {
      config.enable_eq = value != 0.0;
    } else if (key == "enableDynamics" || key == "enable_dynamics") {
      config.enable_dynamics = value != 0.0;
    } else if (key == "enableImage" || key == "enable_image") {
      config.enable_image = value != 0.0;
    } else if (key == "enableHighPass" || key == "enable_high_pass") {
      config.enable_high_pass = value != 0.0;
    } else if (key == "nFft" || key == "n_fft") {
      config.n_fft = static_cast<int>(value);
    } else if (key == "hopLength" || key == "hop_length") {
      config.hop_length = static_cast<int>(value);
    }
  }
  return config;
}

/// @brief Build a TrackProfileConfig from a flat param list.
inline TrackProfileConfig track_profile_config_from_params(const mastering::api::Param* params,
                                                           std::size_t count) {
  mastering::api::validate_params(params, count);
  TrackProfileConfig config;
  for (std::size_t index = 0; index < count; ++index) {
    const std::string& key = params[index].key;
    const double value = params[index].value;
    if (key == "nFft" || key == "n_fft") {
      config.n_fft = static_cast<int>(value);
    } else if (key == "hopLength" || key == "hop_length") {
      config.hop_length = static_cast<int>(value);
    } else if (key == "minDurationSec" || key == "min_duration_sec") {
      config.min_duration_sec = static_cast<float>(value);
    }
  }
  return config;
}

}  // namespace sonare::mixing::assistant
