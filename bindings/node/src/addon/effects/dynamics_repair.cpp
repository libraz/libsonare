#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "analysis/acoustic_analyzer.h"
#include "core/audio.h"
#include "editing/pitch_editor/note_editor.h"
#include "editing/pitch_editor/pitch_corrector.h"
#include "editing/voice_changer/voice_changer.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "mastering/api/chain.h"
#include "mastering/api/internal_processor_runner.h"
#include "mastering/api/named_processor.h"
#include "mastering/api/presets.h"
#include "mastering/assistant/suggester.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/maximizer/loudness_optimize.h"
#include "mastering/maximizer/streaming_preview.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/repair/trim_silence.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"
#include "util/constants.h"

using namespace sonare_node;

namespace {

/// @brief Read a dereverb options bag over @p config, leaving absent keys alone.
sonare::mastering::repair::DereverbClassicalConfig read_dereverb_config(
    const Napi::Object& options, sonare::mastering::repair::DereverbClassicalConfig config) {
  config.threshold = FloatProperty(options, "threshold", config.threshold);
  config.attenuation = FloatProperty(options, "attenuation", config.attenuation);
  config.n_fft = IntProperty(options, "nFft", config.n_fft);
  config.hop_length = IntProperty(options, "hopLength", config.hop_length);
  config.t60_sec = FloatProperty(options, "t60Sec", config.t60_sec);
  config.late_delay_ms = FloatProperty(options, "lateDelayMs", config.late_delay_ms);
  config.over_subtraction = FloatProperty(options, "overSubtraction", config.over_subtraction);
  config.spectral_floor = FloatProperty(options, "spectralFloor", config.spectral_floor);
  config.wpe_enabled = BoolProperty(options, "wpeEnabled", config.wpe_enabled);
  config.wpe_iterations = IntProperty(options, "wpeIterations", config.wpe_iterations);
  config.wpe_taps = IntProperty(options, "wpeTaps", config.wpe_taps);
  config.wpe_strength = FloatProperty(options, "wpeStrength", config.wpe_strength);
  return config;
}

sonare::mastering::repair::DenoiseMode parse_denoise_mode(
    const Napi::Object& options, sonare::mastering::repair::DenoiseMode fallback) {
  Napi::Value value = options.Get("mode");
  if (value.IsUndefined() || value.IsNull()) return fallback;
  if (!value.IsString()) throw std::runtime_error("denoise mode must be a string");
  std::string s = value.As<Napi::String>().Utf8Value();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "logmmse" || s == "log_mmse" || s == "lsa") {
    return sonare::mastering::repair::DenoiseMode::LogMmse;
  }
  if (s == "mmsestsa" || s == "mmse_stsa" || s == "stsa") {
    return sonare::mastering::repair::DenoiseMode::MmseStsa;
  }
  if (s == "spectralsubtraction" || s == "spectral_subtraction" || s == "ss") {
    return sonare::mastering::repair::DenoiseMode::SpectralSubtraction;
  }
  throw std::runtime_error("unknown denoise mode: " + value.As<Napi::String>().Utf8Value());
}

sonare::mastering::repair::DenoiseNoiseEstimator parse_denoise_noise_estimator(
    const Napi::Object& options, sonare::mastering::repair::DenoiseNoiseEstimator fallback) {
  Napi::Value value = options.Get("noiseEstimator");
  if (value.IsUndefined() || value.IsNull()) return fallback;
  if (!value.IsString()) throw std::runtime_error("denoise noise estimator must be a string");
  std::string s = value.As<Napi::String>().Utf8Value();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "quantile") return sonare::mastering::repair::DenoiseNoiseEstimator::Quantile;
  if (s == "mcra") return sonare::mastering::repair::DenoiseNoiseEstimator::Mcra;
  if (s == "imcra") return sonare::mastering::repair::DenoiseNoiseEstimator::Imcra;
  if (s == "spp") return sonare::mastering::repair::DenoiseNoiseEstimator::Spp;
  throw std::runtime_error("unknown denoise noise estimator: " +
                           value.As<Napi::String>().Utf8Value());
}

}  // namespace

namespace {

sonare::mastering::dynamics::DetectorMode parse_compressor_detector(
    Napi::Env env, const Napi::Object& options,
    sonare::mastering::dynamics::DetectorMode fallback) {
  Napi::Value value = options.Get("detector");
  if (value.IsUndefined() || value.IsNull()) return fallback;
  if (value.IsNumber()) {
    int mode = node_narrow_int(value.Env(), value, "mode");
    switch (mode) {
      case 0:
        return sonare::mastering::dynamics::DetectorMode::Peak;
      case 1:
        return sonare::mastering::dynamics::DetectorMode::Rms;
      case 2:
        return sonare::mastering::dynamics::DetectorMode::LogRms;
      default:
        throw std::runtime_error("unknown compressor detector mode");
    }
  }
  if (value.IsString()) {
    std::string s = value.As<Napi::String>().Utf8Value();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "peak") return sonare::mastering::dynamics::DetectorMode::Peak;
    if (s == "rms") return sonare::mastering::dynamics::DetectorMode::Rms;
    if (s == "log_rms" || s == "logrms") return sonare::mastering::dynamics::DetectorMode::LogRms;
    throw std::runtime_error("unknown compressor detector mode: " +
                             value.As<Napi::String>().Utf8Value());
  }
  // Throw rather than ThrowAsJavaScriptException()+return: the latter only
  // schedules a pending JS exception and lets C++ control fall through, so the
  // caller would keep running with `fallback` while an exception is pending. The
  // sibling error paths above all throw std::runtime_error; match them so the
  // N-API wrapper converts it and stops here.
  (void)env;
  throw std::runtime_error("detector must be a string or number");
}

template <typename Processor>
std::vector<float> run_dynamics_offline(Processor& processor, const float* samples, size_t length,
                                        int sample_rate, int& latency_samples_out) {
  std::vector<float> buffer(samples, samples + length);
  // Mirror C-ABI/Python: drain processor lookahead before returning the
  // one-shot result so users receive a time-aligned buffer on every surface.
  sonare::mastering::api::internal::run_processor_mono(processor, buffer, sample_rate);
  latency_samples_out = processor.latency_samples();
  return buffer;
}

Napi::Object make_dynamics_result(Napi::Env env, const std::vector<float>& samples,
                                  int latency_samples) {
  auto typed = Napi::Float32Array::New(env, samples.size());
  if (!samples.empty()) {
    std::memcpy(typed.Data(), samples.data(), samples.size() * sizeof(float));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("samples", typed);
  out.Set("latencySamples", Napi::Number::New(env, latency_samples));
  return out;
}

}  // namespace

