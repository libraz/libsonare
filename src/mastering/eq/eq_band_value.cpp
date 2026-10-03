#include "mastering/eq/eq_band_value.h"

#include <cmath>
#include <string>
#include <utility>

#include "mastering/eq/band_strings.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::mastering::eq {
namespace {

using JsonValue = sonare::util::json::Value;

[[noreturn]] void invalid_band(const char* context, const std::string& message) {
  throw SonareException(ErrorCode::InvalidParameter, context + message);
}

const JsonValue* find_json_value(const JsonValue& object, const char* key) {
  return object.is_object() ? object.find(key) : nullptr;
}

double json_number(const JsonValue& object, const char* key, double fallback, const char* context) {
  const JsonValue* value = find_json_value(object, key);
  if (!value) return fallback;
  if (!value->is_number()) {
    invalid_band(context, std::string("expected numeric JSON field: ") + key);
  }
  return value->as_number();
}

double json_number_any(const JsonValue& object, const char* first_key, const char* second_key,
                       double fallback, const char* context) {
  const JsonValue* value = find_json_value(object, first_key);
  if (value && !value->is_number()) {
    invalid_band(context, std::string("expected numeric JSON field: ") + first_key);
  }
  if (value) return value->as_number();
  return json_number(object, second_key, fallback, context);
}

// Every JSON double reaches a float field through here: as_number() yields a
// full double, so a literal like 1e39 is out of float range and its raw
// narrowing would be undefined behaviour. Mirrors project_serializer_decode's
// float_or.
float json_float(const JsonValue& object, const char* key, float fallback, const char* context) {
  float converted = 0.0f;
  if (!sonare::numeric::checked_float_cast(json_number(object, key, fallback, context),
                                           &converted)) {
    invalid_band(context,
                 std::string("numeric JSON field is non-finite or out of float range: ") + key);
  }
  return converted;
}

float json_float_any(const JsonValue& object, const char* first_key, const char* second_key,
                     float fallback, const char* context) {
  float converted = 0.0f;
  if (!sonare::numeric::checked_float_cast(
          json_number_any(object, first_key, second_key, fallback, context), &converted)) {
    invalid_band(context, std::string("numeric JSON field is non-finite or out of float range: ") +
                              first_key);
  }
  return converted;
}

// A dB field that ends up as a filter gain: the shelf designs raise 10^(dB/20)
// in float, which overflows past roughly 770 dB and no longer realizes the gain.
float json_gain_db_any(const JsonValue& object, const char* first_key, const char* second_key,
                       float fallback, const char* context) {
  const float value = json_float_any(object, first_key, second_key, fallback, context);
  if (!sonare::numeric::finite(db_to_linear(value))) {
    invalid_band(context,
                 std::string("dB JSON field is too large to realize as a filter: ") + first_key);
  }
  return value;
}

int json_int_any(const JsonValue& object, const char* first_key, const char* second_key,
                 int fallback, const char* context) {
  int converted = 0;
  if (!sonare::numeric::checked_integral_cast(
          std::round(json_number_any(object, first_key, second_key, fallback, context)),
          &converted)) {
    invalid_band(context,
                 std::string("integer JSON field is non-finite or out of range: ") + first_key);
  }
  return converted;
}

bool json_bool(const JsonValue& object, const char* key, bool fallback, const char* context) {
  const JsonValue* value = find_json_value(object, key);
  if (!value) return fallback;
  if (value->is_bool()) return value->as_bool();
  if (value->is_number()) return value->as_number() != 0.0;
  invalid_band(context, std::string("expected boolean JSON field: ") + key);
}

bool json_bool_any(const JsonValue& object, const char* first_key, const char* second_key,
                   bool fallback, const char* context) {
  const JsonValue* value = find_json_value(object, first_key);
  if (value) return json_bool(object, first_key, fallback, context);
  return json_bool(object, second_key, fallback, context);
}

std::string json_string(const JsonValue& object, const char* key, const std::string& fallback,
                        const char* context) {
  const JsonValue* value = find_json_value(object, key);
  if (!value) return fallback;
  if (!value->is_string()) {
    invalid_band(context, std::string("expected string JSON field: ") + key);
  }
  return value->as_string();
}

std::string json_string_any(const JsonValue& object, const char* first_key, const char* second_key,
                            const std::string& fallback, const char* context) {
  const JsonValue* value = find_json_value(object, first_key);
  if (value) return json_string(object, first_key, fallback, context);
  return json_string(object, second_key, fallback, context);
}

EqBandType parse_band_type(const std::string& value, const char* context) {
  const auto parsed = band_type_from_string(value);
  if (!parsed) invalid_band(context, "unknown EQ band type: " + value);
  return *parsed;
}

BiquadCoeffMode parse_coeff_mode(const std::string& value, const char* context) {
  const auto parsed = coeff_mode_from_string(value);
  if (!parsed) invalid_band(context, "unknown EQ coefficient mode: " + value);
  return *parsed;
}

StereoPlacement parse_placement(const std::string& value, const char* context) {
  const auto parsed = placement_from_string(value);
  if (!parsed) invalid_band(context, "unknown EQ placement: " + value);
  return *parsed;
}

PhaseMode parse_band_phase(const std::string& value, const char* context) {
  const auto parsed = phase_mode_from_string(value);
  if (!parsed) invalid_band(context, "unknown EQ band phase mode: " + value);
  return *parsed;
}

const char* band_type_to_string(EqBandType type) {
  switch (type) {
    case EqBandType::Peak:
      return "Peak";
    case EqBandType::LowShelf:
      return "LowShelf";
    case EqBandType::HighShelf:
      return "HighShelf";
    case EqBandType::LowPass:
      return "LowPass";
    case EqBandType::HighPass:
      return "HighPass";
    case EqBandType::BandPass:
      return "BandPass";
    case EqBandType::Notch:
      return "Notch";
    case EqBandType::TiltShelf:
      return "TiltShelf";
    case EqBandType::FlatTilt:
      return "FlatTilt";
    case EqBandType::AllPass:
      return "AllPass";
  }
  return "Peak";
}

const char* coeff_mode_to_string(BiquadCoeffMode mode) {
  return mode == BiquadCoeffMode::Vicanek ? "Vicanek" : "Rbj";
}

const char* placement_to_string(StereoPlacement placement) {
  switch (placement) {
    case StereoPlacement::Stereo:
      return "Stereo";
    case StereoPlacement::Left:
      return "Left";
    case StereoPlacement::Right:
      return "Right";
    case StereoPlacement::Mid:
      return "Mid";
    case StereoPlacement::Side:
      return "Side";
  }
  return "Stereo";
}

const char* phase_mode_to_string(PhaseMode phase) {
  switch (phase) {
    case PhaseMode::Inherit:
      return "Inherit";
    case PhaseMode::ZeroLatency:
      return "ZeroLatency";
    case PhaseMode::NaturalPhase:
      return "NaturalPhase";
    case PhaseMode::LinearPhase:
      return "LinearPhase";
  }
  return "Inherit";
}

}  // namespace

