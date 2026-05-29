#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/assistant/suggester.h"
#include "mastering/eq/equalizer.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_spectrum.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/streaming_preview.h"
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

sonare::mastering::maximizer::LoudnessOptimizeConfig to_cpp_config(
    const SonareMasteringConfig* config) {
  sonare::mastering::maximizer::LoudnessOptimizeConfig cpp;
  if (config) {
    cpp.target_lufs = config->target_lufs;
    cpp.ceiling_db = config->ceiling_db;
    cpp.true_peak_oversample = config->true_peak_oversample;
  }
  return cpp;
}

std::vector<sonare::mastering::api::Param> to_params(const SonareMasteringParam* params,
                                                     size_t count) {
  std::vector<sonare::mastering::api::Param> out;
  out.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    if (params[index].key) {
      out.push_back({params[index].key, params[index].value});
    }
  }
  return out;
}

sonare::mastering::assistant::AssistantConfig to_assistant_config(
    const SonareMasteringParam* params, size_t count) {
  sonare::mastering::assistant::AssistantConfig config;
  for (size_t index = 0; index < count; ++index) {
    if (!params[index].key) continue;
    const std::string key = params[index].key;
    if (key == "targetLufs" || key == "target_lufs") {
      config.target_lufs = static_cast<float>(params[index].value);
    } else if (key == "ceilingDb" || key == "ceiling_db") {
      config.ceiling_db = static_cast<float>(params[index].value);
    } else if (key == "enableRepair" || key == "enable_repair") {
      config.enable_repair = params[index].value != 0.0;
    } else if (key == "preferStreamingSafe" || key == "prefer_streaming_safe") {
      config.prefer_streaming_safe = params[index].value != 0.0;
    } else if (key == "speechMonoAmount" || key == "speech_mono_amount") {
      config.speech_mono_amount = static_cast<float>(params[index].value);
    }
  }
  return config;
}

sonare::mastering::assistant::AudioProfileConfig to_audio_profile_config(
    const SonareMasteringParam* params, size_t count) {
  sonare::mastering::assistant::AudioProfileConfig config;
  for (size_t index = 0; index < count; ++index) {
    if (!params[index].key) continue;
    const std::string key = params[index].key;
    if (key == "nFft" || key == "n_fft") {
      config.n_fft = static_cast<int>(params[index].value);
    } else if (key == "hopLength" || key == "hop_length") {
      config.hop_length = static_cast<int>(params[index].value);
    } else if (key == "truePeakOversample" || key == "true_peak_oversample") {
      config.true_peak_oversample = static_cast<int>(params[index].value);
    }
  }
  return config;
}

void set_mastering_result(const sonare::mastering::api::MonoResult& result,
                          SonareMasteringResult* out) {
  out->length = result.samples.size();
  out->sample_rate = result.sample_rate;
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;
  out->latency_samples = result.latency_samples;
  std::unique_ptr<float[]> processed(new float[out->length]);
  std::memcpy(processed.get(), result.samples.data(), out->length * sizeof(float));
  out->samples = release_array(processed);
}

char* copy_string(const std::string& value) {
  std::unique_ptr<char[]> out(new char[value.size() + 1]);
  std::memcpy(out.get(), value.c_str(), value.size() + 1);
  return out.release();
}

const char* join_names(const std::vector<std::string>& values, std::string& storage) {
  std::ostringstream stream;
  for (size_t index = 0; index < values.size(); ++index) {
    if (index > 0) stream << '\n';
    stream << values[index];
  }
  storage = stream.str();
  return storage.c_str();
}

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

