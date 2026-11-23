#include <sonare/sonare_c.h>

#include <array>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "c_api/engine_config_domains.h"
#include "core/channel_layout.h"
#include "engine/realtime_engine.h"
#include "sonare_c_internal.h"
#include "util/constants.h"
#include "util/parameter_checks.h"
#include "util/refusal_predicates.h"
#if defined(SONARE_WITH_MIXING)
#include <string>

#include "c_api/engine_mixer_refusal.h"
#include "c_api/eq_band_json.h"
#include "c_api/mixing_internal.h"
#include "engine/insert_automation_id.h"
#include "mastering/eq/parametric.h"
#include "mixing/api/scene.h"
#include "mixing/channel_strip_eq.h"
#include "mixing/panner.h"
#include "rt/processor_base.h"
#include "util/json.h"
#endif

using namespace sonare;
using namespace sonare_c_detail;

namespace {

SonareError refuse_track_id() {
  return run_validation([] { util::check::refuse_unknown_id("track_id", "a track", "engine"); });
}

SonareError refuse_bus_id() {
  return run_validation([] { util::check::refuse_unknown_id("bus_id", "a bus", "engine"); });
}

SonareError check_param_name(const char* param_name) {
  SONARE_C_REQUIRE_NONNULL(param_name);
  return run_validation([&] { util::check::non_empty(param_name, "param_name"); });
}

SonareError check_finite_value(float value) {
  return run_validation([&] { util::check::finite_float(value, "value"); });
}

SonareError check_band_index(int band_index) {
#if defined(SONARE_WITH_MIXING)
  return run_validation(
      [&] { util::check::integer_within(band_index, kEngineBandIndexDomain, "band_index"); });
#else
  return run_validation(
      [&] { util::check::integer_within(band_index, kEngineBandIndexFloorDomain, "band_index"); });
#endif
}

}  // namespace

