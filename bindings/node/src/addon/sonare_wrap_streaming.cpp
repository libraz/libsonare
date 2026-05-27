#include "sonare_wrap_streaming.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/audio.h"
#include "mastering/api/chain.h"
#include "mastering/eq/eq_band.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/spectrum_engine.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_spectrum.h"
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

bool HasKey(const Napi::Object& object, const char* key) {
  return object.Has(key) && !object.Get(key).IsUndefined() && !object.Get(key).IsNull();
}

double NumberKey(const Napi::Object& object, const char* key, double fallback) {
  if (!HasKey(object, key)) return fallback;
  Napi::Value value = object.Get(key);
  if (!value.IsNumber()) return fallback;
  return value.As<Napi::Number>().DoubleValue();
}

bool BoolKey(const Napi::Object& object, const char* key, bool fallback) {
  if (!HasKey(object, key)) return fallback;
  Napi::Value value = object.Get(key);
  if (!value.IsBoolean()) return fallback;
  return value.As<Napi::Boolean>().Value();
}

std::string StringKey(const Napi::Object& object, const char* key, const char* fallback) {
  if (!HasKey(object, key)) return fallback;
  Napi::Value value = object.Get(key);
  if (!value.IsString()) return fallback;
  return value.As<Napi::String>().Utf8Value();
}

sonare::mastering::eq::EqBandType ParseBandType(const std::string& value) {
  using sonare::mastering::eq::EqBandType;
  if (value == "Peak" || value == "peak") return EqBandType::Peak;
  if (value == "LowShelf" || value == "lowShelf") return EqBandType::LowShelf;
  if (value == "HighShelf" || value == "highShelf") return EqBandType::HighShelf;
  if (value == "LowPass" || value == "lowPass") return EqBandType::LowPass;
  if (value == "HighPass" || value == "highPass") return EqBandType::HighPass;
  if (value == "BandPass" || value == "bandPass") return EqBandType::BandPass;
  if (value == "Notch" || value == "notch") return EqBandType::Notch;
  if (value == "TiltShelf" || value == "tiltShelf") return EqBandType::TiltShelf;
  if (value == "FlatTilt" || value == "flatTilt") return EqBandType::FlatTilt;
  throw std::runtime_error("unknown EQ band type: " + value);
}

sonare::mastering::eq::BiquadCoeffMode ParseCoeffMode(const std::string& value) {
  using sonare::mastering::eq::BiquadCoeffMode;
  if (value == "Rbj" || value == "RBJ" || value == "rbj") return BiquadCoeffMode::Rbj;
  if (value == "Vicanek" || value == "vicanek") return BiquadCoeffMode::Vicanek;
  throw std::runtime_error("unknown EQ coefficient mode: " + value);
}

sonare::mastering::eq::StereoPlacement ParsePlacement(const std::string& value) {
  using sonare::mastering::eq::StereoPlacement;
  if (value == "Stereo" || value == "stereo") return StereoPlacement::Stereo;
  if (value == "Left" || value == "left") return StereoPlacement::Left;
  if (value == "Right" || value == "right") return StereoPlacement::Right;
  if (value == "Mid" || value == "mid") return StereoPlacement::Mid;
  if (value == "Side" || value == "side") return StereoPlacement::Side;
  throw std::runtime_error("unknown EQ placement: " + value);
}

sonare::mastering::eq::PhaseMode ParseBandPhase(const std::string& value) {
  using sonare::mastering::eq::PhaseMode;
  if (value == "Inherit" || value == "inherit") return PhaseMode::Inherit;
  if (value == "ZeroLatency" || value == "zeroLatency") return PhaseMode::ZeroLatency;
  if (value == "NaturalPhase" || value == "naturalPhase") return PhaseMode::NaturalPhase;
  if (value == "LinearPhase" || value == "linearPhase") return PhaseMode::LinearPhase;
  throw std::runtime_error("unknown EQ band phase mode: " + value);
}

