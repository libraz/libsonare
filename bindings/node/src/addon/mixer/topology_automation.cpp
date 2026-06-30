#include <cstdint>
#include <string>
#include <vector>

#include "sonare_wrap_mixer.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {

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
    sonare_node::ThrowSonareError(env, err, "failed to add bus: ");
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
    sonare_node::ThrowSonareError(env, err, "failed to remove bus: ");
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
    sonare_node::ThrowSonareError(env, err, "failed to query bus count: ");
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
    sonare_node::ThrowSonareError(env, err, "failed to add VCA group: ");
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
    sonare_node::ThrowSonareError(env, err, "failed to remove VCA group: ");
  }
  return env.Undefined();
}

Napi::Value MixerWrap::SetVcaGroupGainDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (mixer_ == nullptr) {
    Napi::Error::New(env, "Mixer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (id: string, gainDb: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string id = info[0].As<Napi::String>().Utf8Value();
  const float gain_db = info[1].As<Napi::Number>().FloatValue();
  SonareError err = sonare_mixer_set_vca_group_gain_db(mixer_, id.c_str(), gain_db);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set VCA group gain: ");
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
    sonare_node::ThrowSonareError(env, err, "failed to query VCA group count: ");
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
    sonare_node::ThrowSonareError(env, err, "failed to schedule fader automation: ");
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
    sonare_node::ThrowSonareError(env, err, "failed to schedule pan automation: ");
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
    sonare_node::ThrowSonareError(env, err, "failed to schedule width automation: ");
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
    sonare_node::ThrowSonareError(env, err, "failed to schedule send automation: ");
  }
  return env.Undefined();
}

}  // namespace sonare_node
