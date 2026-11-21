#include <sonare/sonare_c.h>

#include <array>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "c_api/engine_config_domains.h"
#include "core/channel_layout.h"
#include "engine/realtime_engine.h"
#include "mixing/alignment_delay.h"
#include "sonare_c_internal.h"
#include "util/constants.h"
#include "util/parameter_checks.h"
#include "util/refusal_predicates.h"
#if defined(SONARE_WITH_MIXING)
#include <string>

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

/// Parses a mixing scene from a JSON string, mapping a syntactic parse failure
/// to SONARE_ERROR_INVALID_FORMAT. Shared by the strip-setter entry points that
/// accept a scene JSON payload. A semantic failure inside a well-formed document
/// is left to SONARE_C_CATCH; the scene reader codes every refusal of the document,
/// an unknown key included, as InvalidFormat.
SonareError parse_scene_json(const char* json, mixing::api::Scene* out) {
  try {
    *out = mixing::api::scene_from_json(json);
  } catch (const util::json::JsonError& e) {
    set_last_error(SONARE_ERROR_INVALID_FORMAT, e.what());
    return SONARE_ERROR_INVALID_FORMAT;
  }
  return SONARE_OK;
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
  return refuse_state("the insert automation table is full (" +
                      std::to_string(engine::InsertAutomationTargetTable::kCapacity) + " entries)");
}

SonareError refuse_alignment_ceiling(const char* what) {
  return refuse_state(std::string(what) + " is past the alignment ceiling of " +
                      std::to_string(mixing::kMaxAlignmentDelaySamples) + " samples");
}

bool track_has_lane(engine::RealtimeEngine& engine, uint32_t track_id) {
  uint32_t position = 0;
  return engine.track_mixer().track_lane_position(track_id, &position);
}