sonare::mastering::eq::PhaseMode ParsePhaseModeInt(int mode) {
  using sonare::mastering::eq::PhaseMode;
  switch (mode) {
    case 1:
      return PhaseMode::ZeroLatency;
    case 2:
      return PhaseMode::NaturalPhase;
    case 3:
      return PhaseMode::LinearPhase;
    default:
      throw std::runtime_error("unknown EQ phase mode");
  }
}

sonare::mastering::eq::EqBand EqBandFromObject(const Napi::Object& object) {
  sonare::mastering::eq::EqBand band;
  band.type = ParseBandType(StringKey(object, "type", "Peak"));
  band.coeff_mode = ParseCoeffMode(StringKey(object, "coeffMode", "Rbj"));
  band.frequency_hz = static_cast<float>(NumberKey(object, "frequencyHz", band.frequency_hz));
  band.gain_db = static_cast<float>(NumberKey(object, "gainDb", band.gain_db));
  band.q = static_cast<float>(NumberKey(object, "q", band.q));
  band.enabled = BoolKey(object, "enabled", band.enabled);
  band.slope_db_oct =
      static_cast<int>(std::lround(NumberKey(object, "slopeDbOct", band.slope_db_oct)));
  band.placement = ParsePlacement(StringKey(object, "placement", "Stereo"));
  band.phase = ParseBandPhase(StringKey(object, "phase", "Inherit"));
  band.soloed = BoolKey(object, "soloed", band.soloed);
  band.bypassed = BoolKey(object, "bypassed", band.bypassed);
  band.proportional_q = BoolKey(object, "proportionalQ", band.proportional_q);
  band.proportional_q_strength =
      static_cast<float>(NumberKey(object, "proportionalQStrength", band.proportional_q_strength));
  band.dyn.enabled = BoolKey(object, "dynamic", band.dyn.enabled);
  band.dyn.threshold_db =
      static_cast<float>(NumberKey(object, "thresholdDb", band.dyn.threshold_db));
  band.dyn.auto_threshold = BoolKey(object, "autoThreshold", band.dyn.auto_threshold);
  band.dyn.ratio = static_cast<float>(NumberKey(object, "ratio", band.dyn.ratio));
  band.dyn.range_db = static_cast<float>(NumberKey(object, "rangeDb", band.dyn.range_db));
  band.dyn.attack_ms = static_cast<float>(NumberKey(object, "attackMs", band.dyn.attack_ms));
  band.dyn.release_ms = static_cast<float>(NumberKey(object, "releaseMs", band.dyn.release_ms));
  band.dyn.lookahead_ms =
      static_cast<float>(NumberKey(object, "lookaheadMs", band.dyn.lookahead_ms));
  band.dyn.external_sidechain = BoolKey(object, "externalSidechain", band.dyn.external_sidechain);
  band.dyn.sidechain_freq_hz =
      static_cast<float>(NumberKey(object, "sidechainFreqHz", band.dyn.sidechain_freq_hz));
  band.dyn.sidechain_q = static_cast<float>(NumberKey(object, "sidechainQ", band.dyn.sidechain_q));
  return band;
}

}  // namespace

Napi::FunctionReference StreamingMasteringChainWrap::constructor_;

Napi::Object StreamingMasteringChainWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "StreamingMasteringChain",
      {
          InstanceMethod<&StreamingMasteringChainWrap::Prepare>("prepare"),
          InstanceMethod<&StreamingMasteringChainWrap::ProcessMono>("processMono"),
          InstanceMethod<&StreamingMasteringChainWrap::ProcessStereo>("processStereo"),
          InstanceMethod<&StreamingMasteringChainWrap::Reset>("reset"),
          InstanceMethod<&StreamingMasteringChainWrap::LatencySamples>("latencySamples"),
          InstanceMethod<&StreamingMasteringChainWrap::StageNames>("stageNames"),
      });

  constructor_ = Napi::Persistent(func);
  constructor_.SuppressDestruct();
  exports.Set("StreamingMasteringChain", func);
  return exports;
}

