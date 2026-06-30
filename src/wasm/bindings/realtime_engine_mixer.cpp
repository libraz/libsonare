/// @file realtime_engine_mixer.cpp
/// @brief Embind realtime-engine facade: tracks, buses, strips, panning.

#ifdef __EMSCRIPTEN__

#include "c_api/eq_band_json.h"
#include "mixing/api/scene.h"
#include "realtime_engine_wasm.h"

void RealtimeEngineWasm::setTrackLanes(val lanes) {
#if defined(SONARE_WITH_MIXING)
  const int count = lanes["length"].as<int>();
  std::vector<sonare::engine::TrackLaneConfig> configs;
  configs.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    val lane_val = lanes[i];
    uint32_t track_id = 0;
    if (lane_val.typeOf().as<std::string>() == "number") {
      track_id = lane_val.as<uint32_t>();
    } else {
      track_id = static_cast<uint32_t>(intProperty(lane_val, "trackId", 0));
    }
    sonare::engine::TrackLaneConfig config{track_id};
    if (lane_val.typeOf().as<std::string>() == "object") {
      config.output_bus_id = static_cast<uint32_t>(intProperty(lane_val, "outputBusId", 0));
      // Absent defaults to stereo, matching the C ABI / Node surfaces. An
      // out-of-range layout is rejected like the C ABI's is_valid check.
      const int raw_layout = intProperty(lane_val, "sourceChannelLayout",
                                         static_cast<int>(sonare::ChannelLayout::Stereo));
      if (raw_layout < 0 || !sonare::is_valid_channel_layout(static_cast<uint8_t>(raw_layout))) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "invalid source channel layout");
      }
      config.source_layout = static_cast<sonare::ChannelLayout>(raw_layout);
    }
    if (lane_val.typeOf().as<std::string>() == "object" && !lane_val["sends"].isUndefined() &&
        !lane_val["sends"].isNull()) {
      val sends = lane_val["sends"];
      const int send_count = sends["length"].as<int>();
      config.sends.reserve(static_cast<size_t>(send_count));
      for (int send_index = 0; send_index < send_count; ++send_index) {
        val send = sends[send_index];
        // sendTiming integer mirrors SonareSendTiming (0 = post, 1 = pre) and
        // defaults to post-fader, matching the historical lane-send behavior
        // and the scene-JSON default. Post is value 0, so an omitted or zeroed
        // value resolves to post-fader.
        const sonare::mixing::SendTiming timing = intProperty(send, "sendTiming", 0) == 1
                                                      ? sonare::mixing::SendTiming::PreFader
                                                      : sonare::mixing::SendTiming::PostFader;
        config.sends.push_back({static_cast<uint32_t>(intProperty(send, "busId", 0)),
                                floatProperty(send, "levelDb", 0.0f),
                                boolProperty(send, "enabled", true), timing});
      }
    }
    configs.push_back(std::move(config));
  }
  if (!engine_.set_track_lanes(std::move(configs))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track lane configuration");
  }
#else
  (void)lanes;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "mixing support is not enabled");
#endif
}

/// Keys one insert of a lane strip from another lane's post-strip audio
/// (ducking/sidechainRouter inserts); sourceTrackId 0 removes the binding.
/// Matches sonare_engine_set_lane_sidechain.
void RealtimeEngineWasm::setLaneSidechain(uint32_t track_id, unsigned int insert_index,
                                          uint32_t source_track_id) {
#if defined(SONARE_WITH_MIXING)
  if (track_id == 0 || !engine_.set_lane_sidechain(track_id, insert_index, source_track_id)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid lane sidechain binding");
  }
#else
  (void)track_id;
  (void)insert_index;
  (void)source_track_id;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackBuses(val buses) {
#if defined(SONARE_WITH_MIXING)
  const int count = buses["length"].as<int>();
  std::vector<sonare::engine::TrackBusConfig> configs;
  configs.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    val bus = buses[i];
    const int layout_value = intProperty(bus, "channelLayout", 1);
    if (!sonare::is_valid_channel_layout(static_cast<uint8_t>(layout_value))) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "invalid bus channel layout");
    }
    configs.push_back({static_cast<uint32_t>(intProperty(bus, "busId", 0)),
                       floatProperty(bus, "gainDb", 0.0f),
                       static_cast<sonare::ChannelLayout>(layout_value)});
  }
  if (!engine_.set_track_buses(std::move(configs))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track bus configuration");
  }
