#include "sonare_wrap_mixer.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "sonare_wrap_utils.h"

namespace sonare_node {
namespace {

Napi::Object MeterSnapshotToObject(Napi::Env env, const SonareMixMeterSnapshot& snapshot) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("peakDbL", snapshot.peak_db_l);
  out.Set("peakDbR", snapshot.peak_db_r);
  out.Set("rmsDbL", snapshot.rms_db_l);
  out.Set("rmsDbR", snapshot.rms_db_r);
  out.Set("correlation", snapshot.correlation);
  out.Set("monoCompatWidth", snapshot.mono_compat_width);
  out.Set("monoCompatPeak", snapshot.mono_compat_peak);
  out.Set("monoCompatSideRms", snapshot.mono_compat_side_rms);
  out.Set("likelyMonoCompatible", snapshot.likely_mono_compatible != 0);
  out.Set("momentaryLufs", snapshot.momentary_lufs);
  out.Set("shortTermLufs", snapshot.short_term_lufs);
  out.Set("integratedLufs", snapshot.integrated_lufs);
  out.Set("gainReductionDb", snapshot.gain_reduction_db);
  out.Set("truePeakDbL", snapshot.true_peak_db_l);
  out.Set("truePeakDbR", snapshot.true_peak_db_r);
  out.Set("maxTruePeakDb", snapshot.max_true_peak_db);
  out.Set("seq", Napi::Number::New(env, static_cast<double>(snapshot.seq)));
  return out;
}

}  // namespace

Napi::FunctionReference MixerWrap::constructor_;

Napi::Object MixerWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "Mixer",
      {
          InstanceMethod<&MixerWrap::Compile>("compile"),
          InstanceMethod<&MixerWrap::ProcessStereo>("processStereo"),
          InstanceMethod<&MixerWrap::StripCount>("stripCount"),
          InstanceMethod<&MixerWrap::ScheduleInsertAutomation>("scheduleInsertAutomation"),
          InstanceMethod<&MixerWrap::ToSceneJson>("toSceneJson"),
          InstanceMethod<&MixerWrap::Destroy>("destroy"),
          InstanceMethod<&MixerWrap::SetInputTrimDb>("setInputTrimDb"),
          InstanceMethod<&MixerWrap::SetFaderDb>("setFaderDb"),
          InstanceMethod<&MixerWrap::SetPan>("setPan"),
          InstanceMethod<&MixerWrap::SetWidth>("setWidth"),
          InstanceMethod<&MixerWrap::SetMuted>("setMuted"),
          InstanceMethod<&MixerWrap::SetSoloed>("setSoloed"),
          InstanceMethod<&MixerWrap::SetSoloSafe>("setSoloSafe"),
          InstanceMethod<&MixerWrap::SetPolarityInvert>("setPolarityInvert"),
          InstanceMethod<&MixerWrap::SetPanLaw>("setPanLaw"),
          InstanceMethod<&MixerWrap::SetChannelDelaySamples>("setChannelDelaySamples"),
          InstanceMethod<&MixerWrap::SetVcaOffsetDb>("setVcaOffsetDb"),
          InstanceMethod<&MixerWrap::SetDualPan>("setDualPan"),
          InstanceMethod<&MixerWrap::AddSend>("addSend"),
          InstanceMethod<&MixerWrap::SetSendDb>("setSendDb"),
          InstanceMethod<&MixerWrap::StripMeter>("stripMeter"),
          InstanceMethod<&MixerWrap::MeterTap>("meterTap"),
          InstanceMethod<&MixerWrap::ReadGoniometerLatest>("readGoniometerLatest"),
          InstanceMethod<&MixerWrap::StripById>("stripById"),
          InstanceMethod<&MixerWrap::AddBus>("addBus"),
          InstanceMethod<&MixerWrap::RemoveBus>("removeBus"),
          InstanceMethod<&MixerWrap::BusCount>("busCount"),
          InstanceMethod<&MixerWrap::AddVcaGroup>("addVcaGroup"),
          InstanceMethod<&MixerWrap::RemoveVcaGroup>("removeVcaGroup"),
          InstanceMethod<&MixerWrap::VcaGroupCount>("vcaGroupCount"),
          InstanceMethod<&MixerWrap::ScheduleFaderAutomation>("scheduleFaderAutomation"),
          InstanceMethod<&MixerWrap::SchedulePanAutomation>("schedulePanAutomation"),
          InstanceMethod<&MixerWrap::ScheduleWidthAutomation>("scheduleWidthAutomation"),
          InstanceMethod<&MixerWrap::ScheduleSendAutomation>("scheduleSendAutomation"),
      });

  constructor_ = Napi::Persistent(func);
  constructor_.SuppressDestruct();
  exports.Set("Mixer", func);
  return exports;
}

