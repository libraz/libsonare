#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/audio.h"
#include "editing/voice_changer/realtime.h"
#include "mastering/api/chain.h"
#include "mastering/eq/eq_band.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/spectrum_engine.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_spectrum.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_streaming.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {

namespace {

std::string JsonTextFromJs(const Napi::Value& value) {
  if (value.IsUndefined() || value.IsNull()) return "neutral-monitor";
  if (value.IsString()) return node_narrow_string(value.Env(), value, "config");
  Napi::Env env = value.Env();
  Napi::Object json = env.Global().Get("JSON").As<Napi::Object>();
  Napi::Function stringify = json.Get("stringify").As<Napi::Function>();
  return node_narrow_string(env, stringify.Call(json, {value}), "config");
}

sonare::editing::voice_changer::RealtimeVoiceChangerConfig ConfigFromJs(const Napi::Value& value) {
  sonare::editing::voice_changer::RealtimeVoiceChangerConfig config;
  std::string error;
  if (!sonare::editing::voice_changer::realtime_voice_changer_config_from_input(
          JsonTextFromJs(value), &config, &error)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, error);
  }
  return config;
}

}  // namespace

Napi::Object RealtimeVoiceChangerWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "RealtimeVoiceChanger",
      {
          InstanceMethod<&RealtimeVoiceChangerWrap::Prepare>("prepare"),
          InstanceMethod<&RealtimeVoiceChangerWrap::Reset>("reset"),
          InstanceMethod<&RealtimeVoiceChangerWrap::SetConfig>("setConfig"),
          InstanceMethod<&RealtimeVoiceChangerWrap::ConfigJson>("configJson"),
          InstanceMethod<&RealtimeVoiceChangerWrap::LatencySamples>("latencySamples"),
          InstanceMethod<&RealtimeVoiceChangerWrap::NonFiniteDiscardCount>("nonFiniteDiscardCount"),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessMono>("processMono"),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessMonoInto>("processMonoInto"),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessInterleaved>("processInterleaved"),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessInterleavedInto>(
              "processInterleavedInto"),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessPlanarStereo>("processPlanarStereo"),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessMonoWithCarrier>(
              "processMonoWithCarrier"),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessMonoWithCarrierInto>(
              "processMonoWithCarrierInto"),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessInterleavedWithCarrier>(
              "processInterleavedWithCarrier"),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessInterleavedWithCarrierInto>(
              "processInterleavedWithCarrierInto"),
          InstanceMethod<&RealtimeVoiceChangerWrap::ProcessPlanarStereoWithCarrier>(
              "processPlanarStereoWithCarrier"),
          InstanceMethod<&RealtimeVoiceChangerWrap::NoteOn>("noteOn"),
          InstanceMethod<&RealtimeVoiceChangerWrap::NoteOff>("noteOff"),
          InstanceMethod<&RealtimeVoiceChangerWrap::AllNotesOff>("allNotesOff"),
          InstanceMethod<&RealtimeVoiceChangerWrap::Destroy>("destroy"),
      });

  exports.Set("RealtimeVoiceChanger", func);
  return exports;
}

RealtimeVoiceChangerWrap::RealtimeVoiceChangerWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<RealtimeVoiceChangerWrap>(info) {
  Napi::Env env = info.Env();
  try {
    changer_ = std::make_unique<sonare::editing::voice_changer::RealtimeVoiceChanger>();
    changer_->set_config(info.Length() >= 1 ? ConfigFromJs(info[0]) : ConfigFromJs(env.Null()));
  } catch (...) {
    sonare_node::ThrowCurrentException(env);
  }
}

RealtimeVoiceChangerWrap::~RealtimeVoiceChangerWrap() { ReleaseNative(); }

bool RealtimeVoiceChangerWrap::EnsureAlive(Napi::Env env) const {
  if (changer_) return true;
  sonare_node::ThrowHandleClosed(env, "RealtimeVoiceChanger");
  return false;
}

void RealtimeVoiceChangerWrap::ReleaseNative() noexcept {
  changer_.reset();
  prepared_ = false;
  max_block_size_ = 0;
  channels_ = 0;
  std::vector<float>().swap(planar_scratch_);
  std::vector<float*>().swap(planar_ptrs_);
}

Napi::Value RealtimeVoiceChangerWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  ReleaseNative();
  return info.Env().Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::Prepare(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (sampleRate, maxBlockSize?, channels?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  const double sample_rate = info[0].As<Napi::Number>().DoubleValue();
  int max_block_size{};
  if (!OptionalIntArg(env, info, 1, "maxBlockSize", 128, &max_block_size)) return env.Undefined();
  int channels{};
  if (!OptionalIntArg(env, info, 2, "channels", 1, &channels)) return env.Undefined();
  changer_->prepare(sample_rate, max_block_size, channels);
  prepared_ = true;
  max_block_size_ = max_block_size;
  channels_ = channels;
  // RT-safe planar scratch + pointer table. Allocated once per prepare(); the
  // process methods only deinterleave into this buffer without further
  // allocations.
  planar_scratch_.assign(static_cast<size_t>(channels_) * static_cast<size_t>(max_block_size_),
                         0.0f);
  planar_ptrs_.assign(static_cast<size_t>(channels_), nullptr);
  for (int ch = 0; ch < channels_; ++ch) {
    planar_ptrs_[static_cast<size_t>(ch)] =
        planar_scratch_.data() + static_cast<size_t>(ch) * static_cast<size_t>(max_block_size_);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::Reset(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  SONARE_NODE_TRY
  changer_->reset();
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::SetConfig(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Expected (presetIdOrConfig)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  changer_->set_config(ConfigFromJs(info[0]));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::ConfigJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!EnsureAlive(info.Env())) return info.Env().Undefined();
  return Napi::String::New(
      info.Env(),
      sonare::editing::voice_changer::realtime_voice_changer_config_to_json(changer_->config()));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::LatencySamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!EnsureAlive(info.Env())) return info.Env().Undefined();
  return Napi::Number::New(info.Env(), changer_->latency_samples());
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::NonFiniteDiscardCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!EnsureAlive(info.Env())) return info.Env().Undefined();
  return Napi::Number::New(info.Env(), changer_->non_finite_discard_count());
  SONARE_NODE_CATCH(env)
}

namespace {

constexpr const char* kNotPreparedMessage =
    "RealtimeVoiceChanger must be prepared before processing";
constexpr const char* kNoteRefusedMessage =
    "the note was not accepted: the changer is unprepared, uses the grain engine, or its note "
    "queue is full";
constexpr const char* kCarrierRequiredMessage =
    "the vocoder reads an external carrier: use the WithCarrier process methods";

}  // namespace

Napi::Value RealtimeVoiceChangerWrap::ProcessMonoImpl(const Napi::CallbackInfo& info,
                                                      bool with_carrier, bool into) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  if (!prepared_) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE, kNotPreparedMessage);
    return env.Undefined();
  }
  const size_t carrier_index = 1;
  const size_t output_index = with_carrier ? 2 : 1;
  const size_t required = 1 + (with_carrier ? 1 : 0) + (into ? 1 : 0);
  if (info.Length() < required || !IsFloat32Array(info[0]) ||
      (with_carrier && !IsFloat32Array(info[carrier_index])) ||
      (into && !IsFloat32Array(info[output_index]))) {
    Napi::TypeError::New(env, std::string("Expected (inputFloat32Array") +
                                  (with_carrier ? ", carrierFloat32Array" : "") +
                                  (into ? ", outputFloat32Array" : "") + ")")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array input = info[0].As<Napi::Float32Array>();
  if (with_carrier &&
      info[carrier_index].As<Napi::Float32Array>().ElementLength() != input.ElementLength()) {
    Napi::RangeError::New(env, "carrier and input lengths must match").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (into &&
      info[output_index].As<Napi::Float32Array>().ElementLength() != input.ElementLength()) {
    Napi::RangeError::New(env, "input and output lengths must match").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (input.ElementLength() > static_cast<size_t>(max_block_size_)) {
    Napi::RangeError::New(env, "block exceeds maxBlockSize").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Float32Array output = into ? info[output_index].As<Napi::Float32Array>()
                                   : Napi::Float32Array::New(env, input.ElementLength());
  const int frames = static_cast<int>(input.ElementLength());
  const bool ok = with_carrier
                      ? changer_->process_block(input.Data(),
                                                info[carrier_index].As<Napi::Float32Array>().Data(),
                                                output.Data(), frames)
                      : changer_->process_block(input.Data(), output.Data(), frames);
  if (!ok) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE, kCarrierRequiredMessage);
    return env.Undefined();
  }
  return into ? env.Undefined() : static_cast<Napi::Value>(output);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::ProcessInterleavedImpl(const Napi::CallbackInfo& info,
                                                             bool with_carrier, bool into) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  if (!prepared_) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE, kNotPreparedMessage);
    return env.Undefined();
  }
  const size_t carrier_index = 2;
  const size_t output_index = with_carrier ? 3 : 2;
  const size_t required = 2 + (with_carrier ? 1 : 0) + (into ? 1 : 0);
  if (info.Length() < required || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      (with_carrier && !IsFloat32Array(info[carrier_index])) ||
      (into && !IsFloat32Array(info[output_index]))) {
    Napi::TypeError::New(env, std::string("Expected (interleavedFloat32Array, channels") +
                                  (with_carrier ? ", carrierFloat32Array" : "") +
                                  (into ? ", outputInterleavedFloat32Array" : "") + ")")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array input = info[0].As<Napi::Float32Array>();
  const int channels = sonare_node::node_narrow_int(env, info[1], "channels");
  if (into &&
      info[output_index].As<Napi::Float32Array>().ElementLength() != input.ElementLength()) {
    Napi::RangeError::New(env, "input and output lengths must match").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (channels < 1 || channels > channels_ ||
      input.ElementLength() % static_cast<size_t>(channels) != 0) {
    Napi::RangeError::New(env, "invalid channel count").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const size_t frames = input.ElementLength() / static_cast<size_t>(channels);
  if (frames > static_cast<size_t>(max_block_size_)) {
    Napi::RangeError::New(env, "block exceeds maxBlockSize").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (with_carrier && info[carrier_index].As<Napi::Float32Array>().ElementLength() != frames) {
    Napi::RangeError::New(env, "carrier length must equal the number of frames")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // RT-safe path: reuse the per-instance planar scratch (allocated in Prepare) and pointer
  // table. Deinterleave -> process_block -> interleave; no per-call scratch allocation.
  const size_t stride = static_cast<size_t>(max_block_size_);
  const float* in_data = input.Data();
  for (int ch = 0; ch < channels; ++ch) {
    float* dst = planar_scratch_.data() + static_cast<size_t>(ch) * stride;
    for (size_t i = 0; i < frames; ++i) {
      dst[i] = in_data[i * static_cast<size_t>(channels) + ch];
    }
  }
  const bool ok =
      with_carrier
          ? changer_->process_block(planar_ptrs_.data(), channels,
                                    info[carrier_index].As<Napi::Float32Array>().Data(),
                                    static_cast<int>(frames))
          : changer_->process_block(planar_ptrs_.data(), channels, static_cast<int>(frames));
  if (!ok) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE, kCarrierRequiredMessage);
    return env.Undefined();
  }
  Napi::Float32Array output = into ? info[output_index].As<Napi::Float32Array>()
                                   : Napi::Float32Array::New(env, input.ElementLength());
  float* out_data = output.Data();
  for (int ch = 0; ch < channels; ++ch) {
    const float* src = planar_scratch_.data() + static_cast<size_t>(ch) * stride;
    for (size_t i = 0; i < frames; ++i) {
      out_data[i * static_cast<size_t>(channels) + ch] = src[i];
    }
  }
  return into ? env.Undefined() : static_cast<Napi::Value>(output);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::ProcessPlanarStereoImpl(const Napi::CallbackInfo& info,
                                                              bool with_carrier) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  if (!prepared_) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE, kNotPreparedMessage);
    return env.Undefined();
  }
  if (info.Length() < (with_carrier ? 3u : 2u) || !IsFloat32Array(info[0]) ||
      !IsFloat32Array(info[1]) || (with_carrier && !IsFloat32Array(info[2]))) {
    Napi::TypeError::New(env, with_carrier ? "Expected (leftFloat32Array, rightFloat32Array, "
                                             "carrierFloat32Array)"
                                           : "Expected (leftFloat32Array, rightFloat32Array)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (channels_ < 2) {
    sonare_node::ThrowSonareErrorMessage(
        env, SONARE_ERROR_INVALID_STATE,
        "RealtimeVoiceChanger must be prepared with at least 2 channels");
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array left = info[0].As<Napi::Float32Array>();
  Napi::Float32Array right = info[1].As<Napi::Float32Array>();
  if (left.ElementLength() != right.ElementLength()) {
    Napi::RangeError::New(env, "left and right lengths must match").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const size_t frames = left.ElementLength();
  if (frames > static_cast<size_t>(max_block_size_)) {
    Napi::RangeError::New(env, "block exceeds maxBlockSize").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (with_carrier && info[2].As<Napi::Float32Array>().ElementLength() != frames) {
    Napi::RangeError::New(env, "carrier and channel lengths must match")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // In-place planar stereo: the planar process_block reads and writes the
  // supplied channel buffers directly, so the caller's L/R arrays are mutated.
  float* channels[2] = {left.Data(), right.Data()};
  const bool ok =
      with_carrier ? changer_->process_block(channels, 2, info[2].As<Napi::Float32Array>().Data(),
                                             static_cast<int>(frames))
                   : changer_->process_block(channels, 2, static_cast<int>(frames));
  if (!ok) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE, kCarrierRequiredMessage);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::ProcessMono(const Napi::CallbackInfo& info) {
  return ProcessMonoImpl(info, false, false);
}
Napi::Value RealtimeVoiceChangerWrap::ProcessMonoInto(const Napi::CallbackInfo& info) {
  return ProcessMonoImpl(info, false, true);
}
Napi::Value RealtimeVoiceChangerWrap::ProcessMonoWithCarrier(const Napi::CallbackInfo& info) {
  return ProcessMonoImpl(info, true, false);
}
Napi::Value RealtimeVoiceChangerWrap::ProcessMonoWithCarrierInto(const Napi::CallbackInfo& info) {
  return ProcessMonoImpl(info, true, true);
}
Napi::Value RealtimeVoiceChangerWrap::ProcessInterleaved(const Napi::CallbackInfo& info) {
  return ProcessInterleavedImpl(info, false, false);
}
Napi::Value RealtimeVoiceChangerWrap::ProcessInterleavedInto(const Napi::CallbackInfo& info) {
  return ProcessInterleavedImpl(info, false, true);
}
Napi::Value RealtimeVoiceChangerWrap::ProcessInterleavedWithCarrier(
    const Napi::CallbackInfo& info) {
  return ProcessInterleavedImpl(info, true, false);
}
Napi::Value RealtimeVoiceChangerWrap::ProcessInterleavedWithCarrierInto(
    const Napi::CallbackInfo& info) {
  return ProcessInterleavedImpl(info, true, true);
}
Napi::Value RealtimeVoiceChangerWrap::ProcessPlanarStereo(const Napi::CallbackInfo& info) {
  return ProcessPlanarStereoImpl(info, false);
}
Napi::Value RealtimeVoiceChangerWrap::ProcessPlanarStereoWithCarrier(
    const Napi::CallbackInfo& info) {
  return ProcessPlanarStereoImpl(info, true);
}

Napi::Value RealtimeVoiceChangerWrap::NoteOn(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  SONARE_NODE_TRY
  int note{};
  if (!RequiredIntArg(env, info, 0, "note", &note)) return env.Undefined();
  // An absent velocity is full strength; a present non-number (a boolean included) is refused.
  float velocity{};
  if (!OptionalFloatArg(env, info, 1, "velocity", 1.0f, &velocity)) return env.Undefined();
  using Changer = sonare::editing::voice_changer::RealtimeVoiceChanger;
  if (!Changer::is_valid_note(note)) {
    Napi::RangeError::New(env, "note must be within [0, 127]").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!Changer::is_valid_velocity(velocity)) {
    Napi::RangeError::New(env, "velocity must be within (0, 1]; release a note with noteOff")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!prepared_ || !changer_->note_on(note, velocity)) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE, kNoteRefusedMessage);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::NoteOff(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  SONARE_NODE_TRY
  int note{};
  if (!RequiredIntArg(env, info, 0, "note", &note)) return env.Undefined();
  if (!sonare::editing::voice_changer::RealtimeVoiceChanger::is_valid_note(note)) {
    Napi::RangeError::New(env, "note must be within [0, 127]").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!prepared_ || !changer_->note_off(note)) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE, kNoteRefusedMessage);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerWrap::AllNotesOff(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!EnsureAlive(env)) return env.Undefined();
  SONARE_NODE_TRY
  changer_->all_notes_off();
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerPresetNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  Napi::Array out = Napi::Array::New(env);
  const auto names = sonare::editing::voice_changer::realtime_voice_changer_preset_names();
  for (size_t i = 0; i < names.size(); ++i) {
    out.Set(static_cast<uint32_t>(i), names[i]);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeVoiceChangerPresetJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (presetId)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  const auto preset = sonare::editing::voice_changer::realtime_voice_changer_preset_from_id(
      node_narrow_string(env, info[0], "presetId"));
  return Napi::String::New(
      env, sonare::editing::voice_changer::realtime_voice_changer_preset_json(preset));
  SONARE_NODE_CATCH(env)
}

Napi::Value ValidateRealtimeVoiceChangerPresetJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (json)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  std::string normalized;
  std::string error;
  const bool ok = sonare::editing::voice_changer::validate_realtime_voice_changer_preset_json(
      node_narrow_string(env, info[0], "presetJson"), &normalized, &error);
  Napi::Object out = Napi::Object::New(env);
  out.Set("ok", ok);
  if (ok) {
    out.Set("normalizedJson", normalized);
  } else {
    out.Set("error", error);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

}  // namespace sonare_node