#if defined(SONARE_WITH_MIXING)
namespace {

using util::check::indexed_subject;

// A send timing the C enum does not declare, named by the send's path in the call.
SonareError check_send_timing(int timing, const std::string& subject) {
  return run_validation([&] {
    util::check::one_of(
        timing, std::array<int, 2>{SONARE_SEND_TIMING_PRE_FADER, SONARE_SEND_TIMING_POST_FADER},
        subject.c_str());
  });
}

SonareError refuse_source_kind(int source_kind) {
  return run_validation([&] {
    const std::array<int, 2> kinds{SONARE_SIDECHAIN_SOURCE_TRACK, SONARE_SIDECHAIN_SOURCE_BUS};
    util::check::one_of(source_kind, kinds, "source_kind");
  });
}

SonareError refuse_with(const std::string& subject, const std::string& predicate) {
  return run_validation([&] { util::check::refuse(subject.c_str(), predicate); });
}

// A state refusal: the arguments are acceptable, the engine's configuration forbids the call.
SonareError refuse_state(const std::string& text) {
  set_last_error(SONARE_ERROR_INVALID_STATE, text.c_str());
  return SONARE_ERROR_INVALID_STATE;
}

SonareError refuse_out_of_memory(const std::string& text) {
  set_last_error(SONARE_ERROR_OUT_OF_MEMORY, text.c_str());
  return SONARE_ERROR_OUT_OF_MEMORY;
}

SonareError refuse_automation_table_full() {
  return run_validation([] { refuse_insert_automation_table_full(); });
}

bool track_has_lane(engine::RealtimeEngine& engine, uint32_t track_id) {
  uint32_t position = 0;
  return engine.track_mixer().track_lane_position(track_id, &position);
}

SonareError refuse_track_without_strip(engine::RealtimeEngine& engine, uint32_t track_id) {
  return run_validation([&] { throw_track_without_strip(engine, track_id); });
}

SonareError refuse_bus_not_pannable(engine::RealtimeEngine& engine, uint32_t bus_id) {
  if (!engine.track_mixer().configured_bus(bus_id)) return refuse_bus_id();
  return refuse_with("bus_id", "must be the id of a bus with at most two channels");
}

bool has_settable_param(const rt::ProcessorBase* processor, const char* param_name) {
  for (const rt::ParamDescriptor& descriptor : processor->parameter_descriptors()) {
    if (descriptor.key == param_name) return processor->parameter_is_realtime_safe(descriptor.id);
  }
  return false;
}

constexpr const char* kParamNamePredicate =
    "must be the name of a parameter of the insert that can be set while running";

// The insert resolved by name but the engine minted no automation id for it.
SonareError refuse_no_automation_id(unsigned int insert_index) {
  if (insert_index > engine::kInsertIndexMask) {
    return run_validation([&] {
      util::check::integer_within(insert_index, kEngineInsertIndexDomain, "insert_index");
    });
  }
  return refuse_automation_table_full();
}

// SONARE_OK when the track insert and, if given, its parameter name resolve.
SonareError diagnose_track_insert(engine::RealtimeEngine& engine, uint32_t track_id,
                                  unsigned int insert_index, const char* param_name) {
  if (!track_has_lane(engine, track_id)) return refuse_track_id();
  const rt::ProcessorBase* processor =
      engine.track_mixer().track_insert_processor(track_id, insert_index);
  if (processor == nullptr) {
    return refuse_with("insert_index", "must be less than the number of inserts on the track");
  }
  if (param_name != nullptr && !has_settable_param(processor, param_name)) {
    return refuse_with("param_name", kParamNamePredicate);
  }
  return SONARE_OK;
}

SonareError diagnose_bus_insert(engine::RealtimeEngine& engine, uint32_t bus_id,
                                unsigned int insert_index, const char* param_name) {
  if (!engine.track_mixer().configured_bus(bus_id)) return refuse_bus_id();
  const rt::ProcessorBase* processor =
      engine.track_mixer().bus_insert_processor(bus_id, insert_index);
  if (processor == nullptr) {
    return refuse_with("insert_index", "must be less than the number of inserts on the bus");
  }
  if (param_name != nullptr && !has_settable_param(processor, param_name)) {
    return refuse_with("param_name", kParamNamePredicate);
  }
  return SONARE_OK;
}

SonareError track_insert_target_error(engine::RealtimeEngine& engine, uint32_t track_id,
                                      unsigned int insert_index, const char* param_name) {
  const SonareError err = diagnose_track_insert(engine, track_id, insert_index, param_name);
  return err != SONARE_OK ? err : refuse_no_automation_id(insert_index);
}

SonareError bus_insert_target_error(engine::RealtimeEngine& engine, uint32_t bus_id,
                                    unsigned int insert_index, const char* param_name) {
  const SonareError err = diagnose_bus_insert(engine, bus_id, insert_index, param_name);
  return err != SONARE_OK ? err : refuse_no_automation_id(insert_index);
}

// After a restore failed with its target resolved, only the processor's refusal of the value is
// left. A parameter publishes no range, so the text names the parameter and not its domain.
SonareError refuse_restore_value(const char* param_name) {
  return refuse_with("value", std::string("must be accepted by the parameter \"") + param_name +
                                  "\" of that insert");
}

// SONARE_OK when master insert @p insert_index and, if given, its parameter name resolve.
SonareError diagnose_master_insert(const engine::RealtimeEngine& engine, unsigned int insert_index,
                                   const char* param_name) {
  if (insert_index >= engine.master_insert_count()) {
    return refuse_with("insert_index",
                       "must be less than the number of inserts on the master strip");
  }
  if (param_name != nullptr) {
    const int param_id = engine.master_insert_parameter_id(insert_index, param_name);
    if (param_id < 0 || !engine.master_insert_processor(insert_index)
                             ->parameter_is_realtime_safe(static_cast<unsigned int>(param_id))) {
      return refuse_with("param_name", kParamNamePredicate);
    }
  }
  return SONARE_OK;
}

SonareError master_insert_target_error(const engine::RealtimeEngine& engine,
                                       unsigned int insert_index, const char* param_name) {
  const SonareError err = diagnose_master_insert(engine, insert_index, param_name);
  return err != SONARE_OK ? err : refuse_no_automation_id(insert_index);
}

// A bypass the engine refused for a resolved insert: only the processor's own answer is left.
SonareError refuse_bypass() {
  return refuse_state("the insert refused to change its bypass state");
}

SonareError check_eq_band(const mastering::eq::EqBand& band, int band_index, double sample_rate) {
  if (const SonareError err = check_band_index(band_index); err != SONARE_OK) return err;
  return run_validation([&] {
    mixing::validate_eq(mixing::api::StripEq{true, {band}});
    (void)mastering::eq::design_eq_biquad(band, sample_rate);
  });
}

SonareError check_pan_law(int pan_law) {
  return run_validation([&] {
    const std::array<int, 4> laws{SONARE_PAN_LAW_CONST_3DB, SONARE_PAN_LAW_CONST_4P5DB,
                                  SONARE_PAN_LAW_CONST_6DB, SONARE_PAN_LAW_LINEAR_0DB};
    util::check::one_of(pan_law, laws, "pan_law");
  });
}

SonareError check_pan_mode(int pan_mode) {
  return run_validation([&] {
    const std::array<int, 3> modes{SONARE_PAN_MODE_BALANCE, SONARE_PAN_MODE_STEREO_PAN,
                                   SONARE_PAN_MODE_DUAL_PAN};
    util::check::one_of(pan_mode, modes, "pan_mode");
  });
}

SonareError check_pan(float pan, const char* subject) {
  return run_validation(
      [&] { util::check::in_range(pan, mixing::kPanMin, mixing::kPanMax, subject); });
}

template <typename Diagnose>
SonareError insert_param_result_to_error(engine::InsertParamSetResult result, Diagnose&& diagnose) {
  if (result == engine::InsertParamSetResult::kQueued) return SONARE_OK;
  if (result == engine::InsertParamSetResult::kInvalidTarget) return diagnose();
  return refuse_out_of_memory("the realtime command queue is full");
}

// Maps a refused mixer configuration call to its C status by the cause the engine retained.
SonareError mixer_edit_error(engine::RealtimeEngine& realtime, MixerEditScope scope,
                             uint32_t track_id = 0) {
  return run_validation([&] { throw_mixer_edit_refusal(realtime, scope, track_id); });
}

SonareError sidechain_refusal_to_error(engine::RealtimeEngine& realtime,
                                       engine::SidechainRefusal refusal,
                                       const SidechainWording& wording, bool target_exists,
                                       int source_kind) {
  return run_validation(
      [&] { throw_sidechain_refusal(realtime, refusal, wording, target_exists, source_kind); });
}

}  // namespace
#endif

SonareError sonare_engine_set_track_lanes(SonareEngine* engine, const SonareEngineTrackLane* lanes,
                                          size_t lane_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(lanes, lane_count);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, lanes, lane_count);