MixerWrap::MixerWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<MixerWrap>(info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (sceneJson, sampleRate?, blockSize?)")
        .ThrowAsJavaScriptException();
    return;
  }
  std::string json = info[0].As<Napi::String>().Utf8Value();
  sample_rate_ =
      info.Length() >= 2 && info[1].IsNumber() ? info[1].As<Napi::Number>().Int32Value() : 48000;
  block_size_ =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 512;

  mixer_ = sonare_mixer_from_scene_json(json.c_str(), sample_rate_, block_size_);
  if (mixer_ == nullptr) {
    Napi::Error::New(
        env, std::string("failed to build mixer from scene JSON: ") + sonare_last_error_message())
        .ThrowAsJavaScriptException();
    return;
  }
}

MixerWrap::~MixerWrap() {
  if (mixer_ != nullptr) {
    sonare_mixer_destroy(mixer_);
    mixer_ = nullptr;
  }
}

Napi::Value MixerWrap::Compile(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareError err = sonare_mixer_compile(mixer_);
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to compile mixer graph: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::ProcessStereo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 2 || !info[0].IsArray() || !info[1].IsArray()) {
    Napi::TypeError::New(env,
                         "Expected (leftChannels: Float32Array[], rightChannels: Float32Array[])")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array left_input = info[0].As<Napi::Array>();
  Napi::Array right_input = info[1].As<Napi::Array>();
  const size_t count = left_input.Length();
  if (right_input.Length() != count) {
    Napi::TypeError::New(env, "leftChannels and rightChannels must have the same length")
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
      Napi::TypeError::New(env, "all channels must be Float32Array").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    left_arrays.push_back(left_value.As<Napi::Float32Array>());
    right_arrays.push_back(right_value.As<Napi::Float32Array>());
    if (left_arrays.back().ElementLength() != right_arrays.back().ElementLength()) {
      Napi::TypeError::New(env, "left and right channel lengths must match")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    if (index == 0) {
      length = left_arrays.back().ElementLength();
    } else if (left_arrays.back().ElementLength() != length) {
      Napi::TypeError::New(env, "all strips must have the same length")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    left_ptrs.push_back(left_arrays.back().Data());
    right_ptrs.push_back(right_arrays.back().Data());
  }

  if (length > static_cast<size_t>(block_size_)) {
    Napi::TypeError::New(env, "block length exceeds the mixer's configured block size")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Float32Array left_out = Napi::Float32Array::New(env, length);
  Napi::Float32Array right_out = Napi::Float32Array::New(env, length);
  SonareError err = sonare_mixer_process_stereo(mixer_, count > 0 ? left_ptrs.data() : nullptr,
                                                count > 0 ? right_ptrs.data() : nullptr, count,
                                                left_out.Data(), right_out.Data(), length);
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("mixer process failed: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("left", left_out);
  out.Set("right", right_out);
  out.Set("sampleRate", sample_rate_);
  return out;
}

Napi::Value MixerWrap::StripCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(sonare_mixer_strip_count(mixer_)));
}

Napi::Value MixerWrap::ScheduleInsertAutomation(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 5 || !info[0].IsNumber() || !info[1].IsNumber() || !info[2].IsNumber() ||
      !info[3].IsNumber() || !info[4].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (stripIndex, insertIndex, paramId, samplePos, value, curve?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  const size_t strip_index = static_cast<size_t>(info[0].As<Napi::Number>().Int64Value());
  const unsigned int insert_index = info[1].As<Napi::Number>().Uint32Value();
  const unsigned int param_id = info[2].As<Napi::Number>().Uint32Value();
  const int64_t sample_pos = info[3].As<Napi::Number>().Int64Value();
  const float value = info[4].As<Napi::Number>().FloatValue();
  const int curve =
      info.Length() >= 6 && info[5].IsNumber() ? info[5].As<Napi::Number>().Int32Value() : 0;

  SonareStrip* strip = sonare_mixer_strip_at(mixer_, strip_index);
  if (strip == nullptr) {
    Napi::Error::New(env, "mixer strip index out of range").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SonareError err = sonare_strip_schedule_insert_automation(strip, insert_index, param_id,
                                                            sample_pos, value, curve);
  if (err != SONARE_OK) {
    Napi::Error::New(
        env, std::string("failed to schedule insert automation: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::ToSceneJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  char* json = nullptr;
  SonareError err = sonare_mixer_to_scene_json(mixer_, &json);
  if (err != SONARE_OK || json == nullptr) {
    Napi::Error::New(env,
                     std::string("failed to serialize mixer scene: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string out(json);
  sonare_free_string(json);
  return Napi::String::New(env, out);
}

SonareStrip* MixerWrap::ResolveStrip(const Napi::CallbackInfo& info, const Napi::Value& ref) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return nullptr;
  }
  SonareStrip* strip = nullptr;
  if (ref.IsNumber()) {
    const size_t index = static_cast<size_t>(ref.As<Napi::Number>().Int64Value());
    strip = sonare_mixer_strip_at(mixer_, index);
    if (strip == nullptr) {
      Napi::Error::New(env, "mixer strip index out of range").ThrowAsJavaScriptException();
    }
  } else if (ref.IsString()) {
    const std::string id = ref.As<Napi::String>().Utf8Value();
    strip = sonare_mixer_strip_by_id(mixer_, id.c_str());
    if (strip == nullptr) {
      Napi::Error::New(env, std::string("mixer strip not found: ") + id)
          .ThrowAsJavaScriptException();
    }
  } else {
    Napi::TypeError::New(env, "strip reference must be a number (index) or string (id)")
        .ThrowAsJavaScriptException();
  }
  return strip;
}

Napi::Value MixerWrap::SetInputTrimDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, db: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_input_trim_db(strip, info[1].As<Napi::Number>().FloatValue());
  if (err != SONARE_OK) {
    Napi::Error::New(env,
                     std::string("failed to set strip input trim: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::SetFaderDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, db: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_fader_db(strip, info[1].As<Napi::Number>().FloatValue());
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to set strip fader: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::SetPan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, pan: number, panMode?: number)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const int pan_mode =
      info.Length() >= 3 && info[2].IsNumber() ? info[2].As<Napi::Number>().Int32Value() : 0;
  SonareError err = sonare_strip_set_pan(strip, info[1].As<Napi::Number>().FloatValue(), pan_mode);
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to set strip pan: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::SetWidth(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, width: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_width(strip, info[1].As<Napi::Number>().FloatValue());
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to set strip width: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::SetMuted(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[1].IsBoolean()) {
    Napi::TypeError::New(env, "Expected (strip, muted: boolean)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_muted(strip, info[1].As<Napi::Boolean>().Value() ? 1 : 0);
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to set strip muted: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::SetSoloed(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[1].IsBoolean()) {
    Napi::TypeError::New(env, "Expected (strip, soloed: boolean)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_soloed(strip, info[1].As<Napi::Boolean>().Value() ? 1 : 0);
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to set strip solo: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::SetSoloSafe(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[1].IsBoolean()) {
    Napi::TypeError::New(env, "Expected (strip, soloSafe: boolean)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_solo_safe(strip, info[1].As<Napi::Boolean>().Value() ? 1 : 0);
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to set strip solo-safe: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::SetPolarityInvert(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[1].IsBoolean() || !info[2].IsBoolean()) {
    Napi::TypeError::New(env, "Expected (strip, invertLeft: boolean, invertRight: boolean)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err =
      sonare_strip_set_polarity_invert(strip, info[1].As<Napi::Boolean>().Value() ? 1 : 0,
                                       info[2].As<Napi::Boolean>().Value() ? 1 : 0);
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to set strip polarity: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::SetPanLaw(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, panLaw: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_pan_law(strip, info[1].As<Napi::Number>().Int32Value());
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to set strip pan law: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::SetChannelDelaySamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, delaySamples: number)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err =
      sonare_strip_set_channel_delay_samples(strip, info[1].As<Napi::Number>().Int32Value());
  if (err != SONARE_OK) {
    Napi::Error::New(env,
                     std::string("failed to set strip channel delay: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::SetVcaOffsetDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, offsetDb: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_vca_offset_db(strip, info[1].As<Napi::Number>().FloatValue());
  if (err != SONARE_OK) {
    Napi::Error::New(env,
                     std::string("failed to set strip VCA offset: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::SetDualPan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, leftPan: number, rightPan: number)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareError err = sonare_strip_set_dual_pan(strip, info[1].As<Napi::Number>().FloatValue(),
                                              info[2].As<Napi::Number>().FloatValue());
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to set strip dual pan: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::AddSend(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[1].IsString() || !info[2].IsString()) {
    Napi::TypeError::New(
        env, "Expected (strip, sendId: string, destinationBusId: string, sendDb?, timing?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const std::string send_id = info[1].As<Napi::String>().Utf8Value();
  const std::string destination_bus_id = info[2].As<Napi::String>().Utf8Value();
  const float send_db =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().FloatValue() : 0.0f;
  const int timing =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 0;

  size_t index = 0;
  SonareError err = sonare_strip_add_send(strip, send_id.c_str(), destination_bus_id.c_str(),
                                          send_db, timing, &index);
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to add strip send: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(index));
}

Napi::Value MixerWrap::SetSendDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, sendIndex: number, sendDb: number)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const size_t send_index = static_cast<size_t>(info[1].As<Napi::Number>().Int64Value());
  SonareError err =
      sonare_strip_set_send_db(strip, send_index, info[2].As<Napi::Number>().FloatValue());
  if (err != SONARE_OK) {
    Napi::Error::New(env,
                     std::string("failed to set strip send level: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::StripMeter(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1) {
    Napi::TypeError::New(env, "Expected (strip)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareMixMeterSnapshot snapshot{};
  SonareError err = sonare_strip_meter(strip, &snapshot);
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to read strip meter: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return MeterSnapshotToObject(env, snapshot);
}

Napi::Value MixerWrap::MeterTap(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, tap: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  SonareMixMeterSnapshot snapshot{};
  SonareError err =
      sonare_strip_meter_tap(strip, info[1].As<Napi::Number>().Int32Value(), &snapshot);
  if (err != SONARE_OK) {
    Napi::Error::New(env,
                     std::string("failed to read strip meter tap: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return MeterSnapshotToObject(env, snapshot);
}

Napi::Value MixerWrap::ReadGoniometerLatest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, maxPoints: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const int64_t requested = info[1].As<Napi::Number>().Int64Value();
  const size_t max_points = requested > 0 ? static_cast<size_t>(requested) : 0;
  std::vector<SonareMixGoniometerPoint> points(max_points);
  const size_t count = sonare_strip_read_goniometer_latest(
      strip, max_points > 0 ? points.data() : nullptr, max_points);
  Napi::Array out = Napi::Array::New(env, count);
  for (size_t index = 0; index < count; ++index) {
    Napi::Object point = Napi::Object::New(env);
    point.Set("left", points[index].left);
    point.Set("right", points[index].right);
    out.Set(index, point);
  }
  return out;
}

Napi::Value MixerWrap::StripById(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (id: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string id = info[0].As<Napi::String>().Utf8Value();
  SonareStrip* strip = sonare_mixer_strip_by_id(mixer_, id.c_str());
  if (strip == nullptr) {
    return env.Null();
  }
  const size_t count = sonare_mixer_strip_count(mixer_);
  for (size_t index = 0; index < count; ++index) {
    if (sonare_mixer_strip_at(mixer_, index) == strip) {
      return Napi::Number::New(env, static_cast<double>(index));
    }
  }
  return env.Null();
}

Napi::Value MixerWrap::AddBus(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (id: string, role?: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string id = info[0].As<Napi::String>().Utf8Value();
  const bool has_role = info.Length() >= 2 && info[1].IsString();
  const std::string role = has_role ? info[1].As<Napi::String>().Utf8Value() : std::string();
  SonareError err = sonare_mixer_add_bus(mixer_, id.c_str(), has_role ? role.c_str() : nullptr);
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to add bus: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::RemoveBus(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (id: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string id = info[0].As<Napi::String>().Utf8Value();
  SonareError err = sonare_mixer_remove_bus(mixer_, id.c_str());
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to remove bus: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::BusCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  size_t count = 0;
  SonareError err = sonare_mixer_bus_count(mixer_, &count);
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to query bus count: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(count));
}

Napi::Value MixerWrap::AddVcaGroup(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (id: string, gainDb: number, members?: string[])")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string id = info[0].As<Napi::String>().Utf8Value();
  const float gain_db = info[1].As<Napi::Number>().FloatValue();

  std::vector<std::string> member_storage;
  std::vector<const char*> member_ptrs;
  if (info.Length() >= 3 && info[2].IsArray()) {
    Napi::Array members = info[2].As<Napi::Array>();
    member_storage.reserve(members.Length());
    member_ptrs.reserve(members.Length());
    for (uint32_t i = 0; i < members.Length(); ++i) {
      Napi::Value value = members.Get(i);
      if (!value.IsString()) {
        Napi::TypeError::New(env, "VCA group members must be strings").ThrowAsJavaScriptException();
        return env.Undefined();
      }
      member_storage.push_back(value.As<Napi::String>().Utf8Value());
    }
    for (const auto& member : member_storage) {
      member_ptrs.push_back(member.c_str());
    }
  }

  SonareError err = sonare_mixer_add_vca_group(mixer_, id.c_str(), gain_db,
                                               member_ptrs.empty() ? nullptr : member_ptrs.data(),
                                               member_ptrs.size());
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to add VCA group: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::RemoveVcaGroup(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (id: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string id = info[0].As<Napi::String>().Utf8Value();
  SonareError err = sonare_mixer_remove_vca_group(mixer_, id.c_str());
  if (err != SONARE_OK) {
    Napi::Error::New(env, std::string("failed to remove VCA group: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::VcaGroupCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  size_t count = 0;
  SonareError err = sonare_mixer_vca_group_count(mixer_, &count);
  if (err != SONARE_OK) {
    Napi::Error::New(env,
                     std::string("failed to query VCA group count: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(count));
}

Napi::Value MixerWrap::ScheduleFaderAutomation(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, samplePos, faderDb, curve?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const int64_t sample_pos = info[1].As<Napi::Number>().Int64Value();
  const float fader_db = info[2].As<Napi::Number>().FloatValue();
  const int curve =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 0;
  SonareError err = sonare_strip_schedule_fader_automation(strip, sample_pos, fader_db, curve);
  if (err != SONARE_OK) {
    Napi::Error::New(
        env, std::string("failed to schedule fader automation: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::SchedulePanAutomation(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, samplePos, pan, curve?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const int64_t sample_pos = info[1].As<Napi::Number>().Int64Value();
  const float pan = info[2].As<Napi::Number>().FloatValue();
  const int curve =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 0;
  SonareError err = sonare_strip_schedule_pan_automation(strip, sample_pos, pan, curve);
  if (err != SONARE_OK) {
    Napi::Error::New(env,
                     std::string("failed to schedule pan automation: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::ScheduleWidthAutomation(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 3 || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, samplePos, width, curve?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const int64_t sample_pos = info[1].As<Napi::Number>().Int64Value();
  const float width = info[2].As<Napi::Number>().FloatValue();
  const int curve =
      info.Length() >= 4 && info[3].IsNumber() ? info[3].As<Napi::Number>().Int32Value() : 0;
  SonareError err = sonare_strip_schedule_width_automation(strip, sample_pos, width, curve);
  if (err != SONARE_OK) {
    Napi::Error::New(
        env, std::string("failed to schedule width automation: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

Napi::Value MixerWrap::ScheduleSendAutomation(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 4 || !info[1].IsNumber() || !info[2].IsNumber() || !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, sendIndex, samplePos, db, curve?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const size_t send_index = static_cast<size_t>(info[1].As<Napi::Number>().Int64Value());
  const int64_t sample_pos = info[2].As<Napi::Number>().Int64Value();
  const float db = info[3].As<Napi::Number>().FloatValue();
  const int curve =
      info.Length() >= 5 && info[4].IsNumber() ? info[4].As<Napi::Number>().Int32Value() : 0;
  SonareError err = sonare_strip_schedule_send_automation(strip, send_index, sample_pos, db, curve);
  if (err != SONARE_OK) {
    Napi::Error::New(env,
                     std::string("failed to schedule send automation: ") + ErrorMessageForCode(err))
        .ThrowAsJavaScriptException();
  }
  return env.Undefined();
}

void MixerWrap::Destroy(const Napi::CallbackInfo& /*info*/) {
  if (mixer_ != nullptr) {
    sonare_mixer_destroy(mixer_);
    mixer_ = nullptr;
  }
}

}  // namespace sonare_node