Napi::Value SonareWrap::MasteringDynamicsCompressor(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sr");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::dynamics::CompressorConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold_db = FloatProperty(options, "thresholdDb", config.threshold_db);
    config.ratio = FloatProperty(options, "ratio", config.ratio);
    config.attack_ms = FloatProperty(options, "attackMs", config.attack_ms);
    config.release_ms = FloatProperty(options, "releaseMs", config.release_ms);
    config.knee_db = FloatProperty(options, "kneeDb", config.knee_db);
    config.makeup_gain_db = FloatProperty(options, "makeupGainDb", config.makeup_gain_db);
    config.auto_makeup = BoolProperty(options, "autoMakeup", config.auto_makeup);
    config.detector = parse_compressor_detector(env, options, config.detector);
    config.sidechain_hpf_enabled =
        node_bool_option(options, "sidechainHpfEnabled", config.sidechain_hpf_enabled);
    config.sidechain_hpf_hz = node_float_option(options, "sidechainHpfHz", config.sidechain_hpf_hz);
    config.pdr_time_ms = node_float_option(options, "pdrTimeMs", config.pdr_time_ms);
    config.pdr_release_scale =
        node_float_option(options, "pdrReleaseScale", config.pdr_release_scale);
  }
  sonare::mastering::dynamics::Compressor processor(config);
  int latency = 0;
  std::vector<float> out =
      run_dynamics_offline(processor, typed.Data(), typed.ElementLength(), sr, latency);
  return make_dynamics_result(env, out, latency);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringDynamicsGate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sr");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::dynamics::GateConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold_db = FloatProperty(options, "thresholdDb", config.threshold_db);
    config.attack_ms = FloatProperty(options, "attackMs", config.attack_ms);
    config.release_ms = FloatProperty(options, "releaseMs", config.release_ms);
    config.range_db = FloatProperty(options, "rangeDb", config.range_db);
    config.hold_ms = node_float_option(options, "holdMs", config.hold_ms);
    config.close_threshold_db =
        node_float_option(options, "closeThresholdDb", config.close_threshold_db);
    config.key_hpf_hz = node_float_option(options, "keyHpfHz", config.key_hpf_hz);
  }
  sonare::mastering::dynamics::Gate processor(config);
  int latency = 0;
  std::vector<float> out =
      run_dynamics_offline(processor, typed.Data(), typed.ElementLength(), sr, latency);
  return make_dynamics_result(env, out, latency);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringDynamicsTransientShaper(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sr");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::dynamics::TransientShaperConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.attack_gain_db = FloatProperty(options, "attackGainDb", config.attack_gain_db);
    config.sustain_gain_db = FloatProperty(options, "sustainGainDb", config.sustain_gain_db);
    config.fast_attack_ms = FloatProperty(options, "fastAttackMs", config.fast_attack_ms);
    config.fast_release_ms = FloatProperty(options, "fastReleaseMs", config.fast_release_ms);
    config.slow_attack_ms = FloatProperty(options, "slowAttackMs", config.slow_attack_ms);
    config.slow_release_ms = FloatProperty(options, "slowReleaseMs", config.slow_release_ms);
    config.sensitivity = FloatProperty(options, "sensitivity", config.sensitivity);
    config.max_gain_db = FloatProperty(options, "maxGainDb", config.max_gain_db);
    config.gain_smoothing_ms = FloatProperty(options, "gainSmoothingMs", config.gain_smoothing_ms);
    config.lookahead_ms = FloatProperty(options, "lookaheadMs", config.lookahead_ms);
  }
  sonare::mastering::dynamics::TransientShaper processor(config);
  int latency = 0;
  std::vector<float> out =
      run_dynamics_offline(processor, typed.Data(), typed.ElementLength(), sr, latency);
  return make_dynamics_result(env, out, latency);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDeclick(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sr");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::DeclickConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold = FloatProperty(options, "threshold", config.threshold);
    config.neighbor_ratio = FloatProperty(options, "neighborRatio", config.neighbor_ratio);
    if (options.Has("maxClickSamples")) {
      const int max_click_samples =
          IntProperty(options, "maxClickSamples", static_cast<int>(config.max_click_samples));
      if (max_click_samples <= 0) {
        Napi::RangeError::New(env, "maxClickSamples must be positive").ThrowAsJavaScriptException();
        return env.Undefined();
      }
      config.max_click_samples = static_cast<size_t>(max_click_samples);
    }
    config.lpc_order = IntProperty(options, "lpcOrder", config.lpc_order);
    config.residual_ratio = FloatProperty(options, "residualRatio", config.residual_ratio);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::declick(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}

namespace {

/// @brief Marshal one channel's click detection into the JS shape shared by
///        the mono and stereo declick reports.
Napi::Object EmitClickDetection(Napi::Env env, const SonareClickDetection& detection) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("count", Napi::Number::New(env, static_cast<double>(detection.count)));
  out.Set("rejected", Napi::Number::New(env, static_cast<double>(detection.rejected)));
  out.Set("longestRunSamples",
          Napi::Number::New(env, static_cast<double>(detection.longest_run_samples)));
  out.Set("perSecond", Napi::Number::New(env, detection.per_second));
  return out;
}

Napi::Object EmitDeclickReport(Napi::Env env, const SonareDeclickReport& report) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("detected", EmitClickDetection(env, report.detected));
  out.Set("repairedRuns", Napi::Number::New(env, static_cast<double>(report.repaired_runs)));
  out.Set("repairedSamples", Napi::Number::New(env, static_cast<double>(report.repaired_samples)));
  out.Set("linkedRuns", Napi::Number::New(env, static_cast<double>(report.linked_runs)));
  out.Set("lpcModelUsed", Napi::Boolean::New(env, report.lpc_model_used != 0));
  return out;
}

/// @brief Read a SonareDeclickConfig options bag, applying the same field
///        names, defaults and maxClickSamples guard as the mono facade's C++
///        DeclickConfig mapping above -- the two structs share their shape,
///        this is the C-ABI mirror of that reading.
SonareDeclickConfig read_declick_config_c(Napi::Env env, const Napi::Object& options,
                                          SonareDeclickConfig config) {
  config.threshold = FloatProperty(options, "threshold", config.threshold);
  config.neighbor_ratio = FloatProperty(options, "neighborRatio", config.neighbor_ratio);
  if (options.Has("maxClickSamples")) {
    const int max_click_samples =
        IntProperty(options, "maxClickSamples", static_cast<int>(config.max_click_samples));
    if (max_click_samples <= 0) {
      throw Napi::RangeError::New(env, "maxClickSamples must be positive");
    }
    config.max_click_samples = static_cast<size_t>(max_click_samples);
  }
  config.lpc_order = IntProperty(options, "lpcOrder", config.lpc_order);
  config.residual_ratio = FloatProperty(options, "residualRatio", config.residual_ratio);
  return config;
}

/// @brief Frees both heap-owned channels of a SonareDeclickStereoResult on
///        scope exit -- the struct carries no dedicated free function, unlike
///        the CResultGuard-eligible C-ABI results elsewhere in the addon.
class DeclickStereoResultGuard {
 public:
  explicit DeclickStereoResultGuard(SonareDeclickStereoResult* result) : result_(result) {}
  DeclickStereoResultGuard(const DeclickStereoResultGuard&) = delete;
  DeclickStereoResultGuard& operator=(const DeclickStereoResultGuard&) = delete;
  ~DeclickStereoResultGuard() {
    sonare_free_floats(result_->left);
    sonare_free_floats(result_->right);
  }

 private:
  SonareDeclickStereoResult* result_;
};

}  // namespace

