/// @file realtime_engine_mixer.cpp
/// @brief Embind realtime-engine facade: tracks, buses, strips, panning.

#ifdef __EMSCRIPTEN__

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "c_api/eq_band_json.h"
#include "mixing/api/scene.h"
#include "mixing/pan_law.h"
#include "realtime_engine_wasm.h"
#include "util/json.h"
#include "util/parameter_checks.h"

#if defined(SONARE_WITH_MIXING)
#include "c_api/engine_automation_resolver.h"
#include "c_api/engine_eq_band_argument.h"
#include "c_api/engine_mixer_refusal.h"
#include "mixing/channel_strip_eq.h"

namespace {

/// Whether @p track_id names a lane of @p engine's mixer.
bool trackHasLane(sonare::engine::RealtimeEngine& engine, uint32_t track_id) {
  uint32_t position = 0;
  return engine.track_mixer().track_lane_position(track_id, &position);
}

/// Refuses an EQ band index outside the engine's band table.
void checkEqBandIndex(int band_index) {
  sonare::util::check::integer_within(band_index, sonare_c_detail::kEngineBandIndexDomain,
                                      "band_index");
}

// Reads a `sends` array (a lane's, a bus's or the positional ops'), in the one
// shape all share. sendTiming mirrors SonareSendTiming (0 post, 1 pre) and
// defaults to post-fader.
std::vector<sonare::engine::TrackLaneConfig::Send> readSends(const val& sends) {
  std::vector<sonare::engine::TrackLaneConfig::Send> out;
  const int send_count = static_cast<int>(wasmArrayLength(sends, "sends"));
  out.reserve(static_cast<size_t>(send_count));
  for (int send_index = 0; send_index < send_count; ++send_index) {
    const val send = requiredElementObject(sends, send_index, "sends");
    const std::string at = elementAt("sends", send_index);
    const uint32_t bus_id = checkedUintFromVal(requiredNumberElementProperty(send, "busId", at),
                                               elementField(at, "busId").c_str());
    const sonare::engine::TrackLaneConfig::Send defaults{};
    // Wire timing is 0 = post-fader, 1 = pre-fader, the reverse of the core enum's order.
    const int timing_value = intProperty(
        send, "sendTiming", defaults.timing == sonare::mixing::SendTiming::PreFader ? 1 : 0);
    if (timing_value != 0 && timing_value != 1) {
      throw WasmRangeError("unknown mixing send timing");
    }
    const sonare::mixing::SendTiming timing = timing_value == 1
                                                  ? sonare::mixing::SendTiming::PreFader
                                                  : sonare::mixing::SendTiming::PostFader;
    out.push_back({bus_id, floatProperty(send, "levelDb", defaults.level_db),
                   boolProperty(send, "enabled", true), timing});
  }
  return out;
}

std::vector<sonare::engine::TrackLaneConfig::Send> readOptionalSends(const val& owner) {
  if (owner["sends"].isUndefined() || owner["sends"].isNull()) return {};
  return readSends(owner["sends"]);
}

sonare::engine::SidechainSourceKind sidechainSourceKind(int source_kind) {
  const std::array<int, 2> kinds{static_cast<int>(sonare::engine::SidechainSourceKind::Track),
                                 static_cast<int>(sonare::engine::SidechainSourceKind::Bus)};
  sonare::util::check::one_of(source_kind, kinds, "source_kind");
  return static_cast<sonare::engine::SidechainSourceKind>(source_kind);
}

// An unknown kind maps past the enum so the core orders its kInvalidSourceKind
// answer among its other checks, as the C ABI query does.
sonare::engine::SidechainSourceKind sidechainSourceKindForQuery(int source_kind) {
  if (source_kind == static_cast<int>(sonare::engine::SidechainSourceKind::Track) ||
      source_kind == static_cast<int>(sonare::engine::SidechainSourceKind::Bus)) {
    return static_cast<sonare::engine::SidechainSourceKind>(source_kind);
  }
  return static_cast<sonare::engine::SidechainSourceKind>(UINT8_MAX);
}

}  // namespace
#endif

void RealtimeEngineWasm::setTrackLanes(val lanes) {
#if defined(SONARE_WITH_MIXING)
  const int count = static_cast<int>(wasmArrayLength(lanes, "lanes"));
  sonare::util::check::integer_in_range(
      count, 0, static_cast<long long>(sonare::engine::TrackMixerRuntime::kMaxTrackLanes),
      "lanes.length");
  std::vector<sonare::engine::TrackLaneConfig> configs;
  configs.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    val lane_val = lanes[i];
    uint32_t track_id = 0;
    if (lane_val.typeOf().as<std::string>() == "number") {
      track_id = checkedUintFromVal(lane_val, "trackId");
    } else {
      lane_val = requiredElementObject(lanes, i, "lanes");
      const std::string at = elementAt("lanes", i);
      track_id = checkedUintFromVal(requiredNumberElementProperty(lane_val, "trackId", at),
                                    elementField(at, "trackId").c_str());
    }
    sonare::engine::TrackLaneConfig config{track_id};
    if (lane_val.typeOf().as<std::string>() == "object") {
      config.output_bus_id = uintProperty(lane_val, "outputBusId", config.output_bus_id);
      // Absent defaults to stereo, matching the C ABI / Node surfaces. An
      // out-of-range layout is rejected like the C ABI's is_valid check. The
      // full-range bound is checked BEFORE narrowing to uint8_t: narrowing
      // first would let e.g. 257 wrap to 1 (a valid Stereo) and pass.
      const int raw_layout = intProperty(lane_val, "sourceChannelLayout",
                                         static_cast<int>(sonare::ChannelLayout::Stereo));
      if (raw_layout < 0 || raw_layout > 255 ||
          !sonare::is_valid_channel_layout(static_cast<uint8_t>(raw_layout))) {
        throw WasmRangeError("invalid source channel layout");
      }
      if (raw_layout != static_cast<int>(sonare::ChannelLayout::Stereo)) {
        throw sonare::SonareException(
            sonare::ErrorCode::NotSupported,
            sonare::util::check::indexed_subject("lanes", static_cast<std::size_t>(i),
                                                 "sourceChannelLayout") +
                " other than stereo is not supported");
      }
      config.source_layout = sonare::ChannelLayout::Stereo;
    }
    if (lane_val.typeOf().as<std::string>() == "object") config.sends = readOptionalSends(lane_val);
    configs.push_back(std::move(config));
  }
  withCEntryRefusal("engine_set_track_lanes", [&] {
    if (!engine_.set_track_lanes(std::move(configs))) {
      sonare_c_detail::throw_mixer_edit_refusal(engine_, sonare_c_detail::MixerEditScope::kLanes);
    }
  });
