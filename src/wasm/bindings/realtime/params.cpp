/// @file realtime_engine_params.cpp
/// @brief Embind realtime-engine facade: parameters & automation.

#ifdef __EMSCRIPTEN__

#include "realtime_engine_wasm.h"

// Canonical AutomationCurve ordinals (Linear=0, Exp=1, Hold=2, SCurve=3) are
// shared with the C ABI and other bindings; conversion is a direct cast.
sonare::automation::CurveType automationCurveFromInt(int curve) {
  // Reject an out-of-range curve ordinal (not clamp), matching the C ABI and
  // Python so every surface returns the same error for the same invalid input.
  if (curve < 0 || curve > 3) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "automation curve ordinal is out of range");
  }
  return static_cast<sonare::automation::CurveType>(curve);
}

int automationCurveToInt(sonare::automation::CurveType curve) { return static_cast<int>(curve); }

void RealtimeEngineWasm::addParameter(val info) {
  const uint32_t id = static_cast<uint32_t>(intProperty(info, "id", 0));
  if (id == 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "parameter id must be non-zero");
  }
  if (sonare::engine::RealtimeEngine::parameter_target_reserved(id)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "parameter id is reserved by the engine");
  }
  parameter_strings_.push_back(stringProperty(info, "name", ""));
  parameter_strings_.push_back(stringProperty(info, "unit", ""));
  sonare::automation::ParameterInfo parameter{};
  parameter.id = id;
  parameter.name = parameter_strings_[parameter_strings_.size() - 2].c_str();
  parameter.unit = parameter_strings_[parameter_strings_.size() - 1].c_str();
  parameter.min_value = floatProperty(info, "minValue", 0.0f);
  parameter.max_value = floatProperty(info, "maxValue", 1.0f);
  parameter.default_value = floatProperty(info, "defaultValue", 0.0f);
  parameter.rt_safe = boolProperty(info, "rtSafe", true);
  parameter.default_curve = automationCurveFromInt(intProperty(info, "defaultCurve", 1));
  if (!parameters_.add(parameter)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "duplicate parameter id");
  }
  publishParameterMetadata();
}

int RealtimeEngineWasm::parameterCount() const {
  return static_cast<int>(parameters_.parameter_count());
}

val RealtimeEngineWasm::parameterInfoByIndex(int index) const {
  sonare::automation::ParameterInfo info{};
  if (index < 0 || !parameters_.parameter_info_by_index(static_cast<size_t>(index), &info)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "parameter index out of range");
  }
  return parameterToVal(info);
}

val RealtimeEngineWasm::parameterInfo(double id) const {
  sonare::automation::ParameterInfo info{};
  if (!parameters_.parameter_info(static_cast<uint32_t>(id), &info)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown parameter id");
  }
  return parameterToVal(info);
}

void RealtimeEngineWasm::setAutomationLane(double param_id, val points) {
  // NOTE: a registered, explicitly non-RT-safe parameter surfaces
  // synchronously (a throw), whereas setParameter/setParameterSmoothed and
  // the canonical C API (sonare_engine_set_automation_lane) report the same
  // misuse asynchronously via kNonRealtimeSafeParameter telemetry. The
  // synchronous throw is kept here intentionally because setAutomationLane
  // is a control-thread (offline) setter, so an immediate, actionable error
  // is preferable to a deferred telemetry record. Unregistered ids — notably
  // the reserved engine namespace (0x4D58xxxx mixer fader/pan targets) — are
  // accepted, matching the C oracle's gating.
  if (registeredParameterRejectsRealtime(static_cast<uint32_t>(param_id))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "parameter is not realtime safe");
  }
  sonare::automation::AutomationLane lane(static_cast<uint32_t>(param_id));
  std::vector<sonare::automation::Breakpoint> breakpoints;
  const int count = points["length"].as<int>();
  breakpoints.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    val point = points[i];
    const double ppq = objectProperty(point, "ppq").as<double>();
    const float value = floatProperty(point, "value", 0.0f);
    // Match the C ABI: reject non-finite automation breakpoints (WASM bypasses the C-ABI guard).
    if (!std::isfinite(ppq) || !std::isfinite(value)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "automation breakpoint ppq and value must be finite");
    }
    breakpoints.push_back(
        {ppq, value, automationCurveFromInt(intProperty(point, "curveToNext", 0))});
  }
  lane.set_points(std::move(breakpoints));
  bool replaced = false;
  for (auto& existing : automation_lanes_) {
    if (existing.target_param_id() == static_cast<uint32_t>(param_id)) {
      existing = std::move(lane);
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    automation_lanes_.push_back(std::move(lane));
  }
  engine_.automation().set_lanes(automation_lanes_);
}

