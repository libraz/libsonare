/// @file
/// @brief Node bindings for the mastering dynamics processors.

#include <cstddef>
#include <vector>

#include "core/audio.h"
#include "mastering/api/internal_processor_runner.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/gate.h"
#include "mastering/dynamics/transient_shaper.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

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
