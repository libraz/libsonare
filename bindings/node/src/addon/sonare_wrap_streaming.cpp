#include "sonare_wrap_streaming.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/audio.h"
#include "editing/voice_changer/realtime.h"
#include "mastering/api/chain.h"
#include "mastering/eq/band_strings.h"
#include "mastering/eq/eq_band.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/spectrum_engine.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_spectrum.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {

namespace {

// Flatten a nested or already-flat JS config object into dot-notation
// Param entries that sonare::mastering::api::parse_chain_config_params
// understands. Mirrors libsonare/python's _flatten_chain_config.
void FlattenChainConfig(const Napi::Object& object, const std::string& prefix,
                        std::vector<sonare::mastering::api::Param>* out) {
  Napi::Array names = object.GetPropertyNames();
  for (uint32_t index = 0; index < names.Length(); ++index) {
    Napi::Value key_value = names.Get(index);
    if (!key_value.IsString()) continue;
    std::string key = key_value.As<Napi::String>().Utf8Value();
    std::string full_key = prefix.empty() ? key : prefix + "." + key;

    // Top-level streaming-only options are read separately by the constructor
    // and are not chain config keys; skip them so parse_chain_config_params
    // does not reject them as unknown.
    if (prefix.empty() && (key == "loudnessStaticGainDb" || key == "loudnessStaticGainPeakDb")) {
      continue;
    }

    Napi::Value value = object.Get(key_value);
    if (value.IsObject() && !value.IsArray() && !value.IsBuffer() && !value.IsTypedArray() &&
        !value.IsFunction()) {
      FlattenChainConfig(value.As<Napi::Object>(), full_key, out);
    } else if (value.IsNumber()) {
      out->push_back({full_key, value.As<Napi::Number>().DoubleValue()});
    } else if (value.IsBoolean()) {
      out->push_back({full_key, value.As<Napi::Boolean>().Value() ? 1.0 : 0.0});
    }
  }
}

std::vector<sonare::mastering::api::Param> ParseChainConfigFromJs(const Napi::Value& value) {
  std::vector<sonare::mastering::api::Param> params;
  if (!value.IsObject()) return params;
  FlattenChainConfig(value.As<Napi::Object>(), "", &params);
  return params;
}

sonare::mastering::eq::EqBandType ParseBandType(const std::string& value) {
  const auto parsed = sonare::mastering::eq::band_type_from_string(value);
  if (!parsed) throw std::runtime_error("unknown EQ band type: " + value);
  return *parsed;
}

sonare::mastering::eq::BiquadCoeffMode ParseCoeffMode(const std::string& value) {
  const auto parsed = sonare::mastering::eq::coeff_mode_from_string(value);
  if (!parsed) throw std::runtime_error("unknown EQ coefficient mode: " + value);
  return *parsed;
}

sonare::mastering::eq::StereoPlacement ParsePlacement(const std::string& value) {
  const auto parsed = sonare::mastering::eq::placement_from_string(value);
  if (!parsed) throw std::runtime_error("unknown EQ placement: " + value);
  return *parsed;
}

sonare::mastering::eq::PhaseMode ParseBandPhase(const std::string& value) {
  const auto parsed = sonare::mastering::eq::phase_mode_from_string(value);
  if (!parsed) throw std::runtime_error("unknown EQ band phase mode: " + value);
  return *parsed;
}

sonare::mastering::eq::PhaseMode ParsePhaseModeInt(int mode) {
  const auto parsed = sonare::mastering::eq::phase_mode_from_int(mode);
  if (!parsed) throw std::runtime_error("unknown EQ phase mode");
  return *parsed;
}

sonare::mastering::eq::EqBand EqBandFromObject(const Napi::Object& object) {
  sonare::mastering::eq::EqBand band;
  band.type = ParseBandType(node_string_option(object, "type", "Peak"));
  band.coeff_mode = ParseCoeffMode(node_string_option(object, "coeffMode", "Rbj"));
  band.frequency_hz =
      static_cast<float>(node_double_option(object, "frequencyHz", band.frequency_hz));
  band.gain_db = static_cast<float>(node_double_option(object, "gainDb", band.gain_db));
  band.q = static_cast<float>(node_double_option(object, "q", band.q));
  band.enabled = node_bool_option(object, "enabled", band.enabled);
  band.slope_db_oct =
      static_cast<int>(std::lround(node_double_option(object, "slopeDbOct", band.slope_db_oct)));
  band.placement = ParsePlacement(node_string_option(object, "placement", "Stereo"));
  band.phase = ParseBandPhase(node_string_option(object, "phase", "Inherit"));
  band.soloed = node_bool_option(object, "soloed", band.soloed);
  band.bypassed = node_bool_option(object, "bypassed", band.bypassed);
  band.proportional_q = node_bool_option(object, "proportionalQ", band.proportional_q);
  band.proportional_q_strength = static_cast<float>(
      node_double_option(object, "proportionalQStrength", band.proportional_q_strength));
  band.dyn.enabled = node_bool_option(object, "dynamic", band.dyn.enabled);
  band.dyn.threshold_db =
      static_cast<float>(node_double_option(object, "thresholdDb", band.dyn.threshold_db));
  band.dyn.auto_threshold = node_bool_option(object, "autoThreshold", band.dyn.auto_threshold);
  band.dyn.ratio = static_cast<float>(node_double_option(object, "ratio", band.dyn.ratio));
  band.dyn.range_db = static_cast<float>(node_double_option(object, "rangeDb", band.dyn.range_db));
  band.dyn.attack_ms =
      static_cast<float>(node_double_option(object, "attackMs", band.dyn.attack_ms));
  band.dyn.release_ms =
      static_cast<float>(node_double_option(object, "releaseMs", band.dyn.release_ms));
  // "lookaheadMs" is the field's former (misleading) spelling; still accepted
  // so a stored config keeps working, but "detectorDelayMs" wins if both are
  // present.
  band.dyn.detector_delay_ms =
      static_cast<float>(node_double_option(object, "lookaheadMs", band.dyn.detector_delay_ms));
  band.dyn.detector_delay_ms =
      static_cast<float>(node_double_option(object, "detectorDelayMs", band.dyn.detector_delay_ms));
  band.dyn.external_sidechain =
      node_bool_option(object, "externalSidechain", band.dyn.external_sidechain);
  band.dyn.sidechain_freq_hz =
      static_cast<float>(node_double_option(object, "sidechainFreqHz", band.dyn.sidechain_freq_hz));
  band.dyn.sidechain_q =
      static_cast<float>(node_double_option(object, "sidechainQ", band.dyn.sidechain_q));
  return band;
}

}  // namespace