int RealtimeEngineWasm::automationLaneCount() const {
  return static_cast<int>(engine_.automation().lane_count());
}

void RealtimeEngineWasm::setParameter(double param_id, float value, int64_t render_frame) {
  if (registeredParameterRejectsRealtime(static_cast<uint32_t>(param_id))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "parameter is not realtime safe");
  }
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kSetParam;
  command.target_id = static_cast<uint32_t>(param_id);
  command.sample_time = render_frame;
  command.arg.f = value;
  if (!engine_.push_command(command)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "failed to queue set parameter command");
  }
}

void RealtimeEngineWasm::setParameterSmoothed(double param_id, float value, int64_t render_frame) {
  if (registeredParameterRejectsRealtime(static_cast<uint32_t>(param_id))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "parameter is not realtime safe");
  }
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kSetParamSmoothed;
  command.target_id = static_cast<uint32_t>(param_id);
  command.sample_time = render_frame;
  command.arg.f = value;
  if (!engine_.push_command(command)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "failed to queue set parameter command");
  }
}

void RealtimeEngineWasm::setParamSmoothingMs(float smoothing_ms) {
  if (!std::isfinite(smoothing_ms) || smoothing_ms < 0.0f) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "smoothing_ms must be finite and non-negative");
  }
  engine_.set_param_smoothing_ms(smoothing_ms);
}

void RealtimeEngineWasm::setSoloMute(uint32_t lane_index, bool solo, bool mute,
                                     int64_t render_frame) {
  sonare::rt::Command command{};
  command.type = sonare::rt::CommandType::kSetSoloMute;
  command.target_id = lane_index;
  command.sample_time = render_frame;
  command.arg.i = (mute ? 0x1 : 0x0) | (solo ? 0x2 : 0x0);
  if (!engine_.push_command(command)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "failed to queue solo/mute command");
  }
}

// Mirrors the C ABI sonare_engine_clear_parameters.
void RealtimeEngineWasm::clearParameters() {
  parameters_.clear();
  parameter_strings_.clear();
  automation_lanes_.clear();
  publishParameterMetadata();
  engine_.automation().set_lanes(automation_lanes_);
}

val RealtimeEngineWasm::parameterToVal(const sonare::automation::ParameterInfo& info) {
  val out = val::object();
  out.set("id", info.id);
  out.set("name", std::string(info.name ? info.name : ""));
  out.set("unit", std::string(info.unit ? info.unit : ""));
  out.set("minValue", info.min_value);
  out.set("maxValue", info.max_value);
  out.set("defaultValue", info.default_value);
  out.set("rtSafe", info.rt_safe);
  out.set("defaultCurve", automationCurveToInt(info.default_curve));
  return out;
}

void RealtimeEngineWasm::publishParameterMetadata() {
  std::vector<sonare::automation::ParameterInfo> parameters;
  parameters.reserve(parameters_.parameter_count());
  for (size_t i = 0; i < parameters_.parameter_count(); ++i) {
    sonare::automation::ParameterInfo info{};
    if (parameters_.parameter_info_by_index(i, &info)) {
      parameters.push_back(info);
    }
  }
  engine_.automation().set_parameter_metadata(std::move(parameters));
}

bool RealtimeEngineWasm::registeredParameterRejectsRealtime(uint32_t param_id) const {
  sonare::automation::ParameterInfo info{};
  return parameters_.parameter_info(param_id, &info) && !info.rt_safe;
}

void registerRealtimeEngineParams(class_<RealtimeEngineWasm>& cls) {
  cls.function("setParameter", &RealtimeEngineWasm::setParameter)
      .function("setParameterSmoothed", &RealtimeEngineWasm::setParameterSmoothed)
      .function("setParamSmoothingMs", &RealtimeEngineWasm::setParamSmoothingMs)
      .function("setSoloMute", &RealtimeEngineWasm::setSoloMute)
      .function("clearParameters", &RealtimeEngineWasm::clearParameters)
      .function("addParameter", &RealtimeEngineWasm::addParameter)
      .function("parameterCount", &RealtimeEngineWasm::parameterCount)
      .function("parameterInfoByIndex", &RealtimeEngineWasm::parameterInfoByIndex)
      .function("parameterInfo", &RealtimeEngineWasm::parameterInfo)
      .function("setAutomationLane", &RealtimeEngineWasm::setAutomationLane)
      .function("automationLaneCount", &RealtimeEngineWasm::automationLaneCount);
}

#endif  // __EMSCRIPTEN__