#else
  (void)lanes;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

/// Replaces one lane's sends, leaving its output bus and every other lane alone.
/// An empty array clears them. Matches sonare_engine_set_track_sends.
void RealtimeEngineWasm::setTrackSends(const val& track_id_val, const val& sends_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
#if defined(SONARE_WITH_MIXING)
  auto sends = readSends(sends_val);
  withCEntryRefusal("engine_set_track_sends", [&] {
    sonare_c_detail::require_track_lane_edit(engine_,
                                             engine_.set_track_sends(track_id, std::move(sends)),
                                             sonare_c_detail::MixerEditScope::kTrackSends);
  });
#else
  (void)track_id;
  (void)sends_val;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

/// Sets one lane's output bus (0 = master mix), leaving its sends and every other
/// lane alone. Matches sonare_engine_set_track_output_bus.
void RealtimeEngineWasm::setTrackOutputBus(const val& track_id_val, const val& bus_id_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_track_output_bus", [&] {
    sonare_c_detail::require_track_lane_edit(engine_,
                                             engine_.set_track_output_bus(track_id, bus_id),
                                             sonare_c_detail::MixerEditScope::kOutputBus);
  });
#else
  (void)track_id;
  (void)bus_id;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

/// Routes the live input into @p track_id_val's lane. Matches sonare_engine_set_input_track.
void RealtimeEngineWasm::setInputTrack(const val& track_id_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_input_track", [&] { engine_.set_input_track(track_id); });
#else
  (void)track_id;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::clearInputTrack() {
#if defined(SONARE_WITH_MIXING)
  engine_.clear_input_track();
#else
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

val RealtimeEngineWasm::inputTrack() const {
#if defined(SONARE_WITH_MIXING)
  const std::optional<uint32_t> track_id = engine_.input_track();
  return track_id.has_value() ? val(*track_id) : val::undefined();
#else
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

/// Keys one insert of a lane strip from another lane's post-strip audio
/// (ducking/sidechainRouter inserts); source.id 0 removes the binding.
/// Throws, leaving the bindings unchanged, a RangeError for a lane keying itself
/// or a binding that closes a cycle, and InvalidState for a full table or an
/// alignment past the delay ceiling. Matches sonare_engine_set_lane_sidechain.
void RealtimeEngineWasm::setLaneSidechain(const val& track_id_val, const val& insert_index_val,
                                          const val& source_track_id_val) {
  if (!track_id_val.isNumber()) throw WasmTypeError("ref must be a number");
  if (!insert_index_val.isNumber()) throw WasmTypeError("insertIndex must be a number");
  if (!source_track_id_val.isNumber()) throw WasmTypeError("source.id must be a number");
  const uint32_t track_id = checkedUintFromVal(track_id_val, "ref");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const uint32_t source_track_id = checkedUintFromVal(source_track_id_val, "source.id");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_lane_sidechain", [&] {
    if (track_id == 0) sonare_c_detail::refuse_unknown_mixer_track();
    if (engine_.set_lane_sidechain(track_id, insert_index, source_track_id)) return;
    const sonare_c_detail::SidechainWording wording{
        "track_id", "a track", "must be less than the number of inserts on the track",
        "source_track_id", "a track"};
    sonare_c_detail::throw_sidechain_refusal(
        engine_, engine_.can_set_lane_sidechain(track_id, insert_index, source_track_id), wording,
        trackHasLane(engine_, track_id),
        static_cast<int>(sonare::engine::SidechainSourceKind::Track));
  });
#else
  (void)track_id;
  (void)insert_index;
  (void)source_track_id;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

/// Reports what setLaneSidechain would answer without changing anything: a
/// SonareSidechainRefusal ordinal (0 accepted). Matches
/// sonare_engine_can_set_lane_sidechain; a build without mixing throws, as the setter does.
int RealtimeEngineWasm::canSetLaneSidechain(const val& track_id_val, const val& insert_index_val,
                                            const val& source_track_id_val) const {
  if (!track_id_val.isNumber()) throw WasmTypeError("ref must be a number");
  if (!insert_index_val.isNumber()) throw WasmTypeError("insertIndex must be a number");
  if (!source_track_id_val.isNumber()) throw WasmTypeError("source.id must be a number");
  const uint32_t track_id = checkedUintFromVal(track_id_val, "ref");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const uint32_t source_track_id = checkedUintFromVal(source_track_id_val, "source.id");
#if defined(SONARE_WITH_MIXING)
  return static_cast<int>(engine_.can_set_lane_sidechain(track_id, insert_index, source_track_id));
#else
  (void)track_id;
  (void)insert_index;
  (void)source_track_id;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackBuses(val buses) {
#if defined(SONARE_WITH_MIXING)
  const int count = static_cast<int>(wasmArrayLength(buses, "buses"));
  std::vector<sonare::engine::TrackBusConfig> configs;
  configs.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    const val bus = requiredElementObject(buses, i, "buses");
    const std::string at = elementAt("buses", i);
    const uint32_t bus_id = checkedUintFromVal(requiredNumberElementProperty(bus, "busId", at),
                                               elementField(at, "busId").c_str());
    const int layout_value = intProperty(bus, "channelLayout", 1);
    // The full-range bound is checked BEFORE narrowing to uint8_t, the same
    // way the lane sourceChannelLayout check above does: narrowing first
    // would let an out-of-range value (or a negative one) wrap into a valid
    // ChannelLayout and pass silently.
    if (layout_value < 0 || layout_value > 255 ||
        !sonare::is_valid_channel_layout(static_cast<uint8_t>(layout_value))) {
      throw WasmRangeError("invalid bus channel layout");
    }
    sonare::engine::TrackBusConfig config{
        bus_id, floatProperty(bus, "gainDb", sonare::engine::TrackBusConfig{}.gain_db),
        static_cast<sonare::ChannelLayout>(layout_value)};
    config.output_bus_id = uintProperty(bus, "outputBusId", config.output_bus_id);
    config.sends = readOptionalSends(bus);
    configs.push_back(std::move(config));
  }
  withCEntryRefusal("engine_set_track_buses", [&] {
    if (!engine_.set_track_buses(std::move(configs))) {
      sonare_c_detail::throw_mixer_edit_refusal(engine_, sonare_c_detail::MixerEditScope::kBuses);
    }
  });
#else
  (void)buses;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

/// Keys one insert of a bus strip from a track lane or another bus (matches
/// sonare_engine_set_bus_sidechain). source.id 0 removes the binding.
void RealtimeEngineWasm::setBusSidechain(const val& bus_id_val, const val& insert_index_val,
                                         const val& source_kind_val, const val& source_id_val) {
  if (!bus_id_val.isNumber()) throw WasmTypeError("busId must be a number");
  if (!insert_index_val.isNumber()) throw WasmTypeError("insertIndex must be a number");
  if (!source_kind_val.isNumber()) throw WasmTypeError("source.kind must be a number");
  if (!source_id_val.isNumber()) throw WasmTypeError("source.id must be a number");
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const int source_kind = checkedIntFromVal(source_kind_val, "source.kind");
  const uint32_t source_id = checkedUintFromVal(source_id_val, "source.id");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_bus_sidechain", [&] {
    if (bus_id == 0) sonare::util::check::refuse_unknown_id("bus_id", "a bus", "engine");
    const sonare::engine::SidechainSourceKind kind = sidechainSourceKind(source_kind);
    if (engine_.set_bus_sidechain(bus_id, insert_index, kind, source_id)) return;
    const sonare_c_detail::SidechainWording wording{
        "bus_id", "a bus", "must be less than the number of inserts on the bus", "source_id",
        kind == sonare::engine::SidechainSourceKind::Track ? "a track" : "a bus"};
    sonare_c_detail::throw_sidechain_refusal(
        engine_, engine_.can_set_bus_sidechain(bus_id, insert_index, kind, source_id), wording,
        true, source_kind);
  });
#else
  (void)bus_id;
  (void)insert_index;
  (void)source_kind;
  (void)source_id;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

/// Reports what setBusSidechain would answer without changing anything (matches
/// sonare_engine_can_set_bus_sidechain).
int RealtimeEngineWasm::canSetBusSidechain(const val& bus_id_val, const val& insert_index_val,
                                           const val& source_kind_val,
                                           const val& source_id_val) const {
  if (!bus_id_val.isNumber()) throw WasmTypeError("busId must be a number");
  if (!insert_index_val.isNumber()) throw WasmTypeError("insertIndex must be a number");
  if (!source_kind_val.isNumber()) throw WasmTypeError("source.kind must be a number");
  if (!source_id_val.isNumber()) throw WasmTypeError("source.id must be a number");
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const int source_kind = checkedIntFromVal(source_kind_val, "source.kind");
  const uint32_t source_id = checkedUintFromVal(source_id_val, "source.id");
#if defined(SONARE_WITH_MIXING)
  return static_cast<int>(engine_.can_set_bus_sidechain(
      bus_id, insert_index, sidechainSourceKindForQuery(source_kind), source_id));
#else
  (void)bus_id;
  (void)insert_index;
  (void)source_kind;
  (void)source_id;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

/// Reports what setMasterSidechain would answer without changing anything
/// (matches sonare_engine_can_set_master_sidechain).
int RealtimeEngineWasm::canSetMasterSidechain(const val& insert_index_val,
                                              const val& source_kind_val,
                                              const val& source_id_val) const {
  if (!insert_index_val.isNumber()) throw WasmTypeError("insertIndex must be a number");
  if (!source_kind_val.isNumber()) throw WasmTypeError("source.kind must be a number");
  if (!source_id_val.isNumber()) throw WasmTypeError("source.id must be a number");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const int source_kind = checkedIntFromVal(source_kind_val, "source.kind");
  const uint32_t source_id = checkedUintFromVal(source_id_val, "source.id");
#if defined(SONARE_WITH_MIXING)
  return static_cast<int>(engine_.can_set_master_sidechain(
      insert_index, sidechainSourceKindForQuery(source_kind), source_id));
#else
  (void)insert_index;
  (void)source_kind;
  (void)source_id;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

/// Keys one insert of the master strip from a track lane or a bus (matches
/// sonare_engine_set_master_sidechain). source.id 0 removes the binding.
void RealtimeEngineWasm::setMasterSidechain(const val& insert_index_val, const val& source_kind_val,
                                            const val& source_id_val) {
  if (!insert_index_val.isNumber()) throw WasmTypeError("insertIndex must be a number");
  if (!source_kind_val.isNumber()) throw WasmTypeError("source.kind must be a number");
  if (!source_id_val.isNumber()) throw WasmTypeError("source.id must be a number");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const int source_kind = checkedIntFromVal(source_kind_val, "source.kind");
  const uint32_t source_id = checkedUintFromVal(source_id_val, "source.id");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_master_sidechain", [&] {
    const sonare::engine::SidechainSourceKind kind = sidechainSourceKind(source_kind);
    if (engine_.set_master_sidechain(insert_index, kind, source_id)) return;
    const sonare_c_detail::SidechainWording wording{
        nullptr, nullptr, "must be less than the number of inserts on the master strip",
        "source_id", kind == sonare::engine::SidechainSourceKind::Track ? "a track" : "a bus"};
    sonare_c_detail::throw_sidechain_refusal(
        engine_, engine_.can_set_master_sidechain(insert_index, kind, source_id), wording, true,
        source_kind);
  });
#else
  (void)insert_index;
  (void)source_kind;
  (void)source_id;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setBusStripJson(const val& bus_id_val, const std::string& settings_json) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_bus_strip_json", [&] {
    if (bus_id == 0) sonare::util::check::refuse_unknown_id("bus_id", "a bus", "engine");
  });
  const sonare::mixing::api::Bus settings =
      sonare::mixing::api::bus_settings_from_json(settings_json, "settings");
  withCEntryRefusal("engine_set_bus_strip_json", [&] {
    if (!engine_.set_bus_strip(bus_id, settings)) {
      sonare_c_detail::throw_strip_edit_refusal(engine_, settings.inserts, &settings.eq);
    }
  });
#else
  (void)bus_id;
  (void)settings_json;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackStripJson(const val& track_id_val,
                                           const std::string& settings_json) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "ref");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_track_strip_json", [&] {
    if (track_id == 0) sonare_c_detail::refuse_unknown_mixer_track();
  });
  const sonare::mixing::api::Strip settings =
      sonare::mixing::api::strip_settings_from_json(settings_json, "settings");
  withCEntryRefusal("engine_set_track_strip_json", [&] {
    if (!engine_.set_track_strip(track_id, settings)) {
      sonare_c_detail::throw_strip_edit_refusal(engine_, settings.inserts, &settings.eq);
    }
  });