StreamingMasteringChainWrap::StreamingMasteringChainWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<StreamingMasteringChainWrap>(info) {
  Napi::Env env = info.Env();

  std::vector<sonare::mastering::api::Param> params;
  if (info.Length() >= 1 && info[0].IsObject()) {
    params = ParseChainConfigFromJs(info[0]);
  }

  try {
    auto config = sonare::mastering::api::parse_chain_config_params(params.data(), params.size());
    chain_ = std::make_unique<sonare::mastering::api::StreamingMasteringChain>(std::move(config));
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return;
  }
}

StreamingMasteringChainWrap::~StreamingMasteringChainWrap() = default;

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
    return Napi::Number::New(env, 0);
  }
  return Napi::Number::New(env, chain_->latency_samples());
}

Napi::Value StreamingMasteringChainWrap::StageNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!chain_) {
    return Napi::Array::New(env, 0);
  }
  const auto& names = chain_->stage_names();
  Napi::Array out = Napi::Array::New(env, names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    out.Set(static_cast<uint32_t>(i), Napi::String::New(env, names[i]));
  }
  return out;
}

Napi::FunctionReference StreamingEqualizerWrap::constructor_;

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
      });

  constructor_ = Napi::Persistent(func);
  constructor_.SuppressDestruct();
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
    sample_rate = NumberKey(config, "sampleRate", sample_rate);
    max_block_size =
        static_cast<int>(std::lround(NumberKey(config, "maxBlockSize", max_block_size)));
  }

  try {
    sonare::mastering::eq::EqualizerProcessorConfig eq_config;
    eq_config.max_channels = 2;
    eq_ = std::make_unique<sonare::mastering::eq::EqualizerProcessor>(eq_config);
    eq_->prepare(sample_rate, max_block_size);
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return;
  }
}

StreamingEqualizerWrap::~StreamingEqualizerWrap() = default;

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
  sidechain_left_.Reset();
  sidechain_right_.Reset();
  Napi::Float32Array key = info[0].As<Napi::Float32Array>();
  sidechain_left_ = Napi::Persistent(key);
  sidechain_channels_[0] = key.Data();
  sidechain_channels_[1] = nullptr;
  eq_->set_sidechain(sidechain_channels_.data(), 1, static_cast<int>(key.ElementLength()));
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
  sidechain_left_.Reset();
  sidechain_right_.Reset();
  sidechain_left_ = Napi::Persistent(left);
  sidechain_right_ = Napi::Persistent(right);
  sidechain_channels_[0] = left.Data();
  sidechain_channels_[1] = right.Data();
  eq_->set_sidechain(sidechain_channels_.data(), 2, static_cast<int>(left.ElementLength()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::ClearSidechain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (eq_) {
    eq_->clear_sidechain();
  }
  sidechain_left_.Reset();
  sidechain_right_.Reset();
  sidechain_channels_ = {};
  return env.Undefined();
}

Napi::Value StreamingEqualizerWrap::LastAutoGainDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    return Napi::Number::New(env, 0);
  }
  return Napi::Number::New(env, eq_->last_auto_gain_db());
}

Napi::Value StreamingEqualizerWrap::LatencySamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!eq_) {
    return Napi::Number::New(env, 0);
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
  int sample_rate = 48000;
  size_t max_bands = 8;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    sample_rate = static_cast<int>(std::lround(NumberKey(options, "sampleRate", sample_rate)));
    max_bands = static_cast<size_t>(std::lround(NumberKey(options, "maxBands", max_bands)));
  }
  SONARE_NODE_TRY
  Napi::Float32Array source = info[0].As<Napi::Float32Array>();
  Napi::Float32Array reference = info[1].As<Napi::Float32Array>();
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

// ============================================================================
// StreamAnalyzer
// ============================================================================

