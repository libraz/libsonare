/// @file chain_json.cpp
/// @brief JSON serialization for MasteringChainConfig.

#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/param_field_tables.h"
#include "util/exception.h"
#include "util/json.h"
#include "util/json_schema.h"

namespace sonare::mastering::api {
namespace {

using JsonValue = sonare::util::json::Value;

// ---------------------------------------------------------------------------
// Tree builders. Mirror the previous flatten_chain_config() output exactly
// (same key strings, same numeric values) but emit JSON via util::json::dump
// so number formatting is locale-independent and uses max_digits10 precision
// to survive a dump -> parse round-trip without coefficient drift.
// ---------------------------------------------------------------------------

void add_field(sonare::util::json::Object& params, const char* key, bool value) {
  params.emplace(key, JsonValue(value));
}

void add_field(sonare::util::json::Object& params, const char* key, double value) {
  params.emplace(key, JsonValue(value));
}

// Integer config fields (int, std::size_t, etc.) must route to the `double`
// overload — without this template they would implicitly convert to `bool`.
template <typename Int,
          typename = std::enable_if_t<std::is_integral_v<Int> && !std::is_same_v<Int, bool>>>
void add_field(sonare::util::json::Object& params, const char* key, Int value) {
  params.emplace(key, JsonValue(static_cast<double>(value)));
}

// Enum config fields serialize as their underlying integer value (the parser
// restores them via static_cast). Keeps the dump -> parse round-trip lossless.
template <typename Enum, typename = std::enable_if_t<std::is_enum_v<Enum>>, typename = void>
void add_field(sonare::util::json::Object& params, const char* key, Enum value) {
  params.emplace(key, JsonValue(static_cast<double>(static_cast<int>(value))));
}

sonare::util::json::Object build_multiband_params(const MasteringChainConfig& cfg) {
  using sonare::mastering::dynamics::CompressorConfig;

  sonare::util::json::Object multiband;
  add_field(multiband, "enabled", cfg.dynamics.multiband_comp.enabled);

  sonare::util::json::Object crossover;
  sonare::util::json::Array cutoffs;
  cutoffs.reserve(cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz.size());
  for (const float cutoff : cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz) {
    cutoffs.emplace_back(static_cast<double>(cutoff));
  }
  crossover.emplace("cutoffsHz", JsonValue(std::move(cutoffs)));
  add_field(crossover, "slope", cfg.dynamics.multiband_comp.config.crossover.slope);
  add_field(crossover, "mode", cfg.dynamics.multiband_comp.config.crossover.mode);
  add_field(crossover, "firKernelSize",
            cfg.dynamics.multiband_comp.config.crossover.fir_kernel_size);
  multiband.emplace("crossover", JsonValue(std::move(crossover)));

  sonare::util::json::Array bands;
  bands.reserve(cfg.dynamics.multiband_comp.config.bands.size());
  for (const CompressorConfig& band_config : cfg.dynamics.multiband_comp.config.bands) {
    sonare::util::json::Object band;
#define X(key, member) add_field(band, key, band_config.member);
    SONARE_FIELDS_COMPRESSOR(X)
#undef X
    bands.emplace_back(JsonValue(std::move(band)));
  }
  multiband.emplace("bands", JsonValue(std::move(bands)));
  return multiband;
}

sonare::util::json::Object build_chain_params(const MasteringChainConfig& cfg,
                                              bool include_flat_multiband) {
  sonare::util::json::Object params;

  add_field(params, "repair.declick.enabled", cfg.repair.declick.enabled);
  add_field(params, "repair.declick.threshold", cfg.repair.declick.config.threshold);
  add_field(params, "repair.declick.neighborRatio", cfg.repair.declick.config.neighbor_ratio);
  add_field(params, "repair.declick.maxClickSamples", cfg.repair.declick.config.max_click_samples);
  add_field(params, "repair.declick.lpcOrder", cfg.repair.declick.config.lpc_order);
  add_field(params, "repair.declick.residualRatio", cfg.repair.declick.config.residual_ratio);

  add_field(params, "repair.declip.enabled", cfg.repair.declip.enabled);
  add_field(params, "repair.declip.clipThreshold", cfg.repair.declip.config.clip_threshold);
  add_field(params, "repair.declip.lpcOrder", cfg.repair.declip.config.lpc_order);
  add_field(params, "repair.declip.iterations", cfg.repair.declip.config.iterations);
  add_field(params, "repair.declip.lpcBlend", cfg.repair.declip.config.lpc_blend);

  add_field(params, "repair.decrackle.enabled", cfg.repair.decrackle.enabled);
  add_field(params, "repair.decrackle.threshold", cfg.repair.decrackle.config.threshold);
  // Decrackle "mode" is an enum serialized as 0.0 / 1.0 (the parser side
  // restores it via the numeric Param schema), so emit a number — not a bool.
  add_field(params, "repair.decrackle.mode",
            cfg.repair.decrackle.config.mode == mastering::repair::DecrackleMode::WaveletShrinkage
                ? 1.0
                : 0.0);
  add_field(params, "repair.decrackle.levels", cfg.repair.decrackle.config.levels);

  add_field(params, "repair.dehum.enabled", cfg.repair.dehum.enabled);
  add_field(params, "repair.dehum.fundamentalHz", cfg.repair.dehum.config.fundamental_hz);
  add_field(params, "repair.dehum.harmonics", cfg.repair.dehum.config.harmonics);
  add_field(params, "repair.dehum.q", cfg.repair.dehum.config.q);
  add_field(params, "repair.dehum.adaptive", cfg.repair.dehum.config.adaptive);
  add_field(params, "repair.dehum.searchRangeHz", cfg.repair.dehum.config.search_range_hz);
  add_field(params, "repair.dehum.adaptation", cfg.repair.dehum.config.adaptation);
  add_field(params, "repair.dehum.frameSize", cfg.repair.dehum.config.frame_size);
  add_field(params, "repair.dehum.pllBandwidth", cfg.repair.dehum.config.pll_bandwidth);

  add_field(params, "repair.dereverb.enabled", cfg.repair.dereverb.enabled);
  add_field(params, "repair.dereverb.threshold", cfg.repair.dereverb.config.threshold);
  add_field(params, "repair.dereverb.attenuation", cfg.repair.dereverb.config.attenuation);
  add_field(params, "repair.dereverb.nFft", cfg.repair.dereverb.config.n_fft);
  add_field(params, "repair.dereverb.hopLength", cfg.repair.dereverb.config.hop_length);
  add_field(params, "repair.dereverb.t60Sec", cfg.repair.dereverb.config.t60_sec);
  add_field(params, "repair.dereverb.lateDelayMs", cfg.repair.dereverb.config.late_delay_ms);
  add_field(params, "repair.dereverb.overSubtraction", cfg.repair.dereverb.config.over_subtraction);
  add_field(params, "repair.dereverb.spectralFloor", cfg.repair.dereverb.config.spectral_floor);
  add_field(params, "repair.dereverb.wpeEnabled", cfg.repair.dereverb.config.wpe_enabled);
  add_field(params, "repair.dereverb.wpeIterations", cfg.repair.dereverb.config.wpe_iterations);
  add_field(params, "repair.dereverb.wpeTaps", cfg.repair.dereverb.config.wpe_taps);
  add_field(params, "repair.dereverb.wpeStrength", cfg.repair.dereverb.config.wpe_strength);

  add_field(params, "repair.denoise.enabled", cfg.repair.denoise.enabled);
  // `mode` and `noiseEstimator` are enums serialized as their integer values
  // (the parser side restores them via static_cast in chain_params.cpp), so emit
  // a number — not a bool — to keep the dump -> parse round-trip lossless.
  add_field(params, "repair.denoise.mode", static_cast<int>(cfg.repair.denoise.config.mode));
  add_field(params, "repair.denoise.noiseEstimator",
            static_cast<int>(cfg.repair.denoise.config.noise_estimator));
  add_field(params, "repair.denoise.nFft", cfg.repair.denoise.config.n_fft);
  add_field(params, "repair.denoise.hopLength", cfg.repair.denoise.config.hop_length);
  add_field(params, "repair.denoise.ddAlpha", cfg.repair.denoise.config.dd_alpha);
  add_field(params, "repair.denoise.gainFloor", cfg.repair.denoise.config.gain_floor);
  add_field(params, "repair.denoise.overSubtraction", cfg.repair.denoise.config.over_subtraction);
  add_field(params, "repair.denoise.spectralFloor", cfg.repair.denoise.config.spectral_floor);
  add_field(params, "repair.denoise.noiseEstimationQuantile",
            cfg.repair.denoise.config.noise_estimation_quantile);
  add_field(params, "repair.denoise.speechPresenceGain",
            cfg.repair.denoise.config.speech_presence_gain);
  add_field(params, "repair.denoise.gainSmoothing", cfg.repair.denoise.config.gain_smoothing);

  add_field(params, "eq.tilt.enabled", cfg.eq.tilt.enabled);
#define X(key, member) add_field(params, "eq.tilt." key, cfg.eq.tilt.member);
  SONARE_FIELDS_EQ_TILT(X)
#undef X

  add_field(params, "dynamics.deesser.enabled", cfg.dynamics.deesser.enabled);
#define X(key, member) \
  add_field(params, "dynamics.deesser." key, cfg.dynamics.deesser.config.member);
  SONARE_FIELDS_DEESSER(X)
#undef X

  add_field(params, "dynamics.transientShaper.enabled", cfg.dynamics.transient_shaper.enabled);
#define X(key, member) \
  add_field(params, "dynamics.transientShaper." key, cfg.dynamics.transient_shaper.config.member);
  SONARE_FIELDS_TRANSIENT_SHAPER(X)
#undef X

  // `detector` is an enum serialized as its integer value via the enum add_field
  // overload; the parser restores it with static_cast in chain_params.cpp.
  add_field(params, "dynamics.compressor.enabled", cfg.dynamics.compressor.enabled);
#define X(key, member) \
  add_field(params, "dynamics.compressor." key, cfg.dynamics.compressor.config.member);
  SONARE_FIELDS_COMPRESSOR(X)
#undef X

  if (include_flat_multiband) {
    add_field(params, "dynamics.multibandComp.enabled", cfg.dynamics.multiband_comp.enabled);
    if (cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz.size() >= 2) {
      add_field(params, "dynamics.multibandComp.lowCutoffHz",
                cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz[0]);
      add_field(params, "dynamics.multibandComp.highCutoffHz",
                cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz[1]);
    }
    if (cfg.dynamics.multiband_comp.config.bands.size() >= 3) {
      const auto& low = cfg.dynamics.multiband_comp.config.bands[0];
      const auto& mid = cfg.dynamics.multiband_comp.config.bands[1];
      const auto& high = cfg.dynamics.multiband_comp.config.bands[2];
      add_field(params, "dynamics.multibandComp.lowThresholdDb", low.threshold_db);
      add_field(params, "dynamics.multibandComp.lowRatio", low.ratio);
      add_field(params, "dynamics.multibandComp.lowAttackMs", low.attack_ms);
      add_field(params, "dynamics.multibandComp.lowReleaseMs", low.release_ms);
      add_field(params, "dynamics.multibandComp.midThresholdDb", mid.threshold_db);
      add_field(params, "dynamics.multibandComp.midRatio", mid.ratio);
      add_field(params, "dynamics.multibandComp.midAttackMs", mid.attack_ms);
      add_field(params, "dynamics.multibandComp.midReleaseMs", mid.release_ms);
      add_field(params, "dynamics.multibandComp.highThresholdDb", high.threshold_db);
      add_field(params, "dynamics.multibandComp.highRatio", high.ratio);
      add_field(params, "dynamics.multibandComp.highAttackMs", high.attack_ms);
      add_field(params, "dynamics.multibandComp.highReleaseMs", high.release_ms);
    }
  }

  add_field(params, "saturation.tape.enabled", cfg.saturation.tape.enabled);
#define X(key, member) add_field(params, "saturation.tape." key, cfg.saturation.tape.config.member);
  SONARE_FIELDS_TAPE(X)
#undef X

  add_field(params, "saturation.exciter.enabled", cfg.saturation.exciter.enabled);
#define X(key, member) \
  add_field(params, "saturation.exciter." key, cfg.saturation.exciter.config.member);
  SONARE_FIELDS_EXCITER(X)
#undef X

  add_field(params, "spectral.airBand.enabled", cfg.spectral.air_band.enabled);
#define X(key, member) \
  add_field(params, "spectral.airBand." key, cfg.spectral.air_band.config.member);
  SONARE_FIELDS_AIR_BAND(X)
#undef X

  add_field(params, "stereo.imager.enabled", cfg.stereo.imager.enabled);
#define X(key, member) add_field(params, "stereo.imager." key, cfg.stereo.imager.config.member);
  SONARE_FIELDS_IMAGER(X)
#undef X

  add_field(params, "stereo.monoMaker.enabled", cfg.stereo.mono_maker.enabled);
#define X(key, member) \
  add_field(params, "stereo.monoMaker." key, cfg.stereo.mono_maker.config.member);
  SONARE_FIELDS_MONO_MAKER(X)
#undef X

  add_field(params, "maximizer.truePeakLimiter.enabled", cfg.maximizer.true_peak_limiter.enabled);
#define X(key, member)                                \
  add_field(params, "maximizer.truePeakLimiter." key, \
            cfg.maximizer.true_peak_limiter.config.member);
  SONARE_FIELDS_TRUE_PEAK_LIMITER(X)
#undef X

  add_field(params, "loudness.enabled", cfg.loudness.enabled);
#define X(key, member) add_field(params, "loudness." key, cfg.loudness.member);
  SONARE_FIELDS_LOUDNESS(X)
#undef X

  return params;
}

// A chain document is tiny in normal use (even a 64-band v2 document is only
// a few dozen KiB), so cap the encoded input before building the generic JSON
// tree.  This keeps the existing strict parser/API intact while ensuring a
// caller cannot make this convenience API retain an arbitrarily large string.
constexpr std::size_t kMaxChainJsonBytes = 1u * 1024u * 1024u;
constexpr std::size_t kMaxMultibandBands = 64u;
constexpr int kMaxFirKernelSize = 65'535;

[[noreturn]] void throw_invalid_json(const std::string& message) {
  throw SonareException(ErrorCode::InvalidParameter, message);
}

void validate_multiband_for_json(const MasteringChainConfig& cfg) {
  const auto& multiband = cfg.dynamics.multiband_comp.config;
  const auto& crossover = multiband.crossover;
  const auto& bands = multiband.bands;

  if (bands.empty() || bands.size() > kMaxMultibandBands) {
    throw_invalid_json("multiband bands count must be between 1 and 64");
  }
  if (crossover.cutoffs_hz.size() > kMaxMultibandBands - 1u) {
    throw_invalid_json("crossover cutoff count exceeds the 64-band limit");
  }
  if (bands.size() != crossover.cutoffs_hz.size() + 1u) {
    throw_invalid_json("multiband band count must equal cutoff count plus one");
  }

  for (std::size_t index = 0; index < crossover.cutoffs_hz.size(); ++index) {
    const float cutoff = crossover.cutoffs_hz[index];
    if (!std::isfinite(cutoff) || !(cutoff > 0.0f)) {
      throw_invalid_json("crossover cutoffs must be finite and positive");
    }
    if (index > 0 && !(cutoff > crossover.cutoffs_hz[index - 1])) {
      throw_invalid_json("crossover cutoffs must be strictly ascending");
    }
  }

  const int slope = static_cast<int>(crossover.slope);
  if (slope < 0 || slope > 2) {
    throw_invalid_json("crossover slope is out of range");
  }
  const int mode = static_cast<int>(crossover.mode);
  if (mode < 0 || mode > 3) {
    throw_invalid_json("crossover mode is out of range");
  }
  if (crossover.fir_kernel_size < 1 || crossover.fir_kernel_size > kMaxFirKernelSize) {
    throw_invalid_json("crossover FIR kernel size is out of range");
  }
  if (mode == 3 && (crossover.fir_kernel_size < 3 || crossover.fir_kernel_size % 2 == 0)) {
    throw_invalid_json("linear-phase crossover FIR kernel size must be odd and >= 3");
  }

  for (const auto& band : bands) {
    if (!std::isfinite(band.threshold_db) || !std::isfinite(band.ratio) ||
        !std::isfinite(band.attack_ms) || !std::isfinite(band.release_ms) ||
        !std::isfinite(band.knee_db) || !std::isfinite(band.makeup_gain_db) ||
        !std::isfinite(band.sidechain_hpf_hz) || !std::isfinite(band.pdr_time_ms) ||
        !std::isfinite(band.pdr_release_scale)) {
      throw_invalid_json("multiband compressor fields must be finite");
    }
    if (!(band.ratio >= 1.0f) || band.attack_ms < 0.0f || band.release_ms < 0.0f ||
        band.knee_db < 0.0f || !(band.sidechain_hpf_hz > 0.0f) || band.pdr_time_ms < 0.0f ||
        band.pdr_release_scale < 1.0f) {
      throw_invalid_json("multiband compressor field is out of range");
    }
    const int detector = static_cast<int>(band.detector);
    if (detector < 0 || detector > 2) {
      throw_invalid_json("multiband compressor detector is out of range");
    }
  }
}

const sonare::util::json::Object& require_object(const JsonValue& value, std::string_view path) {
  if (!value.is_object()) {
    throw_invalid_json(std::string(path) + " must be an object");
  }
  return value.as_object();
}

void require_allowed_keys(const JsonValue& object, std::initializer_list<std::string_view> keys,
                          std::string_view path) {
  std::string error;
  if (!sonare::util::json::schema::has_allowed_keys(object, keys, path, &error)) {
    throw_invalid_json(error);
  }
}

const JsonValue& require_value(const sonare::util::json::Object& object, const char* key,
                               std::string_view path) {
  const auto it = object.find(key);
  if (it == object.end()) {
    throw_invalid_json("missing field: " + std::string(path) + "." + key);
  }
  return it->second;
}

bool require_bool(const sonare::util::json::Object& object, const char* key,
                  std::string_view path) {
  const JsonValue& value = require_value(object, key, path);
  if (!value.is_bool()) {
    throw_invalid_json("field must be a boolean: " + std::string(path) + "." + key);
  }
  return value.as_bool();
}

double require_number(const sonare::util::json::Object& object, const char* key,
                      std::string_view path, double minimum = -std::numeric_limits<double>::max(),
                      double maximum = std::numeric_limits<double>::max(), bool integer = false) {
  const JsonValue& value = require_value(object, key, path);
  if (!value.is_number()) {
    throw_invalid_json("field must be numeric: " + std::string(path) + "." + key);
  }
  const double number = value.as_number();
  if (!std::isfinite(number)) {
    throw_invalid_json("field must be finite: " + std::string(path) + "." + key);
  }
  if (integer && std::floor(number) != number) {
    throw_invalid_json("field must be an integer: " + std::string(path) + "." + key);
  }
  if (number < minimum || number > maximum) {
    throw_invalid_json("field is out of range: " + std::string(path) + "." + key);
  }
  return number;
}

float require_float(const sonare::util::json::Object& object, const char* key,
                    std::string_view path, double minimum = -std::numeric_limits<float>::max(),
                    double maximum = std::numeric_limits<float>::max()) {
  return static_cast<float>(require_number(object, key, path, minimum, maximum));
}

int require_int(const sonare::util::json::Object& object, const char* key, std::string_view path,
                int minimum, int maximum) {
  return static_cast<int>(require_number(object, key, path, static_cast<double>(minimum),
                                         static_cast<double>(maximum), true));
}

std::vector<float> parse_cutoffs(const JsonValue& value, std::string_view path) {
  if (!value.is_array()) {
    throw_invalid_json("field must be an array: " + std::string(path));
  }
  const auto& values = value.as_array();
  if (values.size() > kMaxMultibandBands - 1u) {
    throw_invalid_json("crossover cutoff count exceeds the 64-band limit");
  }

  std::vector<float> cutoffs;
  cutoffs.reserve(values.size());
  for (std::size_t index = 0; index < values.size(); ++index) {
    const JsonValue& item = values[index];
    if (!item.is_number()) {
      throw_invalid_json("crossover cutoff must be numeric: " + std::string(path) + "[" +
                         std::to_string(index) + "]");
    }
    const double number = item.as_number();
    if (!std::isfinite(number) || number > static_cast<double>(std::numeric_limits<float>::max()) ||
        !(number > 0.0)) {
      throw_invalid_json("crossover cutoff must be finite and positive: " + std::string(path) +
                         "[" + std::to_string(index) + "]");
    }
    const float cutoff = static_cast<float>(number);
    if (!(cutoff > 0.0f)) {
      throw_invalid_json("crossover cutoff is too small: " + std::string(path) + "[" +
                         std::to_string(index) + "]");
    }
    if (index > 0 && !(cutoff > cutoffs.back())) {
      throw_invalid_json("crossover cutoffs must be strictly ascending");
    }
    cutoffs.push_back(cutoff);
  }
  return cutoffs;
}

MultibandCompStage parse_multiband_config(const JsonValue& value) {
  using sonare::mastering::dynamics::CompressorConfig;
  using sonare::mastering::dynamics::DetectorMode;
  using sonare::mastering::multiband::CrossoverConfig;
  using sonare::mastering::multiband::CrossoverMode;
  using sonare::mastering::multiband::CrossoverSlope;
  using sonare::mastering::multiband::MultibandCompressorConfig;

  constexpr std::string_view path = "$.params.dynamics.multibandComp";
  const auto& object = require_object(value, path);
  require_allowed_keys(value, {"enabled", "crossover", "bands"}, path);

  MultibandCompStage stage;
  stage.enabled = require_bool(object, "enabled", path);

  const JsonValue& crossover_value = require_value(object, "crossover", path);
  const auto& crossover_object =
      require_object(crossover_value, "$.params.dynamics.multibandComp.crossover");
  constexpr std::string_view crossover_path = "$.params.dynamics.multibandComp.crossover";
  require_allowed_keys(crossover_value, {"cutoffsHz", "slope", "mode", "firKernelSize"},
                       crossover_path);

  CrossoverConfig crossover;
  crossover.cutoffs_hz = parse_cutoffs(require_value(crossover_object, "cutoffsHz", crossover_path),
                                       "$.params.dynamics.multibandComp.crossover.cutoffsHz");
  crossover.slope = static_cast<CrossoverSlope>(require_int(
      crossover_object, "slope", crossover_path, 0, static_cast<int>(CrossoverSlope::LR8)));
  crossover.mode =
      static_cast<CrossoverMode>(require_int(crossover_object, "mode", crossover_path, 0,
                                             static_cast<int>(CrossoverMode::FirLinearPhase)));
  crossover.fir_kernel_size =
      require_int(crossover_object, "firKernelSize", crossover_path, 1, kMaxFirKernelSize);
  if (crossover.mode == CrossoverMode::FirLinearPhase &&
      (crossover.fir_kernel_size < 3 || crossover.fir_kernel_size % 2 == 0)) {
    throw_invalid_json("linear-phase crossover FIR kernel size must be odd and >= 3");
  }

  const JsonValue& bands_value = require_value(object, "bands", path);
  if (!bands_value.is_array()) {
    throw_invalid_json("field must be an array: " + std::string(path) + ".bands");
  }
  const auto& band_values = bands_value.as_array();
  if (band_values.empty() || band_values.size() > kMaxMultibandBands) {
    throw_invalid_json("multiband bands count must be between 1 and 64");
  }
  if (band_values.size() != crossover.cutoffs_hz.size() + 1u) {
    throw_invalid_json("multiband band count must equal cutoff count plus one");
  }

  MultibandCompressorConfig config;
  config.crossover = std::move(crossover);
  config.bands.clear();
  config.bands.reserve(band_values.size());

  for (std::size_t index = 0; index < band_values.size(); ++index) {
    const std::string band_path = std::string(path) + ".bands[" + std::to_string(index) + "]";
    const JsonValue& band_value = band_values[index];
    const auto& band_object = require_object(band_value, band_path);
    require_allowed_keys(
        band_value,
        {"thresholdDb", "ratio", "attackMs", "releaseMs", "kneeDb", "makeupGainDb", "autoMakeup",
         "detector", "sidechainHpfEnabled", "sidechainHpfHz", "pdrTimeMs", "pdrReleaseScale"},
        band_path);

    CompressorConfig band;
    band.threshold_db = require_float(band_object, "thresholdDb", band_path);
    band.ratio = require_float(band_object, "ratio", band_path, 1.0);
    band.attack_ms = require_float(band_object, "attackMs", band_path, 0.0);
    band.release_ms = require_float(band_object, "releaseMs", band_path, 0.0);
    band.knee_db = require_float(band_object, "kneeDb", band_path, 0.0);
    band.makeup_gain_db = require_float(band_object, "makeupGainDb", band_path);
    band.auto_makeup = require_bool(band_object, "autoMakeup", band_path);
    band.detector = static_cast<DetectorMode>(
        require_int(band_object, "detector", band_path, 0, static_cast<int>(DetectorMode::LogRms)));
    band.sidechain_hpf_enabled = require_bool(band_object, "sidechainHpfEnabled", band_path);
    band.sidechain_hpf_hz = require_float(band_object, "sidechainHpfHz", band_path, 0.0);
    if (!(band.sidechain_hpf_hz > 0.0f)) {
      throw_invalid_json("sidechain HPF frequency must be positive: " + band_path);
    }
    band.pdr_time_ms = require_float(band_object, "pdrTimeMs", band_path, 0.0);
    band.pdr_release_scale = require_float(band_object, "pdrReleaseScale", band_path, 1.0);
    config.bands.push_back(band);
  }

  stage.config = std::move(config);
  return stage;
}

bool is_v1_multiband_representable(const MasteringChainConfig& cfg) {
  using sonare::mastering::dynamics::CompressorConfig;
  using sonare::mastering::dynamics::DetectorMode;
  using sonare::mastering::multiband::CrossoverConfig;

  const auto& config = cfg.dynamics.multiband_comp.config;
  const CrossoverConfig crossover_defaults;
  if (config.crossover.cutoffs_hz.size() != 2 || config.bands.size() != 3 ||
      config.crossover.slope != crossover_defaults.slope ||
      config.crossover.mode != crossover_defaults.mode ||
      config.crossover.fir_kernel_size != crossover_defaults.fir_kernel_size) {
    return false;
  }

  const CompressorConfig band_defaults;
  for (const auto& band : config.bands) {
    if (band.knee_db != band_defaults.knee_db ||
        band.makeup_gain_db != band_defaults.makeup_gain_db ||
        band.auto_makeup != band_defaults.auto_makeup || band.detector != DetectorMode::Rms ||
        band.sidechain_hpf_enabled != band_defaults.sidechain_hpf_enabled ||
        band.sidechain_hpf_hz != band_defaults.sidechain_hpf_hz ||
        band.pdr_time_ms != band_defaults.pdr_time_ms ||
        band.pdr_release_scale != band_defaults.pdr_release_scale) {
      return false;
    }
  }
  return true;
}

struct ParsedChainJson {
  int version = 0;
  std::vector<Param> flat_params;
  std::optional<MultibandCompStage> multiband;
};

class JsonParamParser {
 public:
  explicit JsonParamParser(const std::string& text) : text_(text) {}

