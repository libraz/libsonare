#include "sonare_wrap_mixer.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "c_api/mixing_input_domains.h"
#include "core/audio.h"
#include "mixing/api/scene.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {

Napi::Object MixerWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "Mixer",
      {
          InstanceMethod<&MixerWrap::Compile>("compile", napi_default_method),
          InstanceMethod<&MixerWrap::ProcessStereo>("processStereo", napi_default_method),
          InstanceMethod<&MixerWrap::DrainTailStereo>("drainTailStereo", napi_default_method),
          InstanceMethod<&MixerWrap::FlushMeters>("flushMeters", napi_default_method),
          InstanceMethod<&MixerWrap::TailSamples>("tailSamples", napi_default_method),
          InstanceMethod<&MixerWrap::LatencySamples>("latencySamples", napi_default_method),
          InstanceMethod<&MixerWrap::MaxBlockSize>("maxBlockSize", napi_default_method),
          InstanceMethod<&MixerWrap::StripCount>("stripCount", napi_default_method),
          InstanceMethod<&MixerWrap::SceneWarnings>("sceneWarnings", napi_default_method),
          InstanceMethod<&MixerWrap::ScheduleInsertAutomation>("scheduleInsertAutomation",
                                                               napi_default_method),
          InstanceMethod<&MixerWrap::ToSceneJson>("toSceneJson", napi_default_method),
          InstanceMethod<&MixerWrap::Destroy>("destroy", napi_default_method),
          InstanceMethod<&MixerWrap::SetInputTrimDb>("setInputTrimDb", napi_default_method),
          InstanceMethod<&MixerWrap::SetFaderDb>("setFaderDb", napi_default_method),
          InstanceMethod<&MixerWrap::SetPan>("setPan", napi_default_method),
          InstanceMethod<&MixerWrap::SetWidth>("setWidth", napi_default_method),
          InstanceMethod<&MixerWrap::SetMuted>("setMuted", napi_default_method),
          InstanceMethod<&MixerWrap::SetSoloed>("setSoloed", napi_default_method),
          InstanceMethod<&MixerWrap::SetSoloSafe>("setSoloSafe", napi_default_method),
          InstanceMethod<&MixerWrap::SetPolarityInvert>("setPolarityInvert", napi_default_method),
          InstanceMethod<&MixerWrap::SetPanLaw>("setPanLaw", napi_default_method),
          InstanceMethod<&MixerWrap::SetChannelDelaySamples>("setChannelDelaySamples",
                                                             napi_default_method),
          InstanceMethod<&MixerWrap::SetVcaOffsetDb>("setVcaOffsetDb", napi_default_method),
          InstanceMethod<&MixerWrap::SetDualPan>("setDualPan", napi_default_method),
          InstanceMethod<&MixerWrap::SetSurroundPan>("setSurroundPan", napi_default_method),
          InstanceMethod<&MixerWrap::Settle>("settle", napi_default_method),
          InstanceMethod<&MixerWrap::AddSend>("addSend", napi_default_method),
          InstanceMethod<&MixerWrap::SetSendDb>("setSendDb", napi_default_method),
          InstanceMethod<&MixerWrap::RemoveSend>("removeSend", napi_default_method),
          InstanceMethod<&MixerWrap::StripMeter>("stripMeter", napi_default_method),
          InstanceMethod<&MixerWrap::BusMeter>("busMeter", napi_default_method),
          InstanceMethod<&MixerWrap::ConfigureMeter>("configureMeter", napi_default_method),
          InstanceMethod<&MixerWrap::MeterSnapshot>("meterSnapshot", napi_default_method),
          InstanceMethod<&MixerWrap::ReadGoniometerLatest>("readGoniometerLatest",
                                                           napi_default_method),
          InstanceMethod<&MixerWrap::StripNonFiniteDiscardCount>("stripNonFiniteDiscardCount",
                                                                 napi_default_method),
          InstanceMethod<&MixerWrap::BusNonFiniteDiscardCount>("busNonFiniteDiscardCount",
                                                               napi_default_method),
          InstanceMethod<&MixerWrap::StripById>("stripById", napi_default_method),
          InstanceMethod<&MixerWrap::AddStrip>("addStrip", napi_default_method),
          InstanceMethod<&MixerWrap::AddBus>("addBus", napi_default_method),
          InstanceMethod<&MixerWrap::SetOutputBus>("setOutputBus", napi_default_method),
          InstanceMethod<&MixerWrap::RemoveBus>("removeBus", napi_default_method),
          InstanceMethod<&MixerWrap::BusCount>("busCount", napi_default_method),
          InstanceMethod<&MixerWrap::AddVcaGroup>("addVcaGroup", napi_default_method),
          InstanceMethod<&MixerWrap::SetVcaGroupGainDb>("setVcaGroupGainDb", napi_default_method),
          InstanceMethod<&MixerWrap::SetVcaGroupMembers>("setVcaGroupMembers", napi_default_method),
          InstanceMethod<&MixerWrap::RemoveVcaGroup>("removeVcaGroup", napi_default_method),
          InstanceMethod<&MixerWrap::VcaGroupCount>("vcaGroupCount", napi_default_method),
          InstanceMethod<&MixerWrap::ScheduleFaderAutomation>("scheduleFaderAutomation",
                                                              napi_default_method),
          InstanceMethod<&MixerWrap::SchedulePanAutomation>("schedulePanAutomation",
                                                            napi_default_method),
          InstanceMethod<&MixerWrap::ScheduleWidthAutomation>("scheduleWidthAutomation",
                                                              napi_default_method),
          InstanceMethod<&MixerWrap::ScheduleSendAutomation>("scheduleSendAutomation",
                                                             napi_default_method),
      });

  exports.Set("Mixer", func);
  return exports;
}

