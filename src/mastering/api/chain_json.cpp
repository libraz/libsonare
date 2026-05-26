/// @file chain_json.cpp
/// @brief JSON serialization for MasteringChainConfig.

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "mastering/api/chain.h"
#include "mastering/api/json_params.h"

namespace sonare::mastering::api {
namespace {

void add_param(std::vector<Param>& params, std::string key, double value) {
  params.push_back(Param{std::move(key), value});
}

std::vector<Param> flatten_chain_config(const MasteringChainConfig& cfg) {
  std::vector<Param> params;
  params.reserve(76);

  add_param(params, "repair.declick.enabled", cfg.repair.declick.enabled ? 1.0 : 0.0);
  add_param(params, "repair.declick.threshold", cfg.repair.declick.config.threshold);
  add_param(params, "repair.declick.neighborRatio", cfg.repair.declick.config.neighbor_ratio);
  add_param(params, "repair.declick.maxClickSamples",
            static_cast<double>(cfg.repair.declick.config.max_click_samples));
  add_param(params, "repair.declick.lpcOrder", cfg.repair.declick.config.lpc_order);
  add_param(params, "repair.declick.residualRatio", cfg.repair.declick.config.residual_ratio);

  add_param(params, "repair.declip.enabled", cfg.repair.declip.enabled ? 1.0 : 0.0);
  add_param(params, "repair.declip.clipThreshold", cfg.repair.declip.config.clip_threshold);
  add_param(params, "repair.declip.lpcOrder", cfg.repair.declip.config.lpc_order);
  add_param(params, "repair.declip.iterations", cfg.repair.declip.config.iterations);
  add_param(params, "repair.declip.lpcBlend", cfg.repair.declip.config.lpc_blend);

  add_param(params, "repair.decrackle.enabled", cfg.repair.decrackle.enabled ? 1.0 : 0.0);
  add_param(params, "repair.decrackle.threshold", cfg.repair.decrackle.config.threshold);
  add_param(params, "repair.decrackle.mode",
            cfg.repair.decrackle.config.mode == mastering::repair::DecrackleMode::WaveletShrinkage
                ? 1.0
                : 0.0);
  add_param(params, "repair.decrackle.levels", cfg.repair.decrackle.config.levels);

  add_param(params, "repair.dehum.enabled", cfg.repair.dehum.enabled ? 1.0 : 0.0);
  add_param(params, "repair.dehum.fundamentalHz", cfg.repair.dehum.config.fundamental_hz);
  add_param(params, "repair.dehum.harmonics", cfg.repair.dehum.config.harmonics);
  add_param(params, "repair.dehum.q", cfg.repair.dehum.config.q);
  add_param(params, "repair.dehum.adaptive", cfg.repair.dehum.config.adaptive ? 1.0 : 0.0);
  add_param(params, "repair.dehum.searchRangeHz", cfg.repair.dehum.config.search_range_hz);
  add_param(params, "repair.dehum.adaptation", cfg.repair.dehum.config.adaptation);
  add_param(params, "repair.dehum.frameSize", cfg.repair.dehum.config.frame_size);
  add_param(params, "repair.dehum.pllBandwidth", cfg.repair.dehum.config.pll_bandwidth);

  add_param(params, "repair.dereverb.enabled", cfg.repair.dereverb.enabled ? 1.0 : 0.0);
  add_param(params, "repair.dereverb.threshold", cfg.repair.dereverb.config.threshold);
  add_param(params, "repair.dereverb.attenuation", cfg.repair.dereverb.config.attenuation);
  add_param(params, "repair.dereverb.nFft", cfg.repair.dereverb.config.n_fft);
  add_param(params, "repair.dereverb.hopLength", cfg.repair.dereverb.config.hop_length);
  add_param(params, "repair.dereverb.t60Sec", cfg.repair.dereverb.config.t60_sec);
  add_param(params, "repair.dereverb.lateDelayMs", cfg.repair.dereverb.config.late_delay_ms);
  add_param(params, "repair.dereverb.overSubtraction", cfg.repair.dereverb.config.over_subtraction);
  add_param(params, "repair.dereverb.spectralFloor", cfg.repair.dereverb.config.spectral_floor);
  add_param(params, "repair.dereverb.wpeEnabled",
            cfg.repair.dereverb.config.wpe_enabled ? 1.0 : 0.0);
  add_param(params, "repair.dereverb.wpeIterations", cfg.repair.dereverb.config.wpe_iterations);
  add_param(params, "repair.dereverb.wpeTaps", cfg.repair.dereverb.config.wpe_taps);
  add_param(params, "repair.dereverb.wpeStrength", cfg.repair.dereverb.config.wpe_strength);

  add_param(params, "repair.denoise.enabled", cfg.repair.denoise.enabled ? 1.0 : 0.0);
  add_param(params, "repair.denoise.nFft", cfg.repair.denoise.config.n_fft);
  add_param(params, "repair.denoise.hopLength", cfg.repair.denoise.config.hop_length);
  add_param(params, "repair.denoise.ddAlpha", cfg.repair.denoise.config.dd_alpha);
  add_param(params, "repair.denoise.gainFloor", cfg.repair.denoise.config.gain_floor);

  add_param(params, "eq.tilt.enabled", cfg.eq.tilt.enabled ? 1.0 : 0.0);
  add_param(params, "eq.tilt.tiltDb", cfg.eq.tilt.tilt_db);
  add_param(params, "eq.tilt.pivotHz", cfg.eq.tilt.pivot_hz);

  add_param(params, "dynamics.deesser.enabled", cfg.dynamics.deesser.enabled ? 1.0 : 0.0);
  add_param(params, "dynamics.deesser.frequencyHz", cfg.dynamics.deesser.config.frequency_hz);
  add_param(params, "dynamics.deesser.thresholdDb", cfg.dynamics.deesser.config.threshold_db);
  add_param(params, "dynamics.deesser.ratio", cfg.dynamics.deesser.config.ratio);
  add_param(params, "dynamics.deesser.attackMs", cfg.dynamics.deesser.config.attack_ms);
  add_param(params, "dynamics.deesser.releaseMs", cfg.dynamics.deesser.config.release_ms);
  add_param(params, "dynamics.deesser.rangeDb", cfg.dynamics.deesser.config.range_db);
  add_param(params, "dynamics.deesser.bandpassQ", cfg.dynamics.deesser.config.bandpass_q);

  add_param(params, "dynamics.transientShaper.enabled",
            cfg.dynamics.transient_shaper.enabled ? 1.0 : 0.0);
  add_param(params, "dynamics.transientShaper.attackGainDb",
            cfg.dynamics.transient_shaper.config.attack_gain_db);
  add_param(params, "dynamics.transientShaper.sustainGainDb",
            cfg.dynamics.transient_shaper.config.sustain_gain_db);
  add_param(params, "dynamics.transientShaper.fastAttackMs",
            cfg.dynamics.transient_shaper.config.fast_attack_ms);
  add_param(params, "dynamics.transientShaper.fastReleaseMs",
            cfg.dynamics.transient_shaper.config.fast_release_ms);
  add_param(params, "dynamics.transientShaper.slowAttackMs",
            cfg.dynamics.transient_shaper.config.slow_attack_ms);
  add_param(params, "dynamics.transientShaper.slowReleaseMs",
            cfg.dynamics.transient_shaper.config.slow_release_ms);
  add_param(params, "dynamics.transientShaper.sensitivity",
            cfg.dynamics.transient_shaper.config.sensitivity);
  add_param(params, "dynamics.transientShaper.maxGainDb",
            cfg.dynamics.transient_shaper.config.max_gain_db);
  add_param(params, "dynamics.transientShaper.gainSmoothingMs",
            cfg.dynamics.transient_shaper.config.gain_smoothing_ms);
  add_param(params, "dynamics.transientShaper.lookaheadMs",
            cfg.dynamics.transient_shaper.config.lookahead_ms);

  add_param(params, "dynamics.compressor.enabled", cfg.dynamics.compressor.enabled ? 1.0 : 0.0);
  add_param(params, "dynamics.compressor.thresholdDb", cfg.dynamics.compressor.config.threshold_db);
  add_param(params, "dynamics.compressor.ratio", cfg.dynamics.compressor.config.ratio);
  add_param(params, "dynamics.compressor.attackMs", cfg.dynamics.compressor.config.attack_ms);
  add_param(params, "dynamics.compressor.releaseMs", cfg.dynamics.compressor.config.release_ms);
  add_param(params, "dynamics.compressor.kneeDb", cfg.dynamics.compressor.config.knee_db);
  add_param(params, "dynamics.compressor.makeupGainDb",
            cfg.dynamics.compressor.config.makeup_gain_db);
  add_param(params, "dynamics.compressor.autoMakeup",
            cfg.dynamics.compressor.config.auto_makeup ? 1.0 : 0.0);

  add_param(params, "dynamics.multibandComp.enabled",
            cfg.dynamics.multiband_comp.enabled ? 1.0 : 0.0);
  if (cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz.size() >= 2) {
    add_param(params, "dynamics.multibandComp.lowCutoffHz",
              cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz[0]);
    add_param(params, "dynamics.multibandComp.highCutoffHz",
              cfg.dynamics.multiband_comp.config.crossover.cutoffs_hz[1]);
  }
  if (cfg.dynamics.multiband_comp.config.bands.size() >= 3) {
    const auto& low = cfg.dynamics.multiband_comp.config.bands[0];
    const auto& mid = cfg.dynamics.multiband_comp.config.bands[1];
    const auto& high = cfg.dynamics.multiband_comp.config.bands[2];
    add_param(params, "dynamics.multibandComp.lowThresholdDb", low.threshold_db);
    add_param(params, "dynamics.multibandComp.lowRatio", low.ratio);
    add_param(params, "dynamics.multibandComp.lowAttackMs", low.attack_ms);
    add_param(params, "dynamics.multibandComp.lowReleaseMs", low.release_ms);
    add_param(params, "dynamics.multibandComp.midThresholdDb", mid.threshold_db);
    add_param(params, "dynamics.multibandComp.midRatio", mid.ratio);
    add_param(params, "dynamics.multibandComp.midAttackMs", mid.attack_ms);
    add_param(params, "dynamics.multibandComp.midReleaseMs", mid.release_ms);
    add_param(params, "dynamics.multibandComp.highThresholdDb", high.threshold_db);
    add_param(params, "dynamics.multibandComp.highRatio", high.ratio);
    add_param(params, "dynamics.multibandComp.highAttackMs", high.attack_ms);
    add_param(params, "dynamics.multibandComp.highReleaseMs", high.release_ms);
  }

  add_param(params, "saturation.tape.enabled", cfg.saturation.tape.enabled ? 1.0 : 0.0);
  add_param(params, "saturation.tape.driveDb", cfg.saturation.tape.config.drive_db);
  add_param(params, "saturation.tape.saturation", cfg.saturation.tape.config.saturation);
  add_param(params, "saturation.tape.hysteresis", cfg.saturation.tape.config.hysteresis);
  add_param(params, "saturation.tape.outputGainDb", cfg.saturation.tape.config.output_gain_db);
  add_param(params, "saturation.tape.speedIps", cfg.saturation.tape.config.speed_ips);
  add_param(params, "saturation.tape.headBumpDb", cfg.saturation.tape.config.head_bump_db);
  add_param(params, "saturation.tape.bias", cfg.saturation.tape.config.bias);
  add_param(params, "saturation.tape.gapLoss", cfg.saturation.tape.config.gap_loss);

  add_param(params, "saturation.exciter.enabled", cfg.saturation.exciter.enabled ? 1.0 : 0.0);
  add_param(params, "saturation.exciter.frequencyHz", cfg.saturation.exciter.config.frequency_hz);
  add_param(params, "saturation.exciter.driveDb", cfg.saturation.exciter.config.drive_db);
  add_param(params, "saturation.exciter.amount", cfg.saturation.exciter.config.amount);
  add_param(params, "saturation.exciter.q", cfg.saturation.exciter.config.q);
  add_param(params, "saturation.exciter.evenOddMix", cfg.saturation.exciter.config.even_odd_mix);

  add_param(params, "spectral.airBand.enabled", cfg.spectral.air_band.enabled ? 1.0 : 0.0);
  add_param(params, "spectral.airBand.amount", cfg.spectral.air_band.config.amount);
  add_param(params, "spectral.airBand.shelfFrequencyHz",
            cfg.spectral.air_band.config.shelf_frequency_hz);
  add_param(params, "spectral.airBand.dynamicThresholdDb",
            cfg.spectral.air_band.config.dynamic_threshold_db);
  add_param(params, "spectral.airBand.dynamicRangeDb",
            cfg.spectral.air_band.config.dynamic_range_db);

  add_param(params, "stereo.imager.enabled", cfg.stereo.imager.enabled ? 1.0 : 0.0);
  add_param(params, "stereo.imager.width", cfg.stereo.imager.config.width);
  add_param(params, "stereo.imager.outputGainDb", cfg.stereo.imager.config.output_gain_db);
  add_param(params, "stereo.imager.decorrelationAmount",
            cfg.stereo.imager.config.decorrelation_amount);
  add_param(params, "stereo.imager.preserveEnergy",
            cfg.stereo.imager.config.preserve_energy ? 1.0 : 0.0);

  add_param(params, "stereo.monoMaker.enabled", cfg.stereo.mono_maker.enabled ? 1.0 : 0.0);
  add_param(params, "stereo.monoMaker.amount", cfg.stereo.mono_maker.config.amount);

  add_param(params, "maximizer.truePeakLimiter.enabled",
            cfg.maximizer.true_peak_limiter.enabled ? 1.0 : 0.0);
  add_param(params, "maximizer.truePeakLimiter.ceilingDb",
            cfg.maximizer.true_peak_limiter.config.ceiling_db);
  add_param(params, "maximizer.truePeakLimiter.lookaheadMs",
            cfg.maximizer.true_peak_limiter.config.lookahead_ms);
  add_param(params, "maximizer.truePeakLimiter.releaseMs",
            cfg.maximizer.true_peak_limiter.config.release_ms);
  add_param(params, "maximizer.truePeakLimiter.oversampleFactor",
            cfg.maximizer.true_peak_limiter.config.oversample_factor);
  add_param(params, "maximizer.truePeakLimiter.applyGainAtInputRate",
            cfg.maximizer.true_peak_limiter.config.apply_gain_at_input_rate ? 1.0 : 0.0);

  add_param(params, "loudness.enabled", cfg.loudness.enabled ? 1.0 : 0.0);
  add_param(params, "loudness.targetLufs", cfg.loudness.target_lufs);
  add_param(params, "loudness.ceilingDb", cfg.loudness.ceiling_db);
  add_param(params, "loudness.truePeakOversample", cfg.loudness.true_peak_oversample);
  add_param(params, "loudness.releaseMs", cfg.loudness.release_ms);
  add_param(params, "loudness.applyGainAtInputRate",
            cfg.loudness.apply_gain_at_input_rate ? 1.0 : 0.0);

  return params;
}

class JsonParamParser {
 public:
  explicit JsonParamParser(const std::string& text) : text_(text) {}

