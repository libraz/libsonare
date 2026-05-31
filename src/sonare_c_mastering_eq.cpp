#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

#include "mastering/eq/equalizer.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_spectrum.h"
#include "sonare_c.h"
#include "sonare_c_internal.h"
#include "util/json.h"

using namespace sonare;
using namespace sonare_c_detail;

// Keep the C API band count in sync with the C++ processor and the spectrum
// engine's fixed-size band_gain_db array (std::array<float, 24>).
static_assert(SONARE_EQ_MAX_BANDS == sonare::mastering::eq::EqualizerProcessor::kMaxBands,
              "SONARE_EQ_MAX_BANDS must match EqualizerProcessor::kMaxBands");

struct SonareEq {
  sonare::mastering::eq::EqualizerProcessor processor;
  double sample_rate = 48000.0;
  int max_block_size = 0;
};

namespace {

[[noreturn]] void invalid_eq_json(const std::string& message) {
  throw SonareException(ErrorCode::InvalidParameter, "sonare_eq_set_band: " + message);
}

// EQ band JSON parsing now delegates to the shared util::json::parse so we
// only maintain one JSON grammar across the C API. The accessor helpers below
// adapt the generic util::json::Value to the EQ-specific error model
// (SonareException with the sonare_eq_set_band: prefix).

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

sonare::mastering::eq::PhaseMode parse_phase(int mode) {
  using sonare::mastering::eq::PhaseMode;
  switch (mode) {
    case 1:
      return PhaseMode::ZeroLatency;
    case 2:
      return PhaseMode::NaturalPhase;
    case 3:
      return PhaseMode::LinearPhase;
    default:
      throw SonareException(ErrorCode::InvalidParameter, "unknown EQ phase mode");
  }
}

sonare::mastering::eq::PhaseMode parse_band_phase(const std::string& value) {
  using sonare::mastering::eq::PhaseMode;
  if (value == "Inherit" || value == "inherit") return PhaseMode::Inherit;
  if (value == "ZeroLatency" || value == "zeroLatency") return PhaseMode::ZeroLatency;
  if (value == "NaturalPhase" || value == "naturalPhase") return PhaseMode::NaturalPhase;
  if (value == "LinearPhase" || value == "linearPhase") return PhaseMode::LinearPhase;
  invalid_eq_json("unknown EQ band phase mode: " + value);
}

sonare::mastering::eq::EqBand parse_eq_band_json(const char* band_json) {
  if (!band_json) invalid_eq_json("band_json must not be null");
  JsonValue json;
  try {
    // Strict parse: duplicate `type` (or any other) field is treated as a
    // hard error so callers cannot pass an ambiguous spec and silently take
    // the last value.
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

}  // namespace

SonareEq* sonare_eq_create(double sample_rate, int max_block_size) {
  if (!(sample_rate > 0.0) || max_block_size < 0) {
    return nullptr;
  }
  try {
    auto* handle = new SonareEq;
    handle->sample_rate = sample_rate;
    handle->max_block_size = max_block_size;
    handle->processor.prepare(sample_rate, max_block_size);
    return handle;
  } catch (...) {
    return nullptr;
  }
}

void sonare_eq_destroy(SonareEq* eq) { delete eq; }

SonareError sonare_eq_set_band(SonareEq* eq, int index, const char* band_json) {
  if (!eq || !band_json || index < 0 || index >= static_cast<int>(SONARE_EQ_MAX_BANDS)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  eq->processor.set_band(static_cast<size_t>(index), parse_eq_band_json(band_json));
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_eq_clear(SonareEq* eq) {
  if (eq) {
    eq->processor.clear();
  }
}

SonareError sonare_eq_set_phase_mode(SonareEq* eq, int mode) {
  if (!eq) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  eq->processor.set_phase_mode(parse_phase(mode));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_eq_match(SonareEq* eq, const float* source, const float* reference,
                            size_t length, int sample_rate, int max_bands) {
  if (!eq || max_bands <= 0 || max_bands > static_cast<int>(SONARE_EQ_MAX_BANDS)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SonareError err = validate_audio_params(source, length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params(reference, length, sample_rate);
  if (err != SONARE_OK) return err;

  SONARE_C_TRY
  sonare::mastering::match::MatchEqConfig config;
  config.max_bands = static_cast<size_t>(max_bands);
  const Audio source_audio = Audio::from_buffer(source, length, sample_rate);
  const Audio reference_audio = Audio::from_buffer(reference, length, sample_rate);
  sonare::mastering::match::configure_equalizer_from_match(
      eq->processor, sonare::mastering::match::reference_spectrum(source_audio),
      sonare::mastering::match::reference_spectrum(reference_audio), config);
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_eq_set_auto_gain(SonareEq* eq, int enabled) {
  if (eq) {
    eq->processor.set_auto_gain_enabled(enabled != 0);
  }
}

float sonare_eq_last_auto_gain_db(const SonareEq* eq) {
  return eq ? eq->processor.last_auto_gain_db() : 0.0f;
}

SonareError sonare_eq_set_gain_scale(SonareEq* eq, float scale) {
  if (!eq) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  eq->processor.set_gain_scale(scale);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_eq_set_output_gain_db(SonareEq* eq, float gain_db) {
  if (!eq) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  eq->processor.set_output_gain_db(gain_db);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_eq_set_output_pan(SonareEq* eq, float pan) {
  if (!eq) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  eq->processor.set_output_pan(pan);
  return SONARE_OK;
  SONARE_C_CATCH
}

int sonare_eq_latency_samples(const SonareEq* eq) {
  return eq ? eq->processor.latency_samples() : 0;
}

SonareError sonare_eq_set_sidechain(SonareEq* eq, const float* const* channels, int num_channels,
                                    int num_samples) {
  if (!eq) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  eq->processor.set_sidechain(channels, num_channels, num_samples);
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_eq_clear_sidechain(SonareEq* eq) {
  if (eq) {
    eq->processor.clear_sidechain();
  }
}

SonareError sonare_eq_process(SonareEq* eq, float* const* channels, int num_channels,
                              int num_samples) {
  if (!eq) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  eq->processor.process(channels, num_channels, num_samples);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_eq_spectrum(const SonareEq* eq, SonareEqSnapshot* out) {
  if (!eq || !out) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  const auto snapshot = eq->processor.spectrum_snapshot();
  *out = {};
  out->pre_count = snapshot.pre_count;
  out->post_count = snapshot.post_count;
  out->last_auto_gain_db = eq->processor.last_auto_gain_db();
  out->seq = snapshot.seq;
  for (size_t i = 0; i < SONARE_EQ_SPECTRUM_STREAM_CAPACITY; ++i) {
    out->pre_left[i] = snapshot.pre[i].left;
    out->pre_right[i] = snapshot.pre[i].right;
    out->post_left[i] = snapshot.post[i].left;
    out->post_right[i] = snapshot.post[i].right;
  }
  for (size_t i = 0; i < SONARE_EQ_MAX_BANDS; ++i) {
    out->band_gain_db[i] = snapshot.band_gain_db[i];
  }
  for (size_t i = 0; i < SONARE_EQ_SPECTRUM_PROFILE_BANDS; ++i) {
    out->profile_db[i] = snapshot.profile_db[i];
  }
  return SONARE_OK;
  SONARE_C_CATCH
}