#else
  static_assert(SONARE_ENGINE_MAX_TRACK_LANES == engine::TrackMixerRuntime::kMaxTrackLanes);
  if (const SonareError err = run_validation([&] {
        util::check::integer_within(static_cast<long long>(lane_count), kEngineLaneCountDomain,
                                    "lane_count");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  std::vector<engine::TrackLaneConfig> configs;
  configs.reserve(lane_count);
  for (size_t i = 0; i < lane_count; ++i) {
    if (lanes[i].send_count > 0 && lanes[i].sends == nullptr) {
      return run_validation([&] {
        util::check::non_null_when_count(lanes[i].sends, lanes[i].send_count,
                                         indexed_subject("lanes", i, "sends").c_str(),
                                         indexed_subject("lanes", i, "send_count").c_str());
      });
    }
    ChannelLayout source_layout{};
    if (!ordinal_to_enum(lanes[i].source_channel_layout, 0, kMaxChannelLayoutValue,
                         &source_layout)) {
      return run_validation([&] {
        util::check::refuse(indexed_subject("lanes", i, "source_channel_layout").c_str(),
                            util::refusal::kDeclaredValuePredicate);
      });
    }
    if (lanes[i].source_channel_layout != SONARE_CHANNEL_LAYOUT_STEREO) {
      const std::string text = indexed_subject("lanes", i, "source_channel_layout") +
                               " other than stereo is not supported";
      set_last_error(SONARE_ERROR_NOT_SUPPORTED, text.c_str());
      return SONARE_ERROR_NOT_SUPPORTED;
    }
    engine::TrackLaneConfig lane{lanes[i].track_id};
    lane.output_bus_id = lanes[i].output_bus_id;
    lane.source_layout = source_layout;
    lane.sends.reserve(lanes[i].send_count);
    for (size_t send_index = 0; send_index < lanes[i].send_count; ++send_index) {
      const SonareEngineTrackSend& send = lanes[i].sends[send_index];
      if (const SonareError err =
              check_send_timing(send.send_timing, indexed_subject("lanes", i, "sends") + "[" +
                                                      std::to_string(send_index) + "].send_timing");
          err != SONARE_OK) {
        return err;
      }
      lane.sends.push_back({send.bus_id, send.level_db, send.enabled != 0,
                            sonare_c_mixing_detail::to_send_timing(send.send_timing)});
    }
    configs.push_back(std::move(lane));
  }
  return engine->engine.set_track_lanes(std::move(configs))
             ? SONARE_OK
             : mixer_edit_error(engine->engine, MixerEditScope::kLanes);
  SONARE_C_CATCH
#endif
}

#if defined(SONARE_WITH_MIXING)
namespace {

SonareError track_lane_edit_result_to_error(engine::RealtimeEngine& realtime,
                                            engine::TrackLaneEditResult result,
                                            MixerEditScope scope) {
  return run_validation([&] { require_track_lane_edit(realtime, result, scope); });
}

}  // namespace
#endif

SonareError sonare_engine_set_track_sends(SonareEngine* engine, uint32_t track_id,
                                          const SonareEngineTrackSend* sends, size_t send_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(sends, send_count);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, track_id, sends, send_count);
#else
  SONARE_C_TRY
  std::vector<engine::TrackLaneConfig::Send> configs;
  configs.reserve(send_count);
  for (size_t send_index = 0; send_index < send_count; ++send_index) {
    const SonareEngineTrackSend& send = sends[send_index];
    if (const SonareError err = check_send_timing(
            send.send_timing, indexed_subject("sends", send_index, "send_timing"));
        err != SONARE_OK) {
      return err;
    }
    configs.push_back({send.bus_id, send.level_db, send.enabled != 0,
                       sonare_c_mixing_detail::to_send_timing(send.send_timing)});
  }
  return track_lane_edit_result_to_error(
      engine->engine, engine->engine.set_track_sends(track_id, std::move(configs)),
      MixerEditScope::kTrackSends);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_output_bus(SonareEngine* engine, uint32_t track_id,
                                               uint32_t output_bus_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, track_id, output_bus_id);
#else
  SONARE_C_TRY
  return track_lane_edit_result_to_error(
      engine->engine, engine->engine.set_track_output_bus(track_id, output_bus_id),
      MixerEditScope::kOutputBus);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_lane_sidechain(SonareEngine* engine, uint32_t track_id,
                                             unsigned int insert_index, uint32_t source_track_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(engine);
  if (track_id == 0) return refuse_track_id();
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, insert_index, source_track_id);
#else
  if (engine->engine.set_lane_sidechain(track_id, insert_index, source_track_id)) return SONARE_OK;
  const SidechainWording wording{"track_id", "a track",
                                 "must be less than the number of inserts on the track",
                                 "source_track_id", "a track"};
  return sidechain_refusal_to_error(
      engine->engine,
      engine->engine.can_set_lane_sidechain(track_id, insert_index, source_track_id), wording,
      track_has_lane(engine->engine, track_id), SONARE_SIDECHAIN_SOURCE_TRACK);
#endif
  SONARE_C_CATCH
}

#if defined(SONARE_WITH_MIXING)
namespace {

bool sidechain_source_kind_from_c(int source_kind, engine::SidechainSourceKind* out) {
  switch (source_kind) {
    case SONARE_SIDECHAIN_SOURCE_TRACK:
      *out = engine::SidechainSourceKind::Track;
      return true;
    case SONARE_SIDECHAIN_SOURCE_BUS:
      *out = engine::SidechainSourceKind::Bus;
      return true;
    default:
      return false;
  }
}

}  // namespace
#endif

SonareError sonare_engine_set_bus_sidechain(SonareEngine* engine, uint32_t bus_id,
                                            unsigned int insert_index, int source_kind,
                                            uint32_t source_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(engine);
  if (bus_id == 0) return refuse_bus_id();
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, insert_index, source_kind, source_id);
#else
  engine::SidechainSourceKind kind;
  if (!sidechain_source_kind_from_c(source_kind, &kind)) return refuse_source_kind(source_kind);
  if (engine->engine.set_bus_sidechain(bus_id, insert_index, kind, source_id)) return SONARE_OK;
  const SidechainWording wording{
      "bus_id", "a bus", "must be less than the number of inserts on the bus", "source_id",
      source_kind == SONARE_SIDECHAIN_SOURCE_TRACK ? "a track" : "a bus"};
  return sidechain_refusal_to_error(
      engine->engine, engine->engine.can_set_bus_sidechain(bus_id, insert_index, kind, source_id),
      wording, true, source_kind);
#endif
  SONARE_C_CATCH
}

