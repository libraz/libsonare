#include "sonare_wrap_streaming.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "c_api/mastering_input_domains.h"
#include "core/audio.h"
#include "editing/voice_changer/realtime.h"
#include "editing/voice_changer/streaming_retune.h"
#include "mastering/api/chain.h"
#include "mastering/eq/band_strings.h"
#include "mastering/eq/eq_band.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/spectrum_engine.h"
#include "mastering/match/match_eq.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {

namespace {

/// @brief Parses an EQ token through @p parse, refusing an unknown one with the accepted set.
template <typename Parse, typename Tokens>
auto ParseEqToken(const std::string& value, const char* label, Parse parse, Tokens tokens) {
  const auto parsed = parse(value);
  if (!parsed) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        std::string("unknown EQ ") + label + ": " + value + " (expected " + tokens() + ")");
  }
  return *parsed;
}

sonare::mastering::eq::EqBandType ParseBandType(const std::string& value) {
  return ParseEqToken(value, "band type", sonare::mastering::eq::band_type_from_string,
                      sonare::mastering::eq::band_type_tokens);
}

sonare::mastering::eq::BiquadCoeffMode ParseCoeffMode(const std::string& value) {
  return ParseEqToken(value, "coefficient mode", sonare::mastering::eq::coeff_mode_from_string,
                      sonare::mastering::eq::coeff_mode_tokens);
}

sonare::mastering::eq::StereoPlacement ParsePlacement(const std::string& value) {
  return ParseEqToken(value, "placement", sonare::mastering::eq::placement_from_string,
                      sonare::mastering::eq::placement_tokens);
}

sonare::mastering::eq::PhaseMode ParseBandPhase(const std::string& value) {
  return ParseEqToken(value, "band phase mode", sonare::mastering::eq::phase_mode_from_string,
                      sonare::mastering::eq::phase_mode_tokens);
}

sonare::mastering::eq::PhaseMode ParsePhaseModeInt(int mode) {
  const auto parsed = sonare::mastering::eq::phase_mode_from_int(mode);
  if (!parsed) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown EQ phase mode");
  }
  return *parsed;
}

sonare::mastering::eq::StereoPlacement ParsePlacementInt(int placement) {
  const auto parsed = sonare::mastering::eq::placement_from_int(placement);
  if (!parsed) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown EQ band placement");
  }
  return *parsed;
}

sonare::mastering::eq::EqBand EqBandFromObject(const Napi::Object& object) {
  sonare::mastering::eq::EqBand band;
  band.type = ParseBandType(
      StringProperty(object, "type", sonare::mastering::eq::band_type_to_string(band.type)));
  band.coeff_mode = ParseCoeffMode(StringProperty(
      object, "coeffMode", sonare::mastering::eq::coeff_mode_to_string(band.coeff_mode)));
  band.frequency_hz = FloatProperty(object, "frequencyHz", band.frequency_hz);
  band.gain_db = FloatProperty(object, "gainDb", band.gain_db);
  band.q = FloatProperty(object, "q", band.q);
  band.enabled = BoolProperty(object, "enabled", band.enabled);
  band.slope_db_oct = IntProperty(object, "slopeDbOct", band.slope_db_oct);
  band.placement = ParsePlacement(StringProperty(
      object, "placement", sonare::mastering::eq::placement_to_string(band.placement)));
  band.phase = ParseBandPhase(
      StringProperty(object, "phase", sonare::mastering::eq::phase_mode_to_string(band.phase)));
  band.soloed = BoolProperty(object, "soloed", band.soloed);
  band.bypassed = BoolProperty(object, "bypassed", band.bypassed);
  band.proportional_q = BoolProperty(object, "proportionalQ", band.proportional_q);
  band.proportional_q_strength =
      FloatProperty(object, "proportionalQStrength", band.proportional_q_strength);
  band.dyn.enabled = BoolProperty(object, "dynamic", band.dyn.enabled);
  band.dyn.threshold_db = FloatProperty(object, "thresholdDb", band.dyn.threshold_db);
  band.dyn.auto_threshold = BoolProperty(object, "autoThreshold", band.dyn.auto_threshold);
  band.dyn.ratio = FloatProperty(object, "ratio", band.dyn.ratio);
  band.dyn.range_db = FloatProperty(object, "rangeDb", band.dyn.range_db);
  band.dyn.attack_ms = FloatProperty(object, "attackMs", band.dyn.attack_ms);
  band.dyn.release_ms = FloatProperty(object, "releaseMs", band.dyn.release_ms);
  band.dyn.detector_delay_ms = FloatProperty(object, "detectorDelayMs", band.dyn.detector_delay_ms);
  band.dyn.external_sidechain =
      BoolProperty(object, "externalSidechain", band.dyn.external_sidechain);
  band.dyn.sidechain_freq_hz = FloatProperty(object, "sidechainFreqHz", band.dyn.sidechain_freq_hz);
  band.dyn.sidechain_q = FloatProperty(object, "sidechainQ", band.dyn.sidechain_q);
  return band;
}

}  // namespace

