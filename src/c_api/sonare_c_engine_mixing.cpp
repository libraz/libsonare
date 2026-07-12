#include <sonare/sonare_c.h>

#include <utility>
#include <vector>

#include "core/channel_layout.h"
#include "engine/realtime_engine.h"
#include "sonare_c_internal.h"
#if defined(SONARE_WITH_MIXING)
#include <exception>
#include <string>

#include "c_api/eq_band_json.h"
#include "c_api/mixing_internal.h"
#include "mixing/api/scene.h"
#endif

using namespace sonare;
using namespace sonare_c_detail;

#if defined(SONARE_WITH_MIXING)
namespace {

/// Parses a mixing scene from a JSON string, recording the parse-failure message
/// and mapping a failure to SONARE_ERROR_INVALID_FORMAT. Shared by the
/// strip-setter entry points that accept a scene JSON payload.
SonareError parse_scene_json(const char* json, mixing::api::Scene* out) {
  try {
    *out = mixing::api::scene_from_json(json);
  } catch (const std::exception& e) {
    set_last_error(e.what());
    return SONARE_ERROR_INVALID_FORMAT;
  }
  return SONARE_OK;
}

}  // namespace
#endif

SonareError sonare_engine_set_track_lanes(SonareRealtimeEngine* engine,
                                          const SonareEngineTrackLane* lanes, size_t lane_count) {
  if (!engine || (lane_count > 0 && !lanes)) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)lanes;
  (void)lane_count;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  std::vector<engine::TrackLaneConfig> configs;
  SONARE_C_TRY
  configs.reserve(lane_count);
  for (size_t i = 0; i < lane_count; ++i) {
    if (lanes[i].send_count > 0 && lanes[i].sends == nullptr) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (!is_valid_channel_layout(lanes[i].source_channel_layout)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    engine::TrackLaneConfig lane{lanes[i].track_id};
    lane.output_bus_id = lanes[i].output_bus_id;
    lane.source_layout = static_cast<ChannelLayout>(lanes[i].source_channel_layout);
    lane.sends.reserve(lanes[i].send_count);
    for (size_t send_index = 0; send_index < lanes[i].send_count; ++send_index) {
      const SonareEngineTrackSend& send = lanes[i].sends[send_index];
      lane.sends.push_back({send.bus_id, send.level_db, send.enabled != 0,
                            sonare_c_mixing_detail::to_send_timing(send.send_timing)});
    }
    configs.push_back(std::move(lane));
  }
  return engine->engine.set_track_lanes(std::move(configs)) ? SONARE_OK
                                                            : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_lane_sidechain(SonareRealtimeEngine* engine, uint32_t track_id,
                                             unsigned int insert_index, uint32_t source_track_id) {
  if (!engine || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)insert_index;
  (void)source_track_id;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  return engine->engine.set_lane_sidechain(track_id, insert_index, source_track_id)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
#endif
}

SonareError sonare_engine_set_track_buses(SonareRealtimeEngine* engine,
                                          const SonareEngineBus* buses, size_t bus_count) {
  if (!engine || (bus_count > 0 && !buses)) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)buses;
  (void)bus_count;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  std::vector<engine::TrackBusConfig> configs;
  SONARE_C_TRY
  configs.reserve(bus_count);
  for (size_t i = 0; i < bus_count; ++i) {
    if (!is_valid_channel_layout(buses[i].channel_layout)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    configs.push_back(
        {buses[i].bus_id, buses[i].gain_db, static_cast<ChannelLayout>(buses[i].channel_layout)});
  }
  return engine->engine.set_track_buses(std::move(configs)) ? SONARE_OK
                                                            : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_bus_strip_json(SonareRealtimeEngine* engine, uint32_t bus_id,
                                             const char* scene_json) {
  if (!engine || bus_id == 0 || !scene_json) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)bus_id;
  (void)scene_json;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  mixing::api::Scene scene;
  SonareError perr = parse_scene_json(scene_json, &scene);
  if (perr != SONARE_OK) return perr;
  if (scene.buses.empty()) return SONARE_ERROR_INVALID_PARAMETER;
  return engine->engine.set_bus_strip(bus_id, scene.buses.front()) ? SONARE_OK
                                                                   : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_json(SonareRealtimeEngine* engine, uint32_t track_id,
                                               const char* scene_json) {
  if (!engine || track_id == 0 || !scene_json) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)scene_json;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  mixing::api::Scene scene;
  SonareError perr = parse_scene_json(scene_json, &scene);
  if (perr != SONARE_OK) return perr;
  if (scene.strips.empty()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return engine->engine.set_track_strip(track_id, scene.strips.front())
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_eq_band_json(SonareRealtimeEngine* engine,
                                                       uint32_t track_id, int band_index,
                                                       const char* band_json) {
  if (!engine || track_id == 0 || band_index < 0 || !band_json) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)band_index;
  (void)band_json;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_track_eq_band(track_id, static_cast<size_t>(band_index),
                                          sonare::c_api::parse_eq_band_json(band_json))
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_insert_bypassed(SonareRealtimeEngine* engine,
                                                          uint32_t track_id,
                                                          unsigned int insert_index, int bypassed,
                                                          int reset_on_bypass) {
  if (!engine || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)insert_index;
  (void)bypassed;
  (void)reset_on_bypass;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_track_insert_bypassed(track_id, insert_index, bypassed != 0,
                                                  reset_on_bypass != 0)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_master_strip_json(SonareRealtimeEngine* engine,
                                                const char* scene_json) {
  if (!engine || !scene_json) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)scene_json;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  mixing::api::Scene scene;
  SonareError perr = parse_scene_json(scene_json, &scene);
  if (perr != SONARE_OK) return perr;
  if (scene.strips.empty()) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return engine->engine.set_master_strip(scene.strips.front()) ? SONARE_OK
                                                               : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_master_strip_eq_band_json(SonareRealtimeEngine* engine,
                                                        int band_index, const char* band_json) {
  if (!engine || band_index < 0 || !band_json) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)band_index;
  (void)band_json;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_master_eq_band(static_cast<size_t>(band_index),
                                           sonare::c_api::parse_eq_band_json(band_json))
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_master_strip_insert_bypassed(SonareRealtimeEngine* engine,
                                                           unsigned int insert_index, int bypassed,
                                                           int reset_on_bypass) {
  if (!engine) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)insert_index;
  (void)bypassed;
  (void)reset_on_bypass;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_master_insert_bypassed(insert_index, bypassed != 0,
                                                   reset_on_bypass != 0)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_bus_strip_insert_bypassed(SonareRealtimeEngine* engine,
                                                        uint32_t bus_id, unsigned int insert_index,
                                                        int bypassed, int reset_on_bypass) {
  if (!engine || bus_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)bus_id;
  (void)insert_index;
  (void)bypassed;
  (void)reset_on_bypass;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_bus_insert_bypassed(bus_id, insert_index, bypassed != 0,
                                                reset_on_bypass != 0)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_insert_param_by_name(SonareRealtimeEngine* engine,
                                                               uint32_t track_id,
                                                               unsigned int insert_index,
                                                               const char* param_name,
                                                               float value) {
  if (!engine || track_id == 0 || !param_name || param_name[0] == '\0') {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)insert_index;
  (void)value;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_track_insert_param(track_id, insert_index, param_name, value)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_master_strip_insert_param_by_name(SonareRealtimeEngine* engine,
                                                                unsigned int insert_index,
                                                                const char* param_name,
                                                                float value) {
  if (!engine || !param_name || param_name[0] == '\0') return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)insert_index;
  (void)value;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_master_insert_param(insert_index, param_name, value)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_bus_strip_insert_param_by_name(SonareRealtimeEngine* engine,
                                                             uint32_t bus_id,
                                                             unsigned int insert_index,
                                                             const char* param_name, float value) {
  if (!engine || bus_id == 0 || !param_name || param_name[0] == '\0') {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if !defined(SONARE_WITH_MIXING)
  (void)bus_id;
  (void)insert_index;
  (void)value;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_bus_insert_param(bus_id, insert_index, param_name, value)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_resolve_track_insert_automation_id(SonareRealtimeEngine* engine,
                                                             uint32_t track_id,
                                                             unsigned int insert_index,
                                                             const char* param_name,
                                                             uint32_t* out_id) {
  if (!engine || track_id == 0 || !param_name || param_name[0] == '\0' || !out_id) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)insert_index;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  const int64_t id =
      engine->engine.resolve_track_insert_automation_id(track_id, insert_index, param_name);
  if (id < 0) return SONARE_ERROR_INVALID_PARAMETER;
  *out_id = static_cast<uint32_t>(id);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_resolve_master_insert_automation_id(SonareRealtimeEngine* engine,
                                                              unsigned int insert_index,
                                                              const char* param_name,
                                                              uint32_t* out_id) {
  if (!engine || !param_name || param_name[0] == '\0' || !out_id) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if !defined(SONARE_WITH_MIXING)
  (void)insert_index;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  const int64_t id = engine->engine.resolve_master_insert_automation_id(insert_index, param_name);
  if (id < 0) return SONARE_ERROR_INVALID_PARAMETER;
  *out_id = static_cast<uint32_t>(id);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_resolve_bus_insert_automation_id(SonareRealtimeEngine* engine,
                                                           uint32_t bus_id,
                                                           unsigned int insert_index,
                                                           const char* param_name,
                                                           uint32_t* out_id) {
  if (!engine || bus_id == 0 || !param_name || param_name[0] == '\0' || !out_id) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
#if !defined(SONARE_WITH_MIXING)
  (void)bus_id;
  (void)insert_index;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  const int64_t id =
      engine->engine.resolve_bus_insert_automation_id(bus_id, insert_index, param_name);
  if (id < 0) return SONARE_ERROR_INVALID_PARAMETER;
  *out_id = static_cast<uint32_t>(id);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_pan(SonareRealtimeEngine* engine, uint32_t track_id,
                                              float pan) {
  if (!engine || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)pan;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_track_pan(track_id, pan) ? SONARE_OK : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_pan_law(SonareRealtimeEngine* engine, uint32_t track_id,
                                                  int pan_law) {
  if (!engine || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)pan_law;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_track_pan_law(track_id, sonare_c_mixing_detail::to_pan_law(pan_law))
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_pan_mode(SonareRealtimeEngine* engine, uint32_t track_id,
                                                   int pan_mode) {
  if (!engine || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)pan_mode;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_track_pan_mode(track_id, sonare_c_mixing_detail::to_pan_mode(pan_mode))
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_dual_pan(SonareRealtimeEngine* engine, uint32_t track_id,
                                                   float left_pan, float right_pan) {
  if (!engine || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)left_pan;
  (void)right_pan;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_track_dual_pan(track_id, left_pan, right_pan)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_channel_delay_samples(SonareRealtimeEngine* engine,
                                                                uint32_t track_id,
                                                                int delay_samples) {
  if (!engine || track_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)delay_samples;
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return engine->engine.set_track_channel_delay_samples(track_id, delay_samples)
             ? SONARE_OK
             : SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_CATCH
#endif
}
