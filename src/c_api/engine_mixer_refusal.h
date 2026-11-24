#pragma once

/// @file engine_mixer_refusal.h
/// @brief The refusal a mixer configuration or sidechain call raises for the cause the engine
///        retained, shared by the C-ABI engine entries and the WASM realtime engine so both name
///        the same cause with the same structured refusal. Requires SONARE_WITH_MIXING.
///
/// Subjects are spelled as the C entries name their arguments; each surface respells them
/// through its own refusal-spelling mechanism.

#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "c_api/checked_insert.h"
#include "c_api/engine_config_domains.h"
#include "engine/insert_automation_targets.h"
#include "engine/realtime_engine.h"
#include "mastering/eq/parametric.h"
#include "mixing/alignment_delay.h"
#include "mixing/api/scene.h"
#include "mixing/channel_strip.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/parameter_checks.h"
#include "util/refusal_predicates.h"

namespace sonare_c_detail {

/// @brief The call a refused mixer edit came from; it decides how its causes name the arguments.
enum class MixerEditScope : uint8_t {
  kLanes,       ///< set_track_lanes
  kBuses,       ///< set_track_buses
  kTrackSends,  ///< set_track_sends
  kOutputBus,   ///< set_track_output_bus
  kStrip,       ///< the strip setters that take a scene document
  kEqBand,      ///< the three EQ band setters
};

namespace mixer_refusal_detail {

[[noreturn]] inline void refuse_state(const std::string& text) {
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, text);
}

[[noreturn]] inline void refuse_out_of_memory(const std::string& text) {
  throw sonare::SonareException(sonare::ErrorCode::OutOfMemory, text);
}

[[noreturn]] inline void refuse_alignment_ceiling(const char* what) {
  refuse_state(std::string(what) + " is past the alignment ceiling of " +
               std::to_string(sonare::mixing::kMaxAlignmentDelaySamples) + " samples");
}

// "<list>[i].<member>" for an entry of the call's lane or bus list, or the bare member for a call
// that addresses one lane.
inline std::string owner_subject(MixerEditScope scope, const sonare::engine::MixerEditFault& fault,
                                 const char* member) {
  if (scope == MixerEditScope::kLanes) {
    return sonare::util::check::indexed_subject("lanes", fault.index, member);
  }
  if (scope == MixerEditScope::kBuses) {
    return sonare::util::check::indexed_subject("buses", fault.index, member);
  }
  return member;
}

// The subject of a send of the faulted entry, or of the send list itself when no send is named.
inline std::string send_subject(MixerEditScope scope, const sonare::engine::MixerEditFault& fault,
                                const char* member, const char* list_member) {
  if (fault.send_index == sonare::engine::kNoFaultIndex) {
    return owner_subject(scope, fault, list_member);
  }
  const std::string owner = scope == MixerEditScope::kLanes || scope == MixerEditScope::kBuses
                                ? owner_subject(scope, fault, "sends")
                                : std::string("sends");
  return owner + "[" + std::to_string(fault.send_index) + "]." + member;
}

[[noreturn]] inline void refuse_id_zero(const std::string& subject) {
  sonare::util::check::integer_within(0, kEngineMixerIdDomain, subject.c_str());
  // The domain excludes zero, so the check above has already thrown.
  refuse_state("an id of zero passed the mixer id domain");
}

[[noreturn]] inline void refuse_unknown_bus_in(const std::string& subject) {
  sonare::util::check::refuse_unknown_id(subject.c_str(), "a bus", "engine");
}

[[noreturn]] inline void refuse_count(const char* subject, size_t limit) {
  sonare::util::check::refuse(
      subject, sonare::util::refusal::integer_range_predicate(0, static_cast<double>(limit)));
}

}  // namespace mixer_refusal_detail

/// @brief Refuses a track id that names no lane of @p realtime's mixer.
[[noreturn]] inline void refuse_unknown_mixer_track() {
  sonare::util::check::refuse_unknown_id("track_id", "a track", "engine");
}