namespace {

Napi::Float32Array Float32FromVec(Napi::Env env, const std::vector<float>& vec) {
  Napi::Float32Array out = Napi::Float32Array::New(env, vec.size());
  if (!vec.empty()) {
    std::memcpy(out.Data(), vec.data(), vec.size() * sizeof(float));
  }
  return out;
}

Napi::Int32Array Int32FromVec(Napi::Env env, const std::vector<int>& vec) {
  Napi::Int32Array out = Napi::Int32Array::New(env, vec.size());
  for (size_t i = 0; i < vec.size(); ++i) {
    out[i] = vec[i];
  }
  return out;
}

Napi::Uint8Array Uint8FromVec(Napi::Env env, const std::vector<uint8_t>& vec) {
  Napi::Uint8Array out = Napi::Uint8Array::New(env, vec.size());
  if (!vec.empty()) {
    std::memcpy(out.Data(), vec.data(), vec.size() * sizeof(uint8_t));
  }
  return out;
}

Napi::Int16Array Int16FromVec(Napi::Env env, const std::vector<int16_t>& vec) {
  Napi::Int16Array out = Napi::Int16Array::New(env, vec.size());
  if (!vec.empty()) {
    std::memcpy(out.Data(), vec.data(), vec.size() * sizeof(int16_t));
  }
  return out;
}

}  // namespace

Napi::FunctionReference StreamAnalyzerWrap::constructor_;

Napi::Object StreamAnalyzerWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "StreamAnalyzer",
      {
          InstanceMethod<&StreamAnalyzerWrap::Process>("process"),
          InstanceMethod<&StreamAnalyzerWrap::ProcessWithOffset>("processWithOffset"),
          InstanceMethod<&StreamAnalyzerWrap::AvailableFrames>("availableFrames"),
          InstanceMethod<&StreamAnalyzerWrap::ReadFramesSoa>("readFramesSoa"),
          InstanceMethod<&StreamAnalyzerWrap::ReadFramesU8>("readFramesU8"),
          InstanceMethod<&StreamAnalyzerWrap::ReadFramesI16>("readFramesI16"),
          InstanceMethod<&StreamAnalyzerWrap::Reset>("reset"),
          InstanceMethod<&StreamAnalyzerWrap::Stats>("stats"),
          InstanceMethod<&StreamAnalyzerWrap::FrameCount>("frameCount"),
          InstanceMethod<&StreamAnalyzerWrap::CurrentTime>("currentTime"),
          InstanceMethod<&StreamAnalyzerWrap::SampleRate>("sampleRate"),
          InstanceMethod<&StreamAnalyzerWrap::SetExpectedDuration>("setExpectedDuration"),
          InstanceMethod<&StreamAnalyzerWrap::SetNormalizationGain>("setNormalizationGain"),
          InstanceMethod<&StreamAnalyzerWrap::SetTuningRefHz>("setTuningRefHz"),
      });

  constructor_ = Napi::Persistent(func);
  constructor_.SuppressDestruct();
  exports.Set("StreamAnalyzer", func);
  return exports;
}