#else
  (void)track_id;
  (void)settings_json;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackStripEqBandJson(const val& track_id_val, const val& band_index_val,
                                                 const std::string& band_json) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "ref");
  const int band_index = checkedIntFromVal(band_index_val, "bandIndex");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_track_strip_eq_band_json", [&] {
    if (track_id == 0) sonare_c_detail::refuse_unknown_mixer_track();
    checkEqBandIndex(band_index);
    const sonare::mastering::eq::EqBand band =
        sonare::c_api::parse_eq_band_json(wasmCString(band_json, "bandJson"));
    sonare::c_api::validate_engine_eq_band_argument(band, engine_.sample_rate());
    if (!engine_.set_track_eq_band(track_id, static_cast<size_t>(band_index), band)) {
      sonare_c_detail::throw_mixer_edit_refusal(engine_, sonare_c_detail::MixerEditScope::kEqBand,
                                                track_id);
    }
  });
#else
  (void)track_id;
  (void)band_index;
  (void)band_json;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setBusStripEqBandJson(const val& bus_id_val, const val& band_index_val,
                                               const std::string& band_json) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const int band_index = checkedIntFromVal(band_index_val, "bandIndex");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_bus_strip_eq_band_json", [&] {
    if (bus_id == 0) sonare::util::check::refuse_unknown_id("bus_id", "a bus", "engine");
    checkEqBandIndex(band_index);
    const sonare::mastering::eq::EqBand band =
        sonare::c_api::parse_eq_band_json(wasmCString(band_json, "bandJson"));
    sonare::c_api::validate_engine_eq_band_argument(band, engine_.sample_rate());
    if (!engine_.set_bus_eq_band(bus_id, static_cast<size_t>(band_index), band)) {
      sonare_c_detail::throw_mixer_edit_refusal(engine_, sonare_c_detail::MixerEditScope::kEqBand);
    }
  });
