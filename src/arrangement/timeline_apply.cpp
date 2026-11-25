#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "arrangement/edit_compiler.h"
#include "engine/mixing_runtime.h"
#if defined(SONARE_WITH_MIXING)
#include "engine/strip_spec.h"
#endif
#include "midi/midi_clip.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::arrangement {

using constants::kDefaultBpm;
using transport::kDefaultTimeSignature;

namespace {

ApplyResult apply_refused(ErrorCode code, std::string_view message) {
  ApplyResult result;
  result.code = code;
  result.outcome = ApplyOutcome::kUnchanged;
  try {
    result.message = message;
  } catch (const std::bad_alloc&) {
  }
  return result;
}

// Called inside a handler: keeps the refusal so the entry rethrows it with its subjects.
ApplyResult apply_refused(const SonareParameterRefusal& refusal) {
  ApplyResult result = apply_refused(refusal.code(), refusal.what());
  result.refusal = std::current_exception();
  return result;
}

#if defined(SONARE_WITH_MIXING)
bool has_track_lane(const CompiledTimeline& timeline, TrackId track_id) noexcept {
  for (const CompiledTrackLane& lane : timeline.track_lanes) {
    if (lane.track_id == track_id) return true;
  }
  return false;
}
#endif

// Collects the playback lanes; @p lane_tracks receives, per lane, the track a typed
// lane drives (0 for an opaque one), whose engine id exists only once the engine
// holds the track lanes. Opaque ids remain untouched for legacy host targets; the
// compiler already applied project-order first-wins filtering to them, and
// set_lanes() defensively preserves that contract for a hand-built snapshot.
std::vector<automation::AutomationLane> resolve_playback_lanes(const CompiledTimeline& timeline,
                                                               std::vector<TrackId>* lane_tracks,
                                                               bool bind_strips) {
  std::vector<automation::AutomationLane> playback_lanes;
  playback_lanes.reserve(timeline.mixer.automation_bindings.empty()
                             ? timeline.automation_lanes.size()
                             : timeline.mixer.automation_bindings.size());
  if (timeline.mixer.automation_bindings.empty()) {
    // Keep hand-built/legacy snapshots usable: before track-tagged bindings
    // existed, opaque lanes were already engine-ready here. There is no safe
    // way to resolve a typed lane without an owning track, so leave those out
    // rather than interpreting their persistent edit id as a playback route.
    for (const automation::AutomationLane& lane : timeline.automation_lanes) {
      if (lane.target_kind() == automation::AutomationTargetKind::kOpaque) {
        playback_lanes.push_back(lane);
        lane_tracks->push_back(0);
      }
    }
    return playback_lanes;
  }
  for (const MixerAutomationBinding& binding : timeline.mixer.automation_bindings) {
    const auto kind = binding.lane.target_kind();
    if (!bind_strips && (kind == automation::AutomationTargetKind::kTrackInsertParam ||
                         kind == automation::AutomationTargetKind::kTrackSendDb)) {
      continue;
    }
    TrackId track_id = 0;
    if (kind != automation::AutomationTargetKind::kOpaque) {
#if defined(SONARE_WITH_MIXING)
      if (has_track_lane(timeline, binding.track_id)) track_id = binding.track_id;
#endif
      // A typed lane with no engine track lane has nothing to drive.
      if (track_id == 0) continue;
    }
    playback_lanes.push_back(binding.lane);
    lane_tracks->push_back(track_id);
  }
  return playback_lanes;
}

#if defined(SONARE_WITH_MIXING)
// Points each typed lane at its reserved runtime id, minted by the installed lane/strip set.
bool bind_playback_lane_targets(engine::RealtimeEngine& engine,
                                const std::vector<TrackId>& lane_tracks, bool bind_strips,
                                std::vector<automation::AutomationLane>* lanes) noexcept {
  for (size_t i = 0; i < lane_tracks.size() && i < lanes->size(); ++i) {
    if (lane_tracks[i] == 0) continue;
    automation::AutomationLane& lane = (*lanes)[i];
    int64_t id = -1;
    switch (lane.target_kind()) {
      case automation::AutomationTargetKind::kTrackFaderDb:
        id = engine.track_lane_automation_id(lane_tracks[i],
                                             sonare::engine::TrackMixerRuntime::kFaderDb);
        break;
      case automation::AutomationTargetKind::kTrackPan:
        id = engine.track_lane_automation_id(lane_tracks[i],
                                             sonare::engine::TrackMixerRuntime::kPan);
        break;
      case automation::AutomationTargetKind::kTrackWidth:
        id = engine.track_lane_automation_id(lane_tracks[i],
                                             sonare::engine::TrackMixerRuntime::kWidth);
        break;
      case automation::AutomationTargetKind::kTrackInsertParam: {
        if (!bind_strips) return false;
        const automation::AutomationTargetIdentity& identity = lane.target_identity();
        if (!identity.insert_index.has_value() ||
            *identity.insert_index > std::numeric_limits<unsigned int>::max()) {
          return false;
        }
        id = engine.resolve_track_insert_automation_id(
            lane_tracks[i], static_cast<unsigned int>(*identity.insert_index), identity.param_name);
        break;
      }
      case automation::AutomationTargetKind::kOpaque:
      default:
        return false;
    }
    if (id < 0) return false;
    lane.set_target_param_id(static_cast<uint32_t>(id));
  }
  return true;
}
#endif

#if defined(SONARE_WITH_MIXING)
const mixing::api::Strip* find_scene_strip(const MixerRequest& mixer,
                                           const std::string& id) noexcept {
  for (const mixing::api::Strip& strip : mixer.scene.strips) {
    if (strip.id == id) return &strip;
  }
  return nullptr;
}

// A failed build can carry the factory's domain refusal under the scene entry's index.
void rethrow_timeline_insert_refusal(const MixerRequest& mixer, const mixing::api::Strip& strip,
                                     double sample_rate) {
  if (!engine::strip_eq_acceptable(strip.eq, sample_rate)) return;
  const auto strip_index = static_cast<size_t>(&strip - mixer.scene.strips.data());
  const std::string inserts =
      "timeline.mixer.scene.strips[" + std::to_string(strip_index) + "].inserts";
  for (size_t i = 0; i < strip.inserts.size(); ++i) {
    const std::string path = inserts + "[" + std::to_string(i) + "].params";
    try {
      bool stereo_pair_only = false;
      if (!engine::make_insert_processor(strip.inserts[i], sample_rate, &stereo_pair_only,
                                         path.c_str())) {
        return;
      }
    } catch (const SonareParameterRefusal& refusal) {
      if (refusal.code() == ErrorCode::InvalidParameter && !refusal.shape_error()) throw;
      return;
    } catch (const SonareException&) {
      return;
    }
  }
}

bool has_strip_binding(const CompiledTimeline& timeline, TrackId track_id) noexcept {
  for (const MixerStripBinding& binding : timeline.mixer.bindings) {
    if (binding.track_id == track_id) return true;
  }
  return false;
}

const MixerStripBinding* find_track_strip_binding(const MixerRequest& mixer,
                                                  TrackId track_id) noexcept {
  const MixerStripBinding* found = nullptr;
  for (const MixerStripBinding& binding : mixer.bindings) {
    if (binding.track_id != track_id) continue;
    if (found != nullptr) return nullptr;
    found = &binding;
  }
  return found;
}

bool insert_lane_targets_match_scene(const CompiledTimeline& timeline) noexcept {
  for (const MixerAutomationBinding& binding : timeline.mixer.automation_bindings) {
    if (binding.lane.target_kind() != automation::AutomationTargetKind::kTrackInsertParam) {
      continue;
    }
    const MixerStripBinding* strip_binding =
        find_track_strip_binding(timeline.mixer, binding.track_id);
    if (strip_binding == nullptr) return false;
    const mixing::api::Strip* strip = find_scene_strip(timeline.mixer, strip_binding->strip_id);
    if (strip == nullptr) return false;
    const automation::AutomationTargetIdentity& identity = binding.lane.target_identity();
    if (!identity.insert_index.has_value() ||
        *identity.insert_index > std::numeric_limits<unsigned int>::max()) {
      return false;
    }
    const std::string* processor = engine::strip_insert_processor_name_at(
        *strip, static_cast<unsigned int>(*identity.insert_index));
    if (processor == nullptr || *processor != identity.processor) return false;
  }
  return true;
}
#endif

// Empties every timeline-owned domain. Each step is attempted even when an
// earlier one throws, so the result is as empty as the allocator allows.
void clear_timeline_domains(engine::RealtimeEngine& engine) noexcept {
  const auto attempt = [](auto&& step) noexcept {
    try {
      step();
    } catch (...) {
    }
  };
  attempt([&] { engine.set_clips({}); });
#if defined(SONARE_WITH_ARRANGEMENT)
  attempt([&] { engine.publish_midi_clips(engine.prepare_midi_clips({})); });
#endif
  attempt([&] { engine.automation().set_lanes({}); });
  attempt([&] { engine.set_markers({}); });
#if defined(SONARE_WITH_MIXING)
  // Unbind while the lanes still exist; bind_track_strip on an unknown track claims a free lane.
  std::array<uint32_t, engine::TrackMixerRuntime::kMaxTrackLanes> lane_ids{};
  const size_t lane_count =
      engine.track_mixer().copy_lane_track_ids(lane_ids.data(), lane_ids.size());
  for (size_t i = 0; i < lane_count; ++i) {
    attempt([&] { engine.bind_track_strip(lane_ids[i], nullptr); });
  }
  attempt([&] { engine.set_track_lanes({}); });
#endif
  attempt([&] { engine.set_tempo(kDefaultBpm); });
  attempt([&] {
    engine.set_time_signature(kDefaultTimeSignature.numerator, kDefaultTimeSignature.denominator);
  });
  attempt([&] { engine.set_applied_timeline_sample_rate(0.0); });
}

ApplyResult apply_cleared(engine::RealtimeEngine& engine, ErrorCode code,
                          std::string_view message) {
  clear_timeline_domains(engine);
  ApplyResult result = apply_refused(code, message);
  result.outcome = ApplyOutcome::kCleared;
  return result;
}

}  // namespace