SonareError sonare_mastering_process(const float* samples, size_t length, int sample_rate,
                                     const SonareMasteringConfig* config,
                                     SonareMasteringResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;

  out->samples = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->latency_samples = 0;

  SONARE_C_TRY
  Audio audio = Audio::from_buffer(samples, length, sample_rate);
  auto result = sonare::mastering::maximizer::loudness_optimize(audio, to_cpp_config(config));

  out->length = result.audio.size();
  out->sample_rate = result.audio.sample_rate();
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;

  std::unique_ptr<float[]> processed(new float[out->length]);
  std::memcpy(processed.get(), result.audio.data(), out->length * sizeof(float));
  out->samples = release_array(processed);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_apply_processor(const char* processor_name, const float* samples,
                                             size_t length, int sample_rate,
                                             const SonareMasteringParam* params, size_t param_count,
                                             SonareMasteringResult* out) {
  if (!out || !processor_name) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->samples = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->latency_samples = 0;

  SONARE_C_TRY
  auto result = sonare::mastering::api::apply_named_processor(
      processor_name, samples, length, sample_rate, to_params(params, param_count));
  set_mastering_result(result, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_apply_processor_stereo(const char* processor_name, const float* left,
                                                    const float* right, size_t length,
                                                    int sample_rate,
                                                    const SonareMasteringParam* params,
                                                    size_t param_count,
                                                    SonareMasteringStereoResult* out) {
  if (!out || !processor_name || !left || !right || sample_rate <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->left = nullptr;
  out->right = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->latency_samples = 0;

  SONARE_C_TRY
  auto result = sonare::mastering::api::apply_named_processor_stereo(
      processor_name, left, right, length, sample_rate, to_params(params, param_count));
  out->length = result.left.size();
  out->sample_rate = result.sample_rate;
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;
  out->latency_samples = result.latency_samples;
  std::unique_ptr<float[]> left_out(new float[out->length]);
  std::unique_ptr<float[]> right_out(new float[out->length]);
  std::memcpy(left_out.get(), result.left.data(), out->length * sizeof(float));
  std::memcpy(right_out.get(), result.right.data(), out->length * sizeof(float));
  out->left = release_array(left_out);
  out->right = release_array(right_out);
  return SONARE_OK;
  SONARE_C_CATCH
}

const char* sonare_mastering_processor_names(void) {
  static std::string names;
  if (names.empty()) {
    std::ostringstream stream;
    auto processors = sonare::mastering::api::processor_names();
    for (size_t index = 0; index < processors.size(); ++index) {
      if (index > 0) stream << '\n';
      stream << processors[index];
    }
    names = stream.str();
  }
  return names.c_str();
}

const char* sonare_mastering_pair_processor_names(void) {
  static std::string names;
  return join_names(sonare::mastering::api::pair_processor_names(), names);
}

const char* sonare_mastering_pair_analysis_names(void) {
  static std::string names;
  return join_names(sonare::mastering::api::pair_analysis_names(), names);
}

const char* sonare_mastering_stereo_analysis_names(void) {
  static std::string names;
  return join_names(sonare::mastering::api::stereo_analysis_names(), names);
}

SonareError sonare_mastering_apply_pair_processor(const char* processor_name, const float* source,
                                                  const float* reference, size_t length,
                                                  int sample_rate,
                                                  const SonareMasteringParam* params,
                                                  size_t param_count, SonareMasteringResult* out) {
  if (!out || !processor_name) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(source, length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params(reference, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  out->samples = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->latency_samples = 0;

  SONARE_C_TRY
  auto result = sonare::mastering::api::apply_named_pair_processor(
      processor_name, source, reference, length, sample_rate, to_params(params, param_count));
  set_mastering_result(result, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_analyze_pair(const char* analysis_name, const float* source,
                                          const float* reference, size_t length, int sample_rate,
                                          const SonareMasteringParam* params, size_t param_count,
                                          char** json_out) {
  if (!json_out || !analysis_name) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(source, length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params(reference, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  *json_out = nullptr;
  SONARE_C_TRY
  auto json = sonare::mastering::api::analyze_named_pair(
      analysis_name, source, reference, length, sample_rate, to_params(params, param_count));
  *json_out = copy_string(json);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_analyze_stereo(const char* analysis_name, const float* left,
                                            const float* right, size_t length, int sample_rate,
                                            const SonareMasteringParam* params, size_t param_count,
                                            char** json_out) {
  if (!json_out || !analysis_name) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(left, length, sample_rate);
  if (err != SONARE_OK) return err;
  err = validate_audio_params(right, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  *json_out = nullptr;
  SONARE_C_TRY
  auto json = sonare::mastering::api::analyze_named_stereo(
      analysis_name, left, right, length, sample_rate, to_params(params, param_count));
  *json_out = copy_string(json);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_streaming_preview(const float* samples, size_t length, int sample_rate,
                                               const SonareStreamingPlatform* platforms,
                                               size_t platform_count, char** json_out) {
  if (!json_out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!platforms && platform_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  *json_out = nullptr;

  SONARE_C_TRY
  std::vector<sonare::mastering::maximizer::StreamingPlatform> cpp_platforms;
  cpp_platforms.reserve(platform_count);
  for (size_t index = 0; index < platform_count; ++index) {
    if (!platforms[index].name) return SONARE_ERROR_INVALID_PARAMETER;
    cpp_platforms.push_back(
        {platforms[index].name, platforms[index].target_lufs, platforms[index].ceiling_db});
  }

  const Audio audio = Audio::from_buffer(samples, length, sample_rate);
  const auto results = platform_count == 0
                           ? sonare::mastering::maximizer::streaming_preview(audio)
                           : sonare::mastering::maximizer::streaming_preview(audio, cpp_platforms);

  *json_out = copy_string(sonare::mastering::maximizer::streaming_preview_to_json(results));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_assistant_suggest(const float* samples, size_t length, int sample_rate,
                                               const SonareMasteringParam* params,
                                               size_t param_count, char** json_out) {
  if (!json_out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  *json_out = nullptr;

  SONARE_C_TRY
  const auto config = to_assistant_config(params, param_count);
  const auto result =
      sonare::mastering::assistant::suggest_chain(samples, length, sample_rate, config);
  *json_out = copy_string(sonare::mastering::assistant::assistant_result_to_json(result));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_audio_profile(const float* samples, size_t length, int sample_rate,
                                           const SonareMasteringParam* params, size_t param_count,
                                           char** json_out) {
  if (!json_out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  *json_out = nullptr;

  SONARE_C_TRY
  const auto config = to_audio_profile_config(params, param_count);
  const auto profile =
      sonare::mastering::assistant::analyze_audio_profile(samples, length, sample_rate, config);
  *json_out = copy_string(sonare::mastering::assistant::audio_profile_to_json(profile));
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_free_mastering_result(SonareMasteringResult* result) {
  if (!result) return;
  delete[] result->samples;
  result->samples = nullptr;
  result->length = 0;
}

void sonare_free_mastering_stereo_result(SonareMasteringStereoResult* result) {
  if (!result) return;
  delete[] result->left;
  delete[] result->right;
  result->left = nullptr;
  result->right = nullptr;
  result->length = 0;
}

namespace {

char** copy_stage_array(const std::vector<std::string>& stages) {
  if (stages.empty()) return nullptr;
  std::unique_ptr<char*[]> out(new char*[stages.size()]);
  for (size_t i = 0; i < stages.size(); ++i) {
    out[i] = nullptr;
  }
  for (size_t i = 0; i < stages.size(); ++i) {
    out[i] = copy_string(stages[i]);
  }
  return out.release();
}

}  // namespace

SonareError sonare_mastering_chain(const float* samples, size_t length, int sample_rate,
                                   const SonareMasteringParam* params, size_t param_count,
                                   SonareMasteringChainResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->samples = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->stages = nullptr;
  out->stages_count = 0;

  SONARE_C_TRY
  auto cpp_params = to_params(params, param_count);
  auto result = sonare::mastering::api::run_chain_mono_params(cpp_params.data(), cpp_params.size(),
                                                              samples, length, sample_rate);

  out->length = result.samples.size();
  out->sample_rate = result.sample_rate;
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;

  if (out->length > 0) {
    std::unique_ptr<float[]> processed(new float[out->length]);
    std::memcpy(processed.get(), result.samples.data(), out->length * sizeof(float));
    out->samples = release_array(processed);
  }
  out->stages = copy_stage_array(result.stages);
  out->stages_count = result.stages.size();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_chain_stereo(const float* left, const float* right, size_t length,
                                          int sample_rate, const SonareMasteringParam* params,
                                          size_t param_count,
                                          SonareMasteringChainStereoResult* out) {
  if (!out || !left || !right || sample_rate <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (length == 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->left = nullptr;
  out->right = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->stages = nullptr;
  out->stages_count = 0;

  SONARE_C_TRY
  auto cpp_params = to_params(params, param_count);
  auto result = sonare::mastering::api::run_chain_stereo_params(
      cpp_params.data(), cpp_params.size(), left, right, length, sample_rate);

  out->length = result.left.size();
  out->sample_rate = result.sample_rate;
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;

  if (out->length > 0) {
    std::unique_ptr<float[]> left_out(new float[out->length]);
    std::unique_ptr<float[]> right_out(new float[out->length]);
    std::memcpy(left_out.get(), result.left.data(), out->length * sizeof(float));
    std::memcpy(right_out.get(), result.right.data(), out->length * sizeof(float));
    out->left = release_array(left_out);
    out->right = release_array(right_out);
  }
  out->stages = copy_stage_array(result.stages);
  out->stages_count = result.stages.size();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_chain_with_progress(const float* samples, size_t length,
                                                 int sample_rate,
                                                 const SonareMasteringParam* params,
                                                 size_t param_count,
                                                 SonareMasteringProgressCallback callback,
                                                 void* user_data, SonareMasteringChainResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->samples = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->stages = nullptr;
  out->stages_count = 0;

  SONARE_C_TRY
  auto cpp_params = to_params(params, param_count);
  auto config =
      sonare::mastering::api::parse_chain_config_params(cpp_params.data(), cpp_params.size());
  sonare::mastering::api::MasteringChain chain(std::move(config));
  if (callback) {
    chain.set_progress_callback([callback, user_data](float progress, const char* stage) {
      callback(progress, stage, user_data);
    });
  }
  auto result = chain.process_mono(samples, length, sample_rate);

  out->length = result.samples.size();
  out->sample_rate = result.sample_rate;
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;

  if (out->length > 0) {
    std::unique_ptr<float[]> processed(new float[out->length]);
    std::memcpy(processed.get(), result.samples.data(), out->length * sizeof(float));
    out->samples = release_array(processed);
  }
  out->stages = copy_stage_array(result.stages);
  out->stages_count = result.stages.size();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mastering_chain_stereo_with_progress(const float* left, const float* right,
                                                        size_t length, int sample_rate,
                                                        const SonareMasteringParam* params,
                                                        size_t param_count,
                                                        SonareMasteringProgressCallback callback,
                                                        void* user_data,
                                                        SonareMasteringChainStereoResult* out) {
  if (!out || !left || !right || sample_rate <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!params && param_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (length == 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->left = nullptr;
  out->right = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->stages = nullptr;
  out->stages_count = 0;

  SONARE_C_TRY
  auto cpp_params = to_params(params, param_count);
  auto config =
      sonare::mastering::api::parse_chain_config_params(cpp_params.data(), cpp_params.size());
  sonare::mastering::api::MasteringChain chain(std::move(config));
  if (callback) {
    chain.set_progress_callback([callback, user_data](float progress, const char* stage) {
      callback(progress, stage, user_data);
    });
  }
  auto result = chain.process_stereo(left, right, length, sample_rate);

  out->length = result.left.size();
  out->sample_rate = result.sample_rate;
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;

  if (out->length > 0) {
    std::unique_ptr<float[]> left_out(new float[out->length]);
    std::unique_ptr<float[]> right_out(new float[out->length]);
    std::memcpy(left_out.get(), result.left.data(), out->length * sizeof(float));
    std::memcpy(right_out.get(), result.right.data(), out->length * sizeof(float));
    out->left = release_array(left_out);
    out->right = release_array(right_out);
  }
  out->stages = copy_stage_array(result.stages);
  out->stages_count = result.stages.size();
  return SONARE_OK;
  SONARE_C_CATCH
}

void sonare_free_mastering_chain_result(SonareMasteringChainResult* result) {
  if (!result) return;
  delete[] result->samples;
  result->samples = nullptr;
  result->length = 0;
  if (result->stages) {
    for (size_t i = 0; i < result->stages_count; ++i) {
      delete[] result->stages[i];
    }
    delete[] result->stages;
  }
  result->stages = nullptr;
  result->stages_count = 0;
}

void sonare_free_mastering_chain_stereo_result(SonareMasteringChainStereoResult* result) {
  if (!result) return;
  delete[] result->left;
  delete[] result->right;
  result->left = nullptr;
  result->right = nullptr;
  result->length = 0;
  if (result->stages) {
    for (size_t i = 0; i < result->stages_count; ++i) {
      delete[] result->stages[i];
    }
    delete[] result->stages;
  }
  result->stages = nullptr;
  result->stages_count = 0;
}

// ============================================================================
// Built-in mastering presets
// ============================================================================

namespace {

void fill_mono_chain_result(const sonare::mastering::api::MonoChainResult& result,
                            SonareMasteringChainResult* out) {
  out->length = result.samples.size();
  out->sample_rate = result.sample_rate;
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;

  if (out->length > 0) {
    std::unique_ptr<float[]> processed(new float[out->length]);
    std::memcpy(processed.get(), result.samples.data(), out->length * sizeof(float));
    out->samples = release_array(processed);
  }
  out->stages = copy_stage_array(result.stages);
  out->stages_count = result.stages.size();
}

void fill_stereo_chain_result(const sonare::mastering::api::StereoChainResult& result,
                              SonareMasteringChainStereoResult* out) {
  out->length = result.left.size();
  out->sample_rate = result.sample_rate;
  out->input_lufs = result.input_lufs;
  out->output_lufs = result.output_lufs;
  out->applied_gain_db = result.applied_gain_db;

  if (out->length > 0) {
    std::unique_ptr<float[]> left_out(new float[out->length]);
    std::unique_ptr<float[]> right_out(new float[out->length]);
    std::memcpy(left_out.get(), result.left.data(), out->length * sizeof(float));
    std::memcpy(right_out.get(), result.right.data(), out->length * sizeof(float));
    out->left = release_array(left_out);
    out->right = release_array(right_out);
  }
  out->stages = copy_stage_array(result.stages);
  out->stages_count = result.stages.size();
}

}  // namespace

const char* sonare_mastering_preset_names(void) {
  static std::string names;
  if (names.empty()) {
    std::ostringstream stream;
    auto presets = sonare::mastering::api::preset_names();
    for (size_t index = 0; index < presets.size(); ++index) {
      if (index > 0) stream << '\n';
      stream << presets[index];
    }
    names = stream.str();
  }
  return names.c_str();
}

SonareError sonare_master_audio(const char* preset_name, const float* samples, size_t length,
                                int sample_rate, const SonareMasteringParam* overrides,
                                size_t override_count, SonareMasteringChainResult* out) {
  if (!out || !preset_name) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!overrides && override_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->samples = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->stages = nullptr;
  out->stages_count = 0;

  SONARE_C_TRY
  sonare::mastering::api::Preset preset;
  try {
    preset = sonare::mastering::api::preset_from_string(preset_name);
  } catch (const std::invalid_argument&) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  auto cpp_overrides = to_params(overrides, override_count);
  auto result = sonare::mastering::api::master_audio_mono(
      preset, samples, length, sample_rate, cpp_overrides.data(), cpp_overrides.size());
  fill_mono_chain_result(result, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_master_audio_stereo(const char* preset_name, const float* left,
                                       const float* right, size_t length, int sample_rate,
                                       const SonareMasteringParam* overrides, size_t override_count,
                                       SonareMasteringChainStereoResult* out) {
  if (!out || !preset_name || !left || !right || sample_rate <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!overrides && override_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (length == 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->left = nullptr;
  out->right = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->stages = nullptr;
  out->stages_count = 0;

  SONARE_C_TRY
  sonare::mastering::api::Preset preset;
  try {
    preset = sonare::mastering::api::preset_from_string(preset_name);
  } catch (const std::invalid_argument&) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  auto cpp_overrides = to_params(overrides, override_count);
  auto result = sonare::mastering::api::master_audio_stereo(
      preset, left, right, length, sample_rate, cpp_overrides.data(), cpp_overrides.size());
  fill_stereo_chain_result(result, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_master_audio_with_progress(const char* preset_name, const float* samples,
                                              size_t length, int sample_rate,
                                              const SonareMasteringParam* overrides,
                                              size_t override_count,
                                              SonareMasteringProgressCallback callback,
                                              void* user_data, SonareMasteringChainResult* out) {
  if (!out || !preset_name) return SONARE_ERROR_INVALID_PARAMETER;
  SonareError err = validate_audio_params(samples, length, sample_rate);
  if (err != SONARE_OK) return err;
  if (!overrides && override_count > 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->samples = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->stages = nullptr;
  out->stages_count = 0;

  SONARE_C_TRY
  sonare::mastering::api::Preset preset;
  try {
    preset = sonare::mastering::api::preset_from_string(preset_name);
  } catch (const std::invalid_argument&) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  auto config = sonare::mastering::api::preset_config(preset);
  auto cpp_overrides = to_params(overrides, override_count);
  if (!cpp_overrides.empty()) {
    sonare::mastering::api::apply_chain_config_overrides(config, cpp_overrides.data(),
                                                         cpp_overrides.size());
  }
  sonare::mastering::api::MasteringChain chain(std::move(config));
  if (callback) {
    chain.set_progress_callback([callback, user_data](float progress, const char* stage) {
      callback(progress, stage, user_data);
    });
  }
  auto result = chain.process_mono(samples, length, sample_rate);
  fill_mono_chain_result(result, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_master_audio_stereo_with_progress(
    const char* preset_name, const float* left, const float* right, size_t length, int sample_rate,
    const SonareMasteringParam* overrides, size_t override_count,
    SonareMasteringProgressCallback callback, void* user_data,
    SonareMasteringChainStereoResult* out) {
  if (!out || !preset_name || !left || !right || sample_rate <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!overrides && override_count > 0) return SONARE_ERROR_INVALID_PARAMETER;
  if (length == 0) return SONARE_ERROR_INVALID_PARAMETER;

  out->left = nullptr;
  out->right = nullptr;
  out->length = 0;
  out->sample_rate = sample_rate;
  out->input_lufs = 0.0f;
  out->output_lufs = 0.0f;
  out->applied_gain_db = 0.0f;
  out->stages = nullptr;
  out->stages_count = 0;

  SONARE_C_TRY
  sonare::mastering::api::Preset preset;
  try {
    preset = sonare::mastering::api::preset_from_string(preset_name);
  } catch (const std::invalid_argument&) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  auto config = sonare::mastering::api::preset_config(preset);
  auto cpp_overrides = to_params(overrides, override_count);
  if (!cpp_overrides.empty()) {
    sonare::mastering::api::apply_chain_config_overrides(config, cpp_overrides.data(),
                                                         cpp_overrides.size());
  }
  sonare::mastering::api::MasteringChain chain(std::move(config));
  if (callback) {
    chain.set_progress_callback([callback, user_data](float progress, const char* stage) {
      callback(progress, stage, user_data);
    });
  }
  auto result = chain.process_stereo(left, right, length, sample_rate);
  fill_stereo_chain_result(result, out);
  return SONARE_OK;
  SONARE_C_CATCH
}

// ============================================================================
// Streaming mastering chain
// ============================================================================

struct SonareStreamingMasteringChain {
  std::unique_ptr<sonare::mastering::api::StreamingMasteringChain> chain;
};

SonareStreamingMasteringChain* sonare_streaming_mastering_chain_create(
    const SonareMasteringParam* params, size_t param_count) {
  if (!params && param_count > 0) return nullptr;
  try {
    auto cpp_params = to_params(params, param_count);
    auto config =
        sonare::mastering::api::parse_chain_config_params(cpp_params.data(), cpp_params.size());
    auto chain =
        std::make_unique<sonare::mastering::api::StreamingMasteringChain>(std::move(config));
    auto* handle = new SonareStreamingMasteringChain;
    handle->chain = std::move(chain);
    return handle;
  } catch (const std::exception& e) {
    sonare_c_detail::set_last_error(e.what());
    return nullptr;
  } catch (...) {
    sonare_c_detail::set_last_error("Unknown C++ exception (non-std::exception type)");
    return nullptr;
  }
}

SonareError sonare_streaming_mastering_chain_prepare(SonareStreamingMasteringChain* handle,
                                                     int sample_rate, int max_block_size,
                                                     int num_channels) {
  if (!handle || !handle->chain) return SONARE_ERROR_INVALID_PARAMETER;
  if (sample_rate <= 0 || max_block_size <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  handle->chain->prepare(static_cast<double>(sample_rate), max_block_size, num_channels);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_streaming_mastering_chain_process_mono(SonareStreamingMasteringChain* handle,
                                                          float* samples, size_t num_samples) {
  if (!handle || !handle->chain || (!samples && num_samples > 0)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (num_samples == 0) return SONARE_OK;
  SONARE_C_TRY
  float* channels[] = {samples};
  handle->chain->process_block(channels, 1, static_cast<int>(num_samples));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_streaming_mastering_chain_process_stereo(SonareStreamingMasteringChain* handle,
                                                            float* left, float* right,
                                                            size_t num_samples) {
  if (!handle || !handle->chain || (!left && num_samples > 0) || (!right && num_samples > 0)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (num_samples == 0) return SONARE_OK;
  SONARE_C_TRY
  float* channels[] = {left, right};
  handle->chain->process_block(channels, 2, static_cast<int>(num_samples));
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_streaming_mastering_chain_reset(SonareStreamingMasteringChain* handle) {
  if (!handle || !handle->chain) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  handle->chain->reset();
  return SONARE_OK;
  SONARE_C_CATCH
}

int sonare_streaming_mastering_chain_latency_samples(const SonareStreamingMasteringChain* handle) {
  if (!handle || !handle->chain) return 0;
  return handle->chain->latency_samples();
}

void sonare_streaming_mastering_chain_destroy(SonareStreamingMasteringChain* handle) {
  delete handle;
}