#else
  (void)bus_id;
  (void)band_index;
  (void)band_json;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackStripInsertBypassed(const val& track_id_val,
                                                     const val& insert_index_val, bool bypassed,
                                                     bool reset_on_bypass) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "ref");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_track_strip_insert_bypassed", [&] {
    if (track_id == 0) sonare_c_detail::refuse_unknown_mixer_track();
    if (engine_.set_track_insert_bypassed(track_id, insert_index, bypassed, reset_on_bypass)) {
      return;
    }
    sonare_c_detail::engine_automation::require_track_insert(engine_, track_id, insert_index,
                                                             nullptr);
    sonare_c_detail::engine_automation::refuse_insert_bypass();
  });
#else
  (void)track_id;
  (void)insert_index;
  (void)bypassed;
  (void)reset_on_bypass;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setMasterStripJson(const std::string& settings_json) {
#if defined(SONARE_WITH_MIXING)
  const sonare::mixing::api::Strip settings =
      sonare::mixing::api::strip_settings_from_json(settings_json, "settings");
  withCEntryRefusal("engine_set_master_strip_json", [&] {
    if (!engine_.set_master_strip(settings)) {
      sonare_c_detail::throw_strip_edit_refusal(engine_, settings.inserts, &settings.eq,
                                                /*prepare_inserts=*/true);
    }
  });
#else
  (void)settings_json;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setMasterStripEqBandJson(const val& band_index_val,
                                                  const std::string& band_json) {
  const int band_index = checkedIntFromVal(band_index_val, "bandIndex");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_master_strip_eq_band_json", [&] {
    checkEqBandIndex(band_index);
    const sonare::mastering::eq::EqBand band =
        sonare::c_api::parse_eq_band_json(wasmCString(band_json, "bandJson"));
    sonare::c_api::validate_engine_eq_band_argument(band, engine_.sample_rate());
    if (!engine_.set_master_eq_band(static_cast<size_t>(band_index), band)) {
      sonare_c_detail::throw_mixer_edit_refusal(engine_, sonare_c_detail::MixerEditScope::kEqBand);
    }
  });
#else
  (void)band_index;
  (void)band_json;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setMasterStripInsertBypassed(const val& insert_index_val, bool bypassed,
                                                      bool reset_on_bypass) {
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_master_strip_insert_bypassed", [&] {
    if (engine_.set_master_insert_bypassed(insert_index, bypassed, reset_on_bypass)) {
      return;
    }
    sonare_c_detail::engine_automation::require_master_insert(engine_, insert_index, nullptr);
    sonare_c_detail::engine_automation::refuse_insert_bypass();
  });