Napi::Value SonareWrap::MasteringRepairDeclickStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1]) ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array left, Float32Array right, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::Error::New(env, "masteringRepairDeclickStereo: left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  // Library defaults (sonare_c_mastering.h SonareDeclickConfig), applied
  // before any options key overrides a field.
  SonareDeclickConfig config{0.8f, 4.0f, 8, 20, 8.0f};
  if (info.Length() >= 4 && info[3].IsObject()) {
    config = read_declick_config_c(env, info[3].As<Napi::Object>(), config);
  }
  SonareDeclickStereoResult result{};
  SonareError err = sonare_mastering_repair_declick_stereo(
      left.Data(), right.Data(), left.ElementLength(), sr, &config, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  DeclickStereoResultGuard guard(&result);
  auto left_out = Napi::Float32Array::New(env, result.length);
  auto right_out = Napi::Float32Array::New(env, result.length);
  if (result.length > 0) {
    std::memcpy(left_out.Data(), result.left, result.length * sizeof(float));
    std::memcpy(right_out.Data(), result.right, result.length * sizeof(float));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  out.Set("leftReport", EmitDeclickReport(env, result.left_report));
  out.Set("rightReport", EmitDeclickReport(env, result.right_report));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDenoiseClassical(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sr");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::DenoiseClassicalConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.mode = parse_denoise_mode(options, config.mode);
    config.noise_estimator = parse_denoise_noise_estimator(options, config.noise_estimator);
    config.n_fft = IntProperty(options, "nFft", config.n_fft);
    config.hop_length = IntProperty(options, "hopLength", config.hop_length);
    config.dd_alpha = FloatProperty(options, "ddAlpha", config.dd_alpha);
    config.reduction_db = FloatProperty(options, "reductionDb", config.reduction_db);
    config.over_subtraction = FloatProperty(options, "overSubtraction", config.over_subtraction);
    config.spectral_floor = FloatProperty(options, "spectralFloor", config.spectral_floor);
    config.noise_estimation_quantile =
        node_float_option(options, "noiseEstimationQuantile", config.noise_estimation_quantile);
    config.speech_presence_gain =
        node_bool_option(options, "speechPresenceGain", config.speech_presence_gain);
    config.gain_smoothing = node_bool_option(options, "gainSmoothing", config.gain_smoothing);
  }
  if (config.n_fft <= 0 || (config.n_fft & (config.n_fft - 1)) != 0) {
    Napi::RangeError::New(env, "nFft must be a positive power of two").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (config.hop_length <= 0) {
    Napi::RangeError::New(env, "hopLength must be positive").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::denoise_classical(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}

namespace {

/// @brief Marshal the pair-level noise analysis into the JS shape the denoise
///        stereo report carries.
Napi::Object EmitNoiseDetection(Napi::Env env, const SonareNoiseDetection& detection) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("floorDbfs", Napi::Number::New(env, detection.floor_dbfs));
  Napi::Array band_floor_dbfs = Napi::Array::New(env, SONARE_REPAIR_NOISE_BAND_COUNT);
  for (int i = 0; i < SONARE_REPAIR_NOISE_BAND_COUNT; ++i) {
    band_floor_dbfs.Set(static_cast<uint32_t>(i),
                        Napi::Number::New(env, detection.band_floor_dbfs[i]));
  }
  out.Set("bandFloorDbfs", band_floor_dbfs);
  return out;
}

Napi::Object EmitDenoiseReport(Napi::Env env, const SonareDenoiseReport& report) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("detected", EmitNoiseDetection(env, report.detected));
  out.Set("meanReductionDb", Napi::Number::New(env, report.mean_reduction_db));
  out.Set("maxReductionDb", Napi::Number::New(env, report.max_reduction_db));
  out.Set("floorLimitedFraction", Napi::Number::New(env, report.floor_limited_fraction));
  return out;
}

/// @brief Read a SonareDenoiseClassicalConfig options bag, reusing the mono
///        facade's own mode and estimator string readers above so the two paths
///        cannot recognize different spellings of the same mode.
SonareDenoiseClassicalConfig read_denoise_config(const Napi::Object& options,
                                                 SonareDenoiseClassicalConfig config) {
  config.mode = static_cast<int>(parse_denoise_mode(
      options, static_cast<sonare::mastering::repair::DenoiseMode>(config.mode)));
  config.noise_estimator = static_cast<int>(parse_denoise_noise_estimator(
      options,
      static_cast<sonare::mastering::repair::DenoiseNoiseEstimator>(config.noise_estimator)));
  config.n_fft = IntProperty(options, "nFft", config.n_fft);
  config.hop_length = IntProperty(options, "hopLength", config.hop_length);
  config.dd_alpha = FloatProperty(options, "ddAlpha", config.dd_alpha);
  config.reduction_db = FloatProperty(options, "reductionDb", config.reduction_db);
  config.over_subtraction = FloatProperty(options, "overSubtraction", config.over_subtraction);
  config.spectral_floor = FloatProperty(options, "spectralFloor", config.spectral_floor);
  config.noise_estimation_quantile =
      FloatProperty(options, "noiseEstimationQuantile", config.noise_estimation_quantile);
  config.speech_presence_gain =
      BoolProperty(options, "speechPresenceGain", config.speech_presence_gain != 0) ? 1 : 0;
  config.gain_smoothing =
      BoolProperty(options, "gainSmoothing", config.gain_smoothing != 0) ? 1 : 0;
  return config;
}

/// @brief Frees both heap-owned channels of a SonareDenoiseStereoResult on scope
///        exit -- mirrors DeclickStereoResultGuard above.
class DenoiseStereoResultGuard {
 public:
  explicit DenoiseStereoResultGuard(SonareDenoiseStereoResult* result) : result_(result) {}
  DenoiseStereoResultGuard(const DenoiseStereoResultGuard&) = delete;
  DenoiseStereoResultGuard& operator=(const DenoiseStereoResultGuard&) = delete;
  ~DenoiseStereoResultGuard() {
    sonare_free_floats(result_->left);
    sonare_free_floats(result_->right);
  }

 private:
  SonareDenoiseStereoResult* result_;
};

}  // namespace

Napi::Value SonareWrap::MasteringRepairDenoiseClassicalStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1]) ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array left, Float32Array right, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::Error::New(
        env, "masteringRepairDenoiseClassicalStereo: left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  // Library defaults (sonare_c_mastering.h SonareDenoiseClassicalConfig), applied
  // before any options key overrides a field.
  SonareDenoiseClassicalConfig config{SONARE_DENOISE_MODE_LOG_MMSE,
                                      SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE,
                                      1024,
                                      256,
                                      0.98f,
                                      26.0f,
                                      2.0f,
                                      0.05f,
                                      0.1f,
                                      1,
                                      1};
  if (info.Length() >= 4 && info[3].IsObject()) {
    config = read_denoise_config(info[3].As<Napi::Object>(), config);
  }
  SonareDenoiseStereoResult result{};
  SonareError err = sonare_mastering_repair_denoise_classical_stereo(
      left.Data(), right.Data(), left.ElementLength(), sr, &config, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  DenoiseStereoResultGuard guard(&result);
  auto left_out = Napi::Float32Array::New(env, result.length);
  auto right_out = Napi::Float32Array::New(env, result.length);
  if (result.length > 0) {
    std::memcpy(left_out.Data(), result.left, result.length * sizeof(float));
    std::memcpy(right_out.Data(), result.right, result.length * sizeof(float));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  out.Set("report", EmitDenoiseReport(env, result.report));
  return out;
  SONARE_NODE_CATCH(env)
}

namespace {

sonare::mastering::repair::DecrackleMode parse_decrackle_mode(
    const Napi::Object& options, sonare::mastering::repair::DecrackleMode fallback) {
  Napi::Value value = options.Get("mode");
  if (value.IsUndefined() || value.IsNull()) return fallback;
  if (!value.IsString()) throw std::runtime_error("decrackle mode must be a string");
  std::string s = value.As<Napi::String>().Utf8Value();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "median") return sonare::mastering::repair::DecrackleMode::Median;
  if (s == "waveletshrinkage" || s == "wavelet_shrinkage" || s == "wavelet") {
    return sonare::mastering::repair::DecrackleMode::WaveletShrinkage;
  }
  throw std::runtime_error("unknown decrackle mode: " + value.As<Napi::String>().Utf8Value());
}

int parse_dehum_mode(const Napi::Object& options, int fallback) {
  Napi::Value value = options.Get("mode");
  if (value.IsUndefined() || value.IsNull()) return fallback;
  if (!value.IsString()) throw std::runtime_error("dehum mode must be a string");
  std::string s = value.As<Napi::String>().Utf8Value();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "subtract") return SONARE_DEHUM_MODE_SUBTRACT;
  if (s == "notch") return SONARE_DEHUM_MODE_NOTCH;
  throw std::runtime_error("unknown dehum mode: " + value.As<Napi::String>().Utf8Value());
}

sonare::mastering::repair::TrimSilenceMode parse_trim_silence_mode(
    const Napi::Object& options, sonare::mastering::repair::TrimSilenceMode fallback) {
  Napi::Value value = options.Get("mode");
  if (value.IsUndefined() || value.IsNull()) return fallback;
  if (!value.IsString()) throw std::runtime_error("trim silence mode must be a string");
  std::string s = value.As<Napi::String>().Utf8Value();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "peak") return sonare::mastering::repair::TrimSilenceMode::Peak;
  if (s == "lufsgated" || s == "lufs_gated" || s == "lufs") {
    return sonare::mastering::repair::TrimSilenceMode::LufsGated;
  }
  throw std::runtime_error("unknown trim silence mode: " + value.As<Napi::String>().Utf8Value());
}

}  // namespace

