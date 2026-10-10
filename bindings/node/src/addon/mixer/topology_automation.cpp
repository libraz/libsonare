#include <cstdint>
#include <string>
#include <vector>

#include "sonare_wrap_mixer.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {

Napi::Value MixerWrap::AddStrip(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (mixer_ == nullptr) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE,
                                         "Mixer is not initialized");
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (id: string, metering?: object)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string id = node_narrow_string(env, info[0], "id");
  // Refuse a wrong-typed bag by name, the way the *Property readers below refuse
  // a wrong-typed field. Accepting it and reading nothing out of it would put
  // the caller on the full default metering with nothing to say otherwise.
  Napi::Object metering = Napi::Object::New(env);
  if (info.Length() >= 2 && !info[1].IsUndefined() && !info[1].IsNull()) {
    if (!info[1].IsObject() || info[1].IsArray()) {
      Napi::TypeError::New(env, "addStrip: metering must be a plain object")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    metering = info[1].As<Napi::Object>();
  }
  const bool enabled = BoolProperty(metering, "enabled", true);
  const bool lufs = BoolProperty(metering, "lufs", true);
  const bool true_peak = BoolProperty(metering, "truePeak", true);
  const int true_peak_oversample = IntProperty(metering, "truePeakOversample", kZeroIsSentinel);
  if (env.IsExceptionPending()) return env.Undefined();
  // Returns the strip pointer rather than a SonareError, so NULL is the whole
  // failure signal and the detail is in the thread-local error slot. The pointer
  // is mixer-owned and never reaches JS: strips are addressed by index or id.
  if (sonare_mixer_add_strip_ex(mixer_, id.c_str(), enabled ? 1 : 0, lufs ? 1 : 0,
                                true_peak ? 1 : 0, true_peak_oversample) == nullptr) {
    sonare_node::ThrowLastSonareError(env, "failed to add strip: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::AddBus(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (mixer_ == nullptr) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE,
                                         "Mixer is not initialized");
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (id: string, role?: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string id = node_narrow_string(env, info[0], "id");
  const bool has_role = info.Length() >= 2 && info[1].IsString();
  const std::string role = has_role ? node_narrow_string(env, info[1], "role") : std::string();
  SonareError err = sonare_mixer_add_bus(mixer_, id.c_str(), has_role ? role.c_str() : nullptr);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to add bus: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetOutputBus(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (mixer_ == nullptr) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE,
                                         "Mixer is not initialized");
    return env.Undefined();
  }
  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
    Napi::TypeError::New(env, "Expected (sourceId: string, busId: string)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string source_id = node_narrow_string(env, info[0], "sourceId");
  const std::string bus_id = node_narrow_string(env, info[1], "busId");
  if (env.IsExceptionPending()) return env.Undefined();
  const SonareError err = sonare_mixer_set_output_bus(mixer_, source_id.c_str(), bus_id.c_str());
  if (err != SONARE_OK) {
    sonare_node::ThrowLastSonareError(env, "", err);
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::RemoveBus(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (mixer_ == nullptr) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE,
                                         "Mixer is not initialized");
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (id: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string id = node_narrow_string(env, info[0], "id");
  SonareError err = sonare_mixer_remove_bus(mixer_, id.c_str());
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to remove bus: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::BusCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (mixer_ == nullptr) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE,
                                         "Mixer is not initialized");
    return env.Undefined();
  }
  size_t count = 0;
  SonareError err = sonare_mixer_bus_count(mixer_, &count);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to query bus count: ");
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::AddVcaGroup(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (mixer_ == nullptr) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE,
                                         "Mixer is not initialized");
    return env.Undefined();
  }
  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (id: string, gainDb: number, members?: string[])")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string id = node_narrow_string(env, info[0], "id");
  const float gain_db = node_narrow_finite_float(env, info[1], "gainDb");

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
      member_storage.push_back(node_narrow_string(env, value, "members"));
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
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::RemoveVcaGroup(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (mixer_ == nullptr) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE,
                                         "Mixer is not initialized");
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected (id: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string id = node_narrow_string(env, info[0], "id");
  SonareError err = sonare_mixer_remove_vca_group(mixer_, id.c_str());
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to remove VCA group: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetVcaGroupGainDb(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (mixer_ == nullptr) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE,
                                         "Mixer is not initialized");
    return env.Undefined();
  }
  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (id: string, gainDb: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string id = node_narrow_string(env, info[0], "id");
  const float gain_db = node_narrow_finite_float(env, info[1], "gainDb");
  SonareError err = sonare_mixer_set_vca_group_gain_db(mixer_, id.c_str(), gain_db);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set VCA group gain: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SetVcaGroupMembers(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (mixer_ == nullptr) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE,
                                         "Mixer is not initialized");
    return env.Undefined();
  }
  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsArray()) {
    Napi::TypeError::New(env, "Expected (id: string, members: string[])")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string id = node_narrow_string(env, info[0], "id");
  const Napi::Array members = info[1].As<Napi::Array>();
  std::vector<std::string> member_storage;
  std::vector<const char*> member_ptrs;
  member_storage.reserve(members.Length());
  member_ptrs.reserve(members.Length());
  for (uint32_t i = 0; i < members.Length(); ++i) {
    const Napi::Value value = members.Get(i);
    if (!value.IsString()) {
      Napi::TypeError::New(env, "VCA group members must be strings").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    member_storage.push_back(node_narrow_string(env, value, "members"));
  }
  for (const auto& member : member_storage) member_ptrs.push_back(member.c_str());
  const SonareError err = sonare_mixer_set_vca_group_members(
      mixer_, id.c_str(), member_ptrs.empty() ? nullptr : member_ptrs.data(), member_ptrs.size());
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to set VCA group members: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::VcaGroupCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (mixer_ == nullptr) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_STATE,
                                         "Mixer is not initialized");
    return env.Undefined();
  }
  size_t count = 0;
  SonareError err = sonare_mixer_vca_group_count(mixer_, &count);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to query VCA group count: ");
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(count));
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::ScheduleFaderAutomation(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 3 || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, samplePos, faderDb, curve?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const int64_t sample_pos = sonare_node::node_narrow_int64(env, info[1], "samplePos");
  const float fader_db = node_narrow_finite_float(env, info[2], "faderDb");
  int curve{};
  if (!OptionalIntArg(env, info, 3, "curve", 0, &curve)) return env.Undefined();
  SonareError err = sonare_strip_schedule_fader_automation(strip, sample_pos, fader_db, curve);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to schedule fader automation: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::SchedulePanAutomation(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 3 || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, samplePos, pan, curve?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const int64_t sample_pos = sonare_node::node_narrow_int64(env, info[1], "samplePos");
  const float pan = node_narrow_finite_float(env, info[2], "pan");
  int curve{};
  if (!OptionalIntArg(env, info, 3, "curve", 0, &curve)) return env.Undefined();
  SonareError err = sonare_strip_schedule_pan_automation(strip, sample_pos, pan, curve);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to schedule pan automation: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::ScheduleWidthAutomation(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 3 || !info[1].IsNumber() || !info[2].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, samplePos, width, curve?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const int64_t sample_pos = sonare_node::node_narrow_int64(env, info[1], "samplePos");
  const float width = node_narrow_finite_float(env, info[2], "width");
  int curve{};
  if (!OptionalIntArg(env, info, 3, "curve", 0, &curve)) return env.Undefined();
  SonareError err = sonare_strip_schedule_width_automation(strip, sample_pos, width, curve);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to schedule width automation: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value MixerWrap::ScheduleSendAutomation(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 4 || !info[1].IsNumber() || !info[2].IsNumber() || !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (strip, sendIndex, samplePos, db, curve?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareStrip* strip = ResolveStrip(info, info[0]);
  if (strip == nullptr) {
    return env.Undefined();
  }
  const size_t send_index =
      static_cast<size_t>(sonare_node::node_narrow_int64(env, info[1], "sendIndex"));
  const int64_t sample_pos = sonare_node::node_narrow_int64(env, info[2], "samplePos");
  const float db = node_narrow_finite_float(env, info[3], "db");
  int curve{};
  if (!OptionalIntArg(env, info, 4, "curve", 0, &curve)) return env.Undefined();
  SonareError err = sonare_strip_schedule_send_automation(strip, send_index, sample_pos, db, curve);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err, "failed to schedule send automation: ");
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

}  // namespace sonare_node
