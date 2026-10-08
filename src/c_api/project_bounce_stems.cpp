#include "c_api/project_bounce_stems.h"

#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_MIXING)

namespace sonare_c_bounce_detail {

bool render_midi_source_stems(const arr::CompiledTimeline& timeline,
                              const std::vector<HostedInstrument>& instruments, double sample_rate,
                              int block_size, int64_t render_frames, MidiSourceStemSink* sink) {
  size_t render_frame_count = 0;
  size_t render_floats = 0;
  if (sink == nullptr || !checked_frame_shape(render_frames, 2, &render_floats) ||
      !checked_frame_count(render_frames, &render_frame_count)) {
    return false;
  }
  std::set<uint32_t> track_ids;
  for (const sonare::midi::MidiClipSchedule& clip : timeline.midi_clips) {
    if (clip.track_id != 0) track_ids.insert(clip.track_id);
  }
  if (track_ids.size() > sonare::engine::TrackMixerRuntime::kMaxTrackLanes) return false;

  arr::CompiledTimeline midi_only = timeline;
  midi_only.audio_clips.clear();
  sonare::engine::RealtimeEngine engine;
  engine.prepare(sample_rate, block_size);
  const arr::ApplyResult applied = arr::apply_to_engine(midi_only, engine);
  // Out-of-memory keeps its own error code; every other refusal fails the bounce as before.
  if (applied.code == sonare::ErrorCode::OutOfMemory) {
    throw sonare::SonareException(applied.code, applied.message);
  }
  if (!applied.ok()) return false;

  // apply_to_engine already installed the compiled project-order lanes. Do not
  // replace them with a MIDI-only subset: typed automation ids encode those
  // original indices, and changing the vector here would retarget a lane to a
  // different track. The local set is retained only as a bounded consistency
  // check for source-aware rendering.
  for (const HostedInstrument& hosted : instruments) {
    if (hosted.instrument == nullptr || !hosted.instrument->supports_source_track_rendering()) {
      return false;
    }
    hosted.instrument->reset();
    if (!engine.set_midi_instrument(hosted.destination_id, hosted.instrument)) return false;
  }
  // The engine's current source render bank is intentionally selected only for
  // zero-latency instruments. A per-source PDC bank would otherwise be needed
  // to retain independent delay history.
  if (engine.midi_instrument_latency_samples() != 0) return false;

  // Match render_timeline's offline pre-roll: publish lane state and snap its
  // smoothers before the first audible MIDI block, so the source stems do not
  // fade in relative to the live engine / external scene mixer.
  engine.prime_offline_parameters(2, block_size);
  sink->settle_typed_automation();
  engine.set_instrument_source_render_sink(sink);
  std::vector<std::vector<float>> discard(2, std::vector<float>(render_frame_count, 0.0f));
  float* channels[] = {discard[0].data(), discard[1].data()};
  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  engine.push_command(play);
  engine.render_offline(channels, 2, render_frames, block_size);
  engine.set_instrument_source_render_sink(nullptr);
  for (const HostedInstrument& hosted : instruments) {
    engine.set_midi_instrument(hosted.destination_id, nullptr);
  }
  return true;
}

bool has_shared_hosted_midi_destination(const arr::CompiledTimeline& timeline,
                                        const MixerRouting& routing,
                                        const std::vector<HostedInstrument>& instruments,
                                        bool* all_hosts_source_aware) {
  bool source_aware = true;
  for (const HostedInstrument& hosted : instruments) {
    if (hosted.instrument == nullptr || !hosted.instrument->supports_source_track_rendering()) {
      source_aware = false;
    }
  }
  if (all_hosts_source_aware != nullptr) *all_hosts_source_aware = source_aware;
  // Where a track's audio is mixed: its strip's index, or -1 for the dry master path.
  const auto strip_of = [&routing](uint32_t track_id) {
    for (size_t i = 0; i < routing.strip_tracks.size(); ++i) {
      if (routing.strip_tracks[i].count(track_id) != 0) return static_cast<int>(i);
    }
    return -1;
  };
  std::map<uint32_t, std::set<uint32_t>> tracks_by_destination;
  std::map<uint32_t, std::set<int>> targets_by_destination;
  for (const sonare::midi::MidiClipSchedule& clip : timeline.midi_clips) {
    tracks_by_destination[clip.destination_id].insert(clip.track_id);
    targets_by_destination[clip.destination_id].insert(strip_of(clip.track_id));
  }
  // Tracks split across mixing targets must be rendered apart. Tracks that mix
  // into one target are split only where every host can render per source (so
  // each keeps its own track controls); an opaque host renders them once. The
  // source-stem pass renders the whole project, so the decision is project-wide.
  bool divergent = false;
  bool multi_track = false;
  for (const auto& [destination_id, targets] : targets_by_destination) {
    divergent = divergent || targets.size() >= 2;
    multi_track = multi_track || tracks_by_destination[destination_id].size() >= 2;
  }
  return divergent || (multi_track && source_aware);
}

}  // namespace sonare_c_bounce_detail

#endif  // SONARE_WITH_ARRANGEMENT && SONARE_WITH_MIXING
