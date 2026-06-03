#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "editing/pitch_editor/note_editor.h"
#include "editing/pitch_editor/pitch_corrector.h"
#include "editing/voice_changer/voice_changer.h"
#include "effects/hpss.h"
#include "effects/normalize.h"
#include "effects/pitch_shift.h"
#include "effects/time_stretch.h"
#include "mastering/api/chain.h"
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

using namespace sonare_node;

namespace {

int repair_int_option(const Napi::Object& object, const char* key, int fallback) {
  Napi::Value value = object.Get(key);
  return value.IsNumber() ? value.As<Napi::Number>().Int32Value() : fallback;
}

float repair_float_option(const Napi::Object& object, const char* key, float fallback) {
  Napi::Value value = object.Get(key);
  return value.IsNumber() ? value.As<Napi::Number>().FloatValue() : fallback;
}

bool repair_bool_option(const Napi::Object& object, const char* key, bool fallback) {
  Napi::Value value = object.Get(key);
  return value.IsBoolean() ? value.As<Napi::Boolean>().Value() : fallback;
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
    int mode = value.As<Napi::Number>().Int32Value();
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
  Napi::TypeError::New(env, "detector must be a string or number").ThrowAsJavaScriptException();
  return fallback;
}

template <typename Processor>
std::vector<float> run_dynamics_offline(Processor& processor, const float* samples, size_t length,
                                        int sample_rate, int& latency_samples_out) {
  std::vector<float> buffer(samples, samples + length);
  processor.prepare(static_cast<double>(sample_rate), static_cast<int>(buffer.size()));
  float* channels[] = {buffer.data()};
  processor.process(channels, 1, static_cast<int>(buffer.size()));
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
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::dynamics::CompressorConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold_db = node_float_option(options, "thresholdDb", config.threshold_db);
    config.ratio = node_float_option(options, "ratio", config.ratio);
    config.attack_ms = node_float_option(options, "attackMs", config.attack_ms);
    config.release_ms = node_float_option(options, "releaseMs", config.release_ms);
    config.knee_db = node_float_option(options, "kneeDb", config.knee_db);
    config.makeup_gain_db = node_float_option(options, "makeupGainDb", config.makeup_gain_db);
    config.auto_makeup = node_bool_option(options, "autoMakeup", config.auto_makeup);
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
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::dynamics::GateConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold_db = node_float_option(options, "thresholdDb", config.threshold_db);
    config.attack_ms = node_float_option(options, "attackMs", config.attack_ms);
    config.release_ms = node_float_option(options, "releaseMs", config.release_ms);
    config.range_db = node_float_option(options, "rangeDb", config.range_db);
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
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::dynamics::TransientShaperConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.attack_gain_db = node_float_option(options, "attackGainDb", config.attack_gain_db);
    config.sustain_gain_db = node_float_option(options, "sustainGainDb", config.sustain_gain_db);
    config.fast_attack_ms = node_float_option(options, "fastAttackMs", config.fast_attack_ms);
    config.fast_release_ms = node_float_option(options, "fastReleaseMs", config.fast_release_ms);
    config.slow_attack_ms = node_float_option(options, "slowAttackMs", config.slow_attack_ms);
    config.slow_release_ms = node_float_option(options, "slowReleaseMs", config.slow_release_ms);
    config.sensitivity = node_float_option(options, "sensitivity", config.sensitivity);
    config.max_gain_db = node_float_option(options, "maxGainDb", config.max_gain_db);
    config.gain_smoothing_ms =
        node_float_option(options, "gainSmoothingMs", config.gain_smoothing_ms);
    config.lookahead_ms = node_float_option(options, "lookaheadMs", config.lookahead_ms);
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
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::repair::DeclickConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold = repair_float_option(options, "threshold", config.threshold);
    config.neighbor_ratio = repair_float_option(options, "neighborRatio", config.neighbor_ratio);
    if (options.Has("maxClickSamples")) {
      const int max_click_samples =
          repair_int_option(options, "maxClickSamples", static_cast<int>(config.max_click_samples));
      if (max_click_samples <= 0) {
        Napi::RangeError::New(env, "maxClickSamples must be positive").ThrowAsJavaScriptException();
        return env.Undefined();
      }
      config.max_click_samples = static_cast<size_t>(max_click_samples);
    }
    config.lpc_order = repair_int_option(options, "lpcOrder", config.lpc_order);
    config.residual_ratio = repair_float_option(options, "residualRatio", config.residual_ratio);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::declick(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
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
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::repair::DenoiseClassicalConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.mode = parse_denoise_mode(options, config.mode);
    config.noise_estimator = parse_denoise_noise_estimator(options, config.noise_estimator);
    config.n_fft = repair_int_option(options, "nFft", config.n_fft);
    config.hop_length = repair_int_option(options, "hopLength", config.hop_length);
    config.dd_alpha = repair_float_option(options, "ddAlpha", config.dd_alpha);
    config.gain_floor = repair_float_option(options, "gainFloor", config.gain_floor);
    config.over_subtraction =
        repair_float_option(options, "overSubtraction", config.over_subtraction);
    config.spectral_floor = repair_float_option(options, "spectralFloor", config.spectral_floor);
    config.noise_estimation_quantile =
        repair_float_option(options, "noiseEstimationQuantile", config.noise_estimation_quantile);
    config.speech_presence_gain =
        repair_bool_option(options, "speechPresenceGain", config.speech_presence_gain);
    config.gain_smoothing = repair_bool_option(options, "gainSmoothing", config.gain_smoothing);
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
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::repair::DeclipConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.clip_threshold = repair_float_option(options, "clipThreshold", config.clip_threshold);
    config.lpc_order = repair_int_option(options, "lpcOrder", config.lpc_order);
    config.iterations = repair_int_option(options, "iterations", config.iterations);
    config.lpc_blend = repair_float_option(options, "lpcBlend", config.lpc_blend);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::declip(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
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
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::repair::DecrackleConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold = repair_float_option(options, "threshold", config.threshold);
    config.mode = parse_decrackle_mode(options, config.mode);
    config.levels = repair_int_option(options, "levels", config.levels);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::decrackle(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
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
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::repair::DehumConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.fundamental_hz = repair_float_option(options, "fundamentalHz", config.fundamental_hz);
    config.harmonics = repair_int_option(options, "harmonics", config.harmonics);
    config.q = repair_float_option(options, "q", config.q);
    config.adaptive = repair_bool_option(options, "adaptive", config.adaptive);
    config.search_range_hz = repair_float_option(options, "searchRangeHz", config.search_range_hz);
    config.adaptation = repair_float_option(options, "adaptation", config.adaptation);
    config.frame_size = repair_int_option(options, "frameSize", config.frame_size);
    config.pll_bandwidth = repair_float_option(options, "pllBandwidth", config.pll_bandwidth);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::dehum(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
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
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::repair::DereverbClassicalConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold = repair_float_option(options, "threshold", config.threshold);
    config.attenuation = repair_float_option(options, "attenuation", config.attenuation);
    config.n_fft = repair_int_option(options, "nFft", config.n_fft);
    config.hop_length = repair_int_option(options, "hopLength", config.hop_length);
    config.t60_sec = repair_float_option(options, "t60Sec", config.t60_sec);
    config.late_delay_ms = repair_float_option(options, "lateDelayMs", config.late_delay_ms);
    config.over_subtraction =
        repair_float_option(options, "overSubtraction", config.over_subtraction);
    config.spectral_floor = repair_float_option(options, "spectralFloor", config.spectral_floor);
    config.wpe_enabled = repair_bool_option(options, "wpeEnabled", config.wpe_enabled);
    config.wpe_iterations = repair_int_option(options, "wpeIterations", config.wpe_iterations);
    config.wpe_taps = repair_int_option(options, "wpeTaps", config.wpe_taps);
    config.wpe_strength = repair_float_option(options, "wpeStrength", config.wpe_strength);
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

Napi::Value SonareWrap::MasteringRepairTrimSilence(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const int sr = info[1].As<Napi::Number>().Int32Value();
  sonare::mastering::repair::TrimSilenceConfig config;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    config.threshold = repair_float_option(options, "threshold", config.threshold);
    if (options.Has("paddingSamples")) {
      const int padding_samples =
          repair_int_option(options, "paddingSamples", static_cast<int>(config.padding_samples));
      if (padding_samples < 0) {
        Napi::RangeError::New(env, "paddingSamples must be non-negative")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      config.padding_samples = static_cast<size_t>(padding_samples);
    }
    config.mode = parse_trim_silence_mode(options, config.mode);
    config.gate_lufs = repair_float_option(options, "gateLufs", config.gate_lufs);
    config.window_ms = repair_float_option(options, "windowMs", config.window_ms);
  }
  sonare::Audio audio = sonare::Audio::from_buffer(typed.Data(), typed.ElementLength(), sr);
  sonare::Audio result = sonare::mastering::repair::trim_silence(audio, config);
  std::vector<float> out(result.data(), result.data() + result.size());
  return VecToFloat32(env, out);
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
  int sr = info[1].As<Napi::Number>().Int32Value();
  float threshold_db =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().FloatValue() : -60.0f;

  sonare::Audio audio = sonare::Audio::from_buffer(data, length, sr);
  sonare::Audio result = sonare::trim_absolute(audio, threshold_db);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return VecToFloat32(env, out_vec);
  SONARE_NODE_CATCH(env)
}