Napi::Value SonareWrap::MasteringRepairDeclip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sr");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::DeclipConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.clip_threshold = node_float_option(options, "clipThreshold", config.clip_threshold);
    config.lpc_order = IntProperty(options, "lpcOrder", config.lpc_order);
    config.iterations = node_int_option(options, "iterations", config.iterations);
    config.lpc_blend = node_float_option(options, "lpcBlend", config.lpc_blend);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::declip(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}

namespace {

/// @brief Marshal one channel's clip detection into the JS shape shared by the
///        declip stereo report.
Napi::Object EmitClipDetection(Napi::Env env, const SonareClipDetection& detection) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("sampleCount", Napi::Number::New(env, static_cast<double>(detection.sample_count)));
  out.Set("sampleFraction", Napi::Number::New(env, detection.sample_fraction));
  out.Set("runCount", Napi::Number::New(env, static_cast<double>(detection.run_count)));
  out.Set("longestRunSamples",
          Napi::Number::New(env, static_cast<double>(detection.longest_run_samples)));
  out.Set("flatRunCount", Napi::Number::New(env, static_cast<double>(detection.flat_run_count)));
  out.Set("longestFlatRunSamples",
          Napi::Number::New(env, static_cast<double>(detection.longest_flat_run_samples)));
  out.Set("flatSampleCount",
          Napi::Number::New(env, static_cast<double>(detection.flat_sample_count)));
  out.Set("flatLevel", Napi::Number::New(env, detection.flat_level));
  return out;
}

Napi::Object EmitDeclipReport(Napi::Env env, const SonareDeclipReport& report) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("detected", EmitClipDetection(env, report.detected));
  out.Set("lpcReconstructedRuns",
          Napi::Number::New(env, static_cast<double>(report.lpc_reconstructed_runs)));
  out.Set("interpolatedRuns",
          Napi::Number::New(env, static_cast<double>(report.interpolated_runs)));
  out.Set("repairedSamples", Napi::Number::New(env, static_cast<double>(report.repaired_samples)));
  out.Set("linkedRuns", Napi::Number::New(env, static_cast<double>(report.linked_runs)));
  return out;
}

/// @brief Read a SonareDeclipConfig options bag, applying the same field names
///        and defaults as the mono facade's C++ DeclipConfig mapping above --
///        the two structs share their shape, this is the C-ABI mirror of that
///        reading.
SonareDeclipConfig read_declip_config_c(const Napi::Object& options, SonareDeclipConfig config) {
  config.clip_threshold = FloatProperty(options, "clipThreshold", config.clip_threshold);
  config.lpc_order = IntProperty(options, "lpcOrder", config.lpc_order);
  config.iterations = IntProperty(options, "iterations", config.iterations);
  config.lpc_blend = FloatProperty(options, "lpcBlend", config.lpc_blend);
  return config;
}

/// @brief Frees both heap-owned channels of a SonareDeclipStereoResult on
///        scope exit -- the struct carries no dedicated free function, unlike
///        the CResultGuard-eligible C-ABI results elsewhere in the addon.
class DeclipStereoResultGuard {
 public:
  explicit DeclipStereoResultGuard(SonareDeclipStereoResult* result) : result_(result) {}
  DeclipStereoResultGuard(const DeclipStereoResultGuard&) = delete;
  DeclipStereoResultGuard& operator=(const DeclipStereoResultGuard&) = delete;
  ~DeclipStereoResultGuard() {
    sonare_free_floats(result_->left);
    sonare_free_floats(result_->right);
  }

 private:
  SonareDeclipStereoResult* result_;
};

}  // namespace

