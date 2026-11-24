#pragma once

#include <cstdint>
#include <string_view>

#include "engine/realtime_engine.h"
#include "util/parameter_checks.h"

#if defined(SONARE_WITH_MIXING)
#include "c_api/engine_config_domains.h"
#include "c_api/engine_mixer_refusal.h"
#include "engine/insert_automation_id.h"
#include "rt/processor_base.h"
#endif

namespace sonare_c_detail::engine_automation {

namespace check = sonare::util::check;
using sonare::engine::RealtimeEngine;

#if defined(SONARE_WITH_MIXING)
[[noreturn]] inline void refuse_track_id() {
  check::refuse_unknown_id("track_id", "a track", "engine");
}

[[noreturn]] inline void refuse_bus_id() { check::refuse_unknown_id("bus_id", "a bus", "engine"); }

inline bool track_has_lane(RealtimeEngine& engine, uint32_t track_id) {
  uint32_t position = 0;
  return engine.track_mixer().track_lane_position(track_id, &position);
}

inline bool has_settable_param(const sonare::rt::ProcessorBase& processor, const char* param_name) {
  for (const auto& descriptor : processor.parameter_descriptors()) {
    if (descriptor.key == param_name) return processor.parameter_is_realtime_safe(descriptor.id);
  }
  return false;
}

inline constexpr const char* kParamNamePredicate =
    "must be the name of a parameter of the insert that can be set while running";

inline void require_track_insert(RealtimeEngine& engine, uint32_t track_id,
                                 unsigned int insert_index, const char* param_name) {
  if (!track_has_lane(engine, track_id)) refuse_track_id();
  const auto* processor = engine.track_mixer().track_insert_processor(track_id, insert_index);
  if (processor == nullptr) {
    check::refuse("insert_index", "must be less than the number of inserts on the track");
  }
  if (param_name != nullptr && !has_settable_param(*processor, param_name)) {
    check::refuse("param_name", kParamNamePredicate);
  }
}

inline void require_bus_insert(RealtimeEngine& engine, uint32_t bus_id, unsigned int insert_index,
                               const char* param_name) {
  if (!engine.track_mixer().configured_bus(bus_id)) refuse_bus_id();
  const auto* processor = engine.track_mixer().bus_insert_processor(bus_id, insert_index);
  if (processor == nullptr) {
    check::refuse("insert_index", "must be less than the number of inserts on the bus");
  }
  if (param_name != nullptr && !has_settable_param(*processor, param_name)) {
    check::refuse("param_name", kParamNamePredicate);
  }
}

inline void require_master_insert(const RealtimeEngine& engine, unsigned int insert_index,
                                  const char* param_name) {
  if (insert_index >= engine.master_insert_count()) {
    check::refuse("insert_index", "must be less than the number of inserts on the master strip");
  }
  if (param_name != nullptr) {
    const int param_id = engine.master_insert_parameter_id(insert_index, param_name);
    if (param_id < 0 || !engine.master_insert_processor(insert_index)
                             ->parameter_is_realtime_safe(static_cast<unsigned int>(param_id))) {
      check::refuse("param_name", kParamNamePredicate);
    }
  }
}

[[noreturn]] inline void refuse_no_automation_id(unsigned int insert_index) {
  if (insert_index > sonare::engine::kInsertIndexMask) {
    check::integer_within(insert_index, kEngineInsertIndexDomain, "insert_index");
  }
  refuse_insert_automation_table_full();
}

[[noreturn]] inline void refuse_insert_bypass() {
  throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                "the insert refused to change its bypass state");
}

[[noreturn]] inline void refuse_insert_restore_value(std::string_view param_name) {
  check::refuse("value", "must be accepted by the parameter \"" + std::string(param_name) +
                             "\" of that insert");
}

[[noreturn]] inline void refuse_insert_queue_full() {
  throw sonare::SonareException(sonare::ErrorCode::OutOfMemory,
                                "the realtime command queue is full");
}