MixerWrap::MixerWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<MixerWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!info[0].IsString()) {
    Napi::TypeError::New(env, "document must be a string").ThrowAsJavaScriptException();
    return;
  }
  std::string json = node_narrow_string(env, info[0], "document");
  if (!OptionalDomainArg<int>(env, info, 1, "sampleRate", 48000, sonare::kAudioSampleRateDomain,
                              &sample_rate_))
    return;
  if (!OptionalDomainArg<int>(env, info, 2, "maxBlockSize",
                              sonare::mixing::api::kMixerDefaultMaxBlock,
                              sonare_c_detail::kMixerMaxBlockSizeDomain, &block_size_))
    return;

  mixer_ = sonare_mixer_from_scene_json(json.c_str(), sample_rate_, block_size_);
  if (mixer_ == nullptr) {
    sonare_node::ThrowSonareErrorPreservingRefusal(env, sonare_last_error_code());
    return;
  }
  // Copy load warnings before the next C call overwrites them.
  scene_warnings_ = sonare_node::LastDiagnosticMessages();
  SONARE_NODE_CATCH_VOID(env)
}

MixerWrap::~MixerWrap() {
  if (mixer_ != nullptr) {
    sonare_mixer_destroy(mixer_);
    mixer_ = nullptr;
  }
}

