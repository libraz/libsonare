/// @file chain_json.cpp
/// @brief JSON serialization for MasteringChainConfig.

#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "mastering/api/chain.h"

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
    skip_ws();
    expect('{');
    bool saw_version = false;
    bool saw_params = false;
    while (!consume('}')) {
      const std::string key = parse_string();
      expect(':');
      if (key == "version") {
        const double version = parse_number();
        if (static_cast<int>(version) != 1) {
          throw std::invalid_argument("unsupported chain config JSON version");
        }
        saw_version = true;
      } else if (key == "params") {
        params = parse_params_object();
        saw_params = true;
      } else {
        throw std::invalid_argument("unknown chain config JSON field: " + key);
      }
      if (!consume(',')) {
        expect('}');
        break;
      }
    }
    skip_ws();
    if (pos_ != text_.size()) throw std::invalid_argument("trailing data after JSON object");
    if (!saw_version || !saw_params) {
      throw std::invalid_argument("chain config JSON requires version and params");
    }
    return params;
  }

 private:
  std::vector<Param> parse_params_object() {
    std::vector<Param> params;
    expect('{');
    while (!consume('}')) {
      std::string key = parse_string();
      expect(':');
      double value = 0.0;
      if (peek("true")) {
        pos_ += 4;
        value = 1.0;
      } else if (peek("false")) {
        pos_ += 5;
        value = 0.0;
      } else {
        value = parse_number();
      }
      params.push_back(Param{std::move(key), value});
      if (!consume(',')) {
        expect('}');
        break;
      }
    }
    return params;
  }

  std::string parse_string() {
    skip_ws();
    expect_raw('"');
    std::string out;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return out;
      if (c == '\\') {
        if (pos_ >= text_.size()) throw std::invalid_argument("unterminated JSON escape");
        const char escaped = text_[pos_++];
        if (escaped == '"' || escaped == '\\' || escaped == '/') {
          out.push_back(escaped);
        } else if (escaped == 'n') {
          out.push_back('\n');
        } else if (escaped == 't') {
          out.push_back('\t');
        } else {
          throw std::invalid_argument("unsupported JSON string escape");
        }
      } else {
        out.push_back(c);
      }
    }
    throw std::invalid_argument("unterminated JSON string");
  }

  double parse_number() {
    skip_ws();
    const size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    if (start == pos_) throw std::invalid_argument("expected JSON number");
    return std::stod(text_.substr(start, pos_ - start));
  }

  bool consume(char c) {
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  void expect(char c) {
    skip_ws();
    expect_raw(c);
  }

  void expect_raw(char c) {
    if (pos_ >= text_.size() || text_[pos_] != c) {
      throw std::invalid_argument(std::string("expected JSON character: ") + c);
    }
    ++pos_;
  }

  bool peek(const char* literal) const {
    const std::string value(literal);
    return text_.compare(pos_, value.size(), value) == 0;
  }

  void skip_ws() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
      ++pos_;
    }
  }

  const std::string& text_;
  size_t pos_ = 0;
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