SonareError sonare_engine_set_master_sidechain(SonareEngine* engine, unsigned int insert_index,
                                               int source_kind, uint32_t source_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, insert_index, source_kind, source_id);
#else
  engine::SidechainSourceKind kind;
  if (!sidechain_source_kind_from_c(source_kind, &kind)) return refuse_source_kind(source_kind);
  if (engine->engine.set_master_sidechain(insert_index, kind, source_id)) return SONARE_OK;
  const SidechainWording wording{
      nullptr, nullptr, "must be less than the number of inserts on the master strip", "source_id",
      source_kind == SONARE_SIDECHAIN_SOURCE_TRACK ? "a track" : "a bus"};
  return sidechain_refusal_to_error(
      engine->engine, engine->engine.can_set_master_sidechain(insert_index, kind, source_id),
      wording, true, source_kind);
#endif
  SONARE_C_CATCH
}

SonareError sonare_engine_set_track_buses(SonareEngine* engine, const SonareEngineBus* buses,
                                          size_t bus_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(buses, bus_count);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, buses, bus_count);
#else
  SONARE_C_TRY
  std::vector<engine::TrackBusConfig> configs;
  configs.reserve(bus_count);
  for (size_t i = 0; i < bus_count; ++i) {
    if (buses[i].send_count > 0 && buses[i].sends == nullptr) {
      return run_validation([&] {
        util::check::non_null_when_count(buses[i].sends, buses[i].send_count,
                                         indexed_subject("buses", i, "sends").c_str(),
                                         indexed_subject("buses", i, "send_count").c_str());
      });
    }
    engine::TrackBusConfig bus;
    if (!ordinal_to_enum(buses[i].channel_layout, 0, kMaxChannelLayoutValue, &bus.layout)) {
      return run_validation([&] {
        util::check::refuse(indexed_subject("buses", i, "channel_layout").c_str(),
                            util::refusal::kDeclaredValuePredicate);
      });
    }
    bus.bus_id = buses[i].bus_id;
    bus.gain_db = buses[i].gain_db;
    bus.output_bus_id = buses[i].output_bus_id;
    bus.sends.reserve(buses[i].send_count);
    for (size_t send_index = 0; send_index < buses[i].send_count; ++send_index) {
      const SonareEngineTrackSend& send = buses[i].sends[send_index];
      if (const SonareError err =
              check_send_timing(send.send_timing, indexed_subject("buses", i, "sends") + "[" +
                                                      std::to_string(send_index) + "].send_timing");
          err != SONARE_OK) {
        return err;
      }
      bus.sends.push_back({send.bus_id, send.level_db, send.enabled != 0,
                           sonare_c_mixing_detail::to_send_timing(send.send_timing)});
    }
    configs.push_back(std::move(bus));
  }
  return engine->engine.set_track_buses(std::move(configs))
             ? SONARE_OK
             : mixer_edit_error(engine->engine, MixerEditScope::kBuses);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_bus_strip_json(SonareEngine* engine, uint32_t bus_id,
                                             const char* settings_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (bus_id == 0) return refuse_bus_id();
  SONARE_C_REQUIRE_NONNULL(settings_json);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, bus_id, settings_json);
#else
  SONARE_C_TRY
  const mixing::api::Bus settings = mixing::api::bus_settings_from_json(settings_json, "settings");
  return engine->engine.set_bus_strip(bus_id, settings)
             ? SONARE_OK
             : mixer_edit_error(engine->engine, MixerEditScope::kStrip);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_json(SonareEngine* engine, uint32_t track_id,
                                               const char* settings_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (track_id == 0) return refuse_track_id();
  SONARE_C_REQUIRE_NONNULL(settings_json);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, track_id, settings_json);
#else
  SONARE_C_TRY
  const mixing::api::Strip settings =
      mixing::api::strip_settings_from_json(settings_json, "settings");
  return engine->engine.set_track_strip(track_id, settings)
             ? SONARE_OK
             : mixer_edit_error(engine->engine, MixerEditScope::kStrip);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_eq_band_json(SonareEngine* engine, uint32_t track_id,
                                                       int band_index, const char* band_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (track_id == 0) return refuse_track_id();
  if (const SonareError err = check_band_index(band_index); err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(band_json);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, track_id, band_index, band_json);
#else
  SONARE_C_TRY
  const mastering::eq::EqBand band = sonare::c_api::parse_eq_band_json(band_json);
  if (const SonareError err = check_eq_band(band, band_index, engine->engine.sample_rate());
      err != SONARE_OK) {
    return err;
  }
  return engine->engine.set_track_eq_band(track_id, static_cast<size_t>(band_index), band)
             ? SONARE_OK
             : mixer_edit_error(engine->engine, MixerEditScope::kEqBand, track_id);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_insert_bypassed(SonareEngine* engine, uint32_t track_id,
                                                          unsigned int insert_index, int bypassed,
                                                          int reset_on_bypass) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (track_id == 0) return refuse_track_id();
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, track_id, insert_index, bypassed,
                                reset_on_bypass);
#else
  SONARE_C_TRY
  if (engine->engine.set_track_insert_bypassed(track_id, insert_index, bypassed != 0,
                                               reset_on_bypass != 0)) {
    return SONARE_OK;
  }
  if (const SonareError err =
          diagnose_track_insert(engine->engine, track_id, insert_index, nullptr);
      err != SONARE_OK) {
    return err;
  }
  return refuse_bypass();
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_master_strip_json(SonareEngine* engine, const char* settings_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(settings_json);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, settings_json);
#else
  SONARE_C_TRY
  const mixing::api::Strip settings =
      mixing::api::strip_settings_from_json(settings_json, "settings");
  return engine->engine.set_master_strip(settings)
             ? SONARE_OK
             : mixer_edit_error(engine->engine, MixerEditScope::kStrip);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_master_strip_eq_band_json(SonareEngine* engine, int band_index,
                                                        const char* band_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err = check_band_index(band_index); err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(band_json);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, band_index, band_json);