Napi::Object StreamingMasteringChainWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "StreamingMasteringChain",
      {
          InstanceMethod<&StreamingMasteringChainWrap::Prepare>("prepare", napi_default_method),
          InstanceMethod<&StreamingMasteringChainWrap::ProcessMono>("processMono",
                                                                    napi_default_method),
          InstanceMethod<&StreamingMasteringChainWrap::ProcessStereo>("processStereo",
                                                                      napi_default_method),
          InstanceMethod<&StreamingMasteringChainWrap::FlushMono>("flushMono", napi_default_method),
          InstanceMethod<&StreamingMasteringChainWrap::FlushStereo>("flushStereo",
                                                                    napi_default_method),
          InstanceMethod<&StreamingMasteringChainWrap::Reset>("reset", napi_default_method),
          InstanceMethod<&StreamingMasteringChainWrap::SetParameter>("setParameter",
                                                                     napi_default_method),
          InstanceMethod<&StreamingMasteringChainWrap::LatencySamples>("latencySamples",
                                                                       napi_default_method),
          InstanceMethod<&StreamingMasteringChainWrap::MaxBlockSize>("maxBlockSize",
                                                                     napi_default_method),
          InstanceMethod<&StreamingMasteringChainWrap::StageNames>("stageNames",
                                                                   napi_default_method),
          InstanceMethod<&StreamingMasteringChainWrap::NonFiniteSubstitutionCount>(
              "nonFiniteSubstitutionCount", napi_default_method),
          InstanceMethod<&StreamingMasteringChainWrap::NonFiniteDiscardCount>(
              "nonFiniteDiscardCount", napi_default_method),
          InstanceMethod<&StreamingMasteringChainWrap::Destroy>("destroy", napi_default_method),
      });

  exports.Set("StreamingMasteringChain", func);
  return exports;
}

StreamingMasteringChainWrap::StreamingMasteringChainWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<StreamingMasteringChainWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  const bool has_config = info.Length() >= 1 && !info[0].IsUndefined() && !info[0].IsNull();
  const bool has_options = info.Length() >= 2 && !info[1].IsUndefined() && !info[1].IsNull();
  if (has_config && !info[0].IsString()) {
    Napi::TypeError::New(env, "configJson must be a string").ThrowAsJavaScriptException();
    return;
  }
  if (has_options && !info[1].IsObject()) {
    Napi::TypeError::New(env, "options must be an object").ThrowAsJavaScriptException();
    return;
  }
  sonare::mastering::api::StreamingMasteringChainOptions options;
  if (has_options) {
    Napi::Object options_object = info[1].As<Napi::Object>();
    options.loudness_static_gain_db =
        FloatProperty(options_object, "loudnessStaticGainDb", options.loudness_static_gain_db);
    // -Infinity is the measured peak of digital silence and leaves the gain unclamped.
    const Napi::Value peak = options_object.Get("loudnessStaticGainPeakDbtp");
    const bool silent_peak = peak.IsNumber() && peak.As<Napi::Number>().DoubleValue() ==
                                                    -std::numeric_limits<double>::infinity();
    options.loudness_static_gain_peak_dbtp =
        silent_peak ? -std::numeric_limits<float>::infinity()
                    : FloatProperty(options_object, "loudnessStaticGainPeakDbtp",
                                    options.loudness_static_gain_peak_dbtp);
  }

  auto config = sonare::mastering::api::streaming_chain_config_from_json(
      has_config ? node_narrow_string(env, info[0], "configJson") : std::string("{\"params\":{}}"),
      // The constructor's request object is the chain config itself, so its keys have no root.
      sonare::mastering::api::chain_params_argument(""));
  chain_ =
      std::make_unique<sonare::mastering::api::StreamingMasteringChain>(std::move(config), options);
  SONARE_NODE_CATCH_VOID(env)
}