Napi::Object StreamingMasteringChainWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "StreamingMasteringChain",
      {
          InstanceMethod<&StreamingMasteringChainWrap::Prepare>("prepare"),
          InstanceMethod<&StreamingMasteringChainWrap::ProcessMono>("processMono"),
          InstanceMethod<&StreamingMasteringChainWrap::ProcessStereo>("processStereo"),
          InstanceMethod<&StreamingMasteringChainWrap::FlushMono>("flushMono"),
          InstanceMethod<&StreamingMasteringChainWrap::FlushStereo>("flushStereo"),
          InstanceMethod<&StreamingMasteringChainWrap::Reset>("reset"),
          InstanceMethod<&StreamingMasteringChainWrap::LatencySamples>("latencySamples"),
          InstanceMethod<&StreamingMasteringChainWrap::StageNames>("stageNames"),
          InstanceMethod<&StreamingMasteringChainWrap::Destroy>("destroy"),
      });

  exports.Set("StreamingMasteringChain", func);
  return exports;
}

StreamingMasteringChainWrap::StreamingMasteringChainWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<StreamingMasteringChainWrap>(info) {
  Napi::Env env = info.Env();

  std::vector<sonare::mastering::api::Param> params;
  sonare::mastering::api::StreamingMasteringChainOptions options;
  if (info.Length() >= 1 && info[0].IsObject()) {
    params = ParseChainConfigFromJs(info[0]);
    Napi::Object config_object = info[0].As<Napi::Object>();
    // The presence check the file-local reader used to do here was redundant:
    // the type-checked reader already returns the fallback it is handed for an
    // absent, undefined, null or wrong-typed value, and the fallback IS the
    // current field, so the assignment is a no-op in exactly those cases.
    options.loudness_static_gain_db = static_cast<float>(
        node_double_option(config_object, "loudnessStaticGainDb", options.loudness_static_gain_db));
    options.loudness_static_gain_peak_db = static_cast<float>(node_double_option(
        config_object, "loudnessStaticGainPeakDb", options.loudness_static_gain_peak_db));
  }

  try {
    auto config = sonare::mastering::api::parse_chain_config_params(params.data(), params.size());
    chain_ = std::make_unique<sonare::mastering::api::StreamingMasteringChain>(std::move(config),
                                                                               options);
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return;
  }
}