#else
  (void)insert_index;
  (void)bypassed;
  (void)reset_on_bypass;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setBusStripInsertBypassed(const val& bus_id_val,
                                                   const val& insert_index_val, bool bypassed,
                                                   bool reset_on_bypass) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_bus_strip_insert_bypassed", [&] {
    if (bus_id == 0) sonare_c_detail::refuse_unknown_mixer_bus();
    if (engine_.set_bus_insert_bypassed(bus_id, insert_index, bypassed, reset_on_bypass)) {
      return;
    }
    sonare_c_detail::engine_automation::require_bus_insert(engine_, bus_id, insert_index, nullptr);
    sonare_c_detail::engine_automation::refuse_insert_bypass();
  });
#else
  (void)bus_id;
  (void)insert_index;
  (void)bypassed;
  (void)reset_on_bypass;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackStripInsertParamByName(const val& track_id_val,
                                                        const val& insert_index_val,
                                                        const std::string& param_name,
                                                        const val& value_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "ref");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "param.value");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_track_strip_insert_param_by_name", [&] {
    if (track_id == 0) sonare_c_detail::refuse_unknown_mixer_track();
    const char* key = wasmCString(param_name, "param.name");
    sonare::util::check::non_empty(key, "param_name");
    const auto result = engine_.set_track_insert_param_detailed(track_id, insert_index, key, value);
    if (result == sonare::engine::InsertParamSetResult::kQueued) return;
    if (result == sonare::engine::InsertParamSetResult::kInvalidTarget) {
      sonare_c_detail::engine_automation::require_track_insert(engine_, track_id, insert_index,
                                                               key);
      sonare_c_detail::engine_automation::refuse_no_automation_id(insert_index);
    }
    sonare_c_detail::engine_automation::refuse_insert_queue_full();
  });
#else
  (void)track_id;
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

bool RealtimeEngineWasm::applyTrackStripInsertParamByNameNow(const val& track_id_val,
                                                             const val& insert_index_val,
                                                             const std::string& param_name,
                                                             const val& value_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "ref");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "param.value");
#if defined(SONARE_WITH_MIXING)
  return withCEntryRefusal("engine_apply_track_strip_insert_param_by_name_now", [&] {
    if (track_id == 0) sonare_c_detail::refuse_unknown_mixer_track();
    const char* key = wasmCString(param_name, "param.name");
    sonare::util::check::non_empty(key, "param_name");
    return engine_.apply_track_insert_param_by_name_now(track_id, insert_index, key, value);
  });
#else
  (void)track_id;
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::clearTrackStripInsertParameterBases(const val& track_id_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "ref");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_clear_track_insert_parameter_bases", [&] {
    if (track_id == 0 || !engine_.clear_track_insert_parameter_bases(track_id)) {
      sonare_c_detail::refuse_unknown_mixer_track();
    }
  });
#else
  (void)track_id;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::restoreTrackStripInsertParamByName(const val& track_id_val,
                                                            const val& insert_index_val,
                                                            const std::string& param_name,
                                                            const val& value_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "ref");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "param.value");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_restore_track_strip_insert_param_by_name", [&] {
    if (track_id == 0) sonare_c_detail::refuse_unknown_mixer_track();
    const char* key = wasmCString(param_name, "param.name");
    sonare::util::check::non_empty(key, "param_name");
    if (engine_.restore_track_insert_param_by_name(track_id, insert_index, key, value)) return;
    sonare_c_detail::engine_automation::require_track_insert(engine_, track_id, insert_index, key);
    sonare_c_detail::engine_automation::refuse_insert_restore_value(key);
  });
#else
  (void)track_id;
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setMasterStripInsertParamByName(const val& insert_index_val,
                                                         const std::string& param_name,
                                                         const val& value_val) {
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "param.value");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_master_strip_insert_param_by_name", [&] {
    const char* key = wasmCString(param_name, "param.name");
    sonare::util::check::non_empty(key, "param_name");
    const auto result = engine_.set_master_insert_param_detailed(insert_index, key, value);
    if (result == sonare::engine::InsertParamSetResult::kQueued) return;
    if (result == sonare::engine::InsertParamSetResult::kInvalidTarget) {
      sonare_c_detail::engine_automation::require_master_insert(engine_, insert_index, key);
      sonare_c_detail::engine_automation::refuse_no_automation_id(insert_index);
    }
    sonare_c_detail::engine_automation::refuse_insert_queue_full();
  });
#else
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

bool RealtimeEngineWasm::applyMasterStripInsertParamByNameNow(const val& insert_index_val,
                                                              const std::string& param_name,
                                                              const val& value_val) {
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "param.value");
#if defined(SONARE_WITH_MIXING)
  return withCEntryRefusal("engine_apply_master_strip_insert_param_by_name_now", [&] {
    const char* key = wasmCString(param_name, "param.name");
    sonare::util::check::non_empty(key, "param_name");
    return engine_.apply_master_insert_param_by_name_now(insert_index, key, value);
  });
#else
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::clearMasterStripInsertParameterBases() {
#if defined(SONARE_WITH_MIXING)
  engine_.clear_master_insert_parameter_bases();
#else
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::restoreMasterStripInsertParamByName(const val& insert_index_val,
                                                             const std::string& param_name,
                                                             const val& value_val) {
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "param.value");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_restore_master_strip_insert_param_by_name", [&] {
    const char* key = wasmCString(param_name, "param.name");
    sonare::util::check::non_empty(key, "param_name");
    if (engine_.restore_master_insert_param_by_name(insert_index, key, value)) return;
    sonare_c_detail::engine_automation::require_master_insert(engine_, insert_index, key);
    sonare_c_detail::engine_automation::refuse_insert_restore_value(key);
  });
