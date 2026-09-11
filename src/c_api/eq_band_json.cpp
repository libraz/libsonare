#include "c_api/eq_band_json.h"

#include <cmath>
#include <string>

#include "mastering/eq/band_strings.h"
#include "sonare_c_internal.h"
#include "util/json.h"
#include "util/numeric_validation.h"

namespace {

[[noreturn]] void invalid_eq_json(const std::string& message) {
  throw sonare_c_detail::SonareException(sonare::ErrorCode::InvalidParameter,
                                         "sonare_eq_set_band: " + message);
}

// A syntactically malformed document exits as InvalidFormat across the whole C
// ABI (midi_fx_json.h, parse_scene_json); a well-formed document carrying a bad
// field stays InvalidParameter.
[[noreturn]] void malformed_eq_json(const std::string& message) {
  throw sonare_c_detail::SonareException(sonare::ErrorCode::InvalidFormat,
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

// Every JSON double reaches a float field through here: as_number() yields a
// full double, so a literal like 1e39 is out of float range and its raw
// narrowing would be undefined behaviour. Mirrors project_serializer_decode's
// float_or.
float json_float(const JsonValue& object, const char* key, float fallback) {
  float converted = 0.0f;
  if (!sonare::numeric::checked_float_cast(json_number(object, key, fallback), &converted)) {
    invalid_eq_json(std::string("numeric JSON field is non-finite or out of float range: ") + key);
  }
  return converted;
}

float json_float_any(const JsonValue& object, const char* first_key, const char* second_key,
                     float fallback) {
  float converted = 0.0f;
  if (!sonare::numeric::checked_float_cast(json_number_any(object, first_key, second_key, fallback),
                                           &converted)) {
    invalid_eq_json(std::string("numeric JSON field is non-finite or out of float range: ") +
                    first_key);
  }
  return converted;
}

// A dB field that ends up as a filter gain: rbj_peak raises 10^(dB/40), which
// overflows to +inf past roughly 12330 dB and installs infinite numerator taps
// that normalize() lets through because a0 stays finite.
float json_gain_db_any(const JsonValue& object, const char* first_key, const char* second_key,
                       float fallback) {
  const float value = json_float_any(object, first_key, second_key, fallback);
  if (!std::isfinite(std::pow(10.0, static_cast<double>(value) / 40.0))) {
    invalid_eq_json(std::string("dB JSON field is too large to realize as a filter: ") + first_key);
  }
  return value;
}

int json_int_any(const JsonValue& object, const char* first_key, const char* second_key,
                 int fallback) {
  int converted = 0;
  if (!sonare::numeric::checked_integral_cast(
          std::round(json_number_any(object, first_key, second_key, fallback)), &converted)) {
    invalid_eq_json(std::string("integer JSON field is non-finite or out of range: ") + first_key);
  }
  return converted;
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
  const auto parsed = sonare::mastering::eq::band_type_from_string(value);
  if (!parsed) invalid_eq_json("unknown EQ band type: " + value);
  return *parsed;
}

sonare::mastering::eq::BiquadCoeffMode parse_coeff_mode(const std::string& value) {
  const auto parsed = sonare::mastering::eq::coeff_mode_from_string(value);
  if (!parsed) invalid_eq_json("unknown EQ coefficient mode: " + value);
  return *parsed;
}

sonare::mastering::eq::StereoPlacement parse_placement(const std::string& value) {
  const auto parsed = sonare::mastering::eq::placement_from_string(value);
  if (!parsed) invalid_eq_json("unknown EQ placement: " + value);
  return *parsed;
}

sonare::mastering::eq::PhaseMode parse_band_phase(const std::string& value) {
  const auto parsed = sonare::mastering::eq::phase_mode_from_string(value);
  if (!parsed) invalid_eq_json("unknown EQ band phase mode: " + value);
  return *parsed;
}

}  // namespace

namespace sonare::c_api {

sonare::mastering::eq::EqBand parse_eq_band_json(const char* band_json) {
  if (!band_json) invalid_eq_json("band_json must not be null");
  JsonValue json;
  try {
    json = sonare::util::json::parse_strict(std::string(band_json));
  } catch (const sonare::util::json::JsonError& ex) {
    malformed_eq_json(std::string("invalid JSON: ") + ex.what());
  }
  if (!json.is_object()) invalid_eq_json("band_json must be a JSON object");
  sonare::mastering::eq::EqBand band;
  band.type = parse_band_type(json_string(json, "type", "Peak"));
  band.coeff_mode = parse_coeff_mode(json_string_any(json, "coeffMode", "coeff_mode", "Rbj"));
  band.frequency_hz = json_float_any(json, "frequencyHz", "frequency_hz", band.frequency_hz);
  band.gain_db = json_gain_db_any(json, "gainDb", "gain_db", band.gain_db);
  band.q = json_float(json, "q", band.q);
  band.enabled = json_bool(json, "enabled", band.enabled);
  band.slope_db_oct = json_int_any(json, "slopeDbOct", "slope_db_oct", band.slope_db_oct);
  band.placement = parse_placement(json_string(json, "placement", "Stereo"));
  band.phase = parse_band_phase(json_string(json, "phase", "Inherit"));
  band.soloed = json_bool(json, "soloed", false);
  band.bypassed = json_bool(json, "bypassed", false);
  band.proportional_q = json_bool_any(json, "proportionalQ", "proportional_q", false);
  band.proportional_q_strength = json_float_any(
      json, "proportionalQStrength", "proportional_q_strength", band.proportional_q_strength);

  band.dyn.enabled = json_bool_any(json, "dynamic", "dynEnabled", false);
  band.dyn.enabled = json_bool(json, "dyn_enabled", band.dyn.enabled);
  // threshold_db and range_db are the two dynamic terms that reach the biquad
  // as gain (detector_db - threshold_db, scaled by ratio, clamped to range_db),
  // so they carry the same realizability bound as the static gain.
  band.dyn.threshold_db =
      json_gain_db_any(json, "thresholdDb", "threshold_db", band.dyn.threshold_db);
  band.dyn.auto_threshold =
      json_bool_any(json, "autoThreshold", "auto_threshold", band.dyn.auto_threshold);
  band.dyn.ratio = json_float(json, "ratio", band.dyn.ratio);
  band.dyn.range_db = json_gain_db_any(json, "rangeDb", "range_db", band.dyn.range_db);
  band.dyn.attack_ms = json_float_any(json, "attackMs", "attack_ms", band.dyn.attack_ms);
  band.dyn.release_ms = json_float_any(json, "releaseMs", "release_ms", band.dyn.release_ms);
  // "lookaheadMs"/"lookahead_ms" are the field's former (misleading) spelling;
  // still accepted so a stored config keeps working, but the canonical
  // "detectorDelayMs"/"detector_delay_ms" wins if both are present.
  band.dyn.detector_delay_ms =
      json_float_any(json, "lookaheadMs", "lookahead_ms", band.dyn.detector_delay_ms);
  band.dyn.detector_delay_ms =
      json_float_any(json, "detectorDelayMs", "detector_delay_ms", band.dyn.detector_delay_ms);
  band.dyn.sidechain_freq_hz =
      json_float_any(json, "sidechainFreqHz", "sidechain_freq_hz", band.dyn.sidechain_freq_hz);
  band.dyn.sidechain_q = json_float_any(json, "sidechainQ", "sidechain_q", band.dyn.sidechain_q);
  band.dyn.external_sidechain =
      json_bool_any(json, "externalSidechain", "external_sidechain", band.dyn.external_sidechain);
  return band;
}

}  // namespace sonare::c_api