StreamingMasteringChainWrap::~StreamingMasteringChainWrap() = default;

// Releases the native chain up front rather than at GC. Every method already
// guards on a null chain_, so a call after destroy() throws instead of touching
// freed state, and a second destroy() is a no-op.
Napi::Value StreamingMasteringChainWrap::Destroy(const Napi::CallbackInfo& info) {
  chain_.reset();
  max_block_size_ = 0;
  return info.Env().Undefined();
}

Napi::Value StreamingMasteringChainWrap::Prepare(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!chain_) {
    Napi::Error::New(env, "StreamingMasteringChain is not initialized")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (sampleRate, maxBlockSize, numChannels)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  chain_->prepare(info[0].As<Napi::Number>().DoubleValue(), info[1].As<Napi::Number>().Int32Value(),
                  info[2].As<Napi::Number>().Int32Value());
  max_block_size_ = info[1].As<Napi::Number>().Int32Value();
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::ProcessMono(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!chain_) {
    Napi::Error::New(env, "StreamingMasteringChain is not initialized")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected (Float32Array)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array typed = info[0].As<Napi::Float32Array>();
  size_t length = typed.ElementLength();
  Napi::Float32Array out_arr = Napi::Float32Array::New(env, length);
  if (length > 0) {
    std::memcpy(out_arr.Data(), typed.Data(), length * sizeof(float));
    float* channels[] = {out_arr.Data()};
    chain_->process_block(channels, 1, static_cast<int>(length));
  }
  return out_arr;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::ProcessStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!chain_) {
    Napi::Error::New(env, "StreamingMasteringChain is not initialized")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "Expected (leftFloat32Array, rightFloat32Array)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array left = info[0].As<Napi::Float32Array>();
  Napi::Float32Array right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::TypeError::New(env, "left and right channel lengths must match")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  size_t length = left.ElementLength();
  Napi::Float32Array left_out = Napi::Float32Array::New(env, length);
  Napi::Float32Array right_out = Napi::Float32Array::New(env, length);
  if (length > 0) {
    std::memcpy(left_out.Data(), left.Data(), length * sizeof(float));
    std::memcpy(right_out.Data(), right.Data(), length * sizeof(float));
    float* channels[] = {left_out.Data(), right_out.Data()};
    chain_->process_block(channels, 2, static_cast<int>(length));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::FlushMono(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!chain_ || max_block_size_ <= 0) {
    Napi::Error::New(env, "StreamingMasteringChain must be prepared before flushMono()")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::vector<float> scratch(static_cast<size_t>(max_block_size_));
  float* channels[] = {scratch.data()};
  const int written = chain_->flush(channels, 1, max_block_size_);
  Napi::Float32Array out = Napi::Float32Array::New(env, static_cast<size_t>(written));
  if (written > 0) {
    std::memcpy(out.Data(), scratch.data(), static_cast<size_t>(written) * sizeof(float));
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::FlushStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!chain_ || max_block_size_ <= 0) {
    Napi::Error::New(env, "StreamingMasteringChain must be prepared before flushStereo()")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::vector<float> left_scratch(static_cast<size_t>(max_block_size_));
  std::vector<float> right_scratch(static_cast<size_t>(max_block_size_));
  float* channels[] = {left_scratch.data(), right_scratch.data()};
  const int written = chain_->flush(channels, 2, max_block_size_);
  Napi::Float32Array left = Napi::Float32Array::New(env, static_cast<size_t>(written));
  Napi::Float32Array right = Napi::Float32Array::New(env, static_cast<size_t>(written));
  if (written > 0) {
    const size_t byte_count = static_cast<size_t>(written) * sizeof(float);
    std::memcpy(left.Data(), left_scratch.data(), byte_count);
    std::memcpy(right.Data(), right_scratch.data(), byte_count);
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left);
  out.Set("right", right);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::Reset(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!chain_) {
    Napi::Error::New(env, "StreamingMasteringChain is not initialized")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  chain_->reset();
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::LatencySamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!chain_) {
    Napi::Error::New(env, "StreamingMasteringChain is not initialized")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, chain_->latency_samples());
}

Napi::Value StreamingMasteringChainWrap::StageNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!chain_) {
    Napi::Error::New(env, "StreamingMasteringChain is not initialized")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const auto& names = chain_->stage_names();
  Napi::Array out = Napi::Array::New(env, names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    out.Set(static_cast<uint32_t>(i), Napi::String::New(env, names[i]));
  }
  return out;
}

Napi::Object StreamingEqualizerWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "StreamingEqualizer",
      {
          InstanceMethod<&StreamingEqualizerWrap::SetBand>("setBand"),
          InstanceMethod<&StreamingEqualizerWrap::Clear>("clear"),
          InstanceMethod<&StreamingEqualizerWrap::SetPhaseMode>("setPhaseMode"),
          InstanceMethod<&StreamingEqualizerWrap::SetAutoGain>("setAutoGain"),
          InstanceMethod<&StreamingEqualizerWrap::SetGainScale>("setGainScale"),
          InstanceMethod<&StreamingEqualizerWrap::SetOutputGainDb>("setOutputGainDb"),
          InstanceMethod<&StreamingEqualizerWrap::SetOutputPan>("setOutputPan"),
          InstanceMethod<&StreamingEqualizerWrap::SetSidechainMono>("setSidechainMono"),
          InstanceMethod<&StreamingEqualizerWrap::SetSidechainStereo>("setSidechainStereo"),
          InstanceMethod<&StreamingEqualizerWrap::ClearSidechain>("clearSidechain"),
          InstanceMethod<&StreamingEqualizerWrap::LastAutoGainDb>("lastAutoGainDb"),
          InstanceMethod<&StreamingEqualizerWrap::LatencySamples>("latencySamples"),
          InstanceMethod<&StreamingEqualizerWrap::ProcessMono>("processMono"),
          InstanceMethod<&StreamingEqualizerWrap::ProcessStereo>("processStereo"),
          InstanceMethod<&StreamingEqualizerWrap::Spectrum>("spectrum"),
          InstanceMethod<&StreamingEqualizerWrap::Match>("match"),
          InstanceMethod<&StreamingEqualizerWrap::Destroy>("destroy"),
      });

  exports.Set("StreamingEqualizer", func);
  return exports;
}

StreamingEqualizerWrap::StreamingEqualizerWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<StreamingEqualizerWrap>(info) {
  Napi::Env env = info.Env();

  double sample_rate = 48000.0;
  int max_block_size = 512;
  if (info.Length() >= 1 && info[0].IsObject()) {
    Napi::Object config = info[0].As<Napi::Object>();
    sample_rate = node_double_option(config, "sampleRate", sample_rate);
    max_block_size =
        static_cast<int>(std::lround(node_double_option(config, "maxBlockSize", max_block_size)));
  }

  try {
    sonare::mastering::eq::EqualizerProcessorConfig eq_config;
    eq_config.max_channels = 2;
    eq_ = std::make_unique<sonare::mastering::eq::EqualizerProcessor>(eq_config);
    eq_->prepare(sample_rate, max_block_size);
    sample_rate_ = sample_rate;
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return;
  }
}

StreamingEqualizerWrap::~StreamingEqualizerWrap() = default;

// Releases the native processor and the retained sidechain buffers up front
// rather than at GC; every method already guards on a null eq_.
Napi::Value StreamingEqualizerWrap::Destroy(const Napi::CallbackInfo& info) {
  eq_.reset();
  sidechain_left_.clear();
  sidechain_right_.clear();
  sidechain_channels_ = {};
  return info.Env().Undefined();
}

Napi::Value StreamingEqualizerWrap::SetBand(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    Napi::Error::New(env, "StreamingEqualizer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 2 || !info[0].IsNumber() || !info[1].IsObject()) {
    Napi::TypeError::New(env, "Expected (index, bandObject)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  size_t index = static_cast<size_t>(info[0].As<Napi::Number>().Int32Value());
  sonare::mastering::eq::EqBand band = EqBandFromObject(info[1].As<Napi::Object>());
  eq_->set_band(index, band);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::Clear(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    Napi::Error::New(env, "StreamingEqualizer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  eq_->clear();
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::SetPhaseMode(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    Napi::Error::New(env, "StreamingEqualizer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (mode)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  eq_->set_phase_mode(ParsePhaseModeInt(info[0].As<Napi::Number>().Int32Value()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::SetAutoGain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    Napi::Error::New(env, "StreamingEqualizer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsBoolean()) {
    Napi::TypeError::New(env, "Expected (enabled)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  eq_->set_auto_gain_enabled(info[0].As<Napi::Boolean>().Value());
  return env.Undefined();
}

Napi::Value StreamingEqualizerWrap::SetGainScale(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    Napi::Error::New(env, "StreamingEqualizer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (scale)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  eq_->set_gain_scale(static_cast<float>(info[0].As<Napi::Number>().DoubleValue()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::SetOutputGainDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    Napi::Error::New(env, "StreamingEqualizer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (gainDb)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  eq_->set_output_gain_db(static_cast<float>(info[0].As<Napi::Number>().DoubleValue()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::SetOutputPan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    Napi::Error::New(env, "StreamingEqualizer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (pan)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  eq_->set_output_pan(static_cast<float>(info[0].As<Napi::Number>().DoubleValue()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::SetSidechainMono(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    Napi::Error::New(env, "StreamingEqualizer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected (Float32Array)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  // Copy before anything else: the core holds the sidechain pointers across
  // every later process() call, and a JS ArrayBuffer can be detached or
  // transferred the moment this setter returns.
  Napi::Float32Array key = info[0].As<Napi::Float32Array>();
  std::vector<float> staged(key.Data(), key.Data() + key.ElementLength());
  if (staged.empty()) {
    ClearSidechainStorage();
    return env.Undefined();
  }
  sonare::validate_offline_audio_input(staged.data(), staged.size(),
                                       static_cast<int>(std::lround(sample_rate_)));

  sidechain_left_.swap(staged);
  sidechain_right_.clear();
  sidechain_channels_ = {sidechain_left_.data(), nullptr};
  eq_->set_sidechain(sidechain_channels_.data(), 1, static_cast<int>(sidechain_left_.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::SetSidechainStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    Napi::Error::New(env, "StreamingEqualizer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "Expected (leftFloat32Array, rightFloat32Array)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array left = info[0].As<Napi::Float32Array>();
  Napi::Float32Array right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::TypeError::New(env, "left and right sidechain lengths must match")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // Copy both channels before the core is given any pointer, for the same
  // detach reason as the mono setter.
  std::vector<float> staged_left(left.Data(), left.Data() + left.ElementLength());
  std::vector<float> staged_right(right.Data(), right.Data() + right.ElementLength());
  if (staged_left.empty()) {
    ClearSidechainStorage();
    return env.Undefined();
  }
  const int sr = static_cast<int>(std::lround(sample_rate_));
  sonare::validate_offline_audio_input(staged_left.data(), staged_left.size(), sr);
  sonare::validate_offline_audio_input(staged_right.data(), staged_right.size(), sr);

  sidechain_left_.swap(staged_left);
  sidechain_right_.swap(staged_right);
  sidechain_channels_ = {sidechain_left_.data(), sidechain_right_.data()};
  eq_->set_sidechain(sidechain_channels_.data(), 2, static_cast<int>(sidechain_left_.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

// Drops the core's borrowed pointers first, then the storage they pointed at.
void StreamingEqualizerWrap::ClearSidechainStorage() {
  if (eq_) eq_->clear_sidechain();
  sidechain_left_.clear();
  sidechain_right_.clear();
  sidechain_channels_ = {};
}

Napi::Value StreamingEqualizerWrap::ClearSidechain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    Napi::Error::New(env, "StreamingEqualizer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ClearSidechainStorage();
  return env.Undefined();
}

Napi::Value StreamingEqualizerWrap::LastAutoGainDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    Napi::Error::New(env, "StreamingEqualizer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, eq_->last_auto_gain_db());
}

Napi::Value StreamingEqualizerWrap::LatencySamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    Napi::Error::New(env, "StreamingEqualizer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, eq_->latency_samples());
}

Napi::Value StreamingEqualizerWrap::ProcessMono(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    Napi::Error::New(env, "StreamingEqualizer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected (Float32Array)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array typed = info[0].As<Napi::Float32Array>();
  size_t length = typed.ElementLength();
  Napi::Float32Array out_arr = Napi::Float32Array::New(env, length);
  if (length > 0) {
    std::memcpy(out_arr.Data(), typed.Data(), length * sizeof(float));
    float* channels[] = {out_arr.Data()};
    eq_->process(channels, 1, static_cast<int>(length));
  }
  return out_arr;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::ProcessStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    Napi::Error::New(env, "StreamingEqualizer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "Expected (leftFloat32Array, rightFloat32Array)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array left = info[0].As<Napi::Float32Array>();
  Napi::Float32Array right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::TypeError::New(env, "left and right channel lengths must match")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  size_t length = left.ElementLength();
  Napi::Float32Array left_out = Napi::Float32Array::New(env, length);
  Napi::Float32Array right_out = Napi::Float32Array::New(env, length);
  if (length > 0) {
    std::memcpy(left_out.Data(), left.Data(), length * sizeof(float));
    std::memcpy(right_out.Data(), right.Data(), length * sizeof(float));
    float* channels[] = {left_out.Data(), right_out.Data()};
    eq_->process(channels, 2, static_cast<int>(length));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::Spectrum(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    Napi::Error::New(env, "StreamingEqualizer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  sonare::mastering::eq::EqualizerSpectrumSnapshot snapshot = eq_->spectrum_snapshot();

  auto make_channel = [&env](
                          const std::array<sonare::mastering::eq::SpectrumPoint,
                                           sonare::mastering::eq::kSpectrumStreamCapacity>& points,
                          size_t count, bool right) -> Napi::Float32Array {
    Napi::Float32Array arr = Napi::Float32Array::New(env, count);
    for (size_t i = 0; i < count; ++i) {
      arr[i] = right ? points[i].right : points[i].left;
    }
    return arr;
  };

  Napi::Object out = Napi::Object::New(env);
  out.Set("preLeft", make_channel(snapshot.pre, snapshot.pre_count, false));
  out.Set("preRight", make_channel(snapshot.pre, snapshot.pre_count, true));
  out.Set("postLeft", make_channel(snapshot.post, snapshot.post_count, false));
  out.Set("postRight", make_channel(snapshot.post, snapshot.post_count, true));

  Napi::Float32Array band_gain = Napi::Float32Array::New(env, snapshot.band_gain_db.size());
  for (size_t i = 0; i < snapshot.band_gain_db.size(); ++i) {
    band_gain[i] = snapshot.band_gain_db[i];
  }
  out.Set("bandGainDb", band_gain);

  Napi::Float32Array profile = Napi::Float32Array::New(env, snapshot.profile_db.size());
  for (size_t i = 0; i < snapshot.profile_db.size(); ++i) {
    profile[i] = snapshot.profile_db[i];
  }
  out.Set("profileDb", profile);
  out.Set("lastAutoGainDb", Napi::Number::New(env, eq_->last_auto_gain_db()));
  out.Set("seq", Napi::Number::New(env, static_cast<double>(snapshot.seq)));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::Match(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    Napi::Error::New(env, "StreamingEqualizer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "Expected (sourceFloat32Array, referenceFloat32Array, options?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int sample_rate = static_cast<int>(std::lround(sample_rate_));
  size_t max_bands = 8;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    sample_rate =
        static_cast<int>(std::lround(node_double_option(options, "sampleRate", sample_rate)));
    max_bands =
        static_cast<size_t>(std::lround(node_double_option(options, "maxBands", max_bands)));
  }
  SONARE_NODE_TRY
  Napi::Float32Array source = info[0].As<Napi::Float32Array>();
  Napi::Float32Array reference = info[1].As<Napi::Float32Array>();
  sonare::validate_offline_audio_input(source.Data(), source.ElementLength(), sample_rate);
  sonare::validate_offline_audio_input(reference.Data(), reference.ElementLength(), sample_rate);
  if (max_bands == 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "StreamingEqualizer.match: maxBands must be positive");
  }
  sonare::Audio source_audio =
      sonare::Audio::from_buffer(source.Data(), source.ElementLength(), sample_rate);
  sonare::Audio reference_audio =
      sonare::Audio::from_buffer(reference.Data(), reference.ElementLength(), sample_rate);
  sonare::mastering::match::MatchEqConfig match_config;
  match_config.max_bands = max_bands;
  sonare::mastering::match::configure_equalizer_from_match(
      *eq_, sonare::mastering::match::reference_spectrum(source_audio),
      sonare::mastering::match::reference_spectrum(reference_audio), match_config);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

// ===========================================================================
// StreamingRetune
// ===========================================================================

namespace {

// Reads the three scalar controls out of a JS options object using ONLY the
// shared node_*_option family (type-checked fallback), so this file keeps no
// reader of its own -- the reader-shape scan in the Node test suite rejects one.
void ReadRetuneConfig(const Napi::Value& value, float* semitones, float* mix, int* grain_size) {
  if (!value.IsObject()) return;
  const Napi::Object object = value.As<Napi::Object>();
  *semitones = node_float_option(object, "semitones", *semitones);
  *mix = node_float_option(object, "mix", *mix);
  // "grain_size" is the snake_case spelling the WASM surface also accepts; the
  // camelCase key wins when both are present.
  *grain_size =
      node_int_option(object, "grainSize", node_int_option(object, "grain_size", *grain_size));
}

}  // namespace

Napi::Object StreamingRetuneWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func =
      DefineClass(env, "StreamingRetune",
                  {
                      InstanceMethod<&StreamingRetuneWrap::Prepare>("prepare"),
                      InstanceMethod<&StreamingRetuneWrap::Reset>("reset"),
                      InstanceMethod<&StreamingRetuneWrap::SetConfig>("setConfig"),
                      InstanceMethod<&StreamingRetuneWrap::Config>("config"),
                      InstanceMethod<&StreamingRetuneWrap::GrainSize>("grainSize"),
                      InstanceMethod<&StreamingRetuneWrap::LatencySamples>("latencySamples"),
                      InstanceMethod<&StreamingRetuneWrap::ProcessMono>("processMono"),
                      InstanceMethod<&StreamingRetuneWrap::Destroy>("destroy"),
                  });

  exports.Set("StreamingRetune", func);
  return exports;
}

StreamingRetuneWrap::StreamingRetuneWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<StreamingRetuneWrap>(info) {
  Napi::Env env = info.Env();
  float semitones = 0.0f;
  float mix = 1.0f;
  int grain_size = 0;
  if (info.Length() >= 1) {
    ReadRetuneConfig(info[0], &semitones, &mix, &grain_size);
  }
  requested_grain_size_ = grain_size;
  retune_ = sonare_streaming_retune_create(semitones, mix, grain_size);
  if (retune_ == nullptr) {
    ThrowSonareError(env, SONARE_ERROR_INVALID_PARAMETER, "StreamingRetune: ");
  }
}

StreamingRetuneWrap::~StreamingRetuneWrap() {
  sonare_streaming_retune_destroy(retune_);
  retune_ = nullptr;
}

// Releases the native handle now instead of at GC; every method below guards on
// a null retune_, so a call after destroy() is a clean InvalidParameter.
Napi::Value StreamingRetuneWrap::Destroy(const Napi::CallbackInfo& info) {
  sonare_streaming_retune_destroy(retune_);
  retune_ = nullptr;
  return info.Env().Undefined();
}

Napi::Value StreamingRetuneWrap::Prepare(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  double sample_rate = 0.0;
  int max_block_size = 0;
  if (!OptionalDoubleArg(env, info, 0, "sampleRate", 0.0, &sample_rate) ||
      !OptionalIntArg(env, info, 1, "maxBlockSize", 0, &max_block_size)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_streaming_retune_prepare(retune_, sample_rate, max_block_size));
  return env.Undefined();
}

Napi::Value StreamingRetuneWrap::Reset(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  ThrowIfError(env, sonare_streaming_retune_reset(retune_));
  return env.Undefined();
}

Napi::Value StreamingRetuneWrap::SetConfig(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  // Seed from the currently applied values so an options bag carrying only one
  // key leaves the others alone, matching every other setConfig on this surface.
  float semitones = 0.0f;
  float mix = 1.0f;
  int effective_grain_size = 0;
  ThrowIfError(env,
               sonare_streaming_retune_config(retune_, &semitones, &mix, &effective_grain_size));
  if (env.IsExceptionPending()) return env.Undefined();
  // grainSize is the exception: it is seeded from the last REQUESTED value, not
  // from the effective one config() reports, so an omitted key preserves the 0
  // sentinel and the next prepare re-derives the grain from its sample rate.
  int grain_size = requested_grain_size_;
  if (info.Length() >= 1) {
    ReadRetuneConfig(info[0], &semitones, &mix, &grain_size);
  }
  ThrowIfError(env, sonare_streaming_retune_set_config(retune_, semitones, mix, grain_size));
  if (env.IsExceptionPending()) return env.Undefined();
  requested_grain_size_ = grain_size;
  return env.Undefined();
}

Napi::Value StreamingRetuneWrap::Config(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  float semitones = 0.0f;
  float mix = 0.0f;
  int grain_size = 0;
  ThrowIfError(env, sonare_streaming_retune_config(retune_, &semitones, &mix, &grain_size));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  out.Set("semitones", Napi::Number::New(env, semitones));
  out.Set("mix", Napi::Number::New(env, mix));
  out.Set("grainSize", Napi::Number::New(env, grain_size));
  return out;
}

Napi::Value StreamingRetuneWrap::GrainSize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int grain_size = 0;
  ThrowIfError(env, sonare_streaming_retune_grain_size(retune_, &grain_size));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, grain_size);
}

Napi::Value StreamingRetuneWrap::LatencySamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int latency = 0;
  ThrowIfError(env, sonare_streaming_retune_latency_samples(retune_, &latency));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, latency);
}

Napi::Value StreamingRetuneWrap::ProcessMono(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "StreamingRetune.processMono expects a Float32Array")) {
    return env.Undefined();
  }
  Napi::Float32Array input = info[0].As<Napi::Float32Array>();
  // The C entry point works in place, so copy the caller's samples into the
  // result buffer first and never mutate the argument.
  Napi::Float32Array out = Napi::Float32Array::New(env, input.ElementLength());
  std::memcpy(out.Data(), input.Data(), input.ElementLength() * sizeof(float));
  ThrowIfError(env, sonare_streaming_retune_process_mono(retune_, out.Data(), out.ElementLength()));
  if (env.IsExceptionPending()) return env.Undefined();
  return out;
}

}  // namespace sonare_node