#else
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setBusStripInsertParamByName(const val& bus_id_val,
                                                      const val& insert_index_val,
                                                      const std::string& param_name,
                                                      const val& value_val) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "param.value");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_bus_strip_insert_param_by_name", [&] {
    if (bus_id == 0) sonare_c_detail::refuse_unknown_mixer_bus();
    const char* key = wasmCString(param_name, "param.name");
    sonare::util::check::non_empty(key, "param_name");
    const auto result = engine_.set_bus_insert_param_detailed(bus_id, insert_index, key, value);
    if (result == sonare::engine::InsertParamSetResult::kQueued) return;
    if (result == sonare::engine::InsertParamSetResult::kInvalidTarget) {
      sonare_c_detail::engine_automation::require_bus_insert(engine_, bus_id, insert_index, key);
      sonare_c_detail::engine_automation::refuse_no_automation_id(insert_index);
    }
    sonare_c_detail::engine_automation::refuse_insert_queue_full();
  });
#else
  (void)bus_id;
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

bool RealtimeEngineWasm::applyBusStripInsertParamByNameNow(const val& bus_id_val,
                                                           const val& insert_index_val,
                                                           const std::string& param_name,
                                                           const val& value_val) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "param.value");
#if defined(SONARE_WITH_MIXING)
  return withCEntryRefusal("engine_apply_bus_strip_insert_param_by_name_now", [&] {
    if (bus_id == 0) sonare_c_detail::refuse_unknown_mixer_bus();
    const char* key = wasmCString(param_name, "param.name");
    sonare::util::check::non_empty(key, "param_name");
    return engine_.apply_bus_insert_param_by_name_now(bus_id, insert_index, key, value);
  });
#else
  (void)bus_id;
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::clearBusStripInsertParameterBases(const val& bus_id_val) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_clear_bus_insert_parameter_bases", [&] {
    if (bus_id == 0 || !engine_.clear_bus_insert_parameter_bases(bus_id)) {
      sonare_c_detail::refuse_unknown_mixer_bus();
    }
  });
#else
  (void)bus_id;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::restoreBusStripInsertParamByName(const val& bus_id_val,
                                                          const val& insert_index_val,
                                                          const std::string& param_name,
                                                          const val& value_val) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const float value = checkedFloatFromVal(value_val, "param.value");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_restore_bus_strip_insert_param_by_name", [&] {
    if (bus_id == 0) sonare_c_detail::refuse_unknown_mixer_bus();
    const char* key = wasmCString(param_name, "param.name");
    sonare::util::check::non_empty(key, "param_name");
    if (engine_.restore_bus_insert_param_by_name(bus_id, insert_index, key, value)) return;
    sonare_c_detail::engine_automation::require_bus_insert(engine_, bus_id, insert_index, key);
    sonare_c_detail::engine_automation::refuse_insert_restore_value(key);
  });
#else
  (void)bus_id;
  (void)insert_index;
  (void)param_name;
  (void)value;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

// Resolves a track-lane / master / bus insert parameter (JSON-key name) to the
// reserved insert-automation id passed to setAutomationLane. An unknown strip,
// insert, or key is a RangeError. The id is returned as a double so the full
// 32-bit unsigned reserved id (which exceeds the signed-int range) survives the
// JS boundary; the caller passes it back to setAutomationLane.
double RealtimeEngineWasm::resolveTrackInsertAutomationId(const val& track_id_val,
                                                          const val& insert_index_val,
                                                          const std::string& param_name) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "ref");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
#if defined(SONARE_WITH_MIXING)
  return withCEntryRefusal("engine_resolve_track_insert_automation_id", [&] {
    if (track_id == 0) sonare_c_detail::engine_automation::refuse_track_id();
    const char* key = wasmCString(param_name, "paramName");
    sonare::util::check::non_empty(key, "param_name");
    return sonare_c_detail::engine_automation::resolve_track_insert(engine_, track_id, insert_index,
                                                                    key);
  });
#else
  (void)track_id;
  (void)insert_index;
  (void)param_name;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

double RealtimeEngineWasm::resolveTrackLaneAutomationId(const val& track_id_val,
                                                        const std::string& param_name) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "ref");
#if defined(SONARE_WITH_MIXING)
  return withCEntryRefusal("engine_resolve_track_lane_automation_id", [&] {
    if (track_id == 0) sonare_c_detail::engine_automation::refuse_track_id();
    const char* key = wasmCString(param_name, "kind");
    sonare::util::check::non_empty(key, "param_name");
    return sonare_c_detail::engine_automation::resolve_track_lane(engine_, track_id, key);
  });
#else
  (void)track_id;
  (void)param_name;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

double RealtimeEngineWasm::resolveMasterAutomationId(const std::string& param_name) {
#if defined(SONARE_WITH_MIXING)
  return withCEntryRefusal("engine_resolve_master_automation_id", [&] {
    const char* key = wasmCString(param_name, "kind");
    sonare::util::check::non_empty(key, "param_name");
    return sonare_c_detail::engine_automation::resolve_master(engine_, key);
  });
#else
  (void)param_name;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

double RealtimeEngineWasm::resolveBusAutomationId(const val& bus_id_val,
                                                  const std::string& param_name) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
#if defined(SONARE_WITH_MIXING)
  return withCEntryRefusal("engine_resolve_bus_automation_id", [&] {
    if (bus_id == 0) sonare_c_detail::engine_automation::refuse_bus_id();
    const char* key = wasmCString(param_name, "paramName");
    sonare::util::check::non_empty(key, "param_name");
    return sonare_c_detail::engine_automation::resolve_bus(engine_, bus_id, key);
  });
#else
  (void)bus_id;
  (void)param_name;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

double RealtimeEngineWasm::resolveMasterInsertAutomationId(const val& insert_index_val,
                                                           const std::string& param_name) {
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
#if defined(SONARE_WITH_MIXING)
  return withCEntryRefusal("engine_resolve_master_insert_automation_id", [&] {
    const char* key = wasmCString(param_name, "paramName");
    sonare::util::check::non_empty(key, "param_name");
    return sonare_c_detail::engine_automation::resolve_master_insert(engine_, insert_index, key);
  });
#else
  (void)insert_index;
  (void)param_name;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

double RealtimeEngineWasm::resolveBusInsertAutomationId(const val& bus_id_val,
                                                        const val& insert_index_val,
                                                        const std::string& param_name) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const uint32_t insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
#if defined(SONARE_WITH_MIXING)
  return withCEntryRefusal("engine_resolve_bus_insert_automation_id", [&] {
    if (bus_id == 0) sonare_c_detail::engine_automation::refuse_bus_id();
    const char* key = wasmCString(param_name, "paramName");
    sonare::util::check::non_empty(key, "param_name");
    return sonare_c_detail::engine_automation::resolve_bus_insert(engine_, bus_id, insert_index,
                                                                  key);
  });
#else
  (void)bus_id;
  (void)insert_index;
  (void)param_name;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackStripPan(const val& track_id_val, const val& pan_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "ref");
  const float pan = checkedFloatFromVal(pan_val, "pan");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_track_strip_pan", [&] {
    if (track_id == 0) sonare_c_detail::refuse_unknown_mixer_track();
    sonare_c_detail::check_mixer_pan(pan, "pan");
    if (!engine_.set_track_pan(track_id, pan)) {
      sonare_c_detail::throw_track_without_strip(engine_, track_id);
    }
  });