#else
  SONARE_C_TRY
  const mastering::eq::EqBand band = sonare::c_api::parse_eq_band_json(band_json);
  if (const SonareError err = check_eq_band(band, band_index, engine->engine.sample_rate());
      err != SONARE_OK) {
    return err;
  }
  return engine->engine.set_master_eq_band(static_cast<size_t>(band_index), band)
             ? SONARE_OK
             : mixer_edit_error(engine->engine, MixerEditScope::kEqBand);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_master_strip_insert_bypassed(SonareEngine* engine,
                                                           unsigned int insert_index, int bypassed,
                                                           int reset_on_bypass) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, insert_index, bypassed, reset_on_bypass);
#else
  SONARE_C_TRY
  if (engine->engine.set_master_insert_bypassed(insert_index, bypassed != 0,
                                                reset_on_bypass != 0)) {
    return SONARE_OK;
  }
  if (const SonareError err = diagnose_master_insert(engine->engine, insert_index, nullptr);
      err != SONARE_OK) {
    return err;
  }
  return refuse_bypass();
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_bus_strip_eq_band_json(SonareEngine* engine, uint32_t bus_id,
                                                     int band_index, const char* band_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (bus_id == 0) return refuse_bus_id();
  if (const SonareError err = check_band_index(band_index); err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(band_json);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, bus_id, band_index, band_json);
#else
  SONARE_C_TRY
  const mastering::eq::EqBand band = sonare::c_api::parse_eq_band_json(band_json);
  if (const SonareError err = check_eq_band(band, band_index, engine->engine.sample_rate());
      err != SONARE_OK) {
    return err;
  }
  if (engine->engine.set_bus_eq_band(bus_id, static_cast<size_t>(band_index), band)) {
    return SONARE_OK;
  }
  return mixer_edit_error(engine->engine, MixerEditScope::kEqBand);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_bus_strip_insert_bypassed(SonareEngine* engine, uint32_t bus_id,
                                                        unsigned int insert_index, int bypassed,
                                                        int reset_on_bypass) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (bus_id == 0) return refuse_bus_id();
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, bus_id, insert_index, bypassed, reset_on_bypass);
#else
  SONARE_C_TRY
  if (engine->engine.set_bus_insert_bypassed(bus_id, insert_index, bypassed != 0,
                                             reset_on_bypass != 0)) {
    return SONARE_OK;
  }
  if (const SonareError err = diagnose_bus_insert(engine->engine, bus_id, insert_index, nullptr);
      err != SONARE_OK) {
    return err;
  }
  return refuse_bypass();
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_insert_param_by_name(SonareEngine* engine,
                                                               uint32_t track_id,
                                                               unsigned int insert_index,
                                                               const char* param_name,
                                                               float value) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (track_id == 0) return refuse_track_id();
  if (const SonareError err = check_param_name(param_name); err != SONARE_OK) return err;
  if (const SonareError err = check_finite_value(value); err != SONARE_OK) return err;
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, track_id, insert_index, value);
#else
  SONARE_C_TRY
  return insert_param_result_to_error(
      engine->engine.set_track_insert_param_detailed(track_id, insert_index, param_name, value),
      [&] {
        return track_insert_target_error(engine->engine, track_id, insert_index, param_name);
      });
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_master_strip_insert_param_by_name(SonareEngine* engine,
                                                                unsigned int insert_index,
                                                                const char* param_name,
                                                                float value) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err = check_param_name(param_name); err != SONARE_OK) return err;
  if (const SonareError err = check_finite_value(value); err != SONARE_OK) return err;
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, insert_index, value);
#else
  SONARE_C_TRY
  return insert_param_result_to_error(
      engine->engine.set_master_insert_param_detailed(insert_index, param_name, value),
      [&] { return master_insert_target_error(engine->engine, insert_index, param_name); });
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_bus_strip_insert_param_by_name(SonareEngine* engine, uint32_t bus_id,
                                                             unsigned int insert_index,
                                                             const char* param_name, float value) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (bus_id == 0) return refuse_bus_id();
  if (const SonareError err = check_param_name(param_name); err != SONARE_OK) return err;
  if (const SonareError err = check_finite_value(value); err != SONARE_OK) return err;
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, bus_id, insert_index, value);
#else
  SONARE_C_TRY
  return insert_param_result_to_error(
      engine->engine.set_bus_insert_param_detailed(bus_id, insert_index, param_name, value),
      [&] { return bus_insert_target_error(engine->engine, bus_id, insert_index, param_name); });
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_apply_track_strip_insert_param_by_name_now(
    SonareEngine* engine, uint32_t track_id, unsigned int insert_index, const char* param_name,
    float value, int* out_applied) {
  SONARE_C_API_ENTRY;
  if (out_applied) *out_applied = 0;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (track_id == 0) return refuse_track_id();
  if (const SonareError err = check_param_name(param_name); err != SONARE_OK) return err;
  if (const SonareError err = check_finite_value(value); err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out_applied);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, insert_index);
#else
  SONARE_C_TRY
  *out_applied =
      engine->engine.apply_track_insert_param_by_name_now(track_id, insert_index, param_name, value)
          ? 1
          : 0;
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_apply_master_strip_insert_param_by_name_now(SonareEngine* engine,
                                                                      unsigned int insert_index,
                                                                      const char* param_name,
                                                                      float value,
                                                                      int* out_applied) {
  SONARE_C_API_ENTRY;
  if (out_applied) *out_applied = 0;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err = check_param_name(param_name); err != SONARE_OK) return err;
  if (const SonareError err = check_finite_value(value); err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out_applied);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, insert_index);