Napi::Value SonareWrap::MasteringRepairDeclipStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1]) ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array left, Float32Array right, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::Error::New(env, "masteringRepairDeclipStereo: left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  // Library defaults (sonare_c_mastering.h SonareDeclipConfig), applied before
  // any options key overrides a field.
  SonareDeclipConfig config{0.98f, 36, 2, 0.65f};
  if (info.Length() >= 4 && info[3].IsObject()) {
    config = read_declip_config_c(info[3].As<Napi::Object>(), config);
  }
  SonareDeclipStereoResult result{};
  SonareError err = sonare_mastering_repair_declip_stereo(
      left.Data(), right.Data(), left.ElementLength(), sr, &config, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  DeclipStereoResultGuard guard(&result);
  auto left_out = Napi::Float32Array::New(env, result.length);
  auto right_out = Napi::Float32Array::New(env, result.length);
  if (result.length > 0) {
    std::memcpy(left_out.Data(), result.left, result.length * sizeof(float));
    std::memcpy(right_out.Data(), result.right, result.length * sizeof(float));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  out.Set("leftReport", EmitDeclipReport(env, result.left_report));
  out.Set("rightReport", EmitDeclipReport(env, result.right_report));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDecrackle(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sr");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::DecrackleConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold = FloatProperty(options, "threshold", config.threshold);
    config.mode = parse_decrackle_mode(options, config.mode);
    config.levels = node_int_option(options, "levels", config.levels);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::decrackle(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}

namespace {

/// @brief Marshal one channel's crackle detection into the JS shape shared by
///        the decrackle stereo report.
Napi::Object EmitCrackleDetection(Napi::Env env, const SonareCrackleDetection& detection) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("sampleCount", Napi::Number::New(env, static_cast<double>(detection.sample_count)));
  out.Set("sampleFraction", Napi::Number::New(env, detection.sample_fraction));
  out.Set("perSecond", Napi::Number::New(env, detection.per_second));
  return out;
}

Napi::Object EmitDecrackleReport(Napi::Env env, const SonareDecrackleReport& report) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("detected", EmitCrackleDetection(env, report.detected));
  out.Set("replacedSamples", Napi::Number::New(env, static_cast<double>(report.replaced_samples)));
  out.Set("detailCoefficients",
          Napi::Number::New(env, static_cast<double>(report.detail_coefficients)));
  out.Set("shrunkCoefficients",
          Napi::Number::New(env, static_cast<double>(report.shrunk_coefficients)));
  out.Set("noiseSigma", Napi::Number::New(env, report.noise_sigma));
  return out;
}

/// @brief Read a SonareDecrackleConfig options bag, reusing the mono facade's
///        own mode-string reader above so the two paths cannot recognize
///        different spellings of the same mode.
SonareDecrackleConfig read_decrackle_config_c(const Napi::Object& options,
                                              SonareDecrackleConfig config) {
  config.threshold = FloatProperty(options, "threshold", config.threshold);
  config.mode = static_cast<int>(parse_decrackle_mode(
      options, static_cast<sonare::mastering::repair::DecrackleMode>(config.mode)));
  config.levels = IntProperty(options, "levels", config.levels);
  return config;
}

/// @brief Frees both heap-owned channels of a SonareDecrackleStereoResult on
///        scope exit -- mirrors DeclipStereoResultGuard above.
class DecrackleStereoResultGuard {
 public:
  explicit DecrackleStereoResultGuard(SonareDecrackleStereoResult* result) : result_(result) {}
  DecrackleStereoResultGuard(const DecrackleStereoResultGuard&) = delete;
  DecrackleStereoResultGuard& operator=(const DecrackleStereoResultGuard&) = delete;
  ~DecrackleStereoResultGuard() {
    sonare_free_floats(result_->left);
    sonare_free_floats(result_->right);
  }

 private:
  SonareDecrackleStereoResult* result_;
};

}  // namespace

Napi::Value SonareWrap::MasteringRepairDecrackleStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1]) ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array left, Float32Array right, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::Error::New(env,
                     "masteringRepairDecrackleStereo: left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  // Library defaults (sonare_c_mastering.h SonareDecrackleConfig), applied before
  // any options key overrides a field.
  SonareDecrackleConfig config{0.4f, SONARE_DECRACKLE_MODE_MEDIAN, 4};
  if (info.Length() >= 4 && info[3].IsObject()) {
    config = read_decrackle_config_c(info[3].As<Napi::Object>(), config);
  }
  SonareDecrackleStereoResult result{};
  SonareError err = sonare_mastering_repair_decrackle_stereo(
      left.Data(), right.Data(), left.ElementLength(), sr, &config, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  DecrackleStereoResultGuard guard(&result);
  auto left_out = Napi::Float32Array::New(env, result.length);
  auto right_out = Napi::Float32Array::New(env, result.length);
  if (result.length > 0) {
    std::memcpy(left_out.Data(), result.left, result.length * sizeof(float));
    std::memcpy(right_out.Data(), result.right, result.length * sizeof(float));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  out.Set("leftReport", EmitDecrackleReport(env, result.left_report));
  out.Set("rightReport", EmitDecrackleReport(env, result.right_report));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDehum(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sr");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::DehumConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.fundamental_hz = node_float_option(options, "fundamentalHz", config.fundamental_hz);
    config.harmonics = node_int_option(options, "harmonics", config.harmonics);
    config.q = FloatProperty(options, "q", config.q);
    config.adaptive = node_bool_option(options, "adaptive", config.adaptive);
    config.search_range_hz = node_float_option(options, "searchRangeHz", config.search_range_hz);
    config.adaptation = node_float_option(options, "adaptation", config.adaptation);
    config.frame_size = node_int_option(options, "frameSize", config.frame_size);
    config.pll_bandwidth = node_float_option(options, "pllBandwidth", config.pll_bandwidth);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::dehum(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}

namespace {

/// @brief Marshal one channel's hum detection into the JS shape shared by the
///        dehum stereo report.
Napi::Object EmitHumDetection(Napi::Env env, const SonareHumDetection& detection) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("fundamentalHz", Napi::Number::New(env, detection.fundamental_hz));
  out.Set("fundamentalProminence", Napi::Number::New(env, detection.fundamental_prominence));
  out.Set("harmonics", Napi::Number::New(env, detection.harmonics));
  Napi::Array harmonic_dbfs = Napi::Array::New(env, SONARE_DEHUM_MAX_HARMONICS);
  for (int i = 0; i < SONARE_DEHUM_MAX_HARMONICS; ++i) {
    harmonic_dbfs.Set(static_cast<uint32_t>(i), Napi::Number::New(env, detection.harmonic_dbfs[i]));
  }
  out.Set("harmonicDbfs", harmonic_dbfs);
  return out;
}

Napi::Object EmitDehumReport(Napi::Env env, const SonareDehumReport& report) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("detected", EmitHumDetection(env, report.detected));
  out.Set("notchedHarmonics", Napi::Number::New(env, report.notched_harmonics));
  out.Set("appliedFundamentalHz", Napi::Number::New(env, report.applied_fundamental_hz));
  out.Set("fundamentalDriftHz", Napi::Number::New(env, report.fundamental_drift_hz));
  return out;
}

/// @brief Read a SonareDehumConfig options bag, applying the same field names
///        and defaults as the mono facade's DehumConfig mapping above -- the
///        two structs share their shape, this is the C-ABI mirror of that
///        reading.
SonareDehumConfig read_dehum_config_c(const Napi::Object& options, SonareDehumConfig config) {
  config.fundamental_hz = FloatProperty(options, "fundamentalHz", config.fundamental_hz);
  config.harmonics = IntProperty(options, "harmonics", config.harmonics);
  config.q = FloatProperty(options, "q", config.q);
  config.adaptive = BoolProperty(options, "adaptive", config.adaptive != 0) ? 1 : 0;
  config.search_range_hz = FloatProperty(options, "searchRangeHz", config.search_range_hz);
  config.adaptation = FloatProperty(options, "adaptation", config.adaptation);
  config.frame_size = IntProperty(options, "frameSize", config.frame_size);
  config.pll_bandwidth = FloatProperty(options, "pllBandwidth", config.pll_bandwidth);
  config.mode = parse_dehum_mode(options, config.mode);
  return config;
}

/// @brief Frees both heap-owned channels of a SonareDehumStereoResult on scope
///        exit -- mirrors DecrackleStereoResultGuard above.
class DehumStereoResultGuard {
 public:
  explicit DehumStereoResultGuard(SonareDehumStereoResult* result) : result_(result) {}
  DehumStereoResultGuard(const DehumStereoResultGuard&) = delete;
  DehumStereoResultGuard& operator=(const DehumStereoResultGuard&) = delete;
  ~DehumStereoResultGuard() {
    sonare_free_floats(result_->left);
    sonare_free_floats(result_->right);
  }

 private:
  SonareDehumStereoResult* result_;
};

}  // namespace

Napi::Value SonareWrap::MasteringRepairDehumStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1]) ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array left, Float32Array right, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::Error::New(env, "masteringRepairDehumStereo: left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  // Library defaults (sonare_c_mastering.h SonareDehumConfig), applied before
  // any options key overrides a field.
  SonareDehumConfig config{
      50.0f, 4, 20.0f, 0, 2.0f, 0.25f, 2048, 0.01f, SONARE_DEHUM_MODE_SUBTRACT};
  if (info.Length() >= 4 && info[3].IsObject()) {
    config = read_dehum_config_c(info[3].As<Napi::Object>(), config);
  }
  SonareDehumStereoResult result{};
  SonareError err = sonare_mastering_repair_dehum_stereo(
      left.Data(), right.Data(), left.ElementLength(), sr, &config, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  DehumStereoResultGuard guard(&result);
  auto left_out = Napi::Float32Array::New(env, result.length);
  auto right_out = Napi::Float32Array::New(env, result.length);
  if (result.length > 0) {
    std::memcpy(left_out.Data(), result.left, result.length * sizeof(float));
    std::memcpy(right_out.Data(), result.right, result.length * sizeof(float));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  out.Set("leftReport", EmitDehumReport(env, result.left_report));
  out.Set("rightReport", EmitDehumReport(env, result.right_report));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDereverbClassical(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sr");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::DereverbClassicalConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    config = read_dereverb_config(info[2].As<Napi::Object>(), config);
  }
  if (config.n_fft <= 0 || (config.n_fft & (config.n_fft - 1)) != 0) {
    Napi::RangeError::New(env, "nFft must be a positive power of two").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (config.hop_length <= 0 || config.hop_length > config.n_fft) {
    Napi::RangeError::New(env, "hopLength must be in (0, nFft]").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::dereverb_classical(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}

namespace {

/// @brief Marshal the pair-level reverb analysis into the JS shape the dereverb
///        stereo report carries.
Napi::Object EmitReverbDetection(Napi::Env env, const SonareReverbDetection& detection) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("lateDecayRatioDb", Napi::Number::New(env, detection.late_decay_ratio_db));
  out.Set("latePredictability", Napi::Number::New(env, detection.late_predictability));
  return out;
}

Napi::Object EmitDereverbReport(Napi::Env env, const SonareDereverbReport& report) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("detected", EmitReverbDetection(env, report.detected));
  out.Set("meanReductionDb", Napi::Number::New(env, report.mean_reduction_db));
  out.Set("suppressedFraction", Napi::Number::New(env, report.suppressed_fraction));
  out.Set("wpePredictorNorm", Napi::Number::New(env, report.wpe_predictor_norm));
  return out;
}

/// @brief Read a SonareDereverbClassicalConfig options bag, applying the same
///        field names and defaults as read_dereverb_config above -- the two
///        structs share their shape, this is the C-ABI mirror of that reading.
SonareDereverbClassicalConfig read_dereverb_config_c(const Napi::Object& options,
                                                     SonareDereverbClassicalConfig config) {
  config.threshold = FloatProperty(options, "threshold", config.threshold);
  config.attenuation = FloatProperty(options, "attenuation", config.attenuation);
  config.n_fft = IntProperty(options, "nFft", config.n_fft);
  config.hop_length = IntProperty(options, "hopLength", config.hop_length);
  config.t60_sec = FloatProperty(options, "t60Sec", config.t60_sec);
  config.late_delay_ms = FloatProperty(options, "lateDelayMs", config.late_delay_ms);
  config.over_subtraction = FloatProperty(options, "overSubtraction", config.over_subtraction);
  config.spectral_floor = FloatProperty(options, "spectralFloor", config.spectral_floor);
  config.wpe_enabled = BoolProperty(options, "wpeEnabled", config.wpe_enabled != 0) ? 1 : 0;
  config.wpe_iterations = IntProperty(options, "wpeIterations", config.wpe_iterations);
  config.wpe_taps = IntProperty(options, "wpeTaps", config.wpe_taps);
  config.wpe_strength = FloatProperty(options, "wpeStrength", config.wpe_strength);
  return config;
}

/// @brief Frees both heap-owned channels of a SonareDereverbStereoResult on
///        scope exit -- mirrors DenoiseStereoResultGuard above.
class DereverbStereoResultGuard {
 public:
  explicit DereverbStereoResultGuard(SonareDereverbStereoResult* result) : result_(result) {}
  DereverbStereoResultGuard(const DereverbStereoResultGuard&) = delete;
  DereverbStereoResultGuard& operator=(const DereverbStereoResultGuard&) = delete;
  ~DereverbStereoResultGuard() {
    sonare_free_floats(result_->left);
    sonare_free_floats(result_->right);
  }

 private:
  SonareDereverbStereoResult* result_;
};

}  // namespace

Napi::Value SonareWrap::MasteringRepairDereverbClassicalStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1]) ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array left, Float32Array right, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::Error::New(
        env, "masteringRepairDereverbClassicalStereo: left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  // Library defaults (sonare_c_mastering.h SonareDereverbClassicalConfig),
  // applied before any options key overrides a field.
  SonareDereverbClassicalConfig config{0.0f, 1.0f,  1024, 256, 0.4f, 50.0f,
                                       1.0f, 0.08f, 0,    2,   3,    0.7f};
  if (info.Length() >= 4 && info[3].IsObject()) {
    config = read_dereverb_config_c(info[3].As<Napi::Object>(), config);
  }
  SonareDereverbStereoResult result{};
  SonareError err = sonare_mastering_repair_dereverb_classical_stereo(
      left.Data(), right.Data(), left.ElementLength(), sr, &config, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  DereverbStereoResultGuard guard(&result);
  auto left_out = Napi::Float32Array::New(env, result.length);
  auto right_out = Napi::Float32Array::New(env, result.length);
  if (result.length > 0) {
    std::memcpy(left_out.Data(), result.left, result.length * sizeof(float));
    std::memcpy(right_out.Data(), result.right, result.length * sizeof(float));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  out.Set("report", EmitDereverbReport(env, result.report));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDereverbConfigForRoom(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "Expected (roomEstimate, config?)").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  Napi::Object estimate = info[0].As<Napi::Object>();
  // Read AND written: the caller's config is the base, and only the two fields
  // the measurement determines come back changed.
  sonare::mastering::repair::DereverbClassicalConfig config;
  if (info.Length() >= 2 && info[1].IsObject()) {
    config = read_dereverb_config(info[1].As<Napi::Object>(), config);
  }
  // Record fields, not options: an absent band array is "did not converge"
  // rather than a zero reverberation time.
  const std::vector<float> rt60_bands = FloatArrayProperty(estimate, "rt60Bands");
  sonare::mastering::repair::apply_room_measurement(config, sonare::mid_frequency_rt60(rt60_bands),
                                                    FloatProperty(estimate, "volume", 0.0f));

  Napi::Object out = Napi::Object::New(env);
  out.Set("threshold", Napi::Number::New(env, config.threshold));
  out.Set("attenuation", Napi::Number::New(env, config.attenuation));
  out.Set("nFft", Napi::Number::New(env, config.n_fft));
  out.Set("hopLength", Napi::Number::New(env, config.hop_length));
  out.Set("t60Sec", Napi::Number::New(env, config.t60_sec));
  out.Set("lateDelayMs", Napi::Number::New(env, config.late_delay_ms));
  out.Set("overSubtraction", Napi::Number::New(env, config.over_subtraction));
  out.Set("spectralFloor", Napi::Number::New(env, config.spectral_floor));
  out.Set("wpeEnabled", Napi::Boolean::New(env, config.wpe_enabled));
  out.Set("wpeIterations", Napi::Number::New(env, config.wpe_iterations));
  out.Set("wpeTaps", Napi::Number::New(env, config.wpe_taps));
  out.Set("wpeStrength", Napi::Number::New(env, config.wpe_strength));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairTrimSilence(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sr");
  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(typed.Data(), typed.ElementLength(), sr);
  sonare::mastering::repair::TrimSilenceConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold = FloatProperty(options, "threshold", config.threshold);
    if (options.Has("paddingSamples")) {
      const int padding_samples =
          node_int_option(options, "paddingSamples", static_cast<int>(config.padding_samples));
      if (padding_samples < 0) {
        Napi::RangeError::New(env, "paddingSamples must be non-negative")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      config.padding_samples = static_cast<size_t>(padding_samples);
    }
    config.mode = parse_trim_silence_mode(options, config.mode);
    config.gate_lufs = node_float_option(options, "gateLufs", config.gate_lufs);
    config.window_ms = node_float_option(options, "windowMs", config.window_ms);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::trim_silence(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
  SONARE_NODE_CATCH(env)
}

namespace {

Napi::Object EmitTrimRange(Napi::Env env, const SonareTrimRange& range) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("first", Napi::Number::New(env, static_cast<double>(range.first)));
  out.Set("lastExclusive", Napi::Number::New(env, static_cast<double>(range.last_exclusive)));
  return out;
}

Napi::Object EmitTrimReport(Napi::Env env, const SonareTrimReport& report) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("range", EmitTrimRange(env, report.range));
  out.Set("removedHeadSamples",
          Napi::Number::New(env, static_cast<double>(report.removed_head_samples)));
  out.Set("removedTailSamples",
          Napi::Number::New(env, static_cast<double>(report.removed_tail_samples)));
  return out;
}

/// @brief Read a SonareTrimSilenceConfig options bag, applying the same field
///        names as the mono entry above reads onto the C++ config.
/// @details `paddingSamples` goes through NonNegativeSizeTProperty rather than
///   an int reader: the field is a size_t, so -1 arrives as SIZE_MAX and lands
///   above the core's SIZE_MAX/2 bound, where it reads as an out-of-range
///   padding rather than as the negative the caller wrote. Refusing it by name
///   here reports what was actually wrong, and the false return is why this
///   reports success rather than returning the config.
/// @return false with a JS exception pending; the caller must bail.
bool ReadTrimSilenceConfig(Napi::Env env, const Napi::Object& options,
                           SonareTrimSilenceConfig* config) {
  config->threshold = FloatProperty(options, "threshold", config->threshold);
  if (!NonNegativeSizeTProperty(env, options, "paddingSamples", config->padding_samples,
                                &config->padding_samples)) {
    return false;
  }
  const auto mode =
      parse_trim_silence_mode(options, config->mode == SONARE_TRIM_SILENCE_MODE_LUFS_GATED
                                           ? sonare::mastering::repair::TrimSilenceMode::LufsGated
                                           : sonare::mastering::repair::TrimSilenceMode::Peak);
  config->mode = mode == sonare::mastering::repair::TrimSilenceMode::LufsGated
                     ? SONARE_TRIM_SILENCE_MODE_LUFS_GATED
                     : SONARE_TRIM_SILENCE_MODE_PEAK;
  config->gate_lufs = FloatProperty(options, "gateLufs", config->gate_lufs);
  config->window_ms = FloatProperty(options, "windowMs", config->window_ms);
  return true;
}

/// @brief Frees both heap-owned channels of a SonareTrimSilenceStereoResult on
///        scope exit -- mirrors DereverbStereoResultGuard above.
/// @details A pass that kept nothing hands back two NULLs rather than two
///   zero-length allocations, which no other repair stereo entry can produce;
///   sonare_free_floats accepts NULL, so that case needs no branch here.
class TrimSilenceStereoResultGuard {
 public:
  explicit TrimSilenceStereoResultGuard(SonareTrimSilenceStereoResult* result) : result_(result) {}
  TrimSilenceStereoResultGuard(const TrimSilenceStereoResultGuard&) = delete;
  TrimSilenceStereoResultGuard& operator=(const TrimSilenceStereoResultGuard&) = delete;
  ~TrimSilenceStereoResultGuard() {
    sonare_free_floats(result_->left);
    sonare_free_floats(result_->right);
  }

 private:
  SonareTrimSilenceStereoResult* result_;
};

}  // namespace

Napi::Value SonareWrap::MasteringRepairTrimSilenceStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1]) ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array left, Float32Array right, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::Error::New(env,
                     "masteringRepairTrimSilenceStereo: left and right must have the same "
                     "length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  // Library defaults (sonare_c_mastering.h SonareTrimSilenceConfig), applied
  // before any options key overrides a field.
  SonareTrimSilenceConfig config{0.001f, 0, SONARE_TRIM_SILENCE_MODE_PEAK, -60.0f, 400.0f};
  if (info.Length() >= 4 && info[3].IsObject()) {
    if (!ReadTrimSilenceConfig(env, info[3].As<Napi::Object>(), &config)) return env.Undefined();
  }
  SonareTrimSilenceStereoResult result{};
  SonareError err = sonare_mastering_repair_trim_silence_stereo(
      left.Data(), right.Data(), left.ElementLength(), sr, &config, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  TrimSilenceStereoResultGuard guard(&result);
  // result.length is the OUTPUT length, not the input's: trimming shortens the
  // pair, and an all-silent pair comes back at 0.
  auto left_out = Napi::Float32Array::New(env, result.length);
  auto right_out = Napi::Float32Array::New(env, result.length);
  if (result.length > 0) {
    std::memcpy(left_out.Data(), result.left, result.length * sizeof(float));
    std::memcpy(right_out.Data(), result.right, result.length * sizeof(float));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  out.Set("report", EmitTrimReport(env, result.report));
  out.Set("leftRange", EmitTrimRange(env, result.left_range));
  out.Set("rightRange", EmitTrimRange(env, result.right_range));
  return out;
  SONARE_NODE_CATCH(env)
}

namespace {

// Library defaults (sonare_c_mastering.h), the base an options bag is read
// over. A call with no options bag passes NULL instead, which is how the C ABI
// itself asks for these.
constexpr SonareDeclickConfig kDeclickConfigDefaults{0.8f, 4.0f, 8, 20, 8.0f};
constexpr SonareDenoiseClassicalConfig kDenoiseConfigDefaults{
    SONARE_DENOISE_MODE_LOG_MMSE,
    SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE,
    1024,
    256,
    0.98f,
    26.0f,
    2.0f,
    0.05f,
    0.1f,
    1,
    1};
constexpr SonareDeclipConfig kDeclipConfigDefaults{0.98f, 36, 2, 0.65f};
constexpr SonareDecrackleConfig kDecrackleConfigDefaults{0.4f, SONARE_DECRACKLE_MODE_MEDIAN, 4};
constexpr SonareDehumConfig kDehumConfigDefaults{
    50.0f, 4, 20.0f, 0, 2.0f, 0.25f, 2048, 0.01f, SONARE_DEHUM_MODE_SUBTRACT};
constexpr SonareDereverbClassicalConfig kDereverbConfigDefaults{0.0f, 1.0f,  1024, 256, 0.4f, 50.0f,
                                                                1.0f, 0.08f, 0,    2,   3,    0.7f};
constexpr SonareTrimSilenceConfig kTrimSilenceConfigDefaults{
    0.001f, 0, SONARE_TRIM_SILENCE_MODE_PEAK, -60.0f, 400.0f};

/// @brief Argument check shared by the mono detect entries, which all take
///        (Float32Array, sampleRate, options?).
bool CheckDetectMonoArgs(Napi::Env env, const Napi::CallbackInfo& info) {
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

}  // namespace

Napi::Value SonareWrap::MasteringRepairDetectClicks(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckDetectMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDeclickConfig config = kDeclickConfigDefaults;
  const bool has_options = info.Length() >= 3 && info[2].IsObject();
  if (has_options) config = read_declick_config_c(env, info[2].As<Napi::Object>(), config);
  SonareClickDetection detection{};
  SonareError err = sonare_mastering_repair_detect_clicks(
      typed.Data(), typed.ElementLength(), sr, has_options ? &config : nullptr, &detection);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return EmitClickDetection(env, detection);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectNoiseFloor(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckDetectMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDenoiseClassicalConfig config = kDenoiseConfigDefaults;
  const bool has_options = info.Length() >= 3 && info[2].IsObject();
  if (has_options) config = read_denoise_config(info[2].As<Napi::Object>(), config);
  SonareNoiseDetection detection{};
  SonareError err = sonare_mastering_repair_detect_noise_floor(
      typed.Data(), typed.ElementLength(), sr, has_options ? &config : nullptr, &detection);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return EmitNoiseDetection(env, detection);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairNoiseBandBins(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  SONARE_NODE_TRY
  // Every field is optional, so an omitted request reads as an empty one.
  Napi::Object request = info.Length() >= 1 && info[0].IsObject() ? info[0].As<Napi::Object>()
                                                                  : Napi::Object::New(env);
  const int n_fft = IntProperty(request, "nFft", kDenoiseConfigDefaults.n_fft);
  const int sample_rate = IntProperty(request, "sampleRate", sonare::constants::kDefaultSampleRate);
  // The C entry writes into the array's own storage; no intermediate buffer.
  auto bins = Napi::Int32Array::New(env, SONARE_REPAIR_NOISE_BAND_EDGE_COUNT);
  SonareError err = sonare_mastering_repair_noise_band_bins(n_fft, sample_rate, bins.Data());
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return bins;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectClipping(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckDetectMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDeclipConfig config = kDeclipConfigDefaults;
  const bool has_options = info.Length() >= 3 && info[2].IsObject();
  if (has_options) config = read_declip_config_c(info[2].As<Napi::Object>(), config);
  SonareClipDetection detection{};
  SonareError err = sonare_mastering_repair_detect_clipping(
      typed.Data(), typed.ElementLength(), sr, has_options ? &config : nullptr, &detection);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return EmitClipDetection(env, detection);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectCrackle(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckDetectMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDecrackleConfig config = kDecrackleConfigDefaults;
  const bool has_options = info.Length() >= 3 && info[2].IsObject();
  if (has_options) config = read_decrackle_config_c(info[2].As<Napi::Object>(), config);
  SonareCrackleDetection detection{};
  SonareError err = sonare_mastering_repair_detect_crackle(
      typed.Data(), typed.ElementLength(), sr, has_options ? &config : nullptr, &detection);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return EmitCrackleDetection(env, detection);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectHum(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckDetectMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDehumConfig config = kDehumConfigDefaults;
  const bool has_options = info.Length() >= 3 && info[2].IsObject();
  if (has_options) config = read_dehum_config_c(info[2].As<Napi::Object>(), config);
  SonareHumDetection detection{};
  SonareError err = sonare_mastering_repair_detect_hum(typed.Data(), typed.ElementLength(), sr,
                                                       has_options ? &config : nullptr, &detection);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return EmitHumDetection(env, detection);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectReverb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckDetectMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDereverbClassicalConfig config = kDereverbConfigDefaults;
  const bool has_options = info.Length() >= 3 && info[2].IsObject();
  if (has_options) config = read_dereverb_config_c(info[2].As<Napi::Object>(), config);
  SonareReverbDetection detection{};
  SonareError err = sonare_mastering_repair_detect_reverb(
      typed.Data(), typed.ElementLength(), sr, has_options ? &config : nullptr, &detection);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return EmitReverbDetection(env, detection);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectTrimRange(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckDetectMonoArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareTrimSilenceConfig config = kTrimSilenceConfigDefaults;
  const bool has_options = info.Length() >= 3 && info[2].IsObject();
  if (has_options && !ReadTrimSilenceConfig(env, info[2].As<Napi::Object>(), &config)) {
    return env.Undefined();
  }
  SonareTrimRange range{};
  SonareError err = sonare_mastering_repair_detect_trim_range(
      typed.Data(), typed.ElementLength(), sr, has_options ? &config : nullptr, &range);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return EmitTrimRange(env, range);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDetectTrimRangeStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1]) ||
      !info[2].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array left, Float32Array right, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto left = info[0].As<Napi::Float32Array>();
  auto right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::Error::New(
        env, "masteringRepairDetectTrimRangeStereo: left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[2], "sampleRate");
  SonareTrimSilenceConfig config = kTrimSilenceConfigDefaults;
  const bool has_options = info.Length() >= 4 && info[3].IsObject();
  if (has_options && !ReadTrimSilenceConfig(env, info[3].As<Napi::Object>(), &config)) {
    return env.Undefined();
  }
  SonareTrimRange range{};
  SonareError err = sonare_mastering_repair_detect_trim_range_stereo(
      left.Data(), right.Data(), left.ElementLength(), sr, has_options ? &config : nullptr, &range);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return EmitTrimRange(env, range);
  SONARE_NODE_CATCH(env)
}

namespace {

/// @brief Input and output planes of one channel-linked repair call.
/// @details The linked C entries write caller-owned output planes in place, so
///   the Float32Arrays handed back to JS are allocated up front and passed in
///   rather than copied out of a library allocation.
struct LinkedPlanes {
  std::vector<Napi::Float32Array> inputs;
  std::vector<const float*> in_ptrs;
  std::vector<Napi::Float32Array> outputs;
  std::vector<float*> out_ptrs;
  size_t length = 0;
};

/// @brief Reads N Float32Array channels and allocates the matching output planes.
/// @details Refuses what the C form cannot express: a non-Float32Array element,
///   and a length disagreement, which the single @c length argument has no way to
///   carry. An empty list goes through to the C entry, which owns that rejection.
bool ReadLinkedPlanes(Napi::Env env, const Napi::Value& value, const char* fn_name,
                      LinkedPlanes* planes) {
  Napi::Array channels = value.As<Napi::Array>();
  const size_t count = channels.Length();
  planes->inputs.reserve(count);
  planes->in_ptrs.reserve(count);
  planes->outputs.reserve(count);
  planes->out_ptrs.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    Napi::Value channel = channels.Get(index);
    if (!IsFloat32Array(channel)) {
      Napi::TypeError::New(env, std::string(fn_name) + ": every channel must be a Float32Array")
          .ThrowAsJavaScriptException();
      return false;
    }
    planes->inputs.push_back(channel.As<Napi::Float32Array>());
    const size_t length = planes->inputs.back().ElementLength();
    if (index == 0) {
      planes->length = length;
    } else if (length != planes->length) {
      Napi::Error::New(env, std::string(fn_name) + ": every channel must have the same length")
          .ThrowAsJavaScriptException();
      return false;
    }
    planes->in_ptrs.push_back(planes->inputs.back().Data());
    planes->outputs.push_back(Napi::Float32Array::New(env, planes->length));
    planes->out_ptrs.push_back(planes->outputs.back().Data());
  }
  return true;
}

/// @brief Packs the output planes into a JS array, in input order.
Napi::Array EmitLinkedChannels(Napi::Env env, const LinkedPlanes& planes) {
  Napi::Array out = Napi::Array::New(env, planes.outputs.size());
  for (size_t index = 0; index < planes.outputs.size(); ++index) {
    out.Set(static_cast<uint32_t>(index), planes.outputs[index]);
  }
  return out;
}

/// @brief Argument check shared by the channel-linked entries, which both take
///        (Float32Array[], sampleRate, options?).
bool CheckLinkedArgs(Napi::Env env, const Napi::CallbackInfo& info) {
  if (info.Length() < 2 || !info[0].IsArray() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array[] channels, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

}  // namespace

Napi::Value SonareWrap::MasteringRepairDenoiseClassicalLinked(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckLinkedArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  LinkedPlanes planes;
  if (!ReadLinkedPlanes(env, info[0], "masteringRepairDenoiseClassicalLinked", &planes)) {
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDenoiseClassicalConfig config = kDenoiseConfigDefaults;
  if (info.Length() >= 3 && info[2].IsObject()) {
    config = read_denoise_config(info[2].As<Napi::Object>(), config);
  }
  SonareDenoiseReport report{};
  SonareError err = sonare_mastering_repair_denoise_classical_linked(
      planes.in_ptrs.data(), planes.in_ptrs.size(), planes.length, sr, &config,
      planes.out_ptrs.data(), &report);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("channels", EmitLinkedChannels(env, planes));
  out.Set("report", EmitDenoiseReport(env, report));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MasteringRepairDereverbClassicalLinked(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!CheckLinkedArgs(env, info)) return env.Undefined();

  SONARE_NODE_TRY
  LinkedPlanes planes;
  if (!ReadLinkedPlanes(env, info[0], "masteringRepairDereverbClassicalLinked", &planes)) {
    return env.Undefined();
  }
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  SonareDereverbClassicalConfig config = kDereverbConfigDefaults;
  if (info.Length() >= 3 && info[2].IsObject()) {
    config = read_dereverb_config_c(info[2].As<Napi::Object>(), config);
  }
  SonareDereverbReport report{};
  SonareError err = sonare_mastering_repair_dereverb_classical_linked(
      planes.in_ptrs.data(), planes.in_ptrs.size(), planes.length, sr, &config,
      planes.out_ptrs.data(), &report);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("channels", EmitLinkedChannels(env, planes));
  out.Set("report", EmitDereverbReport(env, report));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::Trim(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, ...)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  int sr = node_narrow_int(env, info[1], "sr");
  float threshold_db = node_arg_finite_float(info, 2, -60.0f);
  auto parse_frame_option = [&](size_t index, const char* name, int fallback, int* output) {
    if (info.Length() <= index || info[index].IsUndefined()) {
      *output = fallback;
      return true;
    }
    if (!info[index].IsNumber()) {
      Napi::TypeError::New(env, std::string("trim: ") + name + " must be an integer")
          .ThrowAsJavaScriptException();
      return false;
    }
    const double value = info[index].As<Napi::Number>().DoubleValue();
    if (!std::isfinite(value) || std::floor(value) != value) {
      Napi::RangeError::New(env, std::string("trim: ") + name + " must be an integer")
          .ThrowAsJavaScriptException();
      return false;
    }
    if (value <= 0 || value > std::numeric_limits<int>::max()) {
      Napi::RangeError::New(env, std::string("trim: ") + name + " must be a positive integer")
          .ThrowAsJavaScriptException();
      return false;
    }
    *output = static_cast<int>(value);
    return true;
  };
  int frame_length = sonare::constants::kDefaultNFft;
  int hop_length = sonare::constants::kDefaultHopLength;
  if (!parse_frame_option(3, "frameLength", sonare::constants::kDefaultNFft, &frame_length) ||
      !parse_frame_option(4, "hopLength", sonare::constants::kDefaultHopLength, &hop_length)) {
    return env.Undefined();
  }

  // Re-apply the C-ABI input validation this direct core call would otherwise bypass.
  sonare::validate_offline_audio_input(data, length, sr);
  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::trim_absolute(audio, threshold_db, frame_length, hop_length);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}
