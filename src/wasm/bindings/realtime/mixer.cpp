/// @file realtime_engine_mixer.cpp
/// @brief Embind realtime-engine facade: tracks, buses, strips, panning.

#ifdef __EMSCRIPTEN__

#include "c_api/eq_band_json.h"
#include "mixing/api/scene.h"
#include "mixing/pan_law.h"
#include "realtime_engine_wasm.h"

#if defined(SONARE_WITH_MIXING)
namespace {

// Reads an optional `sends` array off a track lane or bus object, in the one
// shape both share. sendTiming mirrors SonareSendTiming (0 post, 1 pre) and
// defaults to post-fader.
std::vector<sonare::engine::TrackLaneConfig::Send> readOptionalSends(const val& owner) {
  std::vector<sonare::engine::TrackLaneConfig::Send> out;
  if (owner["sends"].isUndefined() || owner["sends"].isNull()) return out;
  val sends = owner["sends"];
  const int send_count = static_cast<int>(wasmArrayLikeLength(sends, "sends"));
  out.reserve(static_cast<size_t>(send_count));
  for (int send_index = 0; send_index < send_count; ++send_index) {
    val send = sends[send_index];
    const int timing_value = intProperty(send, "sendTiming", 0);
    if (timing_value != 0 && timing_value != 1) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "unknown mixing send timing");
    }
    const sonare::mixing::SendTiming timing = timing_value == 1
                                                  ? sonare::mixing::SendTiming::PreFader
                                                  : sonare::mixing::SendTiming::PostFader;
    out.push_back({static_cast<uint32_t>(intProperty(send, "busId", 0)),
                   floatProperty(send, "levelDb", 0.0f), boolProperty(send, "enabled", true),
                   timing});
  }
  return out;
}

sonare::engine::SidechainSourceKind sidechainSourceKind(int source_kind) {
  if (source_kind != static_cast<int>(sonare::engine::SidechainSourceKind::Track) &&
      source_kind != static_cast<int>(sonare::engine::SidechainSourceKind::Bus)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "unknown sidechain source kind");
  }
  return static_cast<sonare::engine::SidechainSourceKind>(source_kind);
}

}  // namespace
#endif

void RealtimeEngineWasm::setTrackLanes(val lanes) {
#if defined(SONARE_WITH_MIXING)
  const int count = static_cast<int>(wasmArrayLikeLength(lanes, "lanes"));
  std::vector<sonare::engine::TrackLaneConfig> configs;
  configs.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    val lane_val = lanes[i];
    uint32_t track_id = 0;
    if (lane_val.typeOf().as<std::string>() == "number") {
      track_id = checkedUintFromVal(lane_val, "trackId");
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
    if (lane_val.typeOf().as<std::string>() == "object") config.sends = readOptionalSends(lane_val);
    configs.push_back(std::move(config));
  }
  if (!engine_.set_track_lanes(std::move(configs))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track lane configuration");
  }
#else
  (void)lanes;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

/// Keys one insert of a lane strip from another lane's post-strip audio
/// (ducking/sidechainRouter inserts); sourceTrackId 0 removes the binding.
/// Matches sonare_engine_set_lane_sidechain.
void RealtimeEngineWasm::setLaneSidechain(const val& track_id_val, const val& insert_index_val,
                                          const val& source_track_id_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const uint32_t source_track_id = checkedUintFromVal(source_track_id_val, "sourceTrackId");
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
  const int count = static_cast<int>(wasmArrayLikeLength(buses, "buses"));
  std::vector<sonare::engine::TrackBusConfig> configs;
  configs.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    val bus = buses[i];
    const int layout_value = intProperty(bus, "channelLayout", 1);
    if (!sonare::is_valid_channel_layout(static_cast<uint8_t>(layout_value))) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "invalid bus channel layout");
    }
    sonare::engine::TrackBusConfig config{static_cast<uint32_t>(intProperty(bus, "busId", 0)),
                                          floatProperty(bus, "gainDb", 0.0f),
                                          static_cast<sonare::ChannelLayout>(layout_value)};
    config.output_bus_id = static_cast<uint32_t>(intProperty(bus, "outputBusId", 0));
    config.sends = readOptionalSends(bus);
    configs.push_back(std::move(config));
  }
  if (!engine_.set_track_buses(std::move(configs))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track bus configuration");
  }
#else
  (void)buses;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