inline void require_insert_construction_value(RealtimeEngine& engine, uint32_t param_id,
                                              float* out_value) {
  if (engine.insert_parameter_constructed_value(param_id, out_value)) return;
  if (!sonare::engine::is_insert_param_id(param_id)) {
    check::refuse("param_id", "must be an insert parameter automation id");
  }
  check::refuse_unknown_id("param_id", "a live insert parameter with a construction value",
                           "engine");
}

inline uint32_t resolve_track_insert(RealtimeEngine& engine, uint32_t track_id,
                                     unsigned int insert_index, const char* param_name) {
  const int64_t id = engine.resolve_track_insert_automation_id(track_id, insert_index, param_name);
  if (id >= 0) return static_cast<uint32_t>(id);
  require_track_insert(engine, track_id, insert_index, param_name);
  refuse_no_automation_id(insert_index);
}

inline uint32_t resolve_master_insert(RealtimeEngine& engine, unsigned int insert_index,
                                      const char* param_name) {
  const int64_t id = engine.resolve_master_insert_automation_id(insert_index, param_name);
  if (id >= 0) return static_cast<uint32_t>(id);
  require_master_insert(engine, insert_index, param_name);
  refuse_no_automation_id(insert_index);
}

inline uint32_t resolve_bus_insert(RealtimeEngine& engine, uint32_t bus_id,
                                   unsigned int insert_index, const char* param_name) {
  const int64_t id = engine.resolve_bus_insert_automation_id(bus_id, insert_index, param_name);
  if (id >= 0) return static_cast<uint32_t>(id);
  require_bus_insert(engine, bus_id, insert_index, param_name);
  refuse_no_automation_id(insert_index);
}

inline uint32_t resolve_track_lane(RealtimeEngine& engine, uint32_t track_id,
                                   const char* param_name) {
  const int64_t id = engine.resolve_track_lane_automation_id(track_id, param_name);
  if (id >= 0) return static_cast<uint32_t>(id);
  const std::string_view key = param_name;
  if (key != "faderDb" && key != "pan") {
    check::refuse("param_name", "must be the name of a track strip parameter: faderDb or pan");
  }
  if (!track_has_lane(engine, track_id)) refuse_track_id();
  refuse_no_automation_id(0);
}

inline uint32_t resolve_master(RealtimeEngine& engine, const char* param_name) {
  const int64_t id = engine.resolve_master_automation_id(param_name);
  if (id < 0) {
    check::refuse("param_name",
                  "must be the name of a master strip parameter: faderDb, pan or width");
  }
  return static_cast<uint32_t>(id);
}

inline uint32_t resolve_bus(RealtimeEngine& engine, uint32_t bus_id, const char* param_name) {
  const int64_t id = engine.resolve_bus_automation_id(bus_id, param_name);
  if (id >= 0) return static_cast<uint32_t>(id);
  if (std::string_view(param_name) != "faderDb") {
    check::refuse("param_name", "must be the name of a bus strip parameter: faderDb");
  }
  if (!engine.track_mixer().configured_bus(bus_id)) refuse_bus_id();
  refuse_no_automation_id(0);
}
#endif

#if defined(SONARE_WITH_ARRANGEMENT)
[[noreturn]] inline void refuse_instrument_destination() {
  check::refuse_unknown_id("destination_id", "a destination with a MIDI instrument", "engine");
}

inline uint32_t resolve_instrument(RealtimeEngine& engine, uint32_t destination_id,
                                   const char* param_name) {
  const int64_t id = engine.resolve_instrument_automation_id(destination_id, param_name);
  if (id >= 0) return static_cast<uint32_t>(id);
  if (engine.midi_instrument(destination_id) == nullptr) refuse_instrument_destination();
  check::refuse_relation({"param_name", "destination_id"},
                         "{0} must be the name of a parameter of the instrument bound to {1}");
}
#endif

}  // namespace sonare_c_detail::engine_automation