EqBand eq_band_from_value(const JsonValue& value, const char* context) {
  if (!value.is_object()) invalid_band(context, "value must be a JSON object");
  EqBand band;
  band.type = parse_band_type(json_string(value, "type", "Peak", context), context);
  band.coeff_mode =
      parse_coeff_mode(json_string_any(value, "coeffMode", "coeff_mode", "Rbj", context), context);
  band.frequency_hz =
      json_float_any(value, "frequencyHz", "frequency_hz", band.frequency_hz, context);
  band.gain_db = json_gain_db_any(value, "gainDb", "gain_db", band.gain_db, context);
  band.q = json_float(value, "q", band.q, context);
  band.enabled = json_bool(value, "enabled", band.enabled, context);
  band.slope_db_oct = json_int_any(value, "slopeDbOct", "slope_db_oct", band.slope_db_oct, context);
  band.placement = parse_placement(json_string(value, "placement", "Stereo", context), context);
  band.phase = parse_band_phase(json_string(value, "phase", "Inherit", context), context);
  band.soloed = json_bool(value, "soloed", false, context);
  band.bypassed = json_bool(value, "bypassed", false, context);
  band.proportional_q = json_bool_any(value, "proportionalQ", "proportional_q", false, context);
  band.proportional_q_strength =
      json_float_any(value, "proportionalQStrength", "proportional_q_strength",
                     band.proportional_q_strength, context);

  band.dyn.enabled = json_bool_any(value, "dynamic", "dynEnabled", false, context);
  band.dyn.enabled = json_bool(value, "dyn_enabled", band.dyn.enabled, context);
  // threshold_db and range_db are the two dynamic terms that reach the biquad
  // as gain (detector_db - threshold_db, scaled by ratio, clamped to range_db),
  // so they carry the same realizability bound as the static gain.
  band.dyn.threshold_db =
      json_gain_db_any(value, "thresholdDb", "threshold_db", band.dyn.threshold_db, context);
  band.dyn.auto_threshold =
      json_bool_any(value, "autoThreshold", "auto_threshold", band.dyn.auto_threshold, context);
  band.dyn.ratio = json_float(value, "ratio", band.dyn.ratio, context);
  band.dyn.range_db = json_gain_db_any(value, "rangeDb", "range_db", band.dyn.range_db, context);
  band.dyn.attack_ms = json_float_any(value, "attackMs", "attack_ms", band.dyn.attack_ms, context);
  band.dyn.release_ms =
      json_float_any(value, "releaseMs", "release_ms", band.dyn.release_ms, context);
  // "lookaheadMs"/"lookahead_ms" are the field's former (misleading) spelling;
  // still accepted so a stored config keeps working, but the canonical
  // "detectorDelayMs"/"detector_delay_ms" wins if both are present.
  band.dyn.detector_delay_ms =
      json_float_any(value, "lookaheadMs", "lookahead_ms", band.dyn.detector_delay_ms, context);
  band.dyn.detector_delay_ms = json_float_any(value, "detectorDelayMs", "detector_delay_ms",
                                              band.dyn.detector_delay_ms, context);
  band.dyn.sidechain_freq_hz = json_float_any(value, "sidechainFreqHz", "sidechain_freq_hz",
                                              band.dyn.sidechain_freq_hz, context);
  band.dyn.sidechain_q =
      json_float_any(value, "sidechainQ", "sidechain_q", band.dyn.sidechain_q, context);
  band.dyn.external_sidechain = json_bool_any(value, "externalSidechain", "external_sidechain",
                                              band.dyn.external_sidechain, context);
  return band;
}

