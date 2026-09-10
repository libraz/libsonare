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
  arr::apply_to_engine(midi_only, engine);

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
  {
    size_t block_count = 0;
    if (!checked_frame_count(block_size, &block_count)) return false;
    std::vector<float> prime_l(block_count, 0.0f);
    std::vector<float> prime_r(block_count, 0.0f);
    float* prime[] = {prime_l.data(), prime_r.data()};
    engine.process(prime, 2, block_size);
    engine.settle_parameters();
    sink->settle_typed_automation();
  }
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
                                        const std::vector<HostedInstrument>& instruments,
                                        bool* all_hosts_source_aware) {
  if (all_hosts_source_aware != nullptr) *all_hosts_source_aware = true;
  std::map<uint32_t, std::set<uint32_t>> tracks_by_destination;
  for (const sonare::midi::MidiClipSchedule& clip : timeline.midi_clips) {
    tracks_by_destination[clip.destination_id].insert(clip.track_id);
  }
  bool shared = false;
  for (const auto& [destination_id, tracks] : tracks_by_destination) {
    if (tracks.size() < 2) continue;
    shared = true;
  }
  // The source-stem pass renders the project once, so every bound destination
  // participates even when only one destination is shared by several strips.
  // An opaque callback cannot be silently dropped from that pass or rendered
  // separately without recreating the very duplicated-pool bug this path fixes.
  if (shared) {
    for (const HostedInstrument& hosted : instruments) {
      if (hosted.instrument == nullptr || !hosted.instrument->supports_source_track_rendering()) {
        if (all_hosts_source_aware != nullptr) *all_hosts_source_aware = false;
      }
    }
  }
  return shared;
}

}  // namespace sonare_c_bounce_detail

#endif  // SONARE_WITH_ARRANGEMENT && SONARE_WITH_MIXING