/// @brief Refuses @p track_id for a strip call: unknown, or a lane without a channel strip.
[[noreturn]] inline void throw_track_without_strip(sonare::engine::RealtimeEngine& realtime,
                                                   uint32_t track_id) {
  uint32_t position = 0;
  if (!realtime.track_mixer().track_lane_position(track_id, &position)) {
    refuse_unknown_mixer_track();
  }
  sonare::util::check::refuse("track_id", "must be the id of a track that has a channel strip");
}

/// @brief Refuses an insert automation binding because the target table is full.
[[noreturn]] inline void refuse_insert_automation_table_full() {
  mixer_refusal_detail::refuse_state(
      "the insert automation table is full (" +
      std::to_string(sonare::engine::InsertAutomationTargetTable::kCapacity) + " entries)");
}

/// @brief Throws the refusal for the mixer edit @p realtime last refused, worded for the call
///        @p scope names; @p track_id is the lane a single-lane call addressed.
[[noreturn]] inline void throw_mixer_edit_refusal(sonare::engine::RealtimeEngine& realtime,
                                                  MixerEditScope scope, uint32_t track_id = 0) {
  using Cause = sonare::engine::MixerEditCause;
  using sonare::engine::TrackMixerRuntime;
  using sonare::util::check::refuse;
  using sonare::util::refusal::range_predicate;
  namespace d = mixer_refusal_detail;
  const sonare::engine::MixerEditFault fault = realtime.last_mixer_edit_fault();
  switch (fault.cause) {
    case Cause::kTooManyLanes:
      d::refuse_count("lane_count", TrackMixerRuntime::kMaxTrackLanes);
    case Cause::kLaneTrackIdZero:
      d::refuse_id_zero(d::owner_subject(scope, fault, "track_id"));
    case Cause::kLaneLayoutNotStereo:
      throw sonare::SonareException(sonare::ErrorCode::NotSupported,
                                    d::owner_subject(scope, fault, "source_channel_layout") +
                                        " other than stereo is not supported");
    case Cause::kLaneOutputBusUnknown:
      d::refuse_unknown_bus_in(d::owner_subject(scope, fault, "output_bus_id"));
    case Cause::kLaneTrackIdDuplicate:
      refuse(d::owner_subject(scope, fault, "track_id").c_str(), "must be unique among the lanes");
    case Cause::kTooManySends:
      d::refuse_count(d::send_subject(scope, fault, "bus_id", "send_count").c_str(),
                      sonare::mixing::ChannelStrip::kMaxSends);
    case Cause::kSendBusIdZero:
      d::refuse_id_zero(d::send_subject(scope, fault, "bus_id", "sends"));
    case Cause::kSendLevelOutOfRange:
      refuse(d::send_subject(scope, fault, "level_db", "sends").c_str(),
             range_predicate(sonare::constants::kFloorDb, TrackMixerRuntime::kMaxGainDb));
    case Cause::kSendBusUnknown:
      d::refuse_unknown_bus_in(d::send_subject(scope, fault, "bus_id", "sends"));
    case Cause::kSendBusDuplicate:
      refuse(d::send_subject(scope, fault, "bus_id", "sends").c_str(),
             "must not repeat an earlier send");
    case Cause::kTooManyBuses:
      d::refuse_count("bus_count", TrackMixerRuntime::kMaxBusLanes);
    case Cause::kBusIdZero:
      d::refuse_id_zero(d::owner_subject(scope, fault, "bus_id"));
    case Cause::kBusGainOutOfRange:
      refuse(d::owner_subject(scope, fault, "gain_db").c_str(),
             range_predicate(sonare::constants::kFloorDb, TrackMixerRuntime::kMaxGainDb));
    case Cause::kBusIdDuplicate:
      refuse(d::owner_subject(scope, fault, "bus_id").c_str(), "must be unique among the buses");
    case Cause::kBusRouteUnknown:
    case Cause::kBusRouteToSelf: {
      const bool from_send = fault.send_index != sonare::engine::kNoFaultIndex;
      const std::string route = from_send ? d::send_subject(scope, fault, "bus_id", "sends")
                                          : d::owner_subject(scope, fault, "output_bus_id");
      if (fault.cause == Cause::kBusRouteUnknown) {
        refuse(route.c_str(), "must be the id of a bus in buses");
      }
      sonare::util::check::refuse_relation({route, d::owner_subject(scope, fault, "bus_id")},
                                           "{0} must not equal {1}");
    }
    case Cause::kBusRouteCycle:
      refuse("buses",
             "must not route a bus back into itself through outputs, sends or sidechain keys");
    case Cause::kTrackIdZero:
      refuse_unknown_mixer_track();
    case Cause::kTrackHasNoStrip:
      throw_track_without_strip(realtime, track_id);
    case Cause::kBusUnknown:
      sonare::util::check::refuse_unknown_id("bus_id", "a bus", "engine");
    case Cause::kStripEqInvalid:
      refuse("settings",
             "must describe a strip EQ whose bands are valid at the engine's sample rate");
    case Cause::kStripInsertsRefused:
      refuse("settings", "must describe inserts the mixer can create");
    case Cause::kBusPanNotFinite:
      refuse("settings", "must describe a bus whose pan values are finite");
    case Cause::kBusImageOnWideLayout:
      refuse("settings",
             "must leave the pan and width of a bus wider than stereo at their defaults");
    case Cause::kEqBandIndexInvalid:
      refuse("band_index",
             sonare::util::refusal::integer_range_predicate(
                 0, static_cast<double>(sonare::mastering::eq::ParametricEq::kMaxBands) - 1));
    case Cause::kEqBandRefused:
      refuse("band", "must describe a band the EQ stage accepts at the engine's sample rate");
    case Cause::kInsertAutomationTableFull:
      refuse_insert_automation_table_full();
    case Cause::kOwnedStripTableFull:
      d::refuse_state("the owned strip table is full (" +
                      std::to_string(TrackMixerRuntime::kMaxTrackLanes) + " strips)");
    case Cause::kLaneTableFull:
      d::refuse_state("the lane table is full (" +
                      std::to_string(TrackMixerRuntime::kMaxTrackLanes) + " lanes)");
    case Cause::kRetainedDelayPastCeiling:
      d::refuse_alignment_ceiling("the clip delay the mixer retains, which forbids any lane,");
    case Cause::kLatencyPastAlignmentCeiling:
      d::refuse_alignment_ceiling("the latency the mixer has to align");
    case Cause::kBusImageBlocksWiderLayout:
      d::refuse_state(sonare::util::check::indexed_subject("buses", fault.index, "channel_layout") +
                      " cannot be wider than stereo while the bus keeps a pan or width");
    case Cause::kLaneRoutingBlocksBusRemoval:
      d::refuse_state("a lane output or send still routes to a bus the new bus list drops");
    case Cause::kStagingFailed:
      d::refuse_state("the mixer failed while staging the change; nothing was applied");
    case Cause::kOutOfMemory:
      d::refuse_out_of_memory("out of memory while staging the mixer change; nothing was applied");
    case Cause::kNone:
      break;
  }
  d::refuse_state("the mixer refused the change and retained no cause for it");
}