SonareError refuse_track_without_strip(engine::RealtimeEngine& engine, uint32_t track_id) {
  if (!track_has_lane(engine, track_id)) return refuse_track_id();
  return refuse_with("track_id", "must be the id of a track that has a channel strip");
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

constexpr const char* kNoMasterStripText =
    "no master strip is configured; set one with sonare_engine_set_master_strip_json";

// The call a refused mixer edit came from; it decides how its causes name the arguments.
enum class EditScope : uint8_t {
  kLanes,       // sonare_engine_set_track_lanes
  kBuses,       // sonare_engine_set_track_buses
  kTrackSends,  // sonare_engine_set_track_sends
  kOutputBus,   // sonare_engine_set_track_output_bus
  kStrip,       // the strip setters that take scene_json
  kEqBand,      // the three EQ band setters
};

struct EditContext {
  EditScope scope;
  uint32_t track_id = 0;
};

// "<list>[i].<member>" for an entry of the call's lane or bus list, or the bare member for a call
// that addresses one lane.
std::string fault_owner_subject(const EditContext& context, const engine::MixerEditFault& fault,
                                const char* member) {
  if (context.scope == EditScope::kLanes) {
    return util::check::indexed_subject("lanes", fault.index, member);
  }
  if (context.scope == EditScope::kBuses) {
    return util::check::indexed_subject("buses", fault.index, member);
  }
  return member;
}

// The subject of a send of the faulted entry, or of the send list itself when no send is named.
std::string fault_send_subject(const EditContext& context, const engine::MixerEditFault& fault,
                               const char* member, const char* list_member) {
  if (fault.send_index == engine::kNoFaultIndex)
    return fault_owner_subject(context, fault, list_member);
  const std::string owner = context.scope == EditScope::kLanes || context.scope == EditScope::kBuses
                                ? fault_owner_subject(context, fault, "sends")
                                : std::string("sends");
  return owner + "[" + std::to_string(fault.send_index) + "]." + member;
}

SonareError refuse_id_zero(const std::string& subject) {
  return run_validation([&] {
    util::check::integer_in_range(0, 1, std::numeric_limits<uint32_t>::max(), subject.c_str());
  });
}

SonareError refuse_unknown_bus_in(const std::string& subject, const char* owner) {
  return run_validation([&] { util::check::refuse_unknown_id(subject.c_str(), "a bus", owner); });
}

SonareError refuse_bus_route(const EditContext& context, const engine::MixerEditFault& fault,
                             engine::MixerEditCause cause) {
  const bool from_send = fault.send_index != engine::kNoFaultIndex;
  const std::string route = from_send ? fault_send_subject(context, fault, "bus_id", "sends")
                                      : fault_owner_subject(context, fault, "output_bus_id");
  if (cause == engine::MixerEditCause::kBusRouteUnknown) {
    return refuse_with(route, "must be the id of a bus in buses");
  }
  return run_validation([&] {
    util::check::refuse_relation({route, fault_owner_subject(context, fault, "bus_id")},
                                 "{0} must not equal {1}");
  });
}

SonareError eq_band_error(engine::MixerEditCause cause) {
  if (cause == engine::MixerEditCause::kEqBandIndexInvalid) {
    return refuse_with("band_index",
                       util::refusal::integer_range_predicate(
                           0, static_cast<double>(mastering::eq::ParametricEq::kMaxBands) - 1));
  }
  return refuse_with("band_json",
                     "must describe a band the EQ stage accepts at the engine's sample rate");
}

// Maps a refused mixer configuration call to its C status by the cause the engine retained.
SonareError mixer_edit_error(engine::RealtimeEngine& realtime, const EditContext& context) {
  using Cause = engine::MixerEditCause;
  const engine::MixerEditFault fault = realtime.last_mixer_edit_fault();
  const auto count_limit = [](size_t limit) {
    return util::refusal::integer_range_predicate(0, static_cast<double>(limit));
  };
  switch (fault.cause) {
    case Cause::kTooManyLanes:
      return refuse_with("lane_count", count_limit(engine::TrackMixerRuntime::kMaxTrackLanes));
    case Cause::kLaneTrackIdZero:
      return refuse_id_zero(fault_owner_subject(context, fault, "track_id"));
    case Cause::kLaneLayoutNotStereo: {
      const std::string text = fault_owner_subject(context, fault, "source_channel_layout") +
                               " other than stereo is not supported";
      set_last_error(SONARE_ERROR_NOT_SUPPORTED, text.c_str());
      return SONARE_ERROR_NOT_SUPPORTED;
    }
    case Cause::kLaneOutputBusUnknown:
      return refuse_unknown_bus_in(fault_owner_subject(context, fault, "output_bus_id"), "engine");
    case Cause::kLaneTrackIdDuplicate:
      return refuse_with(fault_owner_subject(context, fault, "track_id"),
                         "must be unique among the lanes");
    case Cause::kTooManySends:
      return refuse_with(fault_send_subject(context, fault, "bus_id", "send_count"),
                         count_limit(mixing::ChannelStrip::kMaxSends));
    case Cause::kSendBusIdZero:
      return refuse_id_zero(fault_send_subject(context, fault, "bus_id", "sends"));
    case Cause::kSendLevelOutOfRange:
      return refuse_with(fault_send_subject(context, fault, "level_db", "sends"),
                         util::refusal::range_predicate(constants::kFloorDb,
                                                        engine::TrackMixerRuntime::kMaxGainDb));
    case Cause::kSendBusUnknown:
      return refuse_unknown_bus_in(fault_send_subject(context, fault, "bus_id", "sends"), "engine");
    case Cause::kSendBusDuplicate:
      return refuse_with(fault_send_subject(context, fault, "bus_id", "sends"),
                         "must not repeat the bus_id of an earlier send");
    case Cause::kTooManyBuses:
      return refuse_with("bus_count", count_limit(engine::TrackMixerRuntime::kMaxBusLanes));
    case Cause::kBusIdZero:
      return refuse_id_zero(fault_owner_subject(context, fault, "bus_id"));
    case Cause::kBusGainOutOfRange:
      return refuse_with(fault_owner_subject(context, fault, "gain_db"),
                         util::refusal::range_predicate(constants::kFloorDb,
                                                        engine::TrackMixerRuntime::kMaxGainDb));
    case Cause::kBusIdDuplicate:
      return refuse_with(fault_owner_subject(context, fault, "bus_id"),
                         "must be unique among the buses");
    case Cause::kBusRouteUnknown:
    case Cause::kBusRouteToSelf:
      return refuse_bus_route(context, fault, fault.cause);
    case Cause::kBusRouteCycle:
      return refuse_with("buses",
                         "must not route a bus back into itself through outputs, sends "
                         "or sidechain keys");
    case Cause::kTrackIdZero:
      return refuse_track_id();
    case Cause::kTrackHasNoStrip:
      return refuse_track_without_strip(realtime, context.track_id);
    case Cause::kBusUnknown:
      return refuse_bus_id();
    case Cause::kStripEqInvalid:
      return refuse_with("scene_json",
                         "must describe a strip EQ whose bands are valid at the "
                         "engine's sample rate");
    case Cause::kStripInsertsRefused:
      return refuse_with("scene_json", "must describe inserts the mixer can create");
    case Cause::kBusPanNotFinite:
      return refuse_with("scene_json", "must describe a bus whose pan values are finite");
    case Cause::kBusImageOnWideLayout:
      return refuse_with("scene_json",
                         "must leave the pan and width of a bus wider than stereo "
                         "at their defaults");
    case Cause::kEqBandIndexInvalid:
    case Cause::kEqBandRefused:
      return eq_band_error(fault.cause);
    case Cause::kInsertAutomationTableFull:
      return refuse_automation_table_full();
    case Cause::kOwnedStripTableFull:
      return refuse_state("the owned strip table is full (" +
                          std::to_string(engine::TrackMixerRuntime::kMaxTrackLanes) + " strips)");
    case Cause::kLaneTableFull:
      return refuse_state("the lane table is full (" +
                          std::to_string(engine::TrackMixerRuntime::kMaxTrackLanes) + " lanes)");
    case Cause::kRetainedDelayPastCeiling:
      return refuse_alignment_ceiling("the clip delay the mixer retains, which forbids any lane,");
    case Cause::kLatencyPastAlignmentCeiling:
      return refuse_alignment_ceiling("the latency the mixer has to align");
    case Cause::kBusImageBlocksWiderLayout:
      return refuse_state(util::check::indexed_subject("buses", fault.index, "channel_layout") +
                          " cannot be wider than stereo while the bus keeps a pan or width");
    case Cause::kLaneRoutingBlocksBusRemoval:
      return refuse_state("a lane output or send still routes to a bus the new bus list drops");
    case Cause::kNoMasterStrip:
      return refuse_state(kNoMasterStripText);
    case Cause::kStagingFailed:
      return refuse_state("the mixer failed while staging the change; nothing was applied");
    case Cause::kOutOfMemory:
      return refuse_out_of_memory(
          "out of memory while staging the mixer change; nothing was "
          "applied");
    case Cause::kNone:
      break;
  }
  return refuse_state("the mixer refused the change and retained no cause for it");
}

// The master EQ, insert and sidechain setters address the engine-owned master
// strip; with none configured the refusal is the engine's state, not the call's.
bool require_master_strip(const engine::RealtimeEngine& engine) {
  if (engine.has_master_strip()) return true;
  set_last_error(SONARE_ERROR_INVALID_STATE, kNoMasterStripText);
  return false;
}

struct SidechainWording {
  const char* target_subject;  // nullptr for the master strip, which has no id
  const char* target_thing;
  const char* insert_predicate;
  const char* source_subject;
  const char* source_thing;
};

// Table-full and alignment-ceiling refusals come from bindings and latency earlier
// calls left; every other verdict refuses the call's own target or source. A setter refused
// while its query answers kNone failed re-planning the delays, and the mixer kept why.
SonareError sidechain_refusal_to_error(engine::RealtimeEngine& realtime,
                                       engine::SidechainRefusal refusal,
                                       const SidechainWording& wording, bool target_exists,
                                       int source_kind) {
  const auto refuse_target = [&] {
    if (wording.target_subject == nullptr) return refuse_state(kNoMasterStripText);
    return run_validation([&] {
      util::check::refuse_unknown_id(wording.target_subject, wording.target_thing, "engine");
    });
  };
  switch (refusal) {
    case engine::SidechainRefusal::kTableFull:
      return refuse_state("the sidechain binding table is full (" +
                          std::to_string(engine::TrackMixerRuntime::kMaxSidechainBindings) +
                          " entries)");
    case engine::SidechainRefusal::kPlanRefused:
      return refuse_alignment_ceiling("the latency the sidechain key would add to the mix");
    case engine::SidechainRefusal::kInvalidTarget:
      return refuse_target();
    case engine::SidechainRefusal::kInsertOutOfRange:
      if (!target_exists) return refuse_target();
      return refuse_with("insert_index", wording.insert_predicate);
    case engine::SidechainRefusal::kUndeclaredSource:
      return run_validation([&] {
        util::check::refuse_unknown_id(wording.source_subject, wording.source_thing, "engine");
      });
    case engine::SidechainRefusal::kInvalidSourceKind:
      return refuse_source_kind(source_kind);
    case engine::SidechainRefusal::kSelfKey:
      if (wording.target_subject == nullptr) {
        return refuse_with(wording.source_subject, "must not name the sidechain target itself");
      }
      return run_validation([&] {
        util::check::refuse_relation({wording.source_subject, wording.target_subject},
                                     "{0} must not equal {1}");
      });
    case engine::SidechainRefusal::kCycle:
      return refuse_with(wording.source_subject, "must not make the sidechain keys form a cycle");
    case engine::SidechainRefusal::kNone:
      break;
  }
  const engine::MixerEditFault fault = realtime.track_mixer().last_fault();
  if (fault.cause == engine::MixerEditCause::kOutOfMemory) {
    return refuse_out_of_memory("out of memory while planning the sidechain delays");
  }
  if (fault.cause == engine::MixerEditCause::kLatencyPastAlignmentCeiling) {
    return refuse_alignment_ceiling("the latency the sidechain key would add to the mix");
  }
  return refuse_state("the sidechain binding was refused and the mixer retained no cause for it");
}

}  // namespace
#endif

