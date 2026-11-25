#pragma once

/// @file config_from_params.h
/// @brief Shared param-list parsers for the mixing-assistant config structs.
/// @details Maps flat string-keyed params (camelCase keys only; a snake_case
///          spelling is refused) into MixAssistantConfig / TrackProfileConfig.
///          Used by both the C-ABI helper layer and the WASM binding so the JS
///          and C/Python paths stay in lock-step.

#include <cmath>
#include <cstddef>
#include <string>

#include "core/spectrum.h"
#include "mastering/api/named_processor.h"
#include "mastering/assistant/param_spelling.h"
#include "mixing/assistant/suggester.h"
#include "mixing/assistant/track_profile.h"
#include "util/exception.h"
#include "util/parameter_checks.h"

namespace sonare::mixing::assistant {
namespace detail {

// Validate flat values before narrowing the STFT sizes to their config fields.
inline void read_profile_geometry(const mastering::api::Param* params, std::size_t count,
                                  int& n_fft, int& hop_length) {
  if (params == nullptr) {
    mastering::api::validate_params(params, count);
    return;
  }
  for (std::size_t index = 0; index < count; ++index) {
    const auto& param = params[index];
    const bool fft = param.key == "nFft";
    if (!fft && param.key != "hopLength") {
      mastering::api::validate_params(&param, 1);
      continue;
    }
    const auto& domain = fft ? kStftNFftDomain : kStftHopDomain;
    if (!util::check::in_domain(param.value, domain) ||
        (fft && static_cast<int>(param.value) % 2 != 0)) {
      const std::string predicate =
          fft ? util::refusal::even_integer_predicate(static_cast<long long>(*domain.lo),
                                                      static_cast<long long>(*domain.hi))
              : util::check::domain_predicate(domain);
      throw SonareParameterRefusal::predicate(
          ErrorCode::InvalidParameter, param.key, predicate,
          RefusalDomain{domain.lo, domain.hi, domain.lo_included, domain.hi_included})
          .as_caller_spelled();
    }
    (fft ? n_fft : hop_length) = static_cast<int>(param.value);
  }
}

}  // namespace detail

/// @brief Build a MixAssistantConfig from a flat param list.
/// @details An unknown key is refused, as the mastering assistant's is: a
///          misspelt key would otherwise leave its setting at the default.
inline MixAssistantConfig mix_assistant_config_from_params(const mastering::api::Param* params,
                                                           std::size_t count) {
  MixAssistantConfig config;
  detail::read_profile_geometry(params, count, config.n_fft, config.hop_length);
  for (std::size_t index = 0; index < count; ++index) {
    const std::string& key = params[index].key;
    const double value = params[index].value;
    if (key == "targetTrackLufs") {
      config.target_track_lufs = static_cast<float>(value);
    } else if (key == "suggestionStrength") {
      config.suggestion_strength = static_cast<float>(value);
    } else if (key == "eqMaxCutDb") {
      config.eq_max_cut_db = static_cast<float>(value);
    } else if (key == "mixBusHeadroomDbtp") {
      config.mix_bus_headroom_dbtp = static_cast<float>(value);
    } else if (key == "tempoBpm") {
      // Only exact zero is the transport fallback sentinel; test the source double before
      // narrowing so a small nonzero value cannot underflow into that sentinel.
      if (!(value == 0.0 || (std::isfinite(value) && value >= kMinAssistantTempoBpm &&
                             value <= kMaxAssistantTempoBpm))) {
        throw SonareParameterRefusal::predicate(
            ErrorCode::InvalidParameter, key,
            "must be 0 or a tempo between " +
                std::to_string(static_cast<int>(kMinAssistantTempoBpm)) + " and " +
                std::to_string(static_cast<int>(kMaxAssistantTempoBpm)) + " BPM")
            .as_caller_spelled();
      }
      config.tempo_bpm = static_cast<float>(value);
    } else if (key == "enableStructure") {
      config.enable_structure = value != 0.0;
    } else if (key == "enableGain") {
      config.enable_gain = value != 0.0;
    } else if (key == "enableBalance") {
      config.enable_balance = value != 0.0;
    } else if (key == "enableEq") {
      config.enable_eq = value != 0.0;
    } else if (key == "enableDynamics") {
      config.enable_dynamics = value != 0.0;
    } else if (key == "enableImage") {
      config.enable_image = value != 0.0;
    } else if (key == "enableHighPass") {
      config.enable_high_pass = value != 0.0;
    } else if (key != "nFft" && key != "hopLength") {
      mastering::assistant::refuse_unknown_param(
          key, {"targetTrackLufs", "suggestionStrength", "eqMaxCutDb", "mixBusHeadroomDbtp",
                "tempoBpm", "enableStructure", "enableGain", "enableBalance", "enableEq",
                "enableDynamics", "enableImage", "enableHighPass", "nFft", "hopLength"});
    }
  }
  return config;
}

/// @brief Build a TrackProfileConfig from a flat param list.
inline TrackProfileConfig track_profile_config_from_params(const mastering::api::Param* params,
                                                           std::size_t count) {
  TrackProfileConfig config;
  detail::read_profile_geometry(params, count, config.n_fft, config.hop_length);
  for (std::size_t index = 0; index < count; ++index) {
    const std::string& key = params[index].key;
    const double value = params[index].value;
    if (key == "minDurationSec") {
      SONARE_CHECK_MSG(value >= 0.0, ErrorCode::InvalidParameter, key + " must be non-negative");
      config.min_duration_sec = static_cast<float>(value);
    } else if (key != "nFft" && key != "hopLength") {
      mastering::assistant::refuse_unknown_param(key, {"nFft", "hopLength", "minDurationSec"});
    }
  }
  return config;
}

}  // namespace sonare::mixing::assistant