#else
  SONARE_C_TRY
  *out_applied =
      engine->engine.apply_master_insert_param_by_name_now(insert_index, param_name, value) ? 1 : 0;
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_apply_bus_strip_insert_param_by_name_now(SonareEngine* engine,
                                                                   uint32_t bus_id,
                                                                   unsigned int insert_index,
                                                                   const char* param_name,
                                                                   float value, int* out_applied) {
  SONARE_C_API_ENTRY;
  if (out_applied) *out_applied = 0;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (bus_id == 0) return refuse_bus_id();
  if (const SonareError err = check_param_name(param_name); err != SONARE_OK) return err;
  if (const SonareError err = check_finite_value(value); err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out_applied);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, insert_index);
#else
  SONARE_C_TRY
  *out_applied =
      engine->engine.apply_bus_insert_param_by_name_now(bus_id, insert_index, param_name, value)
          ? 1
          : 0;
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_restore_track_strip_insert_param_by_name(SonareEngine* engine,
                                                                   uint32_t track_id,
                                                                   unsigned int insert_index,
                                                                   const char* param_name,
                                                                   float value) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (track_id == 0) return refuse_track_id();
  if (const SonareError err = check_param_name(param_name); err != SONARE_OK) return err;
  if (const SonareError err = check_finite_value(value); err != SONARE_OK) return err;
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, insert_index);
#else
  SONARE_C_TRY
  if (engine->engine.restore_track_insert_param_by_name(track_id, insert_index, param_name,
                                                        value)) {
    return SONARE_OK;
  }
  if (const SonareError err =
          diagnose_track_insert(engine->engine, track_id, insert_index, param_name);
      err != SONARE_OK) {
    return err;
  }
  return refuse_restore_value(param_name);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_restore_master_strip_insert_param_by_name(SonareEngine* engine,
                                                                    unsigned int insert_index,
                                                                    const char* param_name,
                                                                    float value) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err = check_param_name(param_name); err != SONARE_OK) return err;
  if (const SonareError err = check_finite_value(value); err != SONARE_OK) return err;
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, insert_index);
#else
  SONARE_C_TRY
  if (engine->engine.restore_master_insert_param_by_name(insert_index, param_name, value)) {
    return SONARE_OK;
  }
  if (const SonareError err = diagnose_master_insert(engine->engine, insert_index, param_name);
      err != SONARE_OK) {
    return err;
  }
  return refuse_restore_value(param_name);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_restore_bus_strip_insert_param_by_name(SonareEngine* engine,
                                                                 uint32_t bus_id,
                                                                 unsigned int insert_index,
                                                                 const char* param_name,
                                                                 float value) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (bus_id == 0) return refuse_bus_id();
  if (const SonareError err = check_param_name(param_name); err != SONARE_OK) return err;
  if (const SonareError err = check_finite_value(value); err != SONARE_OK) return err;
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, insert_index);
#else
  SONARE_C_TRY
  if (engine->engine.restore_bus_insert_param_by_name(bus_id, insert_index, param_name, value)) {
    return SONARE_OK;
  }
  if (const SonareError err = diagnose_bus_insert(engine->engine, bus_id, insert_index, param_name);
      err != SONARE_OK) {
    return err;
  }
  return refuse_restore_value(param_name);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_clear_track_insert_parameter_bases(SonareEngine* engine,
                                                             uint32_t track_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (track_id == 0) return refuse_track_id();
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing);
#else
  return engine->engine.clear_track_insert_parameter_bases(track_id) ? SONARE_OK
                                                                     : refuse_track_id();
#endif
}

SonareError sonare_engine_clear_master_insert_parameter_bases(SonareEngine* engine) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing);
#else
  engine->engine.clear_master_insert_parameter_bases();
  return SONARE_OK;
#endif
}

SonareError sonare_engine_clear_bus_insert_parameter_bases(SonareEngine* engine, uint32_t bus_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (bus_id == 0) return refuse_bus_id();
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing);
#else
  return engine->engine.clear_bus_insert_parameter_bases(bus_id) ? SONARE_OK : refuse_bus_id();
#endif
}

SonareError sonare_engine_insert_parameter_constructed_value(SonareEngine* engine,
                                                             uint32_t param_id, float* out_value) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_value);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, param_id);
#else
  if (engine->engine.insert_parameter_constructed_value(param_id, out_value)) return SONARE_OK;
  if (!engine::is_insert_param_id(param_id)) {
    return refuse_with("param_id", "must be an insert parameter automation id");
  }
  return run_validation([&] {
    util::check::refuse_unknown_id("param_id", "a live insert parameter with a construction value",
                                   "engine");
  });
#endif
}

SonareError sonare_engine_resolve_track_insert_automation_id(SonareEngine* engine,
                                                             uint32_t track_id,
                                                             unsigned int insert_index,
                                                             const char* param_name,
                                                             uint32_t* out_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_id);
  SONARE_C_REQUIRE_NONNULL(engine);
  if (track_id == 0) return refuse_track_id();
  if (const SonareError err = check_param_name(param_name); err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out_id);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, track_id, insert_index);