  ParsedChainJson parse() {
    try {
      if (text_.size() > kMaxChainJsonBytes) {
        throw_invalid_json("chain config JSON exceeds resource limit");
      }
      // Strict parse: duplicate top-level keys (e.g. two `"version"` entries)
      // are caller bugs, not "last-write-wins" inputs. Combined with the
      // `has_allowed_keys` allowlist below, the chain config JSON now behaves
      // like an `additionalProperties: false` schema and rejects both unknown
      // and ambiguous fields up front.
      const auto root = sonare::util::json::parse_strict(text_);
      if (!root.is_object())
        throw SonareException(ErrorCode::InvalidParameter, "expected chain config JSON object");
      std::string allowed_keys_error;
      if (!sonare::util::json::schema::has_allowed_keys(root, {"version", "params"}, "$",
                                                        &allowed_keys_error)) {
        throw SonareException(ErrorCode::InvalidParameter, allowed_keys_error);
      }
      const auto* version = root.find("version");
      const auto* params_value = root.find("params");
      if (!version || !params_value) {
        throw SonareException(ErrorCode::InvalidParameter,
                              "chain config JSON requires version and params");
      }
      if (!version->is_number() || !std::isfinite(version->as_number()) ||
          std::floor(version->as_number()) != version->as_number() ||
          (version->as_number() != 1.0 && version->as_number() != 2.0)) {
        throw SonareException(ErrorCode::InvalidParameter, "unsupported chain config JSON version");
      }
      const int version_number = static_cast<int>(version->as_number());
      if (!params_value->is_object())
        throw SonareException(ErrorCode::InvalidParameter, "params must be a JSON object");
      ParsedChainJson parsed;
      parsed.version = version_number;
      parsed.flat_params.reserve(params_value->as_object().size());
      bool has_structured_multiband = false;
      for (const auto& [key, value] : params_value->as_object()) {
        if (version_number == 2 && key == "dynamics.multibandComp") {
          if (has_structured_multiband) {
            throw SonareException(ErrorCode::InvalidParameter,
                                  "duplicate structured multiband configuration");
          }
          parsed.multiband = parse_multiband_config(value);
          has_structured_multiband = true;
          continue;
        }
        if (version_number == 2 && key.rfind("dynamics.multibandComp.", 0) == 0) {
          throw SonareException(ErrorCode::InvalidParameter,
                                "v2 multiband configuration must use structured params");
        }
        if (value.is_bool()) {
          parsed.flat_params.push_back(Param{key, value.as_bool() ? 1.0 : 0.0});
        } else if (value.is_number()) {
          parsed.flat_params.push_back(Param{key, value.as_number()});
        } else {
          throw SonareException(ErrorCode::InvalidParameter,
                                "params values must be numbers or booleans");
        }
      }
      if (version_number == 2 && !has_structured_multiband) {
        throw SonareException(ErrorCode::InvalidParameter,
                              "v2 chain config JSON requires structured multiband params");
      }
      return parsed;
    } catch (const sonare::util::json::JsonError& e) {
      throw SonareException(ErrorCode::InvalidParameter, e.what());
    }
  }