/// Keys one insert of a bus strip from a track lane or another bus (matches
/// sonare_engine_set_bus_sidechain). sourceId 0 removes the binding.
void RealtimeEngineWasm::setBusSidechain(const val& bus_id_val, const val& insert_index_val,
                                         const val& source_kind_val, const val& source_id_val) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const int source_kind = checkedIntFromVal(source_kind_val, "sourceKind");
  const uint32_t source_id = checkedUintFromVal(source_id_val, "sourceId");
#if defined(SONARE_WITH_MIXING)
  const sonare::engine::SidechainSourceKind kind = sidechainSourceKind(source_kind);
  if (bus_id == 0 || !engine_.set_bus_sidechain(bus_id, insert_index, kind, source_id)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid bus sidechain binding");
  }
#else
  (void)bus_id;
  (void)insert_index;
  (void)source_kind;
  (void)source_id;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

/// Keys one insert of the master strip from a track lane or a bus (matches
/// sonare_engine_set_master_sidechain). sourceId 0 removes the binding.
void RealtimeEngineWasm::setMasterSidechain(const val& insert_index_val, const val& source_kind_val,
                                            const val& source_id_val) {
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const int source_kind = checkedIntFromVal(source_kind_val, "sourceKind");
  const uint32_t source_id = checkedUintFromVal(source_id_val, "sourceId");
#if defined(SONARE_WITH_MIXING)
  if (!engine_.set_master_sidechain(insert_index, sidechainSourceKind(source_kind), source_id)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid master sidechain binding");
  }
#else
  (void)insert_index;
  (void)source_kind;
  (void)source_id;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setBusStripJson(const val& bus_id_val, const std::string& scene_json) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
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
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackStripJson(const val& track_id_val, const std::string& scene_json) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
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
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackStripEqBandJson(const val& track_id_val, const val& band_index_val,
                                                 const std::string& band_json) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const int band_index = checkedIntFromVal(band_index_val, "bandIndex");
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
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setBusStripEqBandJson(const val& bus_id_val, const val& band_index_val,
                                               const std::string& band_json) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const int band_index = checkedIntFromVal(band_index_val, "bandIndex");
#if defined(SONARE_WITH_MIXING)
  if (bus_id == 0 || band_index < 0 ||
      !engine_.set_bus_eq_band(bus_id, static_cast<size_t>(band_index),
                               sonare::c_api::parse_eq_band_json(band_json.c_str()))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid bus strip EQ band target");
  }
#else
  (void)bus_id;
  (void)band_index;
  (void)band_json;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackStripInsertBypassed(const val& track_id_val,
                                                     const val& insert_index_val, bool bypassed,
                                                     bool reset_on_bypass) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
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
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
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
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setMasterStripEqBandJson(const val& band_index_val,
                                                  const std::string& band_json) {
  const int band_index = checkedIntFromVal(band_index_val, "bandIndex");
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
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setMasterStripInsertBypassed(const val& insert_index_val, bool bypassed,
                                                      bool reset_on_bypass) {
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
#if defined(SONARE_WITH_MIXING)
  if (!engine_.set_master_insert_bypassed(insert_index, bypassed, reset_on_bypass)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid master strip insert bypass target");
  }
#else
  (void)insert_index;
  (void)bypassed;
  (void)reset_on_bypass;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setBusStripInsertBypassed(const val& bus_id_val,
                                                   const val& insert_index_val, bool bypassed,
                                                   bool reset_on_bypass) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
#if defined(SONARE_WITH_MIXING)
  if (!engine_.set_bus_insert_bypassed(bus_id, insert_index, bypassed, reset_on_bypass)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid bus strip insert bypass target");
  }
#else
  (void)bus_id;
  (void)insert_index;
  (void)bypassed;
  (void)reset_on_bypass;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackStripInsertParamByName(const val& track_id_val,
                                                        const val& insert_index_val,
                                                        const std::string& param_name,
                                                        const val& value_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "value");
#if defined(SONARE_WITH_MIXING)
  const auto result =
      engine_.set_track_insert_param_detailed(track_id, insert_index, param_name, value);
  if (result == sonare::engine::InsertParamSetResult::kInvalidTarget) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track strip insert parameter target");
  }
  if (result == sonare::engine::InsertParamSetResult::kQueueFull) {
    throw sonare::SonareException(sonare::ErrorCode::OutOfMemory,
                                  "failed to queue track strip insert parameter");
  }
#else
  (void)track_id;
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

bool RealtimeEngineWasm::applyTrackStripInsertParamByNameNow(const val& track_id_val,
                                                             const val& insert_index_val,
                                                             const std::string& param_name,
                                                             const val& value_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "value");
#if defined(SONARE_WITH_MIXING)
  return engine_.apply_track_insert_param_by_name_now(track_id, insert_index, param_name, value);
#else
  (void)track_id;
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::clearTrackStripInsertParameterBases(const val& track_id_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
#if defined(SONARE_WITH_MIXING)
  if (!engine_.clear_track_insert_parameter_bases(track_id)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track strip insert base target");
  }
#else
  (void)track_id;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::restoreTrackStripInsertParamByName(const val& track_id_val,
                                                            const val& insert_index_val,
                                                            const std::string& param_name,
                                                            const val& value_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "value");
#if defined(SONARE_WITH_MIXING)
  if (!engine_.restore_track_insert_param_by_name(track_id, insert_index, param_name, value)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track strip insert parameter target");
  }
#else
  (void)track_id;
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setMasterStripInsertParamByName(const val& insert_index_val,
                                                         const std::string& param_name,
                                                         const val& value_val) {
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "value");
#if defined(SONARE_WITH_MIXING)
  const auto result = engine_.set_master_insert_param_detailed(insert_index, param_name, value);
  if (result == sonare::engine::InsertParamSetResult::kInvalidTarget) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid master strip insert parameter target");
  }
  if (result == sonare::engine::InsertParamSetResult::kQueueFull) {
    throw sonare::SonareException(sonare::ErrorCode::OutOfMemory,
                                  "failed to queue master strip insert parameter");
  }
#else
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

bool RealtimeEngineWasm::applyMasterStripInsertParamByNameNow(const val& insert_index_val,
                                                              const std::string& param_name,
                                                              const val& value_val) {
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "value");
#if defined(SONARE_WITH_MIXING)
  return engine_.apply_master_insert_param_by_name_now(insert_index, param_name, value);
#else
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::clearMasterStripInsertParameterBases() {
#if defined(SONARE_WITH_MIXING)
  engine_.clear_master_insert_parameter_bases();
#else
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::restoreMasterStripInsertParamByName(const val& insert_index_val,
                                                             const std::string& param_name,
                                                             const val& value_val) {
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "value");
#if defined(SONARE_WITH_MIXING)
  if (!engine_.restore_master_insert_param_by_name(insert_index, param_name, value)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid master strip insert parameter target");
  }
#else
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setBusStripInsertParamByName(const val& bus_id_val,
                                                      const val& insert_index_val,
                                                      const std::string& param_name,
                                                      const val& value_val) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "value");
#if defined(SONARE_WITH_MIXING)
  const auto result =
      engine_.set_bus_insert_param_detailed(bus_id, insert_index, param_name, value);
  if (result == sonare::engine::InsertParamSetResult::kInvalidTarget) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid bus strip insert parameter target");
  }
  if (result == sonare::engine::InsertParamSetResult::kQueueFull) {
    throw sonare::SonareException(sonare::ErrorCode::OutOfMemory,
                                  "failed to queue bus strip insert parameter");
  }
#else
  (void)bus_id;
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

bool RealtimeEngineWasm::applyBusStripInsertParamByNameNow(const val& bus_id_val,
                                                           const val& insert_index_val,
                                                           const std::string& param_name,
                                                           const val& value_val) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "value");