/// @brief Throws the refusal for a refused strip setter. When the engine retained only its coarse
///        insert-construction fault, replays @p inserts to raise the factory's structured parameter
///        refusal under `settings.inserts[i].params`; otherwise the retained cause, as
///        throw_mixer_edit_refusal raises it for MixerEditScope::kStrip.
[[noreturn]] inline void throw_strip_edit_refusal(
    sonare::engine::RealtimeEngine& realtime,
    const std::vector<sonare::mixing::api::Insert>& inserts) {
  if (realtime.last_mixer_edit_fault().cause ==
      sonare::engine::MixerEditCause::kStripInsertsRefused) {
    namespace schema = sonare::util::json::schema;
    for (size_t i = 0; i < inserts.size(); ++i) {
      const sonare::mixing::api::Insert& insert = inserts[i];
      const std::string path =
          schema::child_path(schema::index_path("settings.inserts", i), "params");
      try {
        if (!sonare::mastering::api::make_checked_insert(insert.processor_name, insert.params_json,
                                                         path, sonare::ErrorCode::InvalidParameter,
                                                         realtime.sample_rate())) {
          break;
        }
      } catch (const sonare::SonareParameterRefusal&) {
        throw;
      } catch (const sonare::SonareException&) {
        break;
      }
    }
  }
  throw_mixer_edit_refusal(realtime, MixerEditScope::kStrip);
}