#else
  (void)track_id;
  (void)pan;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackStripPanLaw(const val& track_id_val, const val& pan_law_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "ref");
  const int pan_law = checkedIntFromVal(pan_law_val, "panLaw");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_track_strip_pan_law", [&] {
    if (track_id == 0) sonare_c_detail::refuse_unknown_mixer_track();
    sonare_c_detail::check_mixer_pan_law(pan_law);
    if (!engine_.set_track_pan_law(track_id, static_cast<sonare::mixing::PanLaw>(pan_law))) {
      sonare_c_detail::throw_track_without_strip(engine_, track_id);
    }
  });
#else
  (void)track_id;
  (void)pan_law;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackStripPanMode(const val& track_id_val, const val& pan_mode_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "ref");
  const int pan_mode = checkedIntFromVal(pan_mode_val, "panMode");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_track_strip_pan_mode", [&] {
    if (track_id == 0) sonare_c_detail::refuse_unknown_mixer_track();
    sonare_c_detail::check_mixer_pan_mode(pan_mode);
    if (!engine_.set_track_pan_mode(track_id, static_cast<sonare::mixing::PanMode>(pan_mode))) {
      sonare_c_detail::throw_track_without_strip(engine_, track_id);
    }
  });
#else
  (void)track_id;
  (void)pan_mode;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackStripDualPan(const val& track_id_val, const val& left_pan_val,
                                              const val& right_pan_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "ref");
  const float left_pan = checkedFloatFromVal(left_pan_val, "leftPan");
  const float right_pan = checkedFloatFromVal(right_pan_val, "rightPan");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_track_strip_dual_pan", [&] {
    if (track_id == 0) sonare_c_detail::refuse_unknown_mixer_track();
    sonare_c_detail::check_mixer_pan(left_pan, "left_pan");
    sonare_c_detail::check_mixer_pan(right_pan, "right_pan");
    if (!engine_.set_track_dual_pan(track_id, left_pan, right_pan)) {
      sonare_c_detail::throw_track_without_strip(engine_, track_id);
    }
  });
#else
  (void)track_id;
  (void)left_pan;
  (void)right_pan;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

// Sets a lane strip's surround pan from {azimuth, divergence, lfe}; absent fields take the centered
// point-source defaults, a present non-number is refused by name, and an out-of-range or non-finite
// field is refused.
void RealtimeEngineWasm::setTrackStripSurroundPan(const val& track_id_val, const val& pan_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "ref");
  if (pan_val.isNull() || pan_val.typeOf().as<std::string>() != "object") {
    throw WasmTypeError("pan must be an object");
  }
  const auto field = [&](const char* key, double fallback) {
    const val value = objectProperty(pan_val, key);
    return value.isUndefined() || value.isNull() ? fallback : numberFromVal(value, key);
  };
#if defined(SONARE_WITH_MIXING)
  const sonare::mixing::SurroundPanParams defaults;
  const double azimuth = field("azimuth", defaults.azimuth);
  const double divergence = field("divergence", defaults.divergence);
  const double lfe = field("lfe", defaults.lfe);
  withCEntryRefusal(
      "engine_set_track_strip_surround_pan",
      [&] {
        if (track_id == 0) sonare_c_detail::refuse_unknown_mixer_track();
        sonare_c_detail::check_mixer_surround_pan(azimuth, divergence, lfe);
        const sonare::mixing::SurroundPanParams params{
            static_cast<float>(azimuth), static_cast<float>(divergence), static_cast<float>(lfe)};
        if (!engine_.set_track_surround_pan(track_id, params)) {
          sonare_c_detail::throw_track_without_strip(engine_, track_id);
        }
      },
      "pan");
#else
  (void)track_id;
  (void)field;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setBusStripPan(const val& bus_id_val, const val& pan_val) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const float pan = checkedFloatFromVal(pan_val, "pan");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_bus_strip_pan", [&] {
    if (bus_id == 0) sonare_c_detail::refuse_unknown_mixer_bus();
    sonare_c_detail::check_mixer_pan(pan, "pan");
    if (!engine_.set_bus_pan(bus_id, pan)) {
      sonare_c_detail::throw_bus_not_pannable(engine_, bus_id);
    }
  });
#else
  (void)bus_id;
  (void)pan;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setBusStripPanLaw(const val& bus_id_val, const val& pan_law_val) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const int pan_law = checkedIntFromVal(pan_law_val, "panLaw");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_bus_strip_pan_law", [&] {
    if (bus_id == 0) sonare_c_detail::refuse_unknown_mixer_bus();
    sonare_c_detail::check_mixer_pan_law(pan_law);
    if (!engine_.set_bus_pan_law(bus_id, static_cast<sonare::mixing::PanLaw>(pan_law))) {
      sonare_c_detail::throw_bus_not_pannable(engine_, bus_id);
    }
  });