#if defined(SONARE_WITH_MIXING)
  return engine_.apply_bus_insert_param_by_name_now(bus_id, insert_index, param_name, value);
#else
  (void)bus_id;
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::clearBusStripInsertParameterBases(const val& bus_id_val) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
#if defined(SONARE_WITH_MIXING)
  if (!engine_.clear_bus_insert_parameter_bases(bus_id)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid bus strip insert base target");
  }
#else
  (void)bus_id;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::restoreBusStripInsertParamByName(const val& bus_id_val,
                                                          const val& insert_index_val,
                                                          const std::string& param_name,
                                                          const val& value_val) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "value");
#if defined(SONARE_WITH_MIXING)
  if (!engine_.restore_bus_insert_param_by_name(bus_id, insert_index, param_name, value)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid bus strip insert parameter target");
  }
#else
  (void)bus_id;
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

// Resolves a track-lane / master / bus insert parameter (JSON-key name) to the
// reserved insert-automation id passed to setAutomationLane. Returns -1 when the
// strip, insert, or key is unknown. The id is returned as a double so the full
// 32-bit unsigned reserved id (which exceeds the signed-int range) survives the
// JS boundary; the caller passes it back to setAutomationLane.
double RealtimeEngineWasm::resolveTrackInsertAutomationId(const val& track_id_val,
                                                          const val& insert_index_val,
                                                          const std::string& param_name) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
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

