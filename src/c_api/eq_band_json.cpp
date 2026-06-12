#include "c_api/eq_band_json.h"

#include <cmath>
#include <string>

#include "sonare_c_internal.h"
#include "util/json.h"

namespace {

[[noreturn]] void invalid_eq_json(const std::string& message) {
  throw sonare_c_detail::SonareException(sonare::ErrorCode::InvalidParameter,
                                         "sonare_eq_set_band: " + message);
}

// EQ band JSON parsing delegates to the shared util::json parser so the C API
// and realtime engine bindings accept the same strict JSON grammar.
using JsonValue = sonare::util::json::Value;

const JsonValue* find_json_value(const JsonValue& object, const char* key) {
  return object.is_object() ? object.find(key) : nullptr;
}

double json_number(const JsonValue& object, const char* key, double fallback) {
  const JsonValue* value = find_json_value(object, key);
  if (!value) return fallback;
  if (!value->is_number()) {
    invalid_eq_json(std::string("expected numeric JSON field: ") + key);
  }
  return value->as_number();
}

double json_number_any(const JsonValue& object, const char* first_key, const char* second_key,
                       double fallback) {
  const JsonValue* value = find_json_value(object, first_key);
  if (value && !value->is_number()) {
    invalid_eq_json(std::string("expected numeric JSON field: ") + first_key);
  }
  if (value) return value->as_number();
  return json_number(object, second_key, fallback);
}

bool json_bool(const JsonValue& object, const char* key, bool fallback) {
  const JsonValue* value = find_json_value(object, key);
  if (!value) return fallback;
  if (value->is_bool()) return value->as_bool();
  if (value->is_number()) return value->as_number() != 0.0;
  invalid_eq_json(std::string("expected boolean JSON field: ") + key);
}

bool json_bool_any(const JsonValue& object, const char* first_key, const char* second_key,
                   bool fallback) {
  const JsonValue* value = find_json_value(object, first_key);
  if (value) return json_bool(object, first_key, fallback);
  return json_bool(object, second_key, fallback);
}

std::string json_string(const JsonValue& object, const char* key, const std::string& fallback) {
  const JsonValue* value = find_json_value(object, key);
  if (!value) return fallback;
  if (!value->is_string()) {
    invalid_eq_json(std::string("expected string JSON field: ") + key);
  }
  return value->as_string();
}

std::string json_string_any(const JsonValue& object, const char* first_key, const char* second_key,
                            const std::string& fallback) {
  const JsonValue* value = find_json_value(object, first_key);
  if (value) return json_string(object, first_key, fallback);
  return json_string(object, second_key, fallback);
}

sonare::mastering::eq::EqBandType parse_band_type(const std::string& value) {
  using sonare::mastering::eq::EqBandType;
  if (value == "Peak" || value == "peak" || value == "Bell" || value == "bell") {
    return EqBandType::Peak;
  }
  if (value == "LowShelf" || value == "lowShelf") return EqBandType::LowShelf;
  if (value == "HighShelf" || value == "highShelf") return EqBandType::HighShelf;
  if (value == "LowPass" || value == "lowPass" || value == "HighCut" || value == "highCut") {
    return EqBandType::LowPass;
  }
  if (value == "HighPass" || value == "highPass" || value == "LowCut" || value == "lowCut") {
    return EqBandType::HighPass;
  }
  if (value == "BandPass" || value == "bandPass") return EqBandType::BandPass;
  if (value == "Notch" || value == "notch") return EqBandType::Notch;
  if (value == "TiltShelf" || value == "tiltShelf") return EqBandType::TiltShelf;
  if (value == "FlatTilt" || value == "flatTilt") return EqBandType::FlatTilt;
  invalid_eq_json("unknown EQ band type: " + value);
}

sonare::mastering::eq::BiquadCoeffMode parse_coeff_mode(const std::string& value) {
  using sonare::mastering::eq::BiquadCoeffMode;
  if (value == "Rbj" || value == "RBJ" || value == "rbj") return BiquadCoeffMode::Rbj;
  if (value == "Vicanek" || value == "vicanek") return BiquadCoeffMode::Vicanek;
  invalid_eq_json("unknown EQ coefficient mode: " + value);
}

sonare::mastering::eq::StereoPlacement parse_placement(const std::string& value) {
  using sonare::mastering::eq::StereoPlacement;
  if (value == "Stereo" || value == "stereo") return StereoPlacement::Stereo;
  if (value == "Left" || value == "left") return StereoPlacement::Left;
  if (value == "Right" || value == "right") return StereoPlacement::Right;
  if (value == "Mid" || value == "mid") return StereoPlacement::Mid;
  if (value == "Side" || value == "side") return StereoPlacement::Side;
  invalid_eq_json("unknown EQ placement: " + value);
}

sonare::mastering::eq::PhaseMode parse_band_phase(const std::string& value) {
  using sonare::mastering::eq::PhaseMode;
  if (value == "Inherit" || value == "inherit") return PhaseMode::Inherit;
  if (value == "ZeroLatency" || value == "zeroLatency") return PhaseMode::ZeroLatency;
  if (value == "NaturalPhase" || value == "naturalPhase") return PhaseMode::NaturalPhase;
  if (value == "LinearPhase" || value == "linearPhase") return PhaseMode::LinearPhase;
  invalid_eq_json("unknown EQ band phase mode: " + value);
}

}  // namespace