 private:
  const std::string& text_;
};

}  // namespace

std::string chain_config_to_json(const MasteringChainConfig& config) {
  // Validate the complete multiband shape before selecting v1 or v2. This is
  // deliberately done at serialization time: otherwise a config beyond the
  // v2 schema (for example 65 bands or a six-digit FIR kernel) would produce
  // JSON that chain_config_from_json must reject, breaking writer->parser
  // round-trips.
  validate_multiband_for_json(config);
  const bool use_v1 = is_v1_multiband_representable(config);
  sonare::util::json::Object root;
  root.emplace("version", JsonValue(use_v1 ? 1 : 2));
  auto params = build_chain_params(config, use_v1);
  if (!use_v1) {
    params.emplace("dynamics.multibandComp", JsonValue(build_multiband_params(config)));
  }
  root.emplace("params", JsonValue(std::move(params)));
  return sonare::util::json::dump(JsonValue(std::move(root)));
}

MasteringChainConfig chain_config_from_json(const std::string& json) {
  JsonParamParser parser(json);
  ParsedChainJson parsed = parser.parse();
  MasteringChainConfig config =
      parse_chain_config_params(parsed.flat_params.data(), parsed.flat_params.size());
  if (parsed.version == 2) {
    config.dynamics.multiband_comp = *parsed.multiband;
  }
  validate_mastering_chain_config(config);
  return config;
}

}  // namespace sonare::mastering::api
