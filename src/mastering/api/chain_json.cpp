/// @file chain_json.cpp
/// @brief JSON serialization for MasteringChainConfig.

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "mastering/api/chain.h"
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
  add_field(params, "repair.denoise.nFft", cfg.repair.denoise.config.n_fft);
  add_field(params, "repair.denoise.hopLength", cfg.repair.denoise.config.hop_length);
  add_field(params, "repair.denoise.ddAlpha", cfg.repair.denoise.config.dd_alpha);
  add_field(params, "repair.denoise.gainFloor", cfg.repair.denoise.config.gain_floor);

  add_field(params, "eq.tilt.enabled", cfg.eq.tilt.enabled);
  add_field(params, "eq.tilt.tiltDb", cfg.eq.tilt.tilt_db);
  add_field(params, "eq.tilt.pivotHz", cfg.eq.tilt.pivot_hz);

  add_field(params, "dynamics.deesser.enabled", cfg.dynamics.deesser.enabled);
  add_field(params, "dynamics.deesser.frequencyHz", cfg.dynamics.deesser.config.frequency_hz);
  add_field(params, "dynamics.deesser.thresholdDb", cfg.dynamics.deesser.config.threshold_db);
  add_field(params, "dynamics.deesser.ratio", cfg.dynamics.deesser.config.ratio);
  add_field(params, "dynamics.deesser.attackMs", cfg.dynamics.deesser.config.attack_ms);
  add_field(params, "dynamics.deesser.releaseMs", cfg.dynamics.deesser.config.release_ms);
  add_field(params, "dynamics.deesser.rangeDb", cfg.dynamics.deesser.config.range_db);
  add_field(params, "dynamics.deesser.bandpassQ", cfg.dynamics.deesser.config.bandpass_q);

  add_field(params, "dynamics.transientShaper.enabled", cfg.dynamics.transient_shaper.enabled);
  add_field(params, "dynamics.transientShaper.attackGainDb",
            cfg.dynamics.transient_shaper.config.attack_gain_db);
  add_field(params, "dynamics.transientShaper.sustainGainDb",
            cfg.dynamics.transient_shaper.config.sustain_gain_db);
  add_field(params, "dynamics.transientShaper.fastAttackMs",
            cfg.dynamics.transient_shaper.config.fast_attack_ms);
  add_field(params, "dynamics.transientShaper.fastReleaseMs",
            cfg.dynamics.transient_shaper.config.fast_release_ms);
  add_field(params, "dynamics.transientShaper.slowAttackMs",
            cfg.dynamics.transient_shaper.config.slow_attack_ms);
  add_field(params, "dynamics.transientShaper.slowReleaseMs",
            cfg.dynamics.transient_shaper.config.slow_release_ms);
  add_field(params, "dynamics.transientShaper.sensitivity",
            cfg.dynamics.transient_shaper.config.sensitivity);
  add_field(params, "dynamics.transientShaper.maxGainDb",
            cfg.dynamics.transient_shaper.config.max_gain_db);
  add_field(params, "dynamics.transientShaper.gainSmoothingMs",
            cfg.dynamics.transient_shaper.config.gain_smoothing_ms);
  add_field(params, "dynamics.transientShaper.lookaheadMs",
            cfg.dynamics.transient_shaper.config.lookahead_ms);

  add_field(params, "dynamics.compressor.enabled", cfg.dynamics.compressor.enabled);
  add_field(params, "dynamics.compressor.thresholdDb", cfg.dynamics.compressor.config.threshold_db);
  add_field(params, "dynamics.compressor.ratio", cfg.dynamics.compressor.config.ratio);
  add_field(params, "dynamics.compressor.attackMs", cfg.dynamics.compressor.config.attack_ms);
  add_field(params, "dynamics.compressor.releaseMs", cfg.dynamics.compressor.config.release_ms);
  add_field(params, "dynamics.compressor.kneeDb", cfg.dynamics.compressor.config.knee_db);
  add_field(params, "dynamics.compressor.makeupGainDb",
            cfg.dynamics.compressor.config.makeup_gain_db);
  add_field(params, "dynamics.compressor.autoMakeup", cfg.dynamics.compressor.config.auto_makeup);

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
  add_field(params, "saturation.tape.driveDb", cfg.saturation.tape.config.drive_db);
  add_field(params, "saturation.tape.saturation", cfg.saturation.tape.config.saturation);
  add_field(params, "saturation.tape.hysteresis", cfg.saturation.tape.config.hysteresis);
  add_field(params, "saturation.tape.outputGainDb", cfg.saturation.tape.config.output_gain_db);
  add_field(params, "saturation.tape.speedIps", cfg.saturation.tape.config.speed_ips);
  add_field(params, "saturation.tape.headBumpDb", cfg.saturation.tape.config.head_bump_db);
  add_field(params, "saturation.tape.bias", cfg.saturation.tape.config.bias);
  add_field(params, "saturation.tape.gapLoss", cfg.saturation.tape.config.gap_loss);

  add_field(params, "saturation.exciter.enabled", cfg.saturation.exciter.enabled);
  add_field(params, "saturation.exciter.frequencyHz", cfg.saturation.exciter.config.frequency_hz);
  add_field(params, "saturation.exciter.driveDb", cfg.saturation.exciter.config.drive_db);
  add_field(params, "saturation.exciter.amount", cfg.saturation.exciter.config.amount);
  add_field(params, "saturation.exciter.q", cfg.saturation.exciter.config.q);
  add_field(params, "saturation.exciter.evenOddMix", cfg.saturation.exciter.config.even_odd_mix);

  add_field(params, "spectral.airBand.enabled", cfg.spectral.air_band.enabled);
  add_field(params, "spectral.airBand.amount", cfg.spectral.air_band.config.amount);
  add_field(params, "spectral.airBand.shelfFrequencyHz",
            cfg.spectral.air_band.config.shelf_frequency_hz);
  add_field(params, "spectral.airBand.dynamicThresholdDb",
            cfg.spectral.air_band.config.dynamic_threshold_db);
  add_field(params, "spectral.airBand.dynamicRangeDb",
            cfg.spectral.air_band.config.dynamic_range_db);

  add_field(params, "stereo.imager.enabled", cfg.stereo.imager.enabled);
  add_field(params, "stereo.imager.width", cfg.stereo.imager.config.width);
  add_field(params, "stereo.imager.outputGainDb", cfg.stereo.imager.config.output_gain_db);
  add_field(params, "stereo.imager.decorrelationAmount",
            cfg.stereo.imager.config.decorrelation_amount);
  add_field(params, "stereo.imager.preserveEnergy", cfg.stereo.imager.config.preserve_energy);

  add_field(params, "stereo.monoMaker.enabled", cfg.stereo.mono_maker.enabled);
  add_field(params, "stereo.monoMaker.amount", cfg.stereo.mono_maker.config.amount);

  add_field(params, "maximizer.truePeakLimiter.enabled", cfg.maximizer.true_peak_limiter.enabled);
  add_field(params, "maximizer.truePeakLimiter.ceilingDb",
            cfg.maximizer.true_peak_limiter.config.ceiling_db);
  add_field(params, "maximizer.truePeakLimiter.lookaheadMs",
            cfg.maximizer.true_peak_limiter.config.lookahead_ms);
  add_field(params, "maximizer.truePeakLimiter.releaseMs",
            cfg.maximizer.true_peak_limiter.config.release_ms);
  add_field(params, "maximizer.truePeakLimiter.oversampleFactor",
            cfg.maximizer.true_peak_limiter.config.oversample_factor);
  add_field(params, "maximizer.truePeakLimiter.applyGainAtInputRate",
            cfg.maximizer.true_peak_limiter.config.apply_gain_at_input_rate);

  add_field(params, "loudness.enabled", cfg.loudness.enabled);
  add_field(params, "loudness.targetLufs", cfg.loudness.target_lufs);
  add_field(params, "loudness.ceilingDb", cfg.loudness.ceiling_db);
  add_field(params, "loudness.truePeakOversample", cfg.loudness.true_peak_oversample);
  add_field(params, "loudness.releaseMs", cfg.loudness.release_ms);
  add_field(params, "loudness.applyGainAtInputRate", cfg.loudness.apply_gain_at_input_rate);

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