#else
  SONARE_C_TRY
  const int64_t id =
      engine->engine.resolve_track_insert_automation_id(track_id, insert_index, param_name);
  if (id < 0) return track_insert_target_error(engine->engine, track_id, insert_index, param_name);
  *out_id = static_cast<uint32_t>(id);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_resolve_master_insert_automation_id(SonareEngine* engine,
                                                              unsigned int insert_index,
                                                              const char* param_name,
                                                              uint32_t* out_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_id);
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err = check_param_name(param_name); err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out_id);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, insert_index);
#else
  SONARE_C_TRY
  const int64_t id = engine->engine.resolve_master_insert_automation_id(insert_index, param_name);
  if (id < 0) return master_insert_target_error(engine->engine, insert_index, param_name);
  *out_id = static_cast<uint32_t>(id);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_resolve_master_automation_id(SonareEngine* engine, const char* param_name,
                                                       uint32_t* out_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_id);
  SONARE_C_REQUIRE_NONNULL(engine);
  if (const SonareError err = check_param_name(param_name); err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out_id);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing);
#else
  SONARE_C_TRY
  const int64_t id = engine->engine.resolve_master_automation_id(param_name);
  if (id < 0) {
    return refuse_with("param_name",
                       "must be the name of a master strip parameter: faderDb, pan or width");
  }
  *out_id = static_cast<uint32_t>(id);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_resolve_bus_insert_automation_id(SonareEngine* engine, uint32_t bus_id,
                                                           unsigned int insert_index,
                                                           const char* param_name,
                                                           uint32_t* out_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_id);
  SONARE_C_REQUIRE_NONNULL(engine);
  if (bus_id == 0) return refuse_bus_id();
  if (const SonareError err = check_param_name(param_name); err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out_id);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, bus_id, insert_index);
#else
  SONARE_C_TRY
  const int64_t id =
      engine->engine.resolve_bus_insert_automation_id(bus_id, insert_index, param_name);
  if (id < 0) return bus_insert_target_error(engine->engine, bus_id, insert_index, param_name);
  *out_id = static_cast<uint32_t>(id);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_resolve_track_lane_automation_id(SonareEngine* engine, uint32_t track_id,
                                                           const char* param_name,
                                                           uint32_t* out_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_id);
  SONARE_C_REQUIRE_NONNULL(engine);
  if (track_id == 0) return refuse_track_id();
  if (const SonareError err = check_param_name(param_name); err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out_id);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, track_id);
#else
  SONARE_C_TRY
  const int64_t id = engine->engine.resolve_track_lane_automation_id(track_id, param_name);
  if (id < 0) {
    const std::string key = param_name;
    if (key != "faderDb" && key != "pan") {
      return refuse_with("param_name",
                         "must be the name of a track strip parameter: faderDb or pan");
    }
    if (!track_has_lane(engine->engine, track_id)) return refuse_track_id();
    return refuse_no_automation_id(0);
  }
  *out_id = static_cast<uint32_t>(id);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_resolve_bus_automation_id(SonareEngine* engine, uint32_t bus_id,
                                                    const char* param_name, uint32_t* out_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_id);
  SONARE_C_REQUIRE_NONNULL(engine);
  if (bus_id == 0) return refuse_bus_id();
  if (const SonareError err = check_param_name(param_name); err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL(out_id);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, bus_id);