#else
  (void)buses;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "mixing support is not enabled");
#endif
}

void RealtimeEngineWasm::setBusStripJson(uint32_t bus_id, const std::string& scene_json) {
#if defined(SONARE_WITH_MIXING)
  sonare::mixing::api::Scene scene;
  try {
    scene = sonare::mixing::api::scene_from_json(scene_json);
  } catch (const std::exception& e) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat, e.what());
  }
  if (bus_id == 0 || scene.buses.empty() || !engine_.set_bus_strip(bus_id, scene.buses.front())) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "invalid bus strip spec");
  }
#else
  (void)bus_id;
  (void)scene_json;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "mixing support is not enabled");
#endif
}

void RealtimeEngineWasm::setTrackStripJson(uint32_t track_id, const std::string& scene_json) {
#if defined(SONARE_WITH_MIXING)
  if (track_id == 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "track id must be non-zero");
  }
  sonare::mixing::api::Scene scene;
  try {
    scene = sonare::mixing::api::scene_from_json(scene_json);
  } catch (const std::exception& e) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat, e.what());
  }
  if (scene.strips.empty() || !engine_.set_track_strip(track_id, scene.strips.front())) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "invalid track strip spec");
  }
#else
  (void)track_id;
  (void)scene_json;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "mixing support is not enabled");
#endif
}

void RealtimeEngineWasm::setTrackStripEqBandJson(uint32_t track_id, int band_index,
                                                 const std::string& band_json) {
#if defined(SONARE_WITH_MIXING)
  if (track_id == 0 || band_index < 0 ||
      !engine_.set_track_eq_band(track_id, static_cast<size_t>(band_index),
                                 sonare::c_api::parse_eq_band_json(band_json.c_str()))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track strip EQ band target");
  }
#else
  (void)track_id;
  (void)band_index;
  (void)band_json;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "mixing support is not enabled");
#endif
}

void RealtimeEngineWasm::setTrackStripInsertBypassed(uint32_t track_id, unsigned int insert_index,
                                                     bool bypassed, bool reset_on_bypass) {
#if defined(SONARE_WITH_MIXING)
  if (!engine_.set_track_insert_bypassed(track_id, insert_index, bypassed, reset_on_bypass)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track strip insert bypass target");
  }
#else
  (void)track_id;
  (void)insert_index;
  (void)bypassed;
  (void)reset_on_bypass;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "mixing support is not enabled");
#endif
}

void RealtimeEngineWasm::setMasterStripJson(const std::string& scene_json) {
#if defined(SONARE_WITH_MIXING)
  sonare::mixing::api::Scene scene;
  try {
    scene = sonare::mixing::api::scene_from_json(scene_json);
  } catch (const std::exception& e) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat, e.what());
  }
  if (scene.strips.empty() || !engine_.set_master_strip(scene.strips.front())) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "invalid master strip spec");
  }
#else
  (void)scene_json;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "mixing support is not enabled");
#endif
}

void RealtimeEngineWasm::setMasterStripEqBandJson(int band_index, const std::string& band_json) {
#if defined(SONARE_WITH_MIXING)
  if (band_index < 0 ||
      !engine_.set_master_eq_band(static_cast<size_t>(band_index),
                                  sonare::c_api::parse_eq_band_json(band_json.c_str()))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid master strip EQ band target");
  }
#else
  (void)band_index;
  (void)band_json;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "mixing support is not enabled");
#endif
}