ApplyResult apply_to_engine(const CompiledTimeline& timeline, engine::RealtimeEngine& engine,
                            const ApplyOptions& options) {
  // (1) Pure validation and every allocation that can precede the first mutation.
  if (engine.max_block_size() <= 0) {
    return apply_refused(ErrorCode::InvalidState,
                         "a timeline can only be applied to a prepared engine");
  }
  if (timeline.sample_rate != 0.0 &&
      !(std::isfinite(timeline.sample_rate) && timeline.sample_rate > 0.0)) {
    return apply_refused(ErrorCode::InvalidParameter,
                         "timeline sample rate must be zero or finite and > 0");
  }
  if (timeline.sample_rate != 0.0 && timeline.sample_rate != engine.sample_rate()) {
    return apply_refused(ErrorCode::InvalidParameter,
                         "timeline sample rate does not match the prepared engine");
  }
  try {
    transport::check_public_tempo_segments(timeline.tempo_segments, "timeline.tempoSegments",
                                           "endBpm");
  } catch (const SonareParameterRefusal& e) {
    return apply_refused(e);
  } catch (const std::bad_alloc&) {
    return apply_refused(ErrorCode::OutOfMemory, "out of memory");
  }
  for (const auto& segment : timeline.tempo_segments) {
    if (!transport::valid_public_ppq(segment.start_ppq)) {
      return apply_refused(ErrorCode::InvalidParameter,
                           "tempo segment startPpq is outside the range the edit API accepts");
    }
  }
  for (const transport::TimeSignatureSegment& segment : timeline.time_signatures) {
    if (!transport::valid_public_time_signature_segment(segment)) {
      return apply_refused(ErrorCode::InvalidParameter,
                           "time signature segments contain invalid timeline or signature values");
    }
  }
#if !defined(SONARE_WITH_MIXING)
  if (options.bind_strips && !timeline.mixer.bindings.empty()) {
    return apply_refused(
        ErrorCode::NotSupported,
        "strip binding requested but SONARE_WITH_MIXING is disabled in this build");
  }
#endif

  std::vector<automation::AutomationLane> playback_lanes;
  std::vector<TrackId> playback_lane_tracks;
  ApplyResult result;
#if defined(SONARE_WITH_MIXING)
  std::vector<engine::TrackLaneConfig> track_lanes;
#endif
#if defined(SONARE_WITH_ARRANGEMENT)
  engine::RealtimeEngine::PreparedMidiClips prepared_midi;
#endif
  try {
#if defined(SONARE_WITH_MIXING)
    track_lanes.reserve(timeline.track_lanes.size());
    for (const CompiledTrackLane& lane : timeline.track_lanes) {
      track_lanes.emplace_back(lane.track_id);
    }
    if (!engine.validate_track_lanes(track_lanes)) {
      return apply_refused(ErrorCode::InvalidParameter, "the engine refuses the track-lane vector");
    }
    if (options.bind_strips) {
      const std::vector<MixerStripBinding>& bindings = timeline.mixer.bindings;
      for (size_t i = 0; i < bindings.size(); ++i) {
        const mixing::api::Strip* strip = find_scene_strip(timeline.mixer, bindings[i].strip_id);
        if (strip == nullptr || !has_track_lane(timeline, bindings[i].track_id)) {
          return apply_refused(ErrorCode::InvalidParameter,
                               "strip binding names a missing scene strip or track lane");
        }
        for (size_t j = i + 1; j < bindings.size(); ++j) {
          if (bindings[j].strip_id == bindings[i].strip_id) {
            return apply_refused(ErrorCode::NotSupported,
                                 "scene strip \"" + bindings[i].strip_id +
                                     "\" is bound by several tracks; live playback is one strip "
                                     "per track");
          }
        }
        if (!engine.validate_track_strip(*strip)) {
          rethrow_timeline_insert_refusal(timeline.mixer, *strip, engine.sample_rate());
          return apply_refused(ErrorCode::InvalidParameter,
                               "scene strip \"" + bindings[i].strip_id + "\" cannot be built");
        }
      }
      for (const MixerAutomationBinding& binding : timeline.mixer.automation_bindings) {
        if (binding.lane.target_kind() == automation::AutomationTargetKind::kTrackSendDb) {
          return apply_refused(ErrorCode::NotSupported,
                               "automation of scene send \"" +
                                   binding.lane.target_identity().send_id + "\" on track " +
                                   std::to_string(binding.track_id) +
                                   " is realized only by the offline bounce");
        }
      }
      if (!insert_lane_targets_match_scene(timeline)) {
        return apply_refused(ErrorCode::InvalidParameter,
                             "track insert automation does not match its bound scene strip insert");
      }
    }
#endif
    // A tempo the capture setup refuses is refused here, before the first mutation.
    engine.check_tempo_segments(
        timeline.tempo_segments.empty()
            ? std::vector<transport::TempoSegment>{transport::kDefaultTempoSegment}
            : timeline.tempo_segments,
        "timeline.tempoSegments");
    playback_lanes = resolve_playback_lanes(timeline, &playback_lane_tracks, options.bind_strips);
    result.installed_automation = playback_lanes;

    // (2) Own and prepare every SysEx payload without publishing.
#if defined(SONARE_WITH_ARRANGEMENT)
    prepared_midi = engine.prepare_midi_clips(timeline.midi_clips);
#endif
  } catch (const SonareParameterRefusal& e) {
    return apply_refused(e);
  } catch (const SonareException& e) {
    return apply_refused(e.code(), e.what());
  } catch (const std::bad_alloc&) {
    return apply_refused(ErrorCode::OutOfMemory, "out of memory while preparing the timeline");
  }

  try {
    // (3) Track lanes: the first mutation. The engine leaves its lanes untouched on refusal.
#if defined(SONARE_WITH_MIXING)
    if (!engine.set_track_lanes(std::move(track_lanes))) {
      // The vector was validated above, so the engine's refusal names its own cause.
      switch (engine.last_mixer_edit_status()) {
        case sonare::engine::MixerEditStatus::kInvalidState:
          return apply_refused(ErrorCode::InvalidState,
                               "earlier engine configuration refuses the track-lane vector");
        case sonare::engine::MixerEditStatus::kOutOfMemory:
          return apply_refused(ErrorCode::OutOfMemory,
                               "out of memory while installing the track lanes");
        default:
          return apply_refused(ErrorCode::InvalidParameter,
                               "the engine refuses the track-lane vector");
      }
    }
    if (options.bind_strips) {
      for (const CompiledTrackLane& lane : timeline.track_lanes) {
        if (has_strip_binding(timeline, lane.track_id)) continue;
        if (!engine.release_track_strip(lane.track_id)) {
          return apply_cleared(engine, ErrorCode::InvalidState,
                               "the engine refused to release a track strip");
        }
      }
      for (const MixerStripBinding& binding : timeline.mixer.bindings) {
        if (!engine.set_track_strip(binding.track_id,
                                    *find_scene_strip(timeline.mixer, binding.strip_id))) {
          return apply_cleared(engine, ErrorCode::InvalidState,
                               "the engine refused a validated track strip");
        }
      }
    }
    if (!bind_playback_lane_targets(engine, playback_lane_tracks, options.bind_strips,
                                    &playback_lanes)) {
      return apply_cleared(engine, ErrorCode::InvalidState,
                           "the engine cannot resolve a typed track automation target");
    }
    // Both lists started as the same resolved lanes, so the bound result is shared.
    result.installed_automation = playback_lanes;
#endif

    // (4) Publish the remaining domains. All are direct-setter / publisher installs, not
    // push_command; tempo goes first so clip rescheduling uses the compiled map.
    if (!timeline.tempo_segments.empty()) {
      engine.set_tempo_segments(timeline.tempo_segments);
    } else {
      engine.set_tempo(kDefaultBpm);
    }
    if (!timeline.time_signatures.empty()) {
      engine.set_time_signature_segments(timeline.time_signatures);
    } else {
      engine.set_time_signature(kDefaultTimeSignature.numerator, kDefaultTimeSignature.denominator);
    }
    // Marker names point into the timeline's own storage.
    engine.set_markers(timeline.markers);
    engine.automation().set_lanes(std::move(playback_lanes));
    // ClipSchedule::storage keeps the baked buffers alive across the RtPublisher swap.
    engine.set_clips(timeline.audio_clips);
#if defined(SONARE_WITH_ARRANGEMENT)
    engine.publish_midi_clips(std::move(prepared_midi));
#endif
  } catch (const SonareException& e) {
    return apply_cleared(engine, e.code(), e.what());
  } catch (const std::bad_alloc&) {
    return apply_cleared(engine, ErrorCode::OutOfMemory,
                         "out of memory while applying the timeline");
  }
  engine.set_applied_timeline_sample_rate(timeline.sample_rate);
  // The graph request has no Project authoring surface; timeline.graph.requested stays false.
  result.outcome = ApplyOutcome::kApplied;
  return result;
}

}  // namespace sonare::arrangement