StreamingMasteringChainWrap::~StreamingMasteringChainWrap() = default;

// Releases the native chain up front rather than at GC. Every method already
// guards on a null chain_, so a call after destroy() throws instead of touching
// freed state, and a second destroy() is a no-op.
Napi::Value StreamingMasteringChainWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  chain_.reset();
  max_block_size_ = 0;
  return info.Env().Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::Prepare(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, chain_, "StreamingMasteringChain");
  SONARE_NODE_TRY
  double sample_rate{};
  if (!sonare_node::RequiredDoubleArg(env, info, 0, "sampleRate", &sample_rate)) {
    return env.Undefined();
  }
  int max_block_size{};
  if (!sonare_node::OptionalIntArg(env, info, 1, "maxBlockSize",
                                   sonare::mastering::api::MasteringChain::kDefaultMaxBlock,
                                   &max_block_size)) {
    return env.Undefined();
  }
  int num_channels{};
  if (!sonare_node::RequiredDomainArg(env, info, 2, "numChannels",
                                      sonare::mastering::api::kStreamingChainChannelCountDomain,
                                      &num_channels)) {
    return env.Undefined();
  }
  chain_->prepare(sample_rate, max_block_size, num_channels);
  max_block_size_ = max_block_size;
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::ProcessMono(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, chain_, "StreamingMasteringChain");
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  SONARE_NODE_TRY
  Napi::Float32Array typed = info[0].As<Napi::Float32Array>();
  size_t length = typed.ElementLength();
  Napi::Float32Array out_arr = Napi::Float32Array::New(env, length);
  if (length > 0) {
    std::memcpy(sonare_node::ElementData(out_arr), sonare_node::ElementData(typed),
                length * sizeof(float));
    float* channels[] = {sonare_node::ElementData(out_arr)};
    chain_->process_block(channels, 1, static_cast<int>(length));
  }
  return out_arr;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::ProcessStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, chain_, "StreamingMasteringChain");
  if (!RequireFloat32Array(info, 0, "left must be a Float32Array")) return env.Undefined();
  if (!RequireFloat32Array(info, 1, "right must be a Float32Array")) return env.Undefined();
  SONARE_NODE_TRY
  Napi::Float32Array left = info[0].As<Napi::Float32Array>();
  Napi::Float32Array right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  size_t length = left.ElementLength();
  Napi::Float32Array left_out = Napi::Float32Array::New(env, length);
  Napi::Float32Array right_out = Napi::Float32Array::New(env, length);
  if (length > 0) {
    std::memcpy(sonare_node::ElementData(left_out), sonare_node::ElementData(left),
                length * sizeof(float));
    std::memcpy(sonare_node::ElementData(right_out), sonare_node::ElementData(right),
                length * sizeof(float));
    float* channels[] = {sonare_node::ElementData(left_out), sonare_node::ElementData(right_out)};
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
    sonare_node::ThrowSonareErrorMessage(
        env, SONARE_ERROR_INVALID_STATE,
        "StreamingMasteringChain must be prepared before flushMono()");
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::vector<float> scratch(static_cast<size_t>(max_block_size_));
  float* channels[] = {scratch.data()};
  const int written = chain_->flush(channels, 1, max_block_size_);
  Napi::Float32Array out = Napi::Float32Array::New(env, static_cast<size_t>(written));
  if (written > 0) {
    std::memcpy(sonare_node::ElementData(out), scratch.data(),
                static_cast<size_t>(written) * sizeof(float));
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::FlushStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!chain_ || max_block_size_ <= 0) {
    sonare_node::ThrowSonareErrorMessage(
        env, SONARE_ERROR_INVALID_STATE,
        "StreamingMasteringChain must be prepared before flushStereo()");
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
    std::memcpy(sonare_node::ElementData(left), left_scratch.data(), byte_count);
    std::memcpy(sonare_node::ElementData(right), right_scratch.data(), byte_count);
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left);
  out.Set("right", right);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::Reset(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, chain_, "StreamingMasteringChain");
  SONARE_NODE_TRY
  chain_->reset();
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::SetParameter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, chain_, "StreamingMasteringChain");
  SONARE_NODE_TRY
  std::string key;
  double value = 0.0;
  if (!RequiredStringValue(env, info[0], "key", &key) ||
      !RequiredDoubleArg(env, info, 1, "value", &value)) {
    return env.Undefined();
  }
  chain_->set_parameter(key, value);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::LatencySamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, chain_, "StreamingMasteringChain");
  return Napi::Number::New(env, chain_->latency_samples());
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::MaxBlockSize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, chain_, "StreamingMasteringChain");
  return Napi::Number::New(env, chain_->max_block_size());
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::StageNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, chain_, "StreamingMasteringChain");
  const auto& names = chain_->stage_names();
  Napi::Array out = Napi::Array::New(env, names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    out.Set(static_cast<uint32_t>(i), Napi::String::New(env, names[i]));
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::NonFiniteSubstitutionCount(
    const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, chain_, "StreamingMasteringChain");
  return Napi::Number::New(env, chain_->non_finite_substitution_count());
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingMasteringChainWrap::NonFiniteDiscardCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, chain_, "StreamingMasteringChain");
  return Napi::Number::New(env, chain_->non_finite_discard_count());
  SONARE_NODE_CATCH(env)
}