double RealtimeEngineWasm::resolveMasterInsertAutomationId(const val& insert_index_val,
                                                           const std::string& param_name) {
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
#if defined(SONARE_WITH_MIXING)
  return static_cast<double>(engine_.resolve_master_insert_automation_id(insert_index, param_name));
#else
  (void)insert_index;
  (void)param_name;
  return -1.0;
#endif
}

double RealtimeEngineWasm::resolveBusInsertAutomationId(const val& bus_id_val,
                                                        const val& insert_index_val,
                                                        const std::string& param_name) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
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

void RealtimeEngineWasm::setTrackStripPan(const val& track_id_val, const val& pan_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const float pan = checkedFloatFromVal(pan_val, "pan");
#if defined(SONARE_WITH_MIXING)
  if (!engine_.set_track_pan(track_id, pan)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track strip pan target");
  }
#else
  (void)track_id;
  (void)pan;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackStripPanLaw(const val& track_id_val, const val& pan_law_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const int pan_law = checkedIntFromVal(pan_law_val, "panLaw");
#if defined(SONARE_WITH_MIXING)
  if (pan_law < 0 || pan_law >= sonare::mixing::kPanLawCount) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown mixing pan law");
  }
  if (!engine_.set_track_pan_law(track_id, static_cast<sonare::mixing::PanLaw>(pan_law))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track strip pan-law target");
  }
#else
  (void)track_id;
  (void)pan_law;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackStripPanMode(const val& track_id_val, const val& pan_mode_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const int pan_mode = checkedIntFromVal(pan_mode_val, "panMode");
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
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackStripDualPan(const val& track_id_val, const val& left_pan_val,
                                              const val& right_pan_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const float left_pan = checkedFloatFromVal(left_pan_val, "leftPan");
  const float right_pan = checkedFloatFromVal(right_pan_val, "rightPan");
#if defined(SONARE_WITH_MIXING)
  if (!engine_.set_track_dual_pan(track_id, left_pan, right_pan)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track strip dual-pan target");
  }
#else
  (void)track_id;
  (void)left_pan;
  (void)right_pan;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setBusStripPan(const val& bus_id_val, const val& pan_val) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const float pan = checkedFloatFromVal(pan_val, "pan");
#if defined(SONARE_WITH_MIXING)
  if (!engine_.set_bus_pan(bus_id, pan)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid bus strip pan target");
  }
#else
  (void)bus_id;
  (void)pan;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setBusStripPanLaw(const val& bus_id_val, const val& pan_law_val) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const int pan_law = checkedIntFromVal(pan_law_val, "panLaw");
#if defined(SONARE_WITH_MIXING)
  if (pan_law < 0 || pan_law >= sonare::mixing::kPanLawCount) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown mixing pan law");
  }
  if (!engine_.set_bus_pan_law(bus_id, static_cast<sonare::mixing::PanLaw>(pan_law))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid bus strip pan-law target");
  }
#else
  (void)bus_id;
  (void)pan_law;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setBusStripPanMode(const val& bus_id_val, const val& pan_mode_val) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const int pan_mode = checkedIntFromVal(pan_mode_val, "panMode");
#if defined(SONARE_WITH_MIXING)
  if (pan_mode < 0 || pan_mode > 2) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown mixing pan mode");
  }
  if (!engine_.set_bus_pan_mode(bus_id, static_cast<sonare::mixing::PanMode>(pan_mode))) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid bus strip pan-mode target");
  }
#else
  (void)bus_id;
  (void)pan_mode;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setBusStripDualPan(const val& bus_id_val, const val& left_pan_val,
                                            const val& right_pan_val) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const float left_pan = checkedFloatFromVal(left_pan_val, "leftPan");
  const float right_pan = checkedFloatFromVal(right_pan_val, "rightPan");
#if defined(SONARE_WITH_MIXING)
  if (!engine_.set_bus_dual_pan(bus_id, left_pan, right_pan)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid bus strip dual-pan target");
  }
#else
  (void)bus_id;
  (void)left_pan;
  (void)right_pan;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackStripChannelDelaySamples(const val& track_id_val,
                                                          const val& delay_samples_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const int delay_samples = checkedIntFromVal(delay_samples_val, "delaySamples");
#if defined(SONARE_WITH_MIXING)
  if (!engine_.set_track_channel_delay_samples(track_id, delay_samples)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid track strip channel-delay target");
  }
#else
  (void)track_id;
  (void)delay_samples;
  throw sonare::SonareException(sonare::ErrorCode::NotImplemented,
                                "mixing support is not compiled in");
#endif
}

