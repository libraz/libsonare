#include <array>
#include <limits>
#include <string>

#include "engine/insert_automation_id.h"
#include "engine/instrument_automation_id.h"
#include "engine/realtime_engine.h"
#include "engine/realtime_engine_internal.h"
#include "engine/strip_spec.h"
#include "engine/track_mixer.h"
#include "mastering/api/insert_factory.h"
#include "util/constants.h"
#include "util/json.h"

namespace sonare::engine {

#if defined(SONARE_WITH_MIXING)

using sonare::constants::kFloorDb;

namespace {

// Reads @p processor_name's catalog entry for @p param_id out of
// insert_param_info_json (the same JSON the capability catalog and the C ABI's
// insert-param-info entry point read), and fills @p out from it. A null
// min/max/unit in the catalog is an unmeasured bound, not a zero one, so it
// maps to +/-infinity / empty string rather than being read as 0.
bool describe_insert_param_from_catalog(const std::string& processor_name, unsigned int param_id,
                                        automation::ParameterDescription* out) {
  util::json::Value parsed;
  try {
    parsed = util::json::parse(mastering::api::insert_param_info_json(processor_name));
  } catch (const util::json::JsonError&) {
    return false;
  }
  if (!parsed.is_array()) return false;
  for (const util::json::Value& entry : parsed.as_array()) {
    const util::json::Value* id_field = entry.find("id");
    if (id_field == nullptr || !id_field->is_number()) continue;
    if (static_cast<unsigned int>(id_field->as_int()) != param_id) continue;
    const util::json::Value* name_field = entry.find("name");
    out->name = name_field != nullptr && name_field->is_string() ? name_field->as_string() : "";
    const util::json::Value* min_field = entry.find("min");
    out->min_value = min_field != nullptr && min_field->is_number()
                         ? min_field->as_float()
                         : -std::numeric_limits<float>::infinity();
    const util::json::Value* max_field = entry.find("max");
    out->max_value = max_field != nullptr && max_field->is_number()
                         ? max_field->as_float()
                         : std::numeric_limits<float>::infinity();
    const util::json::Value* default_field = entry.find("default");
    if (default_field != nullptr && default_field->is_number()) {
      out->default_value = default_field->as_float();
    } else if (default_field != nullptr && default_field->is_bool()) {
      out->default_value = default_field->as_bool() ? 1.0f : 0.0f;
    } else {
      out->default_value = 0.0f;
    }
    const util::json::Value* unit_field = entry.find("unit");
    out->unit = unit_field != nullptr && unit_field->is_string() ? unit_field->as_string() : "";
    const util::json::Value* rt_safe_field = entry.find("rtSafe");
    out->rt_safe = rt_safe_field != nullptr && rt_safe_field->is_bool() && rt_safe_field->as_bool();
    out->default_curve = automation::CurveType::Linear;
    return true;
  }
  return false;
}

}  // namespace

const rt::ProcessorBase* RealtimeEngine::current_insert_processor(
    const InsertAutomationTarget& target) const noexcept {
  switch (target.strip) {
    case InsertStripKind::kTrack:
      return track_mixer_runtime_.track_insert_processor(target.owner_id, target.insert_index);
    case InsertStripKind::kBus:
      return track_mixer_runtime_.bus_insert_processor(target.owner_id, target.insert_index);
    case InsertStripKind::kMaster:
      return owned_master_strip_ != nullptr
                 ? owned_master_strip_->insert_processor(target.insert_index)
                 : nullptr;
    case InsertStripKind::kTrackLane:
    case InsertStripKind::kBusLane:
      return nullptr;
  }
  return nullptr;
}

bool RealtimeEngine::insert_target_current(const InsertAutomationTarget& target) const noexcept {
  if (target.strip == InsertStripKind::kTrackLane) {
    uint32_t position = 0;
    return track_mixer_runtime_.track_lane_position(target.owner_id, &position);
  }
  if (target.strip == InsertStripKind::kBusLane) {
    return track_mixer_runtime_.configured_bus(target.owner_id);
  }
  return insert_processor_layout(current_insert_processor(target)) == target.processor;
}

void RealtimeEngine::sync_insert_automation_targets() noexcept {
  const size_t retired =
      insert_automation_targets_.retire_if([this](const InsertAutomationTarget& target) noexcept {
        return !insert_target_current(target);
      });
  const auto mint = [this](InsertStripKind strip, uint32_t owner_id, size_t insert_count) {
    for (size_t slot = 0; slot < insert_count && slot <= kInsertIndexMask; ++slot) {
      InsertAutomationTarget target{strip, owner_id, static_cast<unsigned int>(slot), {}};
      target.processor = insert_processor_layout(current_insert_processor(target));
      if (!target.processor.empty()) (void)insert_automation_targets_.ensure(target);
    }
  };
  std::array<uint32_t, TrackMixerRuntime::kMaxTrackLanes> track_ids{};
  const size_t track_count =
      track_mixer_runtime_.copy_control_lane_track_ids(track_ids.data(), track_ids.size());
  for (size_t i = 0; i < track_count; ++i) {
    (void)insert_automation_targets_.ensure({InsertStripKind::kTrackLane, track_ids[i], 0, {}});
    mint(InsertStripKind::kTrack, track_ids[i],
         track_mixer_runtime_.track_insert_count(track_ids[i]));
  }
  for (const TrackBusConfig& bus : track_mixer_runtime_.bus_configs()) {
    (void)insert_automation_targets_.ensure({InsertStripKind::kBusLane, bus.bus_id, 0, {}});
    mint(InsertStripKind::kBus, bus.bus_id, track_mixer_runtime_.bus_insert_count(bus.bus_id));
  }
  if (owned_master_strip_ != nullptr) {
    mint(InsertStripKind::kMaster, 0,
         owned_master_strip_->num_pre_inserts() + owned_master_strip_->num_post_inserts());
  }
  if (retired == 0) return;
  // Edits and bases under a retired id can never apply again.
  InsertPurge purge;
  purge.retired_only = true;
  purge_insert_edits(purge);
}

int64_t RealtimeEngine::insert_automation_id(const InsertAutomationTarget& target,
                                             unsigned int param_id) const noexcept {
  if (target.insert_index > kInsertIndexMask || param_id > kInsertParamFieldMask) return -1;
  const int64_t selector = insert_automation_targets_.find(target);
  if (selector < 0) return -1;
  return make_insert_param_id(static_cast<uint32_t>(selector), target.insert_index, param_id);
}

int64_t RealtimeEngine::track_lane_automation_id(uint32_t track_id,
                                                 unsigned int param_id) const noexcept {
  if (param_id != TrackMixerRuntime::kFaderDb && param_id != TrackMixerRuntime::kPan) return -1;
  return insert_automation_id({InsertStripKind::kTrackLane, track_id, 0, {}}, param_id);
}

int64_t RealtimeEngine::resolve_track_lane_automation_id(uint32_t track_id,
                                                         const std::string& key) noexcept {
  if (key == "faderDb") return track_lane_automation_id(track_id, TrackMixerRuntime::kFaderDb);
  if (key == "pan") return track_lane_automation_id(track_id, TrackMixerRuntime::kPan);
  return -1;
}

int64_t RealtimeEngine::resolve_master_automation_id(const std::string& key) const noexcept {
  uint32_t kind = 0;
  if (key == "faderDb") {
    kind = TrackMixerRuntime::kFaderDb;
  } else if (key == "pan") {
    kind = TrackMixerRuntime::kPan;
  } else if (key == "width") {
    kind = TrackMixerRuntime::kWidth;
  } else {
    return -1;
  }
  return static_cast<int64_t>(kEngineParamNamespace |
                              (kEngineParamLaneMaster << kEngineParamLaneShift) | kind);
}

int64_t RealtimeEngine::resolve_bus_automation_id(uint32_t bus_id,
                                                  const std::string& key) noexcept {
  if (key != "faderDb") return -1;
  return insert_automation_id({InsertStripKind::kBusLane, bus_id, 0, {}},
                              TrackMixerRuntime::kFaderDb);
}

int64_t RealtimeEngine::resolve_track_insert_automation_id(uint32_t track_id,
                                                           unsigned int insert_index,
                                                           const std::string& key) noexcept {
  size_t lane_index = 0;
  unsigned int param_id = 0;
  if (!track_mixer_runtime_.resolve_track_insert_param(track_id, insert_index, key, &lane_index,
                                                       &param_id)) {
    return -1;
  }
  InsertAutomationTarget target{InsertStripKind::kTrack, track_id, insert_index, {}};
  target.processor = insert_processor_layout(current_insert_processor(target));
  return insert_automation_id(target, param_id);
}

int64_t RealtimeEngine::resolve_master_insert_automation_id(unsigned int insert_index,
                                                            const std::string& key) noexcept {
  if (owned_master_strip_ == nullptr) return -1;
  const int id = owned_master_strip_->insert_parameter_id_for_key(insert_index, key);
  if (id < 0) return -1;
  InsertAutomationTarget target{InsertStripKind::kMaster, 0, insert_index, {}};
  target.processor = insert_processor_layout(current_insert_processor(target));
  return insert_automation_id(target, static_cast<unsigned int>(id));
}

int64_t RealtimeEngine::resolve_bus_insert_automation_id(uint32_t bus_id, unsigned int insert_index,
                                                         const std::string& key) noexcept {
  size_t bus_index = 0;
  unsigned int param_id = 0;
  if (!track_mixer_runtime_.resolve_bus_insert_param(bus_id, insert_index, key, &bus_index,
                                                     &param_id)) {
    return -1;
  }
  InsertAutomationTarget target{InsertStripKind::kBus, bus_id, insert_index, {}};
  target.processor = insert_processor_layout(current_insert_processor(target));
  return insert_automation_id(target, param_id);
}

bool RealtimeEngine::route_engine_parameter(uint32_t target_id, float value) noexcept {
  if (!parameter_target_reserved(target_id)) return false;
#if defined(SONARE_WITH_ARRANGEMENT)
  // Instrument-automation namespace: hosted instruments live outside the mixer
  // runtimes, so they are decoded first and served by their own slot table.
  if (is_instrument_param_id(target_id)) {
    return route_instrument_parameter(target_id, value);
  }
#endif
  // Insert-automation namespace: the strip field names a target entry, which
  // names the strip by identity. A retired entry, or a slot that now holds
  // another processor layout, is an unknown target rather than a retarget.
  if (is_insert_param_id(target_id)) {
    const InsertAutomationTarget* target = live_insert_target(target_id);
    if (target == nullptr || !insert_target_holds(*target)) return false;
    const unsigned int param_id = static_cast<unsigned int>(insert_param_param(target_id));
    switch (target->strip) {
      case InsertStripKind::kMaster:
        return route_master_insert_param_smoothed(target->insert_index, param_id, value, target_id);
      case InsertStripKind::kBus:
        return track_mixer_runtime_.route_bus_insert_param_smoothed_by_id(
            target->owner_id, target->insert_index, param_id, value);
      case InsertStripKind::kTrack:
        return track_mixer_runtime_.route_track_insert_param_smoothed_by_id(
            target->owner_id, target->insert_index, param_id, value);
      case InsertStripKind::kTrackLane:
        // Width is not a track lane control; set_lane_parameter refuses it.
        return track_mixer_runtime_.set_track_parameter(target->owner_id, param_id, value);
      case InsertStripKind::kBusLane:
        if (param_id != TrackMixerRuntime::kFaderDb) return false;
        return track_mixer_runtime_.set_bus_gain_db(target->owner_id, value);
    }
    return false;
  }
  const uint32_t lane = (target_id & kEngineParamLaneMask) >> kEngineParamLaneShift;
  const uint32_t kind = target_id & kEngineParamKindMask;
  if (lane == kEngineParamLaneMaster) {
    return mixing_runtime_.set_parameter(kind, value);
  }
  return false;
}

bool RealtimeEngine::route_engine_parameter_thunk(void* context, uint32_t param_id,
                                                  float value) noexcept {
  return static_cast<RealtimeEngine*>(context)->route_engine_parameter(param_id, value);
}

bool RealtimeEngine::describe_reserved_parameter(uint32_t id,
                                                 automation::ParameterDescription* out) const {
  if (out == nullptr || !parameter_target_reserved(id)) return false;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (is_instrument_param_id(id)) {
    return describe_instrument_reserved_parameter(id, out);
  }
#endif
  const bool insert_id = is_insert_param_id(id);
  const InsertAutomationTarget* target = insert_id ? live_insert_target(id) : nullptr;
  if (insert_id && (target == nullptr || !insert_target_current(*target))) return false;
  if (insert_id && !insert_strip_slotless(target->strip)) {
    const unsigned int insert_index = target->insert_index;
    const unsigned int param_id = static_cast<unsigned int>(insert_param_param(id));
    std::string processor_name;
    if (target->strip == InsertStripKind::kMaster) {
      // The retained spec describes the owned strip only; a strip bound since
      // through bind_mixing_strip has none.
      if (owned_master_strip_ == nullptr || mixing_runtime_.strip() != owned_master_strip_.get()) {
        return false;
      }
      const std::string* name = strip_insert_processor_name_at(master_strip_spec_, insert_index);
      if (name == nullptr) return false;
      processor_name = *name;
    } else if (target->strip == InsertStripKind::kBus) {
      if (!track_mixer_runtime_.bus_insert_processor_name_by_id(target->owner_id, insert_index,
                                                                &processor_name)) {
        return false;
      }
    } else if (!track_mixer_runtime_.track_insert_processor_name(target->owner_id, insert_index,
                                                                 &processor_name)) {
      return false;
    }
    return describe_insert_param_from_catalog(processor_name, param_id, out);
  }
  const uint32_t lane = (id & kEngineParamLaneMask) >> kEngineParamLaneShift;
  const uint32_t kind = insert_id ? insert_param_param(id) : id & kEngineParamKindMask;
  const bool is_master = !insert_id && lane == kEngineParamLaneMaster;
  const bool is_bus = insert_id && target->strip == InsertStripKind::kBusLane;
  // Mirrors route_engine_parameter's own per-scope kind restriction: a bus
  // owns only its fader, a track lane owns fader and pan, and width is a
  // master-only control.
  if (is_bus) {
    if (kind != TrackMixerRuntime::kFaderDb) return false;
  } else if (insert_id) {
    if (kind != TrackMixerRuntime::kFaderDb && kind != TrackMixerRuntime::kPan) return false;
  } else if (!is_master) {
    return false;
  }
  switch (kind) {
    case TrackMixerRuntime::kFaderDb:
      out->name = "faderDb";
      out->unit = "dB";
      if (is_master) {
        // GainProcessor::set_gain_db (reached by MixingRuntime::set_parameter)
        // only rejects a non-finite value; unlike the lane/bus fader setters
        // it applies no numeric clamp.
        out->min_value = -std::numeric_limits<float>::infinity();
        out->max_value = std::numeric_limits<float>::infinity();
      } else {
        // TrackMixerRuntime::set_lane_parameter / set_bus_gain_db.
        out->min_value = kFloorDb;
        out->max_value = TrackMixerRuntime::kMaxGainDb;
      }
      out->default_value = 0.0f;
      break;
    case TrackMixerRuntime::kPan:
      // mixing::pan_in_range.
      out->name = "pan";
      out->unit = "";
      out->min_value = -1.0f;
      out->max_value = 1.0f;
      out->default_value = 0.0f;
      break;
    case TrackMixerRuntime::kWidth:
      // mixing::width_in_range.
      out->name = "width";
      out->unit = "";
      out->min_value = 0.0f;
      out->max_value = 2.0f;
      out->default_value = 1.0f;
      break;
    default:
      return false;
  }
  out->rt_safe = true;
  out->default_curve = automation::CurveType::Linear;
  return true;
}

bool RealtimeEngine::insert_parameter_constructed_value(uint32_t target_id,
                                                        float* out_value) const noexcept {
  if (out_value == nullptr || !is_insert_param_id(target_id)) return false;
  const InsertAutomationTarget* target = live_insert_target(target_id);
  if (target == nullptr || !insert_target_holds(*target)) return false;
  const unsigned int insert_index = target->insert_index;
  const unsigned int param_id = static_cast<unsigned int>(insert_param_param(target_id));
  switch (target->strip) {
    case InsertStripKind::kMaster:
      return owned_master_strip_->constructed_insert_parameter_value(insert_index, param_id,
                                                                     out_value);
    case InsertStripKind::kBus:
      return track_mixer_runtime_.bus_insert_constructed_parameter_value(
          target->owner_id, insert_index, param_id, out_value);
    case InsertStripKind::kTrack:
      return track_mixer_runtime_.lane_insert_constructed_parameter_value(
          target->owner_id, insert_index, param_id, out_value);
    case InsertStripKind::kTrackLane:
    case InsertStripKind::kBusLane:
      return false;
  }
  return false;
}

const InsertAutomationTarget* RealtimeEngine::live_insert_target(
    uint32_t target_id) const noexcept {
  const InsertAutomationTarget* target =
      insert_automation_targets_.live(insert_param_strip(target_id));
  return target != nullptr && target->insert_index == insert_param_index(target_id) ? target
                                                                                    : nullptr;
}

bool RealtimeEngine::insert_target_holds(const InsertAutomationTarget& target) const noexcept {
  switch (target.strip) {
    case InsertStripKind::kMaster:
      return owned_master_strip_ != nullptr &&
             insert_processor_layout(owned_master_strip_->insert_processor(target.insert_index)) ==
                 target.processor;
    case InsertStripKind::kBus:
      return track_mixer_runtime_.bus_insert_holds(target.owner_id, target.insert_index,
                                                   target.processor);
    case InsertStripKind::kTrack:
      return track_mixer_runtime_.lane_insert_holds(target.owner_id, target.insert_index,
                                                    target.processor);
    case InsertStripKind::kTrackLane:
    case InsertStripKind::kBusLane:
      // A live entry's track or bus is configured; the fader setter finds it by id.
      return true;
  }
  return false;
}

bool RealtimeEngine::restore_track_lane_parameter(uint32_t target_id) noexcept {
  if (!is_insert_param_id(target_id)) return false;
  const InsertAutomationTarget* target = live_insert_target(target_id);
  if (target == nullptr || target->strip != InsertStripKind::kTrackLane) return false;
  return track_mixer_runtime_.restore_track_parameter(target->owner_id,
                                                      insert_param_param(target_id));
}

#endif

}  // namespace sonare::engine
