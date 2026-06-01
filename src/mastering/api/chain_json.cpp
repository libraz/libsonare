/// @file chain_json.cpp
/// @brief JSON serialization for MasteringChainConfig.

#include <string>
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

sonare::util::json::Object build_chain_params(const MasteringChainConfig& cfg) {
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

class JsonParamParser {
 public:
  explicit JsonParamParser(const std::string& text) : text_(text) {}

  std::vector<Param> parse() {
    try {
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
      if (!version->is_number() || static_cast<int>(version->as_number()) != 1) {
        throw SonareException(ErrorCode::InvalidParameter, "unsupported chain config JSON version");
      }
      if (!params_value->is_object())
        throw SonareException(ErrorCode::InvalidParameter, "params must be a JSON object");
      std::vector<Param> params;
      params.reserve(params_value->as_object().size());
      for (const auto& [key, value] : params_value->as_object()) {
        if (value.is_bool()) {
          params.push_back(Param{key, value.as_bool() ? 1.0 : 0.0});
        } else if (value.is_number()) {
          params.push_back(Param{key, value.as_number()});
        } else {
          throw SonareException(ErrorCode::InvalidParameter,
                                "params values must be numbers or booleans");
        }
      }
      return params;
    } catch (const sonare::util::json::JsonError& e) {
      throw SonareException(ErrorCode::InvalidParameter, e.what());
    }
  }

 private:
  const std::string& text_;
};

}  // namespace

std::string chain_config_to_json(const MasteringChainConfig& config) {
  sonare::util::json::Object root;
  root.emplace("version", JsonValue(1));
  root.emplace("params", JsonValue(build_chain_params(config)));
  return sonare::util::json::dump(JsonValue(std::move(root)));
}

MasteringChainConfig chain_config_from_json(const std::string& json) {
  JsonParamParser parser(json);
  const auto params = parser.parse();
  return parse_chain_config_params(params.data(), params.size());
}

}  // namespace sonare::mastering::api