namespace sonare::c_api {

sonare::mastering::eq::EqBand parse_eq_band_json(const char* band_json) {
  if (!band_json) invalid_eq_json("band_json must not be null");
  JsonValue json;
  try {
    json = sonare::util::json::parse_strict(std::string(band_json));
  } catch (const sonare::util::json::JsonError& ex) {
    invalid_eq_json(std::string("invalid JSON: ") + ex.what());
  }
  if (!json.is_object()) invalid_eq_json("band_json must be a JSON object");
  sonare::mastering::eq::EqBand band;
  band.type = parse_band_type(json_string(json, "type", "Peak"));
  band.coeff_mode = parse_coeff_mode(json_string_any(json, "coeffMode", "coeff_mode", "Rbj"));
  band.frequency_hz =
      static_cast<float>(json_number_any(json, "frequencyHz", "frequency_hz", band.frequency_hz));
  band.gain_db = static_cast<float>(json_number_any(json, "gainDb", "gain_db", band.gain_db));
  band.q = static_cast<float>(json_number(json, "q", band.q));
  band.enabled = json_bool(json, "enabled", band.enabled);
  band.slope_db_oct = static_cast<int>(
      std::round(json_number_any(json, "slopeDbOct", "slope_db_oct", band.slope_db_oct)));
  band.placement = parse_placement(json_string(json, "placement", "Stereo"));
  band.phase = parse_band_phase(json_string(json, "phase", "Inherit"));
  band.soloed = json_bool(json, "soloed", false);
  band.bypassed = json_bool(json, "bypassed", false);
  band.proportional_q = json_bool_any(json, "proportionalQ", "proportional_q", false);
  band.proportional_q_strength = static_cast<float>(json_number_any(
      json, "proportionalQStrength", "proportional_q_strength", band.proportional_q_strength));

  band.dyn.enabled = json_bool_any(json, "dynamic", "dynEnabled", false);
  band.dyn.enabled = json_bool(json, "dyn_enabled", band.dyn.enabled);
  band.dyn.threshold_db = static_cast<float>(
      json_number_any(json, "thresholdDb", "threshold_db", band.dyn.threshold_db));
  band.dyn.auto_threshold =
      json_bool_any(json, "autoThreshold", "auto_threshold", band.dyn.auto_threshold);
  band.dyn.ratio = static_cast<float>(json_number(json, "ratio", band.dyn.ratio));
  band.dyn.range_db =
      static_cast<float>(json_number_any(json, "rangeDb", "range_db", band.dyn.range_db));
  band.dyn.attack_ms =
      static_cast<float>(json_number_any(json, "attackMs", "attack_ms", band.dyn.attack_ms));
  band.dyn.release_ms =
      static_cast<float>(json_number_any(json, "releaseMs", "release_ms", band.dyn.release_ms));
  band.dyn.lookahead_ms = static_cast<float>(
      json_number_any(json, "lookaheadMs", "lookahead_ms", band.dyn.lookahead_ms));
  band.dyn.sidechain_freq_hz = static_cast<float>(
      json_number_any(json, "sidechainFreqHz", "sidechain_freq_hz", band.dyn.sidechain_freq_hz));
  band.dyn.sidechain_q =
      static_cast<float>(json_number_any(json, "sidechainQ", "sidechain_q", band.dyn.sidechain_q));
  band.dyn.external_sidechain =
      json_bool_any(json, "externalSidechain", "external_sidechain", band.dyn.external_sidechain);
  return band;
}

}  // namespace sonare::c_api