#else
  (void)bus_id;
  (void)pan_law;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setBusStripPanMode(const val& bus_id_val, const val& pan_mode_val) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const int pan_mode = checkedIntFromVal(pan_mode_val, "panMode");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_bus_strip_pan_mode", [&] {
    if (bus_id == 0) sonare_c_detail::refuse_unknown_mixer_bus();
    sonare_c_detail::check_mixer_pan_mode(pan_mode);
    if (!engine_.set_bus_pan_mode(bus_id, static_cast<sonare::mixing::PanMode>(pan_mode))) {
      sonare_c_detail::throw_bus_not_pannable(engine_, bus_id);
    }
  });
#else
  (void)bus_id;
  (void)pan_mode;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setBusStripDualPan(const val& bus_id_val, const val& left_pan_val,
                                            const val& right_pan_val) {
  const uint32_t bus_id = checkedUintFromVal(bus_id_val, "busId");
  const float left_pan = checkedFloatFromVal(left_pan_val, "leftPan");
  const float right_pan = checkedFloatFromVal(right_pan_val, "rightPan");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_bus_strip_dual_pan", [&] {
    if (bus_id == 0) sonare_c_detail::refuse_unknown_mixer_bus();
    sonare_c_detail::check_mixer_pan(left_pan, "left_pan");
    sonare_c_detail::check_mixer_pan(right_pan, "right_pan");
    if (!engine_.set_bus_dual_pan(bus_id, left_pan, right_pan)) {
      sonare_c_detail::throw_bus_not_pannable(engine_, bus_id);
    }
  });
#else
  (void)bus_id;
  (void)left_pan;
  (void)right_pan;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void RealtimeEngineWasm::setTrackStripChannelDelaySamples(const val& track_id_val,
                                                          const val& delay_samples_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "ref");
  const double delay_samples = numberFromVal(delay_samples_val, "delaySamples");
#if defined(SONARE_WITH_MIXING)
  withCEntryRefusal("engine_set_track_strip_channel_delay_samples", [&] {
    if (track_id == 0) sonare_c_detail::refuse_unknown_mixer_track();
    sonare::util::check::within(delay_samples, sonare_c_detail::kEngineAlignmentDelayDomain,
                                "delay_samples");
    if (!engine_.set_track_channel_delay_samples(track_id, static_cast<int>(delay_samples))) {
      sonare_c_detail::throw_track_without_strip(engine_, track_id);
    }
  });
#else
  (void)track_id;
  (void)delay_samples;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

float RealtimeEngineWasm::insertParameterConstructedValue(const val& param_id_val) {
  const uint32_t param_id = checkedUintFromVal(param_id_val, "paramId");
#if defined(SONARE_WITH_MIXING)
  return withCEntryRefusal("engine_insert_parameter_constructed_value", [&] {
    float value = 0.0f;
    sonare_c_detail::engine_automation::require_insert_construction_value(engine_, param_id,
                                                                          &value);
    return value;
  });
#else
  (void)param_id;
  throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                "mixing support is not compiled in");
#endif
}

void registerRealtimeEngineMixer(class_<RealtimeEngineWasm>& cls) {
  cls.function("setTrackLanes", &RealtimeEngineWasm::setTrackLanes)
      .function("setTrackSends", &RealtimeEngineWasm::setTrackSends)
      .function("setTrackOutputBus", &RealtimeEngineWasm::setTrackOutputBus)
      .function("setInputTrack", &RealtimeEngineWasm::setInputTrack)
      .function("clearInputTrack", &RealtimeEngineWasm::clearInputTrack)
      .function("inputTrack", &RealtimeEngineWasm::inputTrack)
      .function("insertParameterConstructedValue",
                &RealtimeEngineWasm::insertParameterConstructedValue)
      .function("setLaneSidechain", &RealtimeEngineWasm::setLaneSidechain)
      .function("canSetLaneSidechain", &RealtimeEngineWasm::canSetLaneSidechain)
      .function("canSetBusSidechain", &RealtimeEngineWasm::canSetBusSidechain)
      .function("canSetMasterSidechain", &RealtimeEngineWasm::canSetMasterSidechain)
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
      .function("resolveTrackLaneAutomationId", &RealtimeEngineWasm::resolveTrackLaneAutomationId)
      .function("resolveBusAutomationId", &RealtimeEngineWasm::resolveBusAutomationId)
      .function("resolveMasterAutomationId", &RealtimeEngineWasm::resolveMasterAutomationId)
      .function("resolveMasterInsertAutomationId",
                &RealtimeEngineWasm::resolveMasterInsertAutomationId)
      .function("resolveBusInsertAutomationId", &RealtimeEngineWasm::resolveBusInsertAutomationId)
      .function("setTrackStripPan", &RealtimeEngineWasm::setTrackStripPan)
      .function("setTrackStripPanLaw", &RealtimeEngineWasm::setTrackStripPanLaw)
      .function("setTrackStripPanMode", &RealtimeEngineWasm::setTrackStripPanMode)
      .function("setTrackStripDualPan", &RealtimeEngineWasm::setTrackStripDualPan)
      .function("setTrackStripSurroundPan", &RealtimeEngineWasm::setTrackStripSurroundPan)
      .function("setBusStripPan", &RealtimeEngineWasm::setBusStripPan)
      .function("setBusStripPanLaw", &RealtimeEngineWasm::setBusStripPanLaw)
      .function("setBusStripPanMode", &RealtimeEngineWasm::setBusStripPanMode)
      .function("setBusStripDualPan", &RealtimeEngineWasm::setBusStripDualPan)
      .function("setTrackStripChannelDelaySamples",
                &RealtimeEngineWasm::setTrackStripChannelDelaySamples);
}

#endif  // __EMSCRIPTEN__