StreamAnalyzerWrap::StreamAnalyzerWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<StreamAnalyzerWrap>(info) {
  Napi::Env env = info.Env();

  sonare::StreamConfig config;
  if (info.Length() >= 1 && info[0].IsObject()) {
    Napi::Object opts = info[0].As<Napi::Object>();
    config.sample_rate = static_cast<int>(NumberKey(opts, "sampleRate", config.sample_rate));
    config.n_fft = static_cast<int>(NumberKey(opts, "nFft", config.n_fft));
    config.hop_length = static_cast<int>(NumberKey(opts, "hopLength", config.hop_length));
    config.n_mels = static_cast<int>(NumberKey(opts, "nMels", config.n_mels));
    config.fmin = static_cast<float>(NumberKey(opts, "fmin", config.fmin));
    config.fmax = static_cast<float>(NumberKey(opts, "fmax", config.fmax));
    config.tuning_ref_hz = static_cast<float>(NumberKey(opts, "tuningRefHz", config.tuning_ref_hz));
    config.compute_magnitude = BoolKey(opts, "computeMagnitude", config.compute_magnitude);
    config.compute_mel = BoolKey(opts, "computeMel", config.compute_mel);
    config.compute_chroma = BoolKey(opts, "computeChroma", config.compute_chroma);
    config.compute_onset = BoolKey(opts, "computeOnset", config.compute_onset);
    config.compute_spectral = BoolKey(opts, "computeSpectral", config.compute_spectral);
    config.emit_every_n_frames =
        static_cast<int>(NumberKey(opts, "emitEveryNFrames", config.emit_every_n_frames));
    config.magnitude_downsample =
        static_cast<int>(NumberKey(opts, "magnitudeDownsample", config.magnitude_downsample));
    config.key_update_interval_sec =
        static_cast<float>(NumberKey(opts, "keyUpdateIntervalSec", config.key_update_interval_sec));
    config.bpm_update_interval_sec =
        static_cast<float>(NumberKey(opts, "bpmUpdateIntervalSec", config.bpm_update_interval_sec));
    const int window = static_cast<int>(NumberKey(opts, "window", static_cast<int>(config.window)));
    config.window = window == 1   ? sonare::WindowType::Hamming
                    : window == 2 ? sonare::WindowType::Blackman
                    : window == 3 ? sonare::WindowType::Rectangular
                                  : sonare::WindowType::Hann;
    const int output_format =
        static_cast<int>(NumberKey(opts, "outputFormat", static_cast<int>(config.output_format)));
    config.output_format = output_format == 1   ? sonare::OutputFormat::Int16
                           : output_format == 2 ? sonare::OutputFormat::Uint8
                                                : sonare::OutputFormat::Float32;
  }

  config_ = config;
  try {
    analyzer_ = std::make_unique<sonare::StreamAnalyzer>(config_);
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return;
  }
}

StreamAnalyzerWrap::~StreamAnalyzerWrap() = default;

Napi::Value StreamAnalyzerWrap::Process(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected (Float32Array)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array typed = info[0].As<Napi::Float32Array>();
  analyzer_->process(typed.Data(), typed.ElementLength());
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::ProcessWithOffset(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleOffset)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array typed = info[0].As<Napi::Float32Array>();
  size_t offset = static_cast<size_t>(info[1].As<Napi::Number>().Int64Value());
  analyzer_->process(typed.Data(), typed.ElementLength(), offset);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::AvailableFrames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(analyzer_->available_frames()));
}