Napi::Object StreamingEqualizerWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "StreamingEqualizer",
      {
          InstanceMethod<&StreamingEqualizerWrap::SetBand>("setBand", napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::Clear>("clear", napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::SetPhaseMode>("setPhaseMode",
                                                                napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::SetAutoGain>("setAutoGain", napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::SetGainScale>("setGainScale",
                                                                napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::SetOutputGainDb>("setOutputGainDb",
                                                                   napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::SetOutputPan>("setOutputPan",
                                                                napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::SetSidechainMono>("setSidechainMono",
                                                                    napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::SetSidechainStereo>("setSidechainStereo",
                                                                      napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::ClearSidechain>("clearSidechain",
                                                                  napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::LastAutoGainDb>("lastAutoGainDb",
                                                                  napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::LatencySamples>("latencySamples",
                                                                  napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::MaxBlockSize>("maxBlockSize",
                                                                napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::NonFiniteDiscardCount>("nonFiniteDiscardCount",
                                                                         napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::ProcessMono>("processMono", napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::ProcessStereo>("processStereo",
                                                                 napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::MagnitudeResponse>("magnitudeResponse",
                                                                     napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::Spectrum>("spectrum", napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::Match>("match", napi_default_method),
          InstanceMethod<&StreamingEqualizerWrap::Destroy>("destroy", napi_default_method),
      });

  exports.Set("StreamingEqualizer", func);
  return exports;
}

StreamingEqualizerWrap::StreamingEqualizerWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<StreamingEqualizerWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  double sample_rate = 48000.0;
  int max_block_size = sonare::mastering::eq::EqualizerProcessor::kDefaultMaxBlock;
  if (info.Length() >= 1 && info[0].IsObject()) {
    Napi::Object config = info[0].As<Napi::Object>();
    sample_rate = DoubleProperty(config, "sampleRate", sample_rate);
    max_block_size = IntProperty(config, "maxBlockSize", max_block_size);
  }

  sonare::mastering::eq::EqualizerProcessorConfig eq_config;
  eq_config.max_channels = 2;
  eq_ = std::make_unique<sonare::mastering::eq::EqualizerProcessor>(eq_config);
  eq_->prepare(sample_rate, max_block_size);
  sample_rate_ = sample_rate;
  SONARE_NODE_CATCH_VOID(env)
}

StreamingEqualizerWrap::~StreamingEqualizerWrap() = default;

// Releases the native processor and the retained sidechain buffers up front
// rather than at GC; every method already guards on a null eq_.
Napi::Value StreamingEqualizerWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  eq_.reset();
  sidechain_left_.clear();
  sidechain_right_.clear();
  sidechain_channels_ = {};
  return info.Env().Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::SetBand(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  // Read as a size_t rather than cast from an int: a negative index would arrive
  // at set_band as a huge positive one.
  size_t index = 0;
  if (!sonare_node::NonNegativeSizeTArg(env, info, 0, "index", &index)) return env.Undefined();
  if (!info[1].IsObject()) {
    Napi::TypeError::New(env, "band must be an object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  sonare::mastering::eq::EqBand band = EqBandFromObject(info[1].As<Napi::Object>());
  eq_->set_band(index, band);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::Clear(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  SONARE_NODE_TRY
  eq_->clear();
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::SetPhaseMode(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  SONARE_NODE_TRY
  int mode{};
  if (!sonare_node::RequiredIntArg(env, info, 0, "mode", &mode)) return env.Undefined();
  eq_->set_phase_mode(ParsePhaseModeInt(mode));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::SetAutoGain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  bool enabled = false;
  if (!RequiredBoolArg(env, info, 0, "enabled", &enabled)) return env.Undefined();
  eq_->set_auto_gain_enabled(enabled);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::SetGainScale(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  SONARE_NODE_TRY
  float value = 0.0f;
  if (!RequiredFloatArg(env, info, 0, "scale", &value)) return env.Undefined();
  eq_->set_gain_scale(value);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::SetOutputGainDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  SONARE_NODE_TRY
  float value = 0.0f;
  if (!RequiredFloatArg(env, info, 0, "gainDb", &value)) return env.Undefined();
  eq_->set_output_gain_db(value);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::SetOutputPan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  SONARE_NODE_TRY
  float value = 0.0f;
  if (!RequiredFloatArg(env, info, 0, "pan", &value)) return env.Undefined();
  eq_->set_output_pan(value);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::SetSidechainMono(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  SONARE_NODE_TRY
  // Copy before anything else: the core holds the sidechain pointers across
  // every later process() call, and a JS ArrayBuffer can be detached or
  // transferred the moment this setter returns.
  Napi::Float32Array key = info[0].As<Napi::Float32Array>();
  std::vector<float> staged(sonare_node::ElementData(key),
                            sonare_node::ElementData(key) + key.ElementLength());
  if (staged.empty()) {
    ClearSidechainStorage();
    return env.Undefined();
  }
  sonare::validate_offline_audio_input("samples", staged.data(), staged.size(),
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
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  if (!RequireFloat32Array(info, 0, "left must be a Float32Array")) return env.Undefined();
  if (!RequireFloat32Array(info, 1, "right must be a Float32Array")) return env.Undefined();
  SONARE_NODE_TRY
  Napi::Float32Array left = info[0].As<Napi::Float32Array>();
  Napi::Float32Array right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // Copy both channels before the core is given any pointer, for the same
  // detach reason as the mono setter.
  std::vector<float> staged_left(sonare_node::ElementData(left),
                                 sonare_node::ElementData(left) + left.ElementLength());
  std::vector<float> staged_right(sonare_node::ElementData(right),
                                  sonare_node::ElementData(right) + right.ElementLength());
  if (staged_left.empty()) {
    ClearSidechainStorage();
    return env.Undefined();
  }
  const int sr = static_cast<int>(std::lround(sample_rate_));
  sonare::validate_offline_audio_input("left", staged_left.data(), staged_left.size(), sr);
  sonare::validate_offline_audio_input("right", staged_right.data(), staged_right.size(), sr);

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
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  ClearSidechainStorage();
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::LastAutoGainDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  return Napi::Number::New(env, eq_->last_auto_gain_db());
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::LatencySamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  return Napi::Number::New(env, eq_->latency_samples());
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::MaxBlockSize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  return Napi::Number::New(env, eq_->max_block_size());
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::NonFiniteDiscardCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  return Napi::Number::New(env, eq_->non_finite_discard_count());
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::ProcessMono(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) return env.Undefined();
  SONARE_NODE_TRY
  Napi::Float32Array typed = info[0].As<Napi::Float32Array>();
  size_t length = typed.ElementLength();
  Napi::Float32Array out_arr = Napi::Float32Array::New(env, length);
  if (length > 0) {
    std::memcpy(sonare_node::ElementData(out_arr), sonare_node::ElementData(typed),
                length * sizeof(float));
    float* channels[] = {sonare_node::ElementData(out_arr)};
    eq_->process(channels, 1, static_cast<int>(length));
  }
  return out_arr;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::ProcessStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  if (!RequireFloat32Array(info, 0, "left must be a Float32Array")) return env.Undefined();
  if (!RequireFloat32Array(info, 1, "right must be a Float32Array")) return env.Undefined();
  SONARE_NODE_TRY
  Napi::Float32Array left = info[0].As<Napi::Float32Array>();
  Napi::Float32Array right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  size_t length = left.ElementLength();
  Napi::Float32Array left_out = Napi::Float32Array::New(env, length);
  Napi::Float32Array right_out = Napi::Float32Array::New(env, length);
  if (length > 0) {
    std::memcpy(sonare_node::ElementData(left_out), sonare_node::ElementData(left),
                length * sizeof(float));
    std::memcpy(sonare_node::ElementData(right_out), sonare_node::ElementData(right),
                length * sizeof(float));
    float* channels[] = {sonare_node::ElementData(left_out), sonare_node::ElementData(right_out)};
    eq_->process(channels, 2, static_cast<int>(length));
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::MagnitudeResponse(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  SONARE_NODE_TRY
  int placement{};
  if (!sonare_node::RequiredIntArg(env, info, 0, "placement", &placement)) return env.Undefined();
  if (!RequireFloat32Array(info, 1, "frequenciesHz must be a Float32Array")) return env.Undefined();
  Napi::Float32Array frequencies = info[1].As<Napi::Float32Array>();
  const size_t count = frequencies.ElementLength();
  Napi::Float32Array out = Napi::Float32Array::New(env, count);
  eq_->magnitude_response_db(ParsePlacementInt(placement), sonare_node::ElementData(frequencies),
                             count, sonare_node::ElementData(out));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::Spectrum(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
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

  out.Set("bandGainDb",
          Float32ArrayFrom(env, snapshot.band_gain_db.data(), snapshot.band_gain_db.size()));
  out.Set("profileDb",
          Float32ArrayFrom(env, snapshot.profile_db.data(), snapshot.profile_db.size()));
  out.Set("lastAutoGainDb", Napi::Number::New(env, eq_->last_auto_gain_db()));
  out.Set("seq", Napi::Number::New(env, static_cast<double>(snapshot.seq)));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingEqualizerWrap::Match(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, eq_, "StreamingEqualizer");
  if (!RequireFloat32Array(info, 0, "source must be a Float32Array")) return env.Undefined();
  if (!RequireFloat32Array(info, 1, "reference must be a Float32Array")) return env.Undefined();
  int sample_rate = static_cast<int>(std::lround(sample_rate_));
  int reference_sample_rate = sample_rate;
  size_t max_bands = 8;
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object options = info[2].As<Napi::Object>();
    // Read as an int rather than rounded from a double: std::lround of a
    // non-finite is unspecified, and the arbitrary int it yields then meets a
    // range check it can pass. The supported-rate bound stays below.
    if (!Int32Property(env, options, "sampleRate", sample_rate, &sample_rate)) {
      return env.Undefined();
    }
    // An omitted reference rate is the source's.
    if (!Int32Property(env, options, "referenceSampleRate", sample_rate, &reference_sample_rate)) {
      return env.Undefined();
    }
    max_bands = DomainProperty<size_t>(options, "maxBands", max_bands,
                                       sonare_c_detail::kEqMatchMaxBandsDomain);
  }
  Napi::Float32Array source = info[0].As<Napi::Float32Array>();
  Napi::Float32Array reference = info[1].As<Napi::Float32Array>();
  sonare::validate_offline_audio_input("source", sonare_node::ElementData(source),
                                       source.ElementLength(), sample_rate);
  sonare::validate_offline_audio_input("reference", sonare_node::ElementData(reference),
                                       reference.ElementLength(), reference_sample_rate);
  sonare::mastering::match::MatchEqConfig match_config;
  match_config.max_bands = max_bands;
  sonare::mastering::match::configure_equalizer_from_audio(
      *eq_,
      sonare::Audio::from_buffer(sonare_node::ElementData(source), source.ElementLength(),
                                 sample_rate),
      sonare::Audio::from_buffer(sonare_node::ElementData(reference), reference.ElementLength(),
                                 reference_sample_rate),
      match_config);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

// ===========================================================================
// StreamingRetune
// ===========================================================================

namespace {

// Reads the three scalar controls out of a JS options object using ONLY the
// shared reader families, so this file keeps no reader of its own -- the
// reader-shape scan in the Node test suite rejects one. A wrong-typed value is
// refused by name. grainSizeSamples is absent for "automatic": an explicit 0 is
// refused so the sentinel is never typed by accident.
void ReadRetuneConfig(const Napi::Value& value, float* pitch_semitones, float* mix,
                      int* grain_size_samples) {
  if (!value.IsObject()) return;
  const Napi::Object object = value.As<Napi::Object>();
  *pitch_semitones = DomainFloatProperty(object, "pitchSemitones", *pitch_semitones);
  *mix = DomainFloatProperty(object, "mix", *mix);
  const int grain = IntProperty(object, "grainSizeSamples", *grain_size_samples);
  const Napi::Value given = object.Get("grainSizeSamples");
  if (!given.IsUndefined() && !given.IsNull() && grain == 0) {
    throw Napi::RangeError::New(
        object.Env(), "grainSizeSamples must not be 0; omit it for an automatic grain size");
  }
  *grain_size_samples = grain;
  // The class states the domain once; the refusal quotes the fields as the caller spelled them.
  sonare::editing::voice_changer::StreamingRetuneConfig config;
  config.semitones = *pitch_semitones;
  config.mix = *mix;
  config.grain_size = grain;
  const std::string problem = sonare::editing::voice_changer::streaming_retune_config_problem(
      config, nullptr, {"pitchSemitones", "mix", "grainSizeSamples"});
  if (!problem.empty()) throw Napi::RangeError::New(object.Env(), problem);
}

}  // namespace

Napi::Object StreamingRetuneWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "StreamingRetune",
      {
          InstanceMethod<&StreamingRetuneWrap::Prepare>("prepare", napi_default_method),
          InstanceMethod<&StreamingRetuneWrap::Reset>("reset", napi_default_method),
          InstanceMethod<&StreamingRetuneWrap::SetConfig>("setConfig", napi_default_method),
          InstanceMethod<&StreamingRetuneWrap::Config>("config", napi_default_method),
          InstanceMethod<&StreamingRetuneWrap::GrainSizeSamples>("grainSizeSamples",
                                                                 napi_default_method),
          InstanceMethod<&StreamingRetuneWrap::LatencySamples>("latencySamples",
                                                               napi_default_method),
          InstanceMethod<&StreamingRetuneWrap::ProcessMono>("processMono", napi_default_method),
          InstanceMethod<&StreamingRetuneWrap::Destroy>("destroy", napi_default_method),
      });

  exports.Set("StreamingRetune", func);
  return exports;
}

StreamingRetuneWrap::StreamingRetuneWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<StreamingRetuneWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  float semitones = 0.0f;
  float mix = 1.0f;
  int grain_size = 0;
  if (info.Length() >= 1) {
    ReadRetuneConfig(info[0], &semitones, &mix, &grain_size);
  }
  const SonareError create_err =
      sonare_streaming_retune_create(semitones, mix, grain_size, &retune_);
  if (create_err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, create_err);
  }
  SONARE_NODE_CATCH_VOID(env)
}

StreamingRetuneWrap::~StreamingRetuneWrap() {
  sonare_streaming_retune_destroy(retune_);
  retune_ = nullptr;
}

// Releases the native handle now instead of at GC; every method below guards on
// a null retune_, so a call after destroy() is refused.
Napi::Value StreamingRetuneWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  sonare_streaming_retune_destroy(retune_);
  retune_ = nullptr;
  return info.Env().Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingRetuneWrap::Prepare(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, retune_, "StreamingRetune");
  double sample_rate = 0.0;
  int max_block_size = 0;
  if (!OptionalDoubleArg(env, info, 0, "sampleRate", 0.0, &sample_rate) ||
      !RequiredIntArg(env, info, 1, "maxBlockSize", &max_block_size)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_streaming_retune_prepare(retune_, sample_rate, max_block_size));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingRetuneWrap::Reset(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, retune_, "StreamingRetune");
  ThrowIfError(env, sonare_streaming_retune_reset(retune_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingRetuneWrap::SetConfig(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, retune_, "StreamingRetune");
  // Seed from the currently applied values so an options bag carrying only one
  // key leaves the others alone, matching every other setConfig on this surface.
  float semitones = 0.0f;
  float mix = 1.0f;
  // config() reports the requested grain (0 = automatic), so an omitted key keeps the sentinel.
  int grain_size = 0;
  ThrowIfError(env, sonare_streaming_retune_config(retune_, &semitones, &mix, &grain_size));
  if (env.IsExceptionPending()) return env.Undefined();
  if (info.Length() >= 1) {
    ReadRetuneConfig(info[0], &semitones, &mix, &grain_size);
  }
  ThrowIfError(env, sonare_streaming_retune_set_config(retune_, semitones, mix, grain_size));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingRetuneWrap::Config(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, retune_, "StreamingRetune");
  float semitones = 0.0f;
  float mix = 0.0f;
  int grain_size = 0;
  ThrowIfError(env, sonare_streaming_retune_config(retune_, &semitones, &mix, &grain_size));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  out.Set("pitchSemitones", Napi::Number::New(env, semitones));
  out.Set("mix", Napi::Number::New(env, mix));
  out.Set("grainSizeSamples", Napi::Number::New(env, grain_size));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingRetuneWrap::GrainSizeSamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, retune_, "StreamingRetune");
  int grain_size = 0;
  ThrowIfError(env, sonare_streaming_retune_grain_size_samples(retune_, &grain_size));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, grain_size);
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingRetuneWrap::LatencySamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, retune_, "StreamingRetune");
  int latency = 0;
  ThrowIfError(env, sonare_streaming_retune_latency_samples(retune_, &latency));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, latency);
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamingRetuneWrap::ProcessMono(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, retune_, "StreamingRetune");
  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }
  Napi::Float32Array input = info[0].As<Napi::Float32Array>();
  // The C entry point works in place, so copy the caller's samples into the
  // result buffer first and never mutate the argument.
  Napi::Float32Array out =
      Float32ArrayFrom(env, sonare_node::ElementData(input), input.ElementLength());
  ThrowIfError(env, sonare_streaming_retune_process_mono(retune_, sonare_node::ElementData(out),
                                                         out.ElementLength()));
  if (env.IsExceptionPending()) return env.Undefined();
  return out;
  SONARE_NODE_CATCH(env)
}

}  // namespace sonare_node