void RealtimeEngineWasm::setMasterStripInsertBypassed(unsigned int insert_index, bool bypassed,
                                                      bool reset_on_bypass) {
#if defined(SONARE_WITH_MIXING)
  if (!engine_.set_master_insert_bypassed(insert_index, bypassed, reset_on_bypass)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid master strip insert bypass target");
  }
#else
  (void)insert_index;
  (void)bypassed;
  (void)reset_on_bypass;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "mixing support is not enabled");
#endif
}

void RealtimeEngineWasm::setTrackStripInsertParamByName(uint32_t track_id,
                                                        unsigned int insert_index,
                                                        const std::string& param_name,
                                                        float value) {
#if defined(SONARE_WITH_MIXING)
  if (!engine_.set_track_insert_param(track_id, insert_index, param_name, value)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track strip insert parameter target");
  }
#else
  (void)track_id;
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "mixing support is not enabled");
#endif
}

void RealtimeEngineWasm::setMasterStripInsertParamByName(unsigned int insert_index,
                                                         const std::string& param_name,
                                                         float value) {
#if defined(SONARE_WITH_MIXING)
  if (!engine_.set_master_insert_param(insert_index, param_name, value)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid master strip insert parameter target");
  }
#else
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "mixing support is not enabled");
#endif
}

void RealtimeEngineWasm::setBusStripInsertParamByName(uint32_t bus_id, unsigned int insert_index,
                                                      const std::string& param_name, float value) {
#if defined(SONARE_WITH_MIXING)
  if (!engine_.set_bus_insert_param(bus_id, insert_index, param_name, value)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid bus strip insert parameter target");
  }
#else
  (void)bus_id;
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "mixing support is not enabled");
#endif
}

// Resolves a track-lane / master / bus insert parameter (JSON-key name) to the
// reserved insert-automation id passed to setAutomationLane. Returns -1 when the
// strip, insert, or key is unknown. The id is returned as a double so the full
// 32-bit unsigned reserved id (which exceeds the signed-int range) survives the
// JS boundary; the caller passes it back to setAutomationLane.
double RealtimeEngineWasm::resolveTrackInsertAutomationId(uint32_t track_id,
                                                          unsigned int insert_index,
                                                          const std::string& param_name) {
#if defined(SONARE_WITH_MIXING)
  return static_cast<double>(
      engine_.resolve_track_insert_automation_id(track_id, insert_index, param_name));
#else
  (void)track_id;
  (void)insert_index;
  (void)param_name;
  return -1.0;
#endif
}

double RealtimeEngineWasm::resolveMasterInsertAutomationId(unsigned int insert_index,
                                                           const std::string& param_name) {
#if defined(SONARE_WITH_MIXING)
  return static_cast<double>(engine_.resolve_master_insert_automation_id(insert_index, param_name));
#else
  (void)insert_index;
  (void)param_name;
  return -1.0;
#endif
}

double RealtimeEngineWasm::resolveBusInsertAutomationId(uint32_t bus_id, unsigned int insert_index,
                                                        const std::string& param_name) {
#if defined(SONARE_WITH_MIXING)
  return static_cast<double>(
      engine_.resolve_bus_insert_automation_id(bus_id, insert_index, param_name));
#else
  (void)bus_id;
  (void)insert_index;
  (void)param_name;
  return -1.0;
#endif
}

void RealtimeEngineWasm::setTrackStripPan(uint32_t track_id, float pan) {
#if defined(SONARE_WITH_MIXING)
  if (!engine_.set_track_pan(track_id, pan)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track strip pan target");
  }
#else
  (void)track_id;
  (void)pan;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "mixing support is not enabled");
#endif
}

void RealtimeEngineWasm::setTrackStripPanLaw(uint32_t track_id, int pan_law) {
#if defined(SONARE_WITH_MIXING)
  if (pan_law < 0 || pan_law > 3) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown mixing pan law");
  }
  if (!engine_.set_track_pan_law(track_id, static_cast<sonare::mixing::PanLaw>(pan_law))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track strip pan-law target");
  }
