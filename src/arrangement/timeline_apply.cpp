#include <array>
#include <cmath>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "arrangement/edit_compiler.h"
#include "engine/mixing_runtime.h"
#include "midi/midi_clip.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::arrangement {

using constants::kDefaultBpm;

namespace {

constexpr transport::TimeSignature kDefaultTimeSignature{};

ApplyResult apply_refused(ErrorCode code, std::string message) {
  ApplyResult result;
  result.code = code;
  result.message = std::move(message);
  result.outcome = ApplyOutcome::kUnchanged;
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
                                                               std::vector<TrackId>* lane_tracks) {
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
    TrackId track_id = 0;
    if (binding.lane.target_kind() != automation::AutomationTargetKind::kOpaque) {
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
// Points each typed lane at its track's fader/pan id, minted by set_track_lanes.
bool bind_playback_lane_targets(const engine::RealtimeEngine& engine,
                                const std::vector<TrackId>& lane_tracks,
                                std::vector<automation::AutomationLane>* lanes) noexcept {
  for (size_t i = 0; i < lane_tracks.size() && i < lanes->size(); ++i) {
    if (lane_tracks[i] == 0) continue;
    automation::AutomationLane& lane = (*lanes)[i];
    const int64_t id = engine.track_lane_automation_id(
        lane_tracks[i], static_cast<unsigned int>(lane.target_kind()));
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

bool has_strip_binding(const CompiledTimeline& timeline, TrackId track_id) noexcept {
  for (const MixerStripBinding& binding : timeline.mixer.bindings) {
    if (binding.track_id == track_id) return true;
  }
  return false;
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

ApplyResult apply_cleared(engine::RealtimeEngine& engine, ErrorCode code, std::string message) {
  clear_timeline_domains(engine);
  ApplyResult result;
  result.code = code;
  result.outcome = ApplyOutcome::kCleared;
  try {
    result.message = std::move(message);
  } catch (...) {
  }
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
  for (const transport::TempoSegment& segment : timeline.tempo_segments) {
    if (!transport::valid_public_tempo_segment(segment)) {
      return apply_refused(ErrorCode::InvalidParameter,
                           "tempo segments contain invalid timeline or out-of-range BPM values");
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
        ErrorCode::NotImplemented,
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
            return apply_refused(ErrorCode::NotImplemented,
                                 "scene strip \"" + bindings[i].strip_id +
                                     "\" is bound by several tracks; live playback is one strip "
                                     "per track");
          }
        }
        if (!engine.validate_track_strip(*strip)) {
          return apply_refused(ErrorCode::InvalidParameter,
                               "scene strip \"" + bindings[i].strip_id + "\" cannot be built");
        }
      }
    }
#endif
    playback_lanes = resolve_playback_lanes(timeline, &playback_lane_tracks);
    result.installed_automation = playback_lanes;

    // (2) Own and prepare every SysEx payload without publishing.
#if defined(SONARE_WITH_ARRANGEMENT)
    prepared_midi = engine.prepare_midi_clips(timeline.midi_clips);
#endif
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
    if (!bind_playback_lane_targets(engine, playback_lane_tracks, &playback_lanes) ||
        !bind_playback_lane_targets(engine, playback_lane_tracks, &result.installed_automation)) {
      return apply_cleared(engine, ErrorCode::InvalidState,
                           "the engine holds no fader/pan id for a track lane");
    }
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
#if defined(SONARE_WITH_MIXING)
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
