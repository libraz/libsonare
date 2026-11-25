#include "mastering/eq/eq_band_value.h"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include "mastering/eq/band_strings.h"
#include "mastering/eq/eq_band_domains.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/json_schema.h"
#include "util/numeric_validation.h"

namespace sonare::mastering::eq {
namespace {

using JsonValue = sonare::util::json::Value;

namespace schema = sonare::util::json::schema;

// Every refusal is structured so the caller that knows the argument's role can recode it.
[[noreturn]] void wrong_type(const std::string& at, schema::Kind kind) {
  throw SonareParameterRefusal::wrong_type(ErrorCode::InvalidFormat, at,
                                           std::string("must be ") + schema::kind_text(kind))
      .as_caller_spelled();
}

[[noreturn]] void refuse_value(const std::string& at, const std::string& predicate) {
  throw SonareParameterRefusal::predicate(ErrorCode::InvalidFormat, at, predicate)
      .as_caller_spelled();
}

const JsonValue* find_json_value(const JsonValue& object, const char* key) {
  return object.is_object() ? object.find(key) : nullptr;
}

double json_number(const JsonValue& object, const char* key, double fallback,
                   const std::string& path) {
  const JsonValue* value = find_json_value(object, key);
  if (!value) return fallback;
  if (!value->is_number()) {
    wrong_type(schema::child_path(path, key), schema::Kind::Number);
  }
  return value->as_number();
}

// Every JSON double reaches a float field through here: as_number() yields a
// full double, so a literal like 1e39 is out of float range and its raw
// narrowing would be undefined behaviour. Mirrors project_serializer_decode's
// float_or.
float json_float(const JsonValue& object, const char* key, float fallback,
                 const std::string& path) {
  float converted = 0.0f;
  if (!sonare::numeric::checked_float_cast(json_number(object, key, fallback, path), &converted)) {
    refuse_value(schema::child_path(path, key), schema::kFloatRangePredicate);
  }
  return converted;
}

void validate_domain(double value, const sonare::util::check::NumericDomain& domain,
                     const std::string& subject) {
  if (!sonare::util::check::in_domain(value, domain)) {
    throw SonareParameterRefusal::predicate(
        ErrorCode::InvalidFormat, subject, sonare::util::check::domain_predicate(domain),
        RefusalDomain{domain.lo, domain.hi, domain.lo_included, domain.hi_included})
        .as_caller_spelled();
  }
}

float json_float_in_domain(const JsonValue& object, const char* key, float fallback,
                           const std::string& path,
                           const sonare::util::check::NumericDomain& domain) {
  const std::string subject = schema::child_path(path, key);
  const double number = json_number(object, key, fallback, path);
  validate_domain(number, domain, subject);
  float converted = 0.0f;
  if (!sonare::numeric::checked_float_cast(number, &converted)) {
    refuse_value(subject, schema::kFloatRangePredicate);
  }
  return converted;
}

// A dB field that ends up as a filter gain: the shelf designs raise 10^(dB/20)
// in float, which overflows past roughly 770 dB and no longer realizes the gain.
float json_gain_db(const JsonValue& object, const char* key, float fallback,
                   const std::string& path) {
  return json_float_in_domain(object, key, fallback, path, kEqDbGainDomain);
}

int json_int(const JsonValue& object, const char* key, int fallback, const std::string& path) {
  int converted = 0;
  if (!sonare::numeric::checked_integral_cast(std::round(json_number(object, key, fallback, path)),
                                              &converted)) {
    refuse_value(schema::child_path(path, key),
                 schema::integer_range_predicate(std::numeric_limits<int>::min(),
                                                 std::numeric_limits<int>::max()));
  }
  return converted;
}

bool json_bool(const JsonValue& object, const char* key, bool fallback, const std::string& path) {
  const JsonValue* value = find_json_value(object, key);
  if (!value) return fallback;
  if (value->is_bool()) return value->as_bool();
  wrong_type(schema::child_path(path, key), schema::Kind::Boolean);
}

std::string json_string(const JsonValue& object, const char* key, const std::string& fallback,
                        const std::string& path) {
  const JsonValue* value = find_json_value(object, key);
  if (!value) return fallback;
  if (!value->is_string()) {
    wrong_type(schema::child_path(path, key), schema::Kind::String);
  }
  return value->as_string();
}

[[noreturn]] void unknown_token(const char* key, const std::string& tokens,
                                const std::string& path) {
  refuse_value(schema::child_path(path, key), "must be one of " + tokens);
}

EqBandType parse_band_type(const std::string& value, const std::string& path) {
  const auto parsed = band_type_from_string(value);
  if (!parsed) unknown_token("type", band_type_tokens(), path);
  return *parsed;
}

BiquadCoeffMode parse_coeff_mode(const std::string& value, const std::string& path) {
  const auto parsed = coeff_mode_from_string(value);
  if (!parsed) unknown_token("coeffMode", coeff_mode_tokens(), path);
  return *parsed;
}

StereoPlacement parse_placement(const std::string& value, const std::string& path) {
  const auto parsed = placement_from_string(value);
  if (!parsed) unknown_token("placement", placement_tokens(), path);
  return *parsed;
}

PhaseMode parse_band_phase(const std::string& value, const std::string& path) {
  const auto parsed = phase_mode_from_string(value);
  if (!parsed) unknown_token("phase", phase_mode_tokens(), path);
  return *parsed;
}

}  // namespace

EqBand eq_band_from_value(const JsonValue& value, const std::string& path,
                          std::optional<double> sample_rate) {
  if (!value.is_object()) {
    if (path.empty())
      throw SonareException(ErrorCode::InvalidFormat, schema::root_must_be_object());
    wrong_type(path, schema::Kind::Object);
  }
  EqBand band;
  band.type =
      parse_band_type(json_string(value, "type", band_type_to_string(band.type), path), path);
  band.coeff_mode = parse_coeff_mode(
      json_string(value, "coeffMode", coeff_mode_to_string(band.coeff_mode), path), path);
  const JsonValue* enabled_value = find_json_value(value, "enabled");
  const bool enabled_for_frequency =
      enabled_value && enabled_value->is_bool() ? enabled_value->as_bool() : band.enabled;
  band.frequency_hz = sample_rate && enabled_for_frequency
                          ? json_float_in_domain(value, "frequencyHz", band.frequency_hz, path,
                                                 eq_band_frequency_domain(*sample_rate))
                          : json_float(value, "frequencyHz", band.frequency_hz, path);
  band.gain_db = json_gain_db(value, "gainDb", band.gain_db, path);
  band.q = json_float(value, "q", band.q, path);
  band.enabled = json_bool(value, "enabled", band.enabled, path);
  band.slope_db_oct = json_int(value, "slopeDbOct", band.slope_db_oct, path);
  band.placement = parse_placement(
      json_string(value, "placement", placement_to_string(band.placement), path), path);
  band.phase =
      parse_band_phase(json_string(value, "phase", phase_mode_to_string(band.phase), path), path);
  band.soloed = json_bool(value, "soloed", false, path);
  band.bypassed = json_bool(value, "bypassed", false, path);
  band.proportional_q = json_bool(value, "proportionalQ", false, path);
  band.proportional_q_strength =
      json_float(value, "proportionalQStrength", band.proportional_q_strength, path);

  band.dyn.enabled = json_bool(value, "dynamic", false, path);
  // threshold_db and range_db are the two dynamic terms that reach the biquad
  // as gain (detector_db - threshold_db, scaled by ratio, clamped to range_db),
  // so they carry the same realizability bound as the static gain.
  band.dyn.threshold_db = json_gain_db(value, "thresholdDb", band.dyn.threshold_db, path);
  band.dyn.auto_threshold = json_bool(value, "autoThreshold", band.dyn.auto_threshold, path);
  band.dyn.ratio = band.dyn.enabled
                       ? json_float_in_domain(value, "ratio", band.dyn.ratio, path, kEqRatioDomain)
                       : json_float(value, "ratio", band.dyn.ratio, path);
  band.dyn.range_db = json_gain_db(value, "rangeDb", band.dyn.range_db, path);
  band.dyn.attack_ms = band.dyn.enabled
                           ? json_float_in_domain(value, "attackMs", band.dyn.attack_ms, path,
                                                  kEqNonnegativeFloatDomain)
                           : json_float(value, "attackMs", band.dyn.attack_ms, path);
  band.dyn.release_ms = band.dyn.enabled
                            ? json_float_in_domain(value, "releaseMs", band.dyn.release_ms, path,
                                                   kEqNonnegativeFloatDomain)
                            : json_float(value, "releaseMs", band.dyn.release_ms, path);
  band.dyn.detector_delay_ms =
      band.dyn.enabled ? json_float_in_domain(value, "detectorDelayMs", band.dyn.detector_delay_ms,
                                              path, kEqNonnegativeFloatDomain)
                       : json_float(value, "detectorDelayMs", band.dyn.detector_delay_ms, path);
  const double sidechain_frequency =
      json_number(value, "sidechainFreqHz", band.dyn.sidechain_freq_hz, path);
  if (band.dyn.enabled && sidechain_frequency != kEqSidechainFrequencyDomain.sentinel) {
    validate_domain(sidechain_frequency, kEqSidechainFrequencyDomain,
                    schema::child_path(path, "sidechainFreqHz"));
  }
  float sidechain_frequency_float = 0.0f;
  if (!sonare::numeric::checked_float_cast(sidechain_frequency, &sidechain_frequency_float)) {
    refuse_value(schema::child_path(path, "sidechainFreqHz"), schema::kFloatRangePredicate);
  }
  band.dyn.sidechain_freq_hz = sidechain_frequency_float;
  band.dyn.sidechain_q = band.dyn.enabled
                             ? json_float_in_domain(value, "sidechainQ", band.dyn.sidechain_q, path,
                                                    kEqPositiveFloatDomain)
                             : json_float(value, "sidechainQ", band.dyn.sidechain_q, path);
  band.dyn.external_sidechain =
      json_bool(value, "externalSidechain", band.dyn.external_sidechain, path);
  return band;
}

const std::vector<std::string>& eq_band_known_keys() {
  static const std::vector<std::string> keys = {
      "type",
      "coeffMode",
      "frequencyHz",
      "gainDb",
      "q",
      "enabled",
      "slopeDbOct",
      "placement",
      "phase",
      "soloed",
      "bypassed",
      "proportionalQ",
      "proportionalQStrength",
      "dynamic",
      "thresholdDb",
      "autoThreshold",
      "ratio",
      "rangeDb",
      "attackMs",
      "releaseMs",
      "detectorDelayMs",
      "sidechainFreqHz",
      "sidechainQ",
      "externalSidechain",
  };
  return keys;
}

JsonValue eq_band_to_value(const EqBand& band) {
  static const EqBand kDefault{};
  sonare::util::json::Object object;
  if (band.type != kDefault.type) {
    util::json::put(object, "type", band_type_to_string(band.type));
  }
  if (band.frequency_hz != kDefault.frequency_hz) {
    util::json::put(object, "frequencyHz", band.frequency_hz);
  }
  if (band.gain_db != kDefault.gain_db) util::json::put(object, "gainDb", band.gain_db);
  if (band.q != kDefault.q) util::json::put(object, "q", band.q);
  util::json::put(object, "enabled", band.enabled);
  if (band.coeff_mode != kDefault.coeff_mode) {
    util::json::put(object, "coeffMode", coeff_mode_to_string(band.coeff_mode));
  }
  if (band.slope_db_oct != kDefault.slope_db_oct) {
    util::json::put(object, "slopeDbOct", band.slope_db_oct);
  }
  if (band.placement != kDefault.placement) {
    util::json::put(object, "placement", placement_to_string(band.placement));
  }
  if (band.phase != kDefault.phase)
    util::json::put(object, "phase", phase_mode_to_string(band.phase));
  if (band.soloed != kDefault.soloed) util::json::put(object, "soloed", band.soloed);
  if (band.bypassed != kDefault.bypassed) util::json::put(object, "bypassed", band.bypassed);
  if (band.proportional_q != kDefault.proportional_q) {
    util::json::put(object, "proportionalQ", band.proportional_q);
  }
  if (band.proportional_q_strength != kDefault.proportional_q_strength) {
    util::json::put(object, "proportionalQStrength", band.proportional_q_strength);
  }
  const DynamicParams& dyn = band.dyn;
  const DynamicParams& dyn_default = kDefault.dyn;
  if (dyn.enabled != dyn_default.enabled) util::json::put(object, "dynamic", dyn.enabled);
  if (dyn.threshold_db != dyn_default.threshold_db) {
    util::json::put(object, "thresholdDb", dyn.threshold_db);
  }
  if (dyn.auto_threshold != dyn_default.auto_threshold) {
    util::json::put(object, "autoThreshold", dyn.auto_threshold);
  }
  if (dyn.ratio != dyn_default.ratio) util::json::put(object, "ratio", dyn.ratio);
  if (dyn.range_db != dyn_default.range_db) util::json::put(object, "rangeDb", dyn.range_db);
  if (dyn.attack_ms != dyn_default.attack_ms) util::json::put(object, "attackMs", dyn.attack_ms);
  if (dyn.release_ms != dyn_default.release_ms) {
    util::json::put(object, "releaseMs", dyn.release_ms);
  }
  if (dyn.detector_delay_ms != dyn_default.detector_delay_ms) {
    util::json::put(object, "detectorDelayMs", dyn.detector_delay_ms);
  }
  if (dyn.external_sidechain != dyn_default.external_sidechain) {
    util::json::put(object, "externalSidechain", dyn.external_sidechain);
  }
  if (dyn.sidechain_freq_hz != dyn_default.sidechain_freq_hz) {
    util::json::put(object, "sidechainFreqHz", dyn.sidechain_freq_hz);
  }
  if (dyn.sidechain_q != dyn_default.sidechain_q) {
    util::json::put(object, "sidechainQ", dyn.sidechain_q);
  }
  return JsonValue(std::move(object));
}

}  // namespace sonare::mastering::eq