void registerRealtimeEngineMixer(class_<RealtimeEngineWasm>& cls) {
  cls.function("setTrackLanes", &RealtimeEngineWasm::setTrackLanes)
      .function("setLaneSidechain", &RealtimeEngineWasm::setLaneSidechain)
      .function("setTrackBuses", &RealtimeEngineWasm::setTrackBuses)
      .function("setBusSidechain", &RealtimeEngineWasm::setBusSidechain)
      .function("setMasterSidechain", &RealtimeEngineWasm::setMasterSidechain)
      .function("setBusStripJson", &RealtimeEngineWasm::setBusStripJson)
      .function("setTrackStripJson", &RealtimeEngineWasm::setTrackStripJson)
      .function("setTrackStripEqBandJson", &RealtimeEngineWasm::setTrackStripEqBandJson)
      .function("setBusStripEqBandJson", &RealtimeEngineWasm::setBusStripEqBandJson)
      .function("setTrackStripInsertBypassed", &RealtimeEngineWasm::setTrackStripInsertBypassed)
      .function("setMasterStripJson", &RealtimeEngineWasm::setMasterStripJson)
      .function("setMasterStripEqBandJson", &RealtimeEngineWasm::setMasterStripEqBandJson)
      .function("setMasterStripInsertBypassed", &RealtimeEngineWasm::setMasterStripInsertBypassed)
      .function("setTrackStripInsertParamByName",
                &RealtimeEngineWasm::setTrackStripInsertParamByName)
      .function("applyTrackStripInsertParamByNameNow",
                &RealtimeEngineWasm::applyTrackStripInsertParamByNameNow)
      .function("restoreTrackStripInsertParamByName",
                &RealtimeEngineWasm::restoreTrackStripInsertParamByName)
      .function("clearTrackInsertParameterBases",
                &RealtimeEngineWasm::clearTrackStripInsertParameterBases)
      .function("setMasterStripInsertParamByName",
                &RealtimeEngineWasm::setMasterStripInsertParamByName)
      .function("applyMasterStripInsertParamByNameNow",
                &RealtimeEngineWasm::applyMasterStripInsertParamByNameNow)
      .function("restoreMasterStripInsertParamByName",
                &RealtimeEngineWasm::restoreMasterStripInsertParamByName)
      .function("clearMasterInsertParameterBases",
                &RealtimeEngineWasm::clearMasterStripInsertParameterBases)
      .function("setBusStripInsertParamByName", &RealtimeEngineWasm::setBusStripInsertParamByName)
      .function("applyBusStripInsertParamByNameNow",
                &RealtimeEngineWasm::applyBusStripInsertParamByNameNow)
      .function("restoreBusStripInsertParamByName",
                &RealtimeEngineWasm::restoreBusStripInsertParamByName)
      .function("clearBusInsertParameterBases",
                &RealtimeEngineWasm::clearBusStripInsertParameterBases)
      .function("setBusStripInsertBypassed", &RealtimeEngineWasm::setBusStripInsertBypassed)
      .function("resolveTrackInsertAutomationId",
                &RealtimeEngineWasm::resolveTrackInsertAutomationId)
      .function("resolveMasterInsertAutomationId",
                &RealtimeEngineWasm::resolveMasterInsertAutomationId)
      .function("resolveBusInsertAutomationId", &RealtimeEngineWasm::resolveBusInsertAutomationId)
      .function("setTrackStripPan", &RealtimeEngineWasm::setTrackStripPan)
      .function("setTrackStripPanLaw", &RealtimeEngineWasm::setTrackStripPanLaw)
      .function("setTrackStripPanMode", &RealtimeEngineWasm::setTrackStripPanMode)
      .function("setTrackStripDualPan", &RealtimeEngineWasm::setTrackStripDualPan)
      .function("setBusStripPan", &RealtimeEngineWasm::setBusStripPan)
      .function("setBusStripPanLaw", &RealtimeEngineWasm::setBusStripPanLaw)
      .function("setBusStripPanMode", &RealtimeEngineWasm::setBusStripPanMode)
      .function("setBusStripDualPan", &RealtimeEngineWasm::setBusStripDualPan)
      .function("setTrackStripChannelDelaySamples",
                &RealtimeEngineWasm::setTrackStripChannelDelaySamples);
}

#endif  // __EMSCRIPTEN__