SonareError sonare_engine_set_track_lanes(SonareEngine* engine, const SonareEngineTrackLane* lanes,
                                          size_t lane_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(lanes, lane_count);
#if !defined(SONARE_WITH_MIXING)
  (void)lanes;
  (void)lane_count;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
             : mixer_edit_error(engine->engine, {EditScope::kLanes});
  SONARE_C_CATCH
#endif
}

#if defined(SONARE_WITH_MIXING)
namespace {

SonareError track_lane_edit_result_to_error(engine::RealtimeEngine& realtime,
                                            engine::TrackLaneEditResult result, EditScope scope) {
  if (result == engine::TrackLaneEditResult::kApplied) return SONARE_OK;
  if (result == engine::TrackLaneEditResult::kUnknownTrack) return refuse_track_id();
  return mixer_edit_error(realtime, {scope});
}

}  // namespace
#endif

SonareError sonare_engine_set_track_sends(SonareEngine* engine, uint32_t track_id,
                                          const SonareEngineTrackSend* sends, size_t send_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(sends, send_count);
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)sends;
  (void)send_count;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
      EditScope::kTrackSends);
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_output_bus(SonareEngine* engine, uint32_t track_id,
                                               uint32_t output_bus_id) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)output_bus_id;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  return track_lane_edit_result_to_error(
      engine->engine, engine->engine.set_track_output_bus(track_id, output_bus_id),
      EditScope::kOutputBus);
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
  (void)insert_index;
  (void)source_track_id;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)insert_index;
  (void)source_kind;
  (void)source_id;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)insert_index;
  (void)source_kind;
  (void)source_id;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  engine::SidechainSourceKind kind;
  if (!sidechain_source_kind_from_c(source_kind, &kind)) return refuse_source_kind(source_kind);
  if (!require_master_strip(engine->engine)) return SONARE_ERROR_INVALID_STATE;
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
  (void)buses;
  (void)bus_count;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
             : mixer_edit_error(engine->engine, {EditScope::kBuses});
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_bus_strip_json(SonareEngine* engine, uint32_t bus_id,
                                             const char* scene_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (bus_id == 0) return refuse_bus_id();
  SONARE_C_REQUIRE_NONNULL(scene_json);
#if !defined(SONARE_WITH_MIXING)
  (void)bus_id;
  (void)scene_json;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  mixing::api::Scene scene;
  SonareError perr = parse_scene_json(scene_json, &scene);
  if (perr != SONARE_OK) return perr;
  if (scene.buses.empty()) {
    set_last_error(SONARE_ERROR_INVALID_FORMAT, "scene_json must contain at least one bus");
    return SONARE_ERROR_INVALID_FORMAT;
  }
  return engine->engine.set_bus_strip(bus_id, scene.buses.front())
             ? SONARE_OK
             : mixer_edit_error(engine->engine, {EditScope::kStrip});
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_track_strip_json(SonareEngine* engine, uint32_t track_id,
                                               const char* scene_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  if (track_id == 0) return refuse_track_id();
  SONARE_C_REQUIRE_NONNULL(scene_json);
#if !defined(SONARE_WITH_MIXING)
  (void)track_id;
  (void)scene_json;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  mixing::api::Scene scene;
  SonareError perr = parse_scene_json(scene_json, &scene);
  if (perr != SONARE_OK) return perr;
  if (scene.strips.empty()) {
    set_last_error(SONARE_ERROR_INVALID_FORMAT, "scene_json must contain at least one strip");
    return SONARE_ERROR_INVALID_FORMAT;
  }
  return engine->engine.set_track_strip(track_id, scene.strips.front())
             ? SONARE_OK
             : mixer_edit_error(engine->engine, {EditScope::kStrip});
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
  (void)track_id;
  (void)band_index;
  (void)band_json;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  const mastering::eq::EqBand band = sonare::c_api::parse_eq_band_json(band_json);
  if (const SonareError err = check_eq_band(band, band_index, engine->engine.sample_rate());
      err != SONARE_OK) {
    return err;
  }
  return engine->engine.set_track_eq_band(track_id, static_cast<size_t>(band_index), band)
             ? SONARE_OK
             : mixer_edit_error(engine->engine, {EditScope::kEqBand, track_id});
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
  (void)track_id;
  (void)insert_index;
  (void)bypassed;
  (void)reset_on_bypass;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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

SonareError sonare_engine_set_master_strip_json(SonareEngine* engine, const char* scene_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
  SONARE_C_REQUIRE_NONNULL(scene_json);
#if !defined(SONARE_WITH_MIXING)
  (void)scene_json;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  mixing::api::Scene scene;
  SonareError perr = parse_scene_json(scene_json, &scene);
  if (perr != SONARE_OK) return perr;
  if (scene.strips.empty()) {
    set_last_error(SONARE_ERROR_INVALID_FORMAT, "scene_json must contain at least one strip");
    return SONARE_ERROR_INVALID_FORMAT;
  }
  return engine->engine.set_master_strip(scene.strips.front())
             ? SONARE_OK
             : mixer_edit_error(engine->engine, {EditScope::kStrip});
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
  (void)band_index;
  (void)band_json;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  const mastering::eq::EqBand band = sonare::c_api::parse_eq_band_json(band_json);
  if (!require_master_strip(engine->engine)) return SONARE_ERROR_INVALID_STATE;
  if (const SonareError err = check_eq_band(band, band_index, engine->engine.sample_rate());
      err != SONARE_OK) {
    return err;
  }
  return engine->engine.set_master_eq_band(static_cast<size_t>(band_index), band)
             ? SONARE_OK
             : mixer_edit_error(engine->engine, {EditScope::kEqBand});
  SONARE_C_CATCH
#endif
}

SonareError sonare_engine_set_master_strip_insert_bypassed(SonareEngine* engine,
                                                           unsigned int insert_index, int bypassed,
                                                           int reset_on_bypass) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_MIXING)
  (void)insert_index;
  (void)bypassed;
  (void)reset_on_bypass;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  if (!require_master_strip(engine->engine)) return SONARE_ERROR_INVALID_STATE;
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
  (void)bus_id;
  (void)band_index;
  (void)band_json;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  return mixer_edit_error(engine->engine, {EditScope::kEqBand});
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
  (void)bus_id;
  (void)insert_index;
  (void)bypassed;
  (void)reset_on_bypass;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)track_id;
  (void)insert_index;
  (void)value;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)insert_index;
  (void)value;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  if (!require_master_strip(engine->engine)) return SONARE_ERROR_INVALID_STATE;
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
  (void)bus_id;
  (void)insert_index;
  (void)value;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)insert_index;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)insert_index;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)insert_index;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)insert_index;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)insert_index;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  if (!require_master_strip(engine->engine)) return SONARE_ERROR_INVALID_STATE;
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
  (void)insert_index;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  return engine->engine.clear_track_insert_parameter_bases(track_id) ? SONARE_OK
                                                                     : refuse_track_id();