/// @brief Throws the refusal for a refused single-lane edit (set_track_sends /
///        set_track_output_bus); returns when @p result is kApplied.
inline void require_track_lane_edit(sonare::engine::RealtimeEngine& realtime,
                                    sonare::engine::TrackLaneEditResult result,
                                    MixerEditScope scope) {
  if (result == sonare::engine::TrackLaneEditResult::kApplied) return;
  if (result == sonare::engine::TrackLaneEditResult::kUnknownTrack) refuse_unknown_mixer_track();
  throw_mixer_edit_refusal(realtime, scope);
}

/// @brief How a sidechain call names its target and source.
struct SidechainWording {
  const char* target_subject;  ///< nullptr for the master strip, which has no id
  const char* target_thing;
  const char* insert_predicate;
  const char* source_subject;
  const char* source_thing;
};

/// @brief Throws the refusal for a refused sidechain call. Table-full and alignment-ceiling
///        verdicts come from bindings and latency earlier calls left; every other verdict
///        refuses the call's own target or source. A verdict of kNone means re-planning the
///        delays failed, and the mixer kept why.
[[noreturn]] inline void throw_sidechain_refusal(sonare::engine::RealtimeEngine& realtime,
                                                 sonare::engine::SidechainRefusal refusal,
                                                 const SidechainWording& wording,
                                                 bool target_exists, int source_kind) {
  using sonare::engine::SidechainRefusal;
  using sonare::util::check::refuse;
  namespace d = mixer_refusal_detail;
  const auto refuse_target = [&] {
    sonare::util::check::refuse_unknown_id(wording.target_subject, wording.target_thing, "engine");
  };
  switch (refusal) {
    case SidechainRefusal::kTableFull:
      d::refuse_state("the sidechain binding table is full (" +
                      std::to_string(sonare::engine::TrackMixerRuntime::kMaxSidechainBindings) +
                      " entries)");
    case SidechainRefusal::kPlanRefused:
      d::refuse_alignment_ceiling("the latency the sidechain key would add to the mix");
    case SidechainRefusal::kInvalidTarget:
      refuse_target();
      break;
    case SidechainRefusal::kInsertOutOfRange:
      if (!target_exists) refuse_target();
      refuse("insert_index", wording.insert_predicate);
    case SidechainRefusal::kUndeclaredSource:
      sonare::util::check::refuse_unknown_id(wording.source_subject, wording.source_thing,
                                             "engine");
    case SidechainRefusal::kInvalidSourceKind: {
      const std::array<int, 2> kinds{static_cast<int>(sonare::engine::SidechainSourceKind::Track),
                                     static_cast<int>(sonare::engine::SidechainSourceKind::Bus)};
      sonare::util::check::one_of(source_kind, kinds, "source_kind");
      break;
    }
    case SidechainRefusal::kSelfKey:
      // The master strip has no id to key itself by, so only a lane or bus reaches here.
      assert(wording.target_subject != nullptr);
      sonare::util::check::refuse_relation({wording.source_subject, wording.target_subject},
                                           "{0} must not equal {1}");
    case SidechainRefusal::kCycle:
      refuse(wording.source_subject, "must not make the sidechain keys form a cycle");
    case SidechainRefusal::kNone:
      break;
  }
  const sonare::engine::MixerEditFault fault = realtime.track_mixer().last_fault();
  if (fault.cause == sonare::engine::MixerEditCause::kOutOfMemory) {
    d::refuse_out_of_memory("out of memory while planning the sidechain delays");
  }
  if (fault.cause == sonare::engine::MixerEditCause::kLatencyPastAlignmentCeiling) {
    d::refuse_alignment_ceiling("the latency the sidechain key would add to the mix");
  }
  d::refuse_state("the sidechain binding was refused and the mixer retained no cause for it");
}

}  // namespace sonare_c_detail