#else
  SONARE_C_TRY
  const int64_t id = engine->engine.resolve_bus_automation_id(bus_id, param_name);
  if (id < 0) {
    if (std::string(param_name) != "faderDb") {
      return refuse_with("param_name", "must be the name of a bus strip parameter: faderDb");
    }
    if (!engine->engine.track_mixer().configured_bus(bus_id)) return refuse_bus_id();
    return refuse_no_automation_id(0);
  }
  *out_id = static_cast<uint32_t>(id);
  return SONARE_OK;
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_pan(SonareEngine* engine, uint32_t track_id, float pan) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (track_id == 0) return refuse_track_id();
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, track_id, pan);
#else
  SONARE_C_TRY
  if (const SonareError err = check_pan(pan, "pan"); err != SONARE_OK) return err;
  return engine->engine.set_track_pan(track_id, pan)
             ? SONARE_OK
             : refuse_track_without_strip(engine->engine, track_id);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_pan_law(SonareEngine* engine, uint32_t track_id,
                                                  int pan_law) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (track_id == 0) return refuse_track_id();
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, track_id, pan_law);
#else
  SONARE_C_TRY
  if (const SonareError err = check_pan_law(pan_law); err != SONARE_OK) return err;
  return engine->engine.set_track_pan_law(track_id, sonare_c_mixing_detail::to_pan_law(pan_law))
             ? SONARE_OK
             : refuse_track_without_strip(engine->engine, track_id);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_pan_mode(SonareEngine* engine, uint32_t track_id,
                                                   int pan_mode) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (track_id == 0) return refuse_track_id();
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, track_id, pan_mode);
#else
  SONARE_C_TRY
  if (const SonareError err = check_pan_mode(pan_mode); err != SONARE_OK) return err;
  return engine->engine.set_track_pan_mode(track_id, sonare_c_mixing_detail::to_pan_mode(pan_mode))
             ? SONARE_OK
             : refuse_track_without_strip(engine->engine, track_id);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_dual_pan(SonareEngine* engine, uint32_t track_id,
                                                   float left_pan, float right_pan) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (track_id == 0) return refuse_track_id();
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, track_id, left_pan, right_pan);
#else
  SONARE_C_TRY
  if (const SonareError err = check_pan(left_pan, "left_pan"); err != SONARE_OK) return err;
  if (const SonareError err = check_pan(right_pan, "right_pan"); err != SONARE_OK) return err;
  return engine->engine.set_track_dual_pan(track_id, left_pan, right_pan)
             ? SONARE_OK
             : refuse_track_without_strip(engine->engine, track_id);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_surround_pan(SonareEngine* engine, uint32_t track_id,
                                                       const SonareSurroundPan* pan) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (track_id == 0) return refuse_track_id();
  SONARE_C_REQUIRE_NONNULL(pan);
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, track_id);
#else
  if (const SonareError err = run_validation([&] {
        util::check::in_range(pan->azimuth, -constants::kDegreesPerHalfTurn,
                              constants::kDegreesPerHalfTurn, "pan.azimuth");
        util::check::in_range(pan->divergence, 0.0, 1.0, "pan.divergence");
        util::check::in_range(pan->lfe, 0.0, 1.0, "pan.lfe");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  const sonare::mixing::SurroundPanParams params{pan->azimuth, pan->divergence, pan->lfe};
  return engine->engine.set_track_surround_pan(track_id, params)
             ? SONARE_OK
             : refuse_track_without_strip(engine->engine, track_id);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_bus_strip_pan(SonareEngine* engine, uint32_t bus_id, float pan) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (bus_id == 0) return refuse_bus_id();
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, bus_id, pan);
#else
  SONARE_C_TRY
  if (const SonareError err = check_pan(pan, "pan"); err != SONARE_OK) return err;
  return engine->engine.set_bus_pan(bus_id, pan) ? SONARE_OK
                                                 : refuse_bus_not_pannable(engine->engine, bus_id);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_bus_strip_pan_law(SonareEngine* engine, uint32_t bus_id,
                                                int pan_law) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (bus_id == 0) return refuse_bus_id();
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, bus_id, pan_law);
#else
  SONARE_C_TRY
  if (const SonareError err = check_pan_law(pan_law); err != SONARE_OK) return err;
  return engine->engine.set_bus_pan_law(bus_id, sonare_c_mixing_detail::to_pan_law(pan_law))
             ? SONARE_OK
             : refuse_bus_not_pannable(engine->engine, bus_id);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_bus_strip_pan_mode(SonareEngine* engine, uint32_t bus_id,
                                                 int pan_mode) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (bus_id == 0) return refuse_bus_id();
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, bus_id, pan_mode);
#else
  SONARE_C_TRY
  if (const SonareError err = check_pan_mode(pan_mode); err != SONARE_OK) return err;
  return engine->engine.set_bus_pan_mode(bus_id, sonare_c_mixing_detail::to_pan_mode(pan_mode))
             ? SONARE_OK
             : refuse_bus_not_pannable(engine->engine, bus_id);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_bus_strip_dual_pan(SonareEngine* engine, uint32_t bus_id,
                                                 float left_pan, float right_pan) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (bus_id == 0) return refuse_bus_id();
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, bus_id, left_pan, right_pan);
#else
  SONARE_C_TRY
  if (const SonareError err = check_pan(left_pan, "left_pan"); err != SONARE_OK) return err;
  if (const SonareError err = check_pan(right_pan, "right_pan"); err != SONARE_OK) return err;
  return engine->engine.set_bus_dual_pan(bus_id, left_pan, right_pan)
             ? SONARE_OK
             : refuse_bus_not_pannable(engine->engine, bus_id);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_channel_delay_samples(SonareEngine* engine,
                                                                uint32_t track_id,
                                                                int delay_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (track_id == 0) return refuse_track_id();
  if (const SonareError err = run_validation([&] {
        util::check::integer_within(delay_samples, kEngineAlignmentDelayDomain, "delay_samples");
      });
      err != SONARE_OK) {
    return err;
  }
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, track_id, delay_samples);
#else
  SONARE_C_TRY
  return engine->engine.set_track_channel_delay_samples(track_id, delay_samples)
             ? SONARE_OK
             : refuse_track_without_strip(engine->engine, track_id);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_can_set_lane_sidechain(SonareEngine* engine, uint32_t track_id,
                                                 unsigned int insert_index,
                                                 uint32_t source_track_id, int* out_refusal) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_refusal);
  // Defined on every exit path; a feature-disabled build reports no valid target.
  *out_refusal = SONARE_SIDECHAIN_REFUSAL_INVALID_TARGET;
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, track_id, insert_index, source_track_id);
#else
  *out_refusal = static_cast<int>(
      engine->engine.can_set_lane_sidechain(track_id, insert_index, source_track_id));
  return SONARE_OK;
#endif
}

#if defined(SONARE_WITH_MIXING)
namespace {

// An unknown kind maps to a value past the enum so the core orders its
// kInvalidSourceKind answer among its other checks, as the setter's refusal does.
engine::SidechainSourceKind sidechain_source_kind_for_query(int source_kind) {
  engine::SidechainSourceKind kind;
  if (sidechain_source_kind_from_c(source_kind, &kind)) return kind;
  return static_cast<engine::SidechainSourceKind>(UINT8_MAX);
}

}  // namespace
#endif

SonareError sonare_engine_can_set_bus_sidechain(SonareEngine* engine, uint32_t bus_id,
                                                unsigned int insert_index, int source_kind,
                                                uint32_t source_id, int* out_refusal) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_refusal);
  // Defined on every exit path; a feature-disabled build reports no valid target.
  *out_refusal = SONARE_SIDECHAIN_REFUSAL_INVALID_TARGET;
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, bus_id, insert_index, source_kind, source_id);
#else
  *out_refusal = static_cast<int>(engine->engine.can_set_bus_sidechain(
      bus_id, insert_index, sidechain_source_kind_for_query(source_kind), source_id));
  return SONARE_OK;
#endif
}

SonareError sonare_engine_can_set_master_sidechain(SonareEngine* engine, unsigned int insert_index,
                                                   int source_kind, uint32_t source_id,
                                                   int* out_refusal) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(out_refusal);
  // Defined on every exit path; a feature-disabled build reports no valid target.
  *out_refusal = SONARE_SIDECHAIN_REFUSAL_INVALID_TARGET;
#if !defined(SONARE_WITH_MIXING)
  return refuse_not_compiled_in(Feature::kMixing, insert_index, source_kind, source_id);
#else
  *out_refusal = static_cast<int>(engine->engine.can_set_master_sidechain(
      insert_index, sidechain_source_kind_for_query(source_kind), source_id));
  return SONARE_OK;
#endif
}