#endif
}

SonareError sonare_engine_clear_master_insert_parameter_bases(SonareEngine* engine) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(engine);
#if !defined(SONARE_WITH_MIXING)
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)param_id;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)track_id;
  (void)insert_index;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)insert_index;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  SONARE_C_TRY
  if (!require_master_strip(engine->engine)) return SONARE_ERROR_INVALID_STATE;
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
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)bus_id;
  (void)insert_index;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)track_id;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)bus_id;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)track_id;
  (void)pan;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)track_id;
  (void)pan_law;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)track_id;
  (void)pan_mode;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)track_id;
  (void)left_pan;
  (void)right_pan;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)track_id;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)bus_id;
  (void)pan;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)bus_id;
  (void)pan_law;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)bus_id;
  (void)pan_mode;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)bus_id;
  (void)left_pan;
  (void)right_pan;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)track_id;
  (void)delay_samples;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)track_id;
  (void)insert_index;
  (void)source_track_id;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)bus_id;
  (void)insert_index;
  (void)source_kind;
  (void)source_id;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)insert_index;
  (void)source_kind;
  (void)source_id;
  set_last_error(SONARE_ERROR_NOT_SUPPORTED, "mixing support is not compiled in");
  return SONARE_ERROR_NOT_SUPPORTED;
#else
  *out_refusal = static_cast<int>(engine->engine.can_set_master_sidechain(
      insert_index, sidechain_source_kind_for_query(source_kind), source_id));
  return SONARE_OK;
#endif
}