JsonValue eq_band_to_value(const EqBand& band) {
  static const EqBand kDefault{};
  sonare::util::json::Object object;
  if (band.type != kDefault.type) {
    object.emplace("type", JsonValue(band_type_to_string(band.type)));
  }
  if (band.frequency_hz != kDefault.frequency_hz) {
    object.emplace("frequencyHz", JsonValue(band.frequency_hz));
  }
  if (band.gain_db != kDefault.gain_db) object.emplace("gainDb", JsonValue(band.gain_db));
  if (band.q != kDefault.q) object.emplace("q", JsonValue(band.q));
  object.emplace("enabled", JsonValue(band.enabled));
  if (band.coeff_mode != kDefault.coeff_mode) {
    object.emplace("coeffMode", JsonValue(coeff_mode_to_string(band.coeff_mode)));
  }
  if (band.slope_db_oct != kDefault.slope_db_oct) {
    object.emplace("slopeDbOct", JsonValue(band.slope_db_oct));
  }
  if (band.placement != kDefault.placement) {
    object.emplace("placement", JsonValue(placement_to_string(band.placement)));
  }
  if (band.phase != kDefault.phase)
    object.emplace("phase", JsonValue(phase_mode_to_string(band.phase)));
  if (band.soloed != kDefault.soloed) object.emplace("soloed", JsonValue(band.soloed));
  if (band.bypassed != kDefault.bypassed) object.emplace("bypassed", JsonValue(band.bypassed));
  if (band.proportional_q != kDefault.proportional_q) {
    object.emplace("proportionalQ", JsonValue(band.proportional_q));
  }
  if (band.proportional_q_strength != kDefault.proportional_q_strength) {
    object.emplace("proportionalQStrength", JsonValue(band.proportional_q_strength));
  }
  const DynamicParams& dyn = band.dyn;
  const DynamicParams& dyn_default = kDefault.dyn;
  if (dyn.enabled != dyn_default.enabled) object.emplace("dynamic", JsonValue(dyn.enabled));
  if (dyn.threshold_db != dyn_default.threshold_db) {
    object.emplace("thresholdDb", JsonValue(dyn.threshold_db));
  }
  if (dyn.auto_threshold != dyn_default.auto_threshold) {
    object.emplace("autoThreshold", JsonValue(dyn.auto_threshold));
  }
  if (dyn.ratio != dyn_default.ratio) object.emplace("ratio", JsonValue(dyn.ratio));
  if (dyn.range_db != dyn_default.range_db) object.emplace("rangeDb", JsonValue(dyn.range_db));
  if (dyn.attack_ms != dyn_default.attack_ms) object.emplace("attackMs", JsonValue(dyn.attack_ms));
  if (dyn.release_ms != dyn_default.release_ms) {
    object.emplace("releaseMs", JsonValue(dyn.release_ms));
  }
  if (dyn.detector_delay_ms != dyn_default.detector_delay_ms) {
    object.emplace("detectorDelayMs", JsonValue(dyn.detector_delay_ms));
  }
  if (dyn.external_sidechain != dyn_default.external_sidechain) {
    object.emplace("externalSidechain", JsonValue(dyn.external_sidechain));
  }
  if (dyn.sidechain_freq_hz != dyn_default.sidechain_freq_hz) {
    object.emplace("sidechainFreqHz", JsonValue(dyn.sidechain_freq_hz));
  }
  if (dyn.sidechain_q != dyn_default.sidechain_q) {
    object.emplace("sidechainQ", JsonValue(dyn.sidechain_q));
  }
  return JsonValue(std::move(object));
}

}  // namespace sonare::mastering::eq
