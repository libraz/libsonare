#include <cstdint>
#include <string>

#include "engine/common.h"
#include "sonare_wrap_engine.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::engine;

Napi::Value RealtimeEngineWrap::SetBusStripJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t bus_id = 0;
  std::string scene_json;
  if (!OptionalUint32Arg(env, info, 0, "busId", 0, &bus_id) ||
      !OptionalStringArg(env, info, 1, "sceneJson", "", &scene_json)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_bus_strip_json(engine_, bus_id, scene_json.c_str()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetBusStripEqBandJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t bus_id = 0;
  int band_index = -1;
  std::string band_json;
  if (!OptionalUint32Arg(env, info, 0, "busId", 0, &bus_id) ||
      !OptionalIntArg(env, info, 1, "bandIndex", -1, &band_index) ||
      !OptionalStringArg(env, info, 2, "bandJson", "", &band_json)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_bus_strip_eq_band_json(engine_, bus_id, band_index,
                                                             band_json.c_str()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTrackStripJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t track_id = 0;
  std::string scene_json;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalStringArg(env, info, 1, "sceneJson", "", &scene_json)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_track_strip_json(engine_, track_id, scene_json.c_str()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTrackStripEqBandJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t track_id = 0;
  int band_index = -1;
  std::string band_json;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalIntArg(env, info, 1, "bandIndex", -1, &band_index) ||
      !OptionalStringArg(env, info, 2, "bandJson", "", &band_json)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_track_strip_eq_band_json(engine_, track_id, band_index,
                                                               band_json.c_str()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTrackStripInsertBypassed(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t track_id = 0;
  uint32_t insert_index = 0;
  bool bypassed = false;
  bool reset_on_bypass = false;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalUint32Arg(env, info, 1, "insertIndex", 0, &insert_index) ||
      !OptionalBoolArg(env, info, 2, "bypassed", false, &bypassed) ||
      !OptionalBoolArg(env, info, 3, "resetOnBypass", false, &reset_on_bypass)) {
    return env.Undefined();
  }
  ThrowIfError(env,
               sonare_engine_set_track_strip_insert_bypassed(
                   engine_, track_id, insert_index, bypassed ? 1 : 0, reset_on_bypass ? 1 : 0));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetMasterStripJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  std::string scene_json;
  if (!OptionalStringArg(env, info, 0, "sceneJson", "", &scene_json)) return env.Undefined();
  ThrowIfError(env, sonare_engine_set_master_strip_json(engine_, scene_json.c_str()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetMasterStripEqBandJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  int band_index = -1;
  std::string band_json;
  if (!OptionalIntArg(env, info, 0, "bandIndex", -1, &band_index) ||
      !OptionalStringArg(env, info, 1, "bandJson", "", &band_json)) {
    return env.Undefined();
  }
  ThrowIfError(env,
               sonare_engine_set_master_strip_eq_band_json(engine_, band_index, band_json.c_str()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetMasterStripInsertBypassed(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t insert_index = 0;
  bool bypassed = false;
  bool reset_on_bypass = false;
  if (!OptionalUint32Arg(env, info, 0, "insertIndex", 0, &insert_index) ||
      !OptionalBoolArg(env, info, 1, "bypassed", false, &bypassed) ||
      !OptionalBoolArg(env, info, 2, "resetOnBypass", false, &reset_on_bypass)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_master_strip_insert_bypassed(
                        engine_, insert_index, bypassed ? 1 : 0, reset_on_bypass ? 1 : 0));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTrackStripInsertParamByName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t track_id = 0;
  uint32_t insert_index = 0;
  std::string param_name;
  float value = 0.0f;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalUint32Arg(env, info, 1, "insertIndex", 0, &insert_index) ||
      !OptionalStringArg(env, info, 2, "paramName", "", &param_name) ||
      !OptionalFloatArg(env, info, 3, "value", 0.0f, &value)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_track_strip_insert_param_by_name(
                        engine_, track_id, insert_index, param_name.c_str(), value));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetMasterStripInsertParamByName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t insert_index = 0;
  std::string param_name;
  float value = 0.0f;
  if (!OptionalUint32Arg(env, info, 0, "insertIndex", 0, &insert_index) ||
      !OptionalStringArg(env, info, 1, "paramName", "", &param_name) ||
      !OptionalFloatArg(env, info, 2, "value", 0.0f, &value)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_master_strip_insert_param_by_name(engine_, insert_index,
                                                                        param_name.c_str(), value));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetBusStripInsertParamByName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t bus_id = 0;
  uint32_t insert_index = 0;
  std::string param_name;
  float value = 0.0f;
  if (!OptionalUint32Arg(env, info, 0, "busId", 0, &bus_id) ||
      !OptionalUint32Arg(env, info, 1, "insertIndex", 0, &insert_index) ||
      !OptionalStringArg(env, info, 2, "paramName", "", &param_name) ||
      !OptionalFloatArg(env, info, 3, "value", 0.0f, &value)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_bus_strip_insert_param_by_name(engine_, bus_id, insert_index,
                                                                     param_name.c_str(), value));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ApplyTrackStripInsertParamByNameNow(
    const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t track_id = 0;
  uint32_t insert_index = 0;
  std::string param_name;
  float value = 0.0f;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalUint32Arg(env, info, 1, "insertIndex", 0, &insert_index) ||
      !OptionalStringArg(env, info, 2, "paramName", "", &param_name) ||
      !OptionalFloatArg(env, info, 3, "value", 0.0f, &value)) {
    return env.Undefined();
  }
  int applied = 0;
  ThrowIfError(env, sonare_engine_apply_track_strip_insert_param_by_name_now(
                        engine_, track_id, insert_index, param_name.c_str(), value, &applied));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Boolean::New(env, applied != 0);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::RestoreTrackStripInsertParamByName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t track_id = 0;
  uint32_t insert_index = 0;
  std::string param_name;
  float value = 0.0f;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalUint32Arg(env, info, 1, "insertIndex", 0, &insert_index) ||
      !OptionalStringArg(env, info, 2, "paramName", "", &param_name) ||
      !OptionalFloatArg(env, info, 3, "value", 0.0f, &value)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_restore_track_strip_insert_param_by_name(
                        engine_, track_id, insert_index, param_name.c_str(), value));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ApplyMasterStripInsertParamByNameNow(
    const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t insert_index = 0;
  std::string param_name;
  float value = 0.0f;
  if (!OptionalUint32Arg(env, info, 0, "insertIndex", 0, &insert_index) ||
      !OptionalStringArg(env, info, 1, "paramName", "", &param_name) ||
      !OptionalFloatArg(env, info, 2, "value", 0.0f, &value)) {
    return env.Undefined();
  }
  int applied = 0;
  ThrowIfError(env, sonare_engine_apply_master_strip_insert_param_by_name_now(
                        engine_, insert_index, param_name.c_str(), value, &applied));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Boolean::New(env, applied != 0);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::RestoreMasterStripInsertParamByName(
    const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t insert_index = 0;
  std::string param_name;
  float value = 0.0f;
  if (!OptionalUint32Arg(env, info, 0, "insertIndex", 0, &insert_index) ||
      !OptionalStringArg(env, info, 1, "paramName", "", &param_name) ||
      !OptionalFloatArg(env, info, 2, "value", 0.0f, &value)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_restore_master_strip_insert_param_by_name(
                        engine_, insert_index, param_name.c_str(), value));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ApplyBusStripInsertParamByNameNow(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t bus_id = 0;
  uint32_t insert_index = 0;
  std::string param_name;
  float value = 0.0f;
  if (!OptionalUint32Arg(env, info, 0, "busId", 0, &bus_id) ||
      !OptionalUint32Arg(env, info, 1, "insertIndex", 0, &insert_index) ||
      !OptionalStringArg(env, info, 2, "paramName", "", &param_name) ||
      !OptionalFloatArg(env, info, 3, "value", 0.0f, &value)) {
    return env.Undefined();
  }
  int applied = 0;
  ThrowIfError(env, sonare_engine_apply_bus_strip_insert_param_by_name_now(
                        engine_, bus_id, insert_index, param_name.c_str(), value, &applied));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Boolean::New(env, applied != 0);
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::RestoreBusStripInsertParamByName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t bus_id = 0;
  uint32_t insert_index = 0;
  std::string param_name;
  float value = 0.0f;
  if (!OptionalUint32Arg(env, info, 0, "busId", 0, &bus_id) ||
      !OptionalUint32Arg(env, info, 1, "insertIndex", 0, &insert_index) ||
      !OptionalStringArg(env, info, 2, "paramName", "", &param_name) ||
      !OptionalFloatArg(env, info, 3, "value", 0.0f, &value)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_restore_bus_strip_insert_param_by_name(
                        engine_, bus_id, insert_index, param_name.c_str(), value));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClearTrackInsertParameterBases(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t track_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_clear_track_insert_parameter_bases(engine_, track_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClearMasterInsertParameterBases(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  ThrowIfError(env, sonare_engine_clear_master_insert_parameter_bases(engine_));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ClearBusInsertParameterBases(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t bus_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "busId", 0, &bus_id)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_clear_bus_insert_parameter_bases(engine_, bus_id));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetBusStripInsertBypassed(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t bus_id = 0;
  uint32_t insert_index = 0;
  bool bypassed = false;
  bool reset_on_bypass = false;
  if (!OptionalUint32Arg(env, info, 0, "busId", 0, &bus_id) ||
      !OptionalUint32Arg(env, info, 1, "insertIndex", 0, &insert_index) ||
      !OptionalBoolArg(env, info, 2, "bypassed", false, &bypassed) ||
      !OptionalBoolArg(env, info, 3, "resetOnBypass", false, &reset_on_bypass)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_bus_strip_insert_bypassed(
                        engine_, bus_id, insert_index, bypassed ? 1 : 0, reset_on_bypass ? 1 : 0));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ResolveTrackInsertAutomationId(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t track_id = 0;
  uint32_t insert_index = 0;
  std::string param_name;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalUint32Arg(env, info, 1, "insertIndex", 0, &insert_index) ||
      !OptionalStringArg(env, info, 2, "paramName", "", &param_name)) {
    return env.Undefined();
  }
  uint32_t out_id = 0;
  const SonareError err = sonare_engine_resolve_track_insert_automation_id(
      engine_, track_id, insert_index, param_name.c_str(), &out_id);
  if (err == SONARE_ERROR_INVALID_PARAMETER) {
    return Napi::Number::New(env, -1.0);
  }
  ThrowIfError(env, err);
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out_id));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ResolveMasterInsertAutomationId(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t insert_index = 0;
  std::string param_name;
  if (!OptionalUint32Arg(env, info, 0, "insertIndex", 0, &insert_index) ||
      !OptionalStringArg(env, info, 1, "paramName", "", &param_name)) {
    return env.Undefined();
  }
  uint32_t out_id = 0;
  const SonareError err = sonare_engine_resolve_master_insert_automation_id(
      engine_, insert_index, param_name.c_str(), &out_id);
  if (err == SONARE_ERROR_INVALID_PARAMETER) {
    return Napi::Number::New(env, -1.0);
  }
  ThrowIfError(env, err);
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out_id));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ResolveBusInsertAutomationId(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t bus_id = 0;
  uint32_t insert_index = 0;
  std::string param_name;
  if (!OptionalUint32Arg(env, info, 0, "busId", 0, &bus_id) ||
      !OptionalUint32Arg(env, info, 1, "insertIndex", 0, &insert_index) ||
      !OptionalStringArg(env, info, 2, "paramName", "", &param_name)) {
    return env.Undefined();
  }
  uint32_t out_id = 0;
  const SonareError err = sonare_engine_resolve_bus_insert_automation_id(
      engine_, bus_id, insert_index, param_name.c_str(), &out_id);
  if (err == SONARE_ERROR_INVALID_PARAMETER) {
    return Napi::Number::New(env, -1.0);
  }
  ThrowIfError(env, err);
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out_id));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::ResolveInstrumentAutomationId(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t destination_id = 0;
  std::string param_name;
  if (!OptionalUint32Arg(env, info, 0, "destinationId", 0, &destination_id) ||
      !OptionalStringArg(env, info, 1, "paramName", "", &param_name)) {
    return env.Undefined();
  }
  uint32_t out_id = 0;
  const SonareError err = sonare_engine_resolve_instrument_automation_id(
      engine_, destination_id, param_name.c_str(), &out_id);
  if (err == SONARE_ERROR_INVALID_PARAMETER) {
    return Napi::Number::New(env, -1.0);
  }
  ThrowIfError(env, err);
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out_id));
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTrackStripPan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t track_id = 0;
  float pan = 0.0f;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalFloatArg(env, info, 1, "pan", 0.0f, &pan)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_track_strip_pan(engine_, track_id, pan));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTrackStripPanLaw(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t track_id = 0;
  int pan_law = 0;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalIntArg(env, info, 1, "panLaw", 0, &pan_law)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_track_strip_pan_law(engine_, track_id, pan_law));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTrackStripPanMode(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t track_id = 0;
  int pan_mode = 0;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalIntArg(env, info, 1, "panMode", 0, &pan_mode)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_track_strip_pan_mode(engine_, track_id, pan_mode));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTrackStripDualPan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t track_id = 0;
  float left_pan = 0.0f;
  float right_pan = 0.0f;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalFloatArg(env, info, 1, "leftPan", 0.0f, &left_pan) ||
      !OptionalFloatArg(env, info, 2, "rightPan", 0.0f, &right_pan)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_track_strip_dual_pan(engine_, track_id, left_pan, right_pan));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTrackStripSurroundPan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t track_id = 0;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id)) {
    return env.Undefined();
  }
  if (info.Length() < 2 || !info[1].IsObject()) {
    Napi::TypeError::New(env, "Expected (trackId, pan: SurroundPan)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const Napi::Object obj = info[1].As<Napi::Object>();
  SonareSurroundPan pan{};
  pan.azimuth = FloatProperty(obj, "azimuth", 0.0f);
  pan.elevation = FloatProperty(obj, "elevation", 0.0f);
  pan.divergence = FloatProperty(obj, "divergence", 0.0f);
  pan.lfe = FloatProperty(obj, "lfe", 0.0f);
  pan.distance = FloatProperty(obj, "distance", 1.0f);
  if (env.IsExceptionPending()) return env.Undefined();
  ThrowIfError(env, sonare_engine_set_track_strip_surround_pan(engine_, track_id, &pan));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetBusStripPan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t bus_id = 0;
  float pan = 0.0f;
  if (!OptionalUint32Arg(env, info, 0, "busId", 0, &bus_id) ||
      !OptionalFloatArg(env, info, 1, "pan", 0.0f, &pan)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_bus_strip_pan(engine_, bus_id, pan));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetBusStripPanLaw(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t bus_id = 0;
  int pan_law = 0;
  if (!OptionalUint32Arg(env, info, 0, "busId", 0, &bus_id) ||
      !OptionalIntArg(env, info, 1, "panLaw", 0, &pan_law)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_bus_strip_pan_law(engine_, bus_id, pan_law));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetBusStripPanMode(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t bus_id = 0;
  int pan_mode = 0;
  if (!OptionalUint32Arg(env, info, 0, "busId", 0, &bus_id) ||
      !OptionalIntArg(env, info, 1, "panMode", 0, &pan_mode)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_bus_strip_pan_mode(engine_, bus_id, pan_mode));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetBusStripDualPan(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t bus_id = 0;
  float left_pan = 0.0f;
  float right_pan = 0.0f;
  if (!OptionalUint32Arg(env, info, 0, "busId", 0, &bus_id) ||
      !OptionalFloatArg(env, info, 1, "leftPan", 0.0f, &left_pan) ||
      !OptionalFloatArg(env, info, 2, "rightPan", 0.0f, &right_pan)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_engine_set_bus_strip_dual_pan(engine_, bus_id, left_pan, right_pan));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value RealtimeEngineWrap::SetTrackStripChannelDelaySamples(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  uint32_t track_id = 0;
  int delay_samples = 0;
  if (!OptionalUint32Arg(env, info, 0, "trackId", 0, &track_id) ||
      !OptionalIntArg(env, info, 1, "delaySamples", 0, &delay_samples)) {
    return env.Undefined();
  }
  ThrowIfError(
      env, sonare_engine_set_track_strip_channel_delay_samples(engine_, track_id, delay_samples));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}