Napi::Value StreamAnalyzerWrap::ReadFramesSoa(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (maxFrames)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  size_t max_frames = static_cast<size_t>(info[0].As<Napi::Number>().Int64Value());
  sonare::FrameBuffer buffer;
  analyzer_->read_frames_soa(max_frames, buffer);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nFrames", Napi::Number::New(env, static_cast<double>(buffer.n_frames)));
  out.Set("timestamps", Float32FromVec(env, buffer.timestamps));
  out.Set("mel", Float32FromVec(env, buffer.mel));
  out.Set("chroma", Float32FromVec(env, buffer.chroma));
  out.Set("onsetStrength", Float32FromVec(env, buffer.onset_strength));
  out.Set("rmsEnergy", Float32FromVec(env, buffer.rms_energy));
  out.Set("spectralCentroid", Float32FromVec(env, buffer.spectral_centroid));
  out.Set("spectralFlatness", Float32FromVec(env, buffer.spectral_flatness));
  out.Set("chordRoot", Int32FromVec(env, buffer.chord_root));
  out.Set("chordQuality", Int32FromVec(env, buffer.chord_quality));
  out.Set("chordConfidence", Float32FromVec(env, buffer.chord_confidence));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::ReadFramesU8(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (maxFrames)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  size_t max_frames = static_cast<size_t>(info[0].As<Napi::Number>().Int64Value());
  sonare::QuantizedFrameBufferU8 buffer;
  sonare::QuantizeConfig qconfig;
  analyzer_->read_frames_quantized_u8(max_frames, buffer, qconfig);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nFrames", Napi::Number::New(env, static_cast<double>(buffer.n_frames)));
  out.Set("nMels", Napi::Number::New(env, buffer.n_mels));
  out.Set("timestamps", Float32FromVec(env, buffer.timestamps));
  out.Set("mel", Uint8FromVec(env, buffer.mel));
  out.Set("chroma", Uint8FromVec(env, buffer.chroma));
  out.Set("onsetStrength", Uint8FromVec(env, buffer.onset_strength));
  out.Set("rmsEnergy", Uint8FromVec(env, buffer.rms_energy));
  out.Set("spectralCentroid", Uint8FromVec(env, buffer.spectral_centroid));
  out.Set("spectralFlatness", Uint8FromVec(env, buffer.spectral_flatness));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::ReadFramesI16(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (maxFrames)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  size_t max_frames = static_cast<size_t>(info[0].As<Napi::Number>().Int64Value());
  sonare::QuantizedFrameBufferI16 buffer;
  sonare::QuantizeConfig qconfig;
  analyzer_->read_frames_quantized_i16(max_frames, buffer, qconfig);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nFrames", Napi::Number::New(env, static_cast<double>(buffer.n_frames)));
  out.Set("nMels", Napi::Number::New(env, buffer.n_mels));
  out.Set("timestamps", Float32FromVec(env, buffer.timestamps));
  out.Set("mel", Int16FromVec(env, buffer.mel));
  out.Set("chroma", Int16FromVec(env, buffer.chroma));
  out.Set("onsetStrength", Int16FromVec(env, buffer.onset_strength));
  out.Set("rmsEnergy", Int16FromVec(env, buffer.rms_energy));
  out.Set("spectralCentroid", Int16FromVec(env, buffer.spectral_centroid));
  out.Set("spectralFlatness", Int16FromVec(env, buffer.spectral_flatness));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::Reset(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  size_t base_offset = info.Length() >= 1 && info[0].IsNumber()
                           ? static_cast<size_t>(info[0].As<Napi::Number>().Int64Value())
                           : 0;
  analyzer_->reset(base_offset);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::Stats(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  sonare::AnalyzerStats s = analyzer_->stats();

  Napi::Object out = Napi::Object::New(env);
  out.Set("totalFrames", Napi::Number::New(env, s.total_frames));
  out.Set("totalSamples", Napi::Number::New(env, static_cast<double>(s.total_samples)));
  out.Set("durationSeconds", Napi::Number::New(env, s.duration_seconds));

  const sonare::ProgressiveEstimate& est = s.estimate;
  Napi::Object estimate = Napi::Object::New(env);
  estimate.Set("bpm", Napi::Number::New(env, est.bpm));
  estimate.Set("bpmConfidence", Napi::Number::New(env, est.bpm_confidence));
  estimate.Set("bpmCandidateCount", Napi::Number::New(env, est.bpm_candidate_count));
  estimate.Set("key", Napi::Number::New(env, est.key));
  estimate.Set("keyMinor", Napi::Boolean::New(env, est.key_minor));
  estimate.Set("keyConfidence", Napi::Number::New(env, est.key_confidence));
  estimate.Set("chordRoot", Napi::Number::New(env, est.chord_root));
  estimate.Set("chordQuality", Napi::Number::New(env, est.chord_quality));
  estimate.Set("chordConfidence", Napi::Number::New(env, est.chord_confidence));
  estimate.Set("chordStartTime", Napi::Number::New(env, est.chord_start_time));

  Napi::Array chordProgression = Napi::Array::New(env, est.chord_progression.size());
  for (size_t i = 0; i < est.chord_progression.size(); ++i) {
    const sonare::ChordChange& chord = est.chord_progression[i];
    Napi::Object c = Napi::Object::New(env);
    c.Set("root", Napi::Number::New(env, chord.root));
    c.Set("quality", Napi::Number::New(env, chord.quality));
    c.Set("startTime", Napi::Number::New(env, chord.start_time));
    c.Set("confidence", Napi::Number::New(env, chord.confidence));
    chordProgression.Set(static_cast<uint32_t>(i), c);
  }
  estimate.Set("chordProgression", chordProgression);

  Napi::Array barChordProgression = Napi::Array::New(env, est.bar_chord_progression.size());
  for (size_t i = 0; i < est.bar_chord_progression.size(); ++i) {
    const sonare::BarChord& chord = est.bar_chord_progression[i];
    Napi::Object c = Napi::Object::New(env);
    c.Set("barIndex", Napi::Number::New(env, chord.bar_index));
    c.Set("root", Napi::Number::New(env, chord.root));
    c.Set("quality", Napi::Number::New(env, chord.quality));
    c.Set("startTime", Napi::Number::New(env, chord.start_time));
    c.Set("confidence", Napi::Number::New(env, chord.confidence));
    barChordProgression.Set(static_cast<uint32_t>(i), c);
  }
  estimate.Set("barChordProgression", barChordProgression);
  estimate.Set("currentBar", Napi::Number::New(env, est.current_bar));
  estimate.Set("barDuration", Napi::Number::New(env, est.bar_duration));

  Napi::Array votedPattern = Napi::Array::New(env, est.voted_pattern.size());
  for (size_t i = 0; i < est.voted_pattern.size(); ++i) {
    const sonare::BarChord& chord = est.voted_pattern[i];
    Napi::Object c = Napi::Object::New(env);
    c.Set("barIndex", Napi::Number::New(env, chord.bar_index));
    c.Set("root", Napi::Number::New(env, chord.root));
    c.Set("quality", Napi::Number::New(env, chord.quality));
    c.Set("startTime", Napi::Number::New(env, chord.start_time));
    c.Set("confidence", Napi::Number::New(env, chord.confidence));
    votedPattern.Set(static_cast<uint32_t>(i), c);
  }
  estimate.Set("votedPattern", votedPattern);
  estimate.Set("patternLength", Napi::Number::New(env, est.pattern_length));

  estimate.Set("detectedPatternName", Napi::String::New(env, est.detected_pattern_name));
  estimate.Set("detectedPatternScore", Napi::Number::New(env, est.detected_pattern_score));

  Napi::Array allPatternScores = Napi::Array::New(env, est.all_pattern_scores.size());
  for (size_t i = 0; i < est.all_pattern_scores.size(); ++i) {
    Napi::Object ps = Napi::Object::New(env);
    ps.Set("name", Napi::String::New(env, est.all_pattern_scores[i].first));
    ps.Set("score", Napi::Number::New(env, est.all_pattern_scores[i].second));
    allPatternScores.Set(static_cast<uint32_t>(i), ps);
  }
  estimate.Set("allPatternScores", allPatternScores);

  estimate.Set("accumulatedSeconds", Napi::Number::New(env, est.accumulated_seconds));
  estimate.Set("usedFrames", Napi::Number::New(env, est.used_frames));
  estimate.Set("updated", Napi::Boolean::New(env, est.updated));
  out.Set("estimate", estimate);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::FrameCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, analyzer_->frame_count());
}

Napi::Value StreamAnalyzerWrap::CurrentTime(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, analyzer_->current_time());
}

Napi::Value StreamAnalyzerWrap::SampleRate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return Napi::Number::New(env, config_.sample_rate);
}

Napi::Value StreamAnalyzerWrap::SetExpectedDuration(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (durationSeconds)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  analyzer_->set_expected_duration(info[0].As<Napi::Number>().FloatValue());
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::SetNormalizationGain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (gain)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  analyzer_->set_normalization_gain(info[0].As<Napi::Number>().FloatValue());
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::SetTuningRefHz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (refHz)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  analyzer_->set_tuning_ref_hz(info[0].As<Napi::Number>().FloatValue());
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

}  // namespace sonare_node