  std::vector<Param> parse() {
    std::vector<Param> params;
    detail::JsonParamReader reader(text_);
    reader.skip_ws();
    reader.expect('{');
    bool saw_version = false;
    bool saw_params = false;
    while (!reader.consume('}')) {
      const std::string key = reader.parse_string();
      reader.expect(':');
      if (key == "version") {
        const double version = reader.parse_number();
        if (static_cast<int>(version) != 1) {
          throw std::invalid_argument("unsupported chain config JSON version");
        }
        saw_version = true;
      } else if (key == "params") {
        params = reader.parse_flat_object();
        saw_params = true;
      } else {
        throw std::invalid_argument("unknown chain config JSON field: " + key);
      }
      if (!reader.consume(',')) {
        reader.expect('}');
        break;
      }
    }
    reader.skip_ws();
    if (!reader.at_end()) throw std::invalid_argument("trailing data after JSON object");
    if (!saw_version || !saw_params) {
      throw std::invalid_argument("chain config JSON requires version and params");
    }
    return params;
  }

 private:
  const std::string& text_;
};

}  // namespace

std::string chain_config_to_json(const MasteringChainConfig& config) {
  const auto params = flatten_chain_config(config);
  std::ostringstream out;
  out << "{\"version\":1,\"params\":{";
  out << std::setprecision(9);
  for (size_t i = 0; i < params.size(); ++i) {
    if (i != 0) out << ',';
    out << '"' << params[i].key << "\":" << params[i].value;
  }
  out << "}}";
  return out.str();
}

MasteringChainConfig chain_config_from_json(const std::string& json) {
  JsonParamParser parser(json);
  const auto params = parser.parse();
  return parse_chain_config_params(params.data(), params.size());
}

}  // namespace sonare::mastering::api