#else
  (void)track_id;
  (void)pan_law;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "mixing support is not enabled");
#endif
}

void RealtimeEngineWasm::setTrackStripPanMode(uint32_t track_id, int pan_mode) {
#if defined(SONARE_WITH_MIXING)
  if (pan_mode < 0 || pan_mode > 2) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown mixing pan mode");
  }
  if (!engine_.set_track_pan_mode(track_id, static_cast<sonare::mixing::PanMode>(pan_mode))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track strip pan-mode target");
  }
#else
  (void)track_id;
  (void)pan_mode;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "mixing support is not enabled");
#endif
}

void RealtimeEngineWasm::setTrackStripDualPan(uint32_t track_id, float left_pan, float right_pan) {
#if defined(SONARE_WITH_MIXING)
  if (!engine_.set_track_dual_pan(track_id, left_pan, right_pan)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track strip dual-pan target");
  }
#else
  (void)track_id;
  (void)left_pan;
  (void)right_pan;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "mixing support is not enabled");
#endif
}

void RealtimeEngineWasm::setTrackStripChannelDelaySamples(uint32_t track_id, int delay_samples) {
#if defined(SONARE_WITH_MIXING)
  if (!engine_.set_track_channel_delay_samples(track_id, delay_samples)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track strip channel-delay target");
  }
#else
  (void)track_id;
  (void)delay_samples;
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, "mixing support is not enabled");
#endif
}

void registerRealtimeEngineMixer(class_<RealtimeEngineWasm>& cls) {
  cls.function("setTrackLanes", &RealtimeEngineWasm::setTrackLanes)
      .function("setLaneSidechain", &RealtimeEngineWasm::setLaneSidechain)
      .function("setTrackBuses", &RealtimeEngineWasm::setTrackBuses)
      .function("setBusStripJson", &RealtimeEngineWasm::setBusStripJson)
      .function("setTrackStripJson", &RealtimeEngineWasm::setTrackStripJson)
      .function("setTrackStripEqBandJson", &RealtimeEngineWasm::setTrackStripEqBandJson)
      .function("setTrackStripInsertBypassed", &RealtimeEngineWasm::setTrackStripInsertBypassed)
      .function("setMasterStripJson", &RealtimeEngineWasm::setMasterStripJson)
      .function("setMasterStripEqBandJson", &RealtimeEngineWasm::setMasterStripEqBandJson)
      .function("setMasterStripInsertBypassed", &RealtimeEngineWasm::setMasterStripInsertBypassed)
      .function("setTrackStripInsertParamByName",
                &RealtimeEngineWasm::setTrackStripInsertParamByName)
      .function("setMasterStripInsertParamByName",
                &RealtimeEngineWasm::setMasterStripInsertParamByName)
      .function("setBusStripInsertParamByName", &RealtimeEngineWasm::setBusStripInsertParamByName)
      .function("resolveTrackInsertAutomationId",
                &RealtimeEngineWasm::resolveTrackInsertAutomationId)
      .function("resolveMasterInsertAutomationId",
                &RealtimeEngineWasm::resolveMasterInsertAutomationId)
      .function("resolveBusInsertAutomationId", &RealtimeEngineWasm::resolveBusInsertAutomationId)
      .function("setTrackStripPan", &RealtimeEngineWasm::setTrackStripPan)
      .function("setTrackStripPanLaw", &RealtimeEngineWasm::setTrackStripPanLaw)
      .function("setTrackStripPanMode", &RealtimeEngineWasm::setTrackStripPanMode)
      .function("setTrackStripDualPan", &RealtimeEngineWasm::setTrackStripDualPan)
      .function("setTrackStripChannelDelaySamples",
                &RealtimeEngineWasm::setTrackStripChannelDelaySamples);
}

#endif  // __EMSCRIPTEN__