Napi::Value MixerWrap::Compile(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareError err = sonare_mixer_compile(mixer_);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::ProcessStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  Napi::Array left_input;
  Napi::Array right_input;
  if (!sonare_node::RequiredPlaneListValue(env, info[0], "leftInputs", &left_input) ||
      !sonare_node::RequiredPlaneListValue(env, info[1], "rightInputs", &right_input)) {
    return env.Undefined();
  }
  const size_t count = node_list_length(left_input, "leftInputs");
  if (node_list_length(right_input, "rightInputs") != count) {
    Napi::RangeError::New(env, "leftInputs and rightInputs must have the same length")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // Fewer inputs than strips leave the rest silent; more is an argument error.
  const size_t strip_count = sonare_mixer_strip_count(mixer_);
  if (count > strip_count) {
    Napi::RangeError::New(env, "got " + std::to_string(count) + " input channels for a " +
                                   std::to_string(strip_count) + "-strip mixer")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::vector<Napi::Float32Array> left_arrays;
  std::vector<Napi::Float32Array> right_arrays;
  std::vector<const float*> left_ptrs;
  std::vector<const float*> right_ptrs;
  left_arrays.reserve(count);
  right_arrays.reserve(count);
  left_ptrs.reserve(count);
  right_ptrs.reserve(count);

  size_t length = 0;
  for (size_t index = 0; index < count; ++index) {
    Napi::Value left_value = left_input.Get(index);
    Napi::Value right_value = right_input.Get(index);
    if (!IsFloat32Array(left_value) || !IsFloat32Array(right_value)) {
      const char* side = IsFloat32Array(left_value) ? "rightInputs" : "leftInputs";
      Napi::TypeError::New(
          env, std::string(side) + "[" + std::to_string(index) + "] must be a Float32Array")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    left_arrays.push_back(left_value.As<Napi::Float32Array>());
    right_arrays.push_back(right_value.As<Napi::Float32Array>());
    if (left_arrays.back().ElementLength() != right_arrays.back().ElementLength()) {
      const std::string at = "[" + std::to_string(index) + "]";
      Napi::RangeError::New(
          env, "leftInputs" + at + " and rightInputs" + at + " must have the same length")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    if (index == 0) {
      length = left_arrays.back().ElementLength();
    } else if (left_arrays.back().ElementLength() != length) {
      Napi::RangeError::New(env, "all strips must have the same length")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    left_ptrs.push_back(sonare_node::ElementData(left_arrays.back()));
    right_ptrs.push_back(sonare_node::ElementData(right_arrays.back()));
  }

  if (length > static_cast<size_t>(block_size_)) {
    Napi::RangeError::New(env, "block length exceeds the mixer's configured block size")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Float32Array left_out = Napi::Float32Array::New(env, length);
  Napi::Float32Array right_out = Napi::Float32Array::New(env, length);
  SonareError err = sonare_mixer_process_stereo(
      mixer_, count > 0 ? sonare_node::ElementData(left_ptrs) : nullptr,
      count > 0 ? sonare_node::ElementData(right_ptrs) : nullptr, count,
      sonare_node::ElementData(left_out), sonare_node::ElementData(right_out), length);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  out.Set("sampleRate", sample_rate_);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::TailSamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  int tail = 0;
  SonareError err = sonare_mixer_tail_samples(mixer_, &tail);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return Napi::Number::New(env, tail);
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::LatencySamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  int latency = 0;
  SonareError err = sonare_mixer_latency_samples(mixer_, &latency);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return Napi::Number::New(env, latency);
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::MaxBlockSize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  int max_block = 0;
  sonare_node::ThrowIfError(env, sonare_mixer_max_block_size(mixer_, &max_block));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, max_block);
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::FlushMeters(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  SonareError err = sonare_mixer_flush_meters(mixer_);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::DrainTailStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  if (!info[0].IsNumber()) {
    Napi::TypeError::New(env, "numSamples must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // Unify the accepted range with the WASM and Python facades: numSamples must be
  // an integer in [1, blockSize]. Rejecting 0 and non-integers here (rather than
  // truncating a float or draining a zero-length block) keeps drainTailStereo's
  // contract identical across every surface.
  const double requested = info[0].As<Napi::Number>().DoubleValue();
  if (!std::isfinite(requested) || std::floor(requested) != requested || requested < 1.0 ||
      requested > static_cast<double>(block_size_)) {
    Napi::RangeError::New(
        env, "numSamples must be an integer in [1, " + std::to_string(block_size_) + "]")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const size_t num_samples = static_cast<size_t>(requested);

  Napi::Float32Array left_out = Napi::Float32Array::New(env, num_samples);
  Napi::Float32Array right_out = Napi::Float32Array::New(env, num_samples);
  SonareError err = sonare_mixer_drain_tail_stereo(
      mixer_, sonare_node::ElementData(left_out), sonare_node::ElementData(right_out), num_samples);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  out.Set("sampleRate", sample_rate_);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::StripCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  return Napi::Number::New(env, static_cast<double>(sonare_mixer_strip_count(mixer_)));
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SceneWarnings(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  Napi::Array out = Napi::Array::New(env, scene_warnings_.size());
  for (uint32_t index = 0; index < scene_warnings_.size(); ++index) {
    out.Set(index, Napi::String::New(env, scene_warnings_[index]));
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::ScheduleInsertAutomation(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  if (!info[0].IsNumber()) {
    Napi::TypeError::New(env, "strip must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "insertIndex must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "paramId must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsNumber()) {
    Napi::TypeError::New(env, "samplePos must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[4].IsNumber()) {
    Napi::TypeError::New(env, "value must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  const size_t strip_index = sonare_node::node_narrow_uint32(env, info[0], "strip");
  const unsigned int insert_index = sonare_node::node_narrow_uint32(env, info[1], "insertIndex");
  const unsigned int param_id = node_domain_number<unsigned int>(
      info[2], "paramId", sonare_c_detail::kStripInsertAutomationParamIdDomain);
  const int64_t sample_pos = sonare_node::node_narrow_int64(env, info[3], "samplePos");
  const float value = sonare_node::node_narrow_float(env, info[4], "value");
  int curve{};
  if (!RequiredDomainArg<int>(env, info, 5, "curve", sonare_c_detail::kStripAutomationCurveDomain,
                              &curve))
    return env.Undefined();

  SonareStrip* strip = sonare_mixer_strip_at(mixer_, strip_index);
  if (strip == nullptr) {
    sonare_node::ThrowSonareError(env, sonare_last_error_code());
    return env.Undefined();
  }

  SonareError err = sonare_strip_schedule_insert_automation(strip, insert_index, param_id,
                                                            sample_pos, value, curve);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::ToSceneJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, mixer_, "Mixer");
  char* json = nullptr;
  SonareError err = sonare_mixer_to_scene_json(mixer_, &json);
  if (err != SONARE_OK || json == nullptr) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  std::string out(json);
  sonare_free_string(json);
  return Napi::String::New(env, out);
  SONARE_NODE_CATCH(env)
}

void MixerWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (mixer_ != nullptr) {
    sonare_mixer_destroy(mixer_);
    mixer_ = nullptr;
  }
  SONARE_NODE_CATCH_VOID(env)
}

}  // namespace sonare_node
