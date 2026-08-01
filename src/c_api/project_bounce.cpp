#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>

#include "c_api/synth_patch_common.h"
#include "mastering/api/insert_factory.h"
#include "midi/builtin_synth.h"
#include "midi/synth/sf2_player.h"
#include "midi/synth/synth_presets.h"
#if defined(SONARE_WITH_MIXING)
#include <sonare/sonare_c_mixing.h>

#include "c_api/mixing_internal.h"
#include "engine/mixing_runtime.h"
#include "mixing/api/scene.h"
#endif

namespace {

// Adapts a host's C callback table to a sonare::midi::MidiInstrument so the
// bounce engine can drive an external instrument: events are forwarded to
// on_event at their sample-accurate render frame and render() sums the audio.
// Only opaque UMP words / planar buffers cross the seam (invariant 6).
class CallbackInstrument final : public sonare::midi::MidiInstrument {
 public:
  explicit CallbackInstrument(const SonareInstrumentCallbacks& callbacks) : cb_(callbacks) {}

  void prepare(double sample_rate, int max_block_size) override {
    if (cb_.prepare) cb_.prepare(cb_.user_data, sample_rate, max_block_size);
  }
  void process(float* const* channels, int num_channels, int num_samples) override {
    if (cb_.render) cb_.render(cb_.user_data, channels, num_channels, num_samples);
  }
  void reset() override {}
  int latency_samples() const noexcept override { return cb_.latency_samples; }
  int tail_samples() const noexcept override { return cb_.tail_samples; }
  void on_event(uint32_t destination_id, const sonare::midi::MidiEvent& event) noexcept override {
    if (cb_.on_event) {
      cb_.on_event(cb_.user_data, destination_id, event.ump.words, event.ump.word_count,
                   event.render_frame);
    }
  }

 private:
  SonareInstrumentCallbacks cb_;
};

// A destination id paired with a borrowed instrument pointer (the owning storage
// outlives the render in the caller). Used by the shared bounce core so the
// callback and built-in-synth paths share one render implementation.
struct HostedInstrument {
  uint32_t destination_id = 0;
  sonare::midi::MidiInstrument* instrument = nullptr;
};

// End of the arrangement in frames at the render sample rate: the latest sample
// touched by any audio or MIDI clip on the compiled timeline. Used to
// auto-derive a bounce length when the caller does not supply total_frames.
int64_t arrangement_end_frames(const arr::CompiledTimeline& timeline) noexcept {
  int64_t end = 0;
  for (const auto& clip : timeline.audio_clips) {
    end = std::max(end, clip.start_sample + clip.length_samples);
  }
  for (const auto& clip : timeline.midi_clips) {
    int64_t clip_end = clip.start_sample + clip.length_samples;
    for (const auto& event : clip.events) {
      clip_end = std::max(clip_end, event.render_frame + 1);
    }
    end = std::max(end, clip_end);
  }
  return end;
}

// Renders the compiled timeline offline through a fresh engine into `channels`
// (num_channels deinterleaved buffers of length render_frames). `keep` selects
// which clips to include by track id (a null/empty function keeps everything),
// so the channel-strip bounce can isolate one track's audio into a dry stem.
// The hosted instruments are reset and re-registered per render so a stem starts
// from a clean voice state; only clips whose track passes `keep` fire events.
void render_timeline(const arr::CompiledTimeline& timeline,
                     const std::function<bool(uint32_t)>& keep,
                     const std::vector<HostedInstrument>& instruments, double sample_rate,
                     int block_size, int num_channels, int64_t render_frames,
                     std::vector<std::vector<float>>* channels, bool include_audio = true,
                     bool include_midi = true) {
  arr::CompiledTimeline filtered = timeline;  // copy re-points marker name pointers
  if (!include_audio) filtered.audio_clips.clear();
  if (!include_midi) filtered.midi_clips.clear();
  if (keep) {
    filtered.audio_clips.erase(
        std::remove_if(filtered.audio_clips.begin(), filtered.audio_clips.end(),
                       [&](const sonare::engine::ClipSchedule& c) { return !keep(c.track_id); }),
        filtered.audio_clips.end());
    filtered.midi_clips.erase(
        std::remove_if(filtered.midi_clips.begin(), filtered.midi_clips.end(),
                       [&](const sonare::midi::MidiClipSchedule& c) { return !keep(c.track_id); }),
        filtered.midi_clips.end());
  }

  sonare::engine::RealtimeEngine engine;
  engine.prepare(sample_rate, block_size);
  arr::apply_to_engine(filtered, engine);
  for (const HostedInstrument& hosted : instruments) {
    hosted.instrument->reset();
    engine.set_midi_instrument(hosted.destination_id, hosted.instrument);
  }

  // Prime the parameter smoothers before the audible render so a non-default
  // static fader/pan does not fade in over the first ~5 ms block. Lane fader/pan
  // smoothers only advance while lanes render, so one process() pass with the
  // transport stopped applies automation at the start position and drains queued
  // commands (setting the smoother targets), then settle_parameters() snaps the
  // smoothers to those targets. Without this the bounce's first block ramps in
  // from 0 dB / centre, which live playback never does and which breaks bit-exact
  // determinism. The primed block renders into a throwaway buffer.
  {
    std::vector<std::vector<float>> prime(
        static_cast<size_t>(num_channels),
        std::vector<float>(static_cast<size_t>(block_size), 0.0f));
    std::vector<float*> prime_ptrs;
    prime_ptrs.reserve(prime.size());
    for (auto& channel : prime) prime_ptrs.push_back(channel.data());
    engine.process(prime_ptrs.data(), num_channels, block_size);
    engine.settle_parameters();
  }

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  engine.push_command(play);

  channels->assign(static_cast<size_t>(num_channels),
                   std::vector<float>(static_cast<size_t>(render_frames), 0.0f));
  std::vector<float*> ptrs;
  ptrs.reserve(channels->size());
  for (auto& channel : *channels) ptrs.push_back(channel.data());
  engine.render_offline(ptrs.data(), num_channels, render_frames, block_size);
  for (const HostedInstrument& hosted : instruments) {
    engine.set_midi_instrument(hosted.destination_id, nullptr);
  }
}

#if defined(SONARE_WITH_MIXING)
// Offline-only collector for the engine's source-aware instrument seam. All
// storage is allocated before rendering; on_instrument_source_audio only sums
// a block into its already-present source stem.
class MidiSourceStemSink final : public sonare::engine::InstrumentSourceRenderSink {
 public:
  MidiSourceStemSink(const std::set<uint32_t>& track_ids, int num_channels, int64_t frames)
      : num_channels_(num_channels), frames_(frames) {
    for (uint32_t track_id : track_ids) allocate(track_id);
    default_ = make_stem();
  }

  void on_instrument_source_audio(uint32_t /*destination_id*/, uint32_t source_track_id,
                                  float* const* channels, int num_channels, int num_frames,
                                  int64_t render_frame) noexcept override {
    if (channels == nullptr || num_channels <= 0 || num_frames <= 0 || render_frame < 0 ||
        render_frame >= frames_) {
      return;
    }
    auto it = stems_.find(source_track_id);
    std::vector<std::vector<float>>* stem = it != stems_.end() ? &it->second : &default_;
    const int64_t available = frames_ - render_frame;
    const int frames = static_cast<int>(std::min<int64_t>(num_frames, available));
    const int channel_count = std::min(num_channels_, num_channels);
    for (int ch = 0; ch < channel_count; ++ch) {
      const float* source = channels[ch];
      float* destination = (*stem)[static_cast<size_t>(ch)].data() + render_frame;
      if (source == nullptr) continue;
      for (int frame = 0; frame < frames; ++frame) destination[frame] += source[frame];
    }
  }

  const std::vector<std::vector<float>>* stem(uint32_t track_id) const noexcept {
    const auto it = stems_.find(track_id);
    return it == stems_.end() ? nullptr : &it->second;
  }
  const std::vector<std::vector<float>>& default_stem() const noexcept { return default_; }

 private:
  std::vector<std::vector<float>> make_stem() const {
    return std::vector<std::vector<float>>(static_cast<size_t>(num_channels_),
                                           std::vector<float>(static_cast<size_t>(frames_), 0.0f));
  }
  void allocate(uint32_t track_id) { stems_.emplace(track_id, make_stem()); }

  int num_channels_ = 0;
  int64_t frames_ = 0;
  std::map<uint32_t, std::vector<std::vector<float>>> stems_;
  std::vector<std::vector<float>> default_;
};

bool render_midi_source_stems(const arr::CompiledTimeline& timeline,
                              const std::vector<HostedInstrument>& instruments, double sample_rate,
                              int block_size, int64_t render_frames, MidiSourceStemSink* sink) {
  if (sink == nullptr) return false;
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

  std::vector<sonare::engine::TrackLaneConfig> lanes;
  lanes.reserve(track_ids.size());
  for (uint32_t track_id : track_ids) lanes.emplace_back(track_id);
  if (!engine.set_track_lanes(std::move(lanes))) return false;

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
    std::vector<float> prime_l(static_cast<size_t>(block_size), 0.0f);
    std::vector<float> prime_r(static_cast<size_t>(block_size), 0.0f);
    float* prime[] = {prime_l.data(), prime_r.data()};
    engine.process(prime, 2, block_size);
    engine.settle_parameters();
  }
  engine.set_instrument_source_render_sink(sink);
  std::vector<std::vector<float>> discard(
      2, std::vector<float>(static_cast<size_t>(render_frames), 0.0f));
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

void add_stem(std::vector<std::vector<float>>* destination,
              const std::vector<std::vector<float>>& source) {
  if (destination == nullptr) return;
  const size_t channels = std::min(destination->size(), source.size());
  for (size_t ch = 0; ch < channels; ++ch) {
    const size_t frames = std::min((*destination)[ch].size(), source[ch].size());
    for (size_t frame = 0; frame < frames; ++frame) (*destination)[ch][frame] += source[ch][frame];
  }
}
#endif

#if defined(SONARE_WITH_MIXING)
sonare::mixing::AutomationCurveType to_mixing_curve(sonare::automation::CurveType curve) noexcept {
  return static_cast<sonare::mixing::AutomationCurveType>(static_cast<int>(curve));
}

// Resolved Track->Strip routing for a channel-strip bounce: the scene strip ids
// in their canonical order (= the mixer's process_stereo input index order),
// each strip's set of source tracks, and the union of all bound tracks.
struct MixerRouting {
  std::vector<std::string> strip_ids;
  std::vector<std::set<uint32_t>> strip_tracks;  // index-aligned with strip_ids
  std::set<uint32_t> bound_tracks;
};

struct MixerDeleter {
  void operator()(SonareMixer* mixer) const noexcept {
    if (mixer != nullptr) sonare_mixer_destroy(mixer);
  }
};

using MixerPtr = std::unique_ptr<SonareMixer, MixerDeleter>;

bool timeline_has_unbound_tracks(const arr::CompiledTimeline& timeline,
                                 const MixerRouting& routing) {
  const auto unbound = [&](uint32_t track_id) { return routing.bound_tracks.count(track_id) == 0; };
  for (const auto& clip : timeline.audio_clips) {
    if (unbound(clip.track_id)) return true;
  }
  for (const auto& clip : timeline.midi_clips) {
    if (unbound(clip.track_id)) return true;
  }
  return false;
}

std::string unique_direct_strip_id(const sonare::mixing::api::Scene& scene) {
  constexpr const char* kBase = "__sonare_direct_master__";
  auto exists = [&](const std::string& candidate) {
    return std::any_of(
        scene.strips.begin(), scene.strips.end(),
        [&](const sonare::mixing::api::Strip& strip) { return strip.id == candidate; });
  };
  if (!exists(kBase)) return kBase;
  for (int suffix = 1;; ++suffix) {
    std::string candidate = std::string(kBase) + "_" + std::to_string(suffix);
    if (!exists(candidate)) return candidate;
  }
}

// Resolves the compiled timeline's mixer bindings against its scene. Only
// bindings whose strip actually exists in the scene are honored; a binding to a
// missing strip leaves its track unbound (rendered dry into the master).
MixerRouting resolve_mixer_routing(const arr::CompiledTimeline& timeline) {
  MixerRouting routing;
  std::map<std::string, size_t> index_of;
  for (const auto& strip : timeline.mixer.scene.strips) {
    index_of.emplace(strip.id, routing.strip_ids.size());
    routing.strip_ids.push_back(strip.id);
    routing.strip_tracks.emplace_back();
  }
  for (const auto& binding : timeline.mixer.bindings) {
    const auto it = index_of.find(binding.strip_id);
    if (it == index_of.end()) continue;  // strip not in scene -> track stays unbound
    routing.strip_tracks[it->second].insert(binding.track_id);
    routing.bound_tracks.insert(binding.track_id);
  }
  return routing;
}

void schedule_mixer_automation(const arr::CompiledTimeline& timeline, const MixerRouting& routing,
                               double sample_rate, SonareMixer* mixer) {
  if (mixer == nullptr) return;
  std::map<uint32_t, std::string> strip_for_track;
  for (size_t i = 0; i < routing.strip_ids.size(); ++i) {
    for (uint32_t track_id : routing.strip_tracks[i]) {
      strip_for_track.emplace(track_id, routing.strip_ids[i]);
    }
  }

  sonare::transport::TempoMap tempo_map;
  tempo_map.prepare(sample_rate);
  if (!timeline.tempo_segments.empty()) {
    tempo_map.set_segments(timeline.tempo_segments);
  }
  if (!timeline.time_signatures.empty()) {
    tempo_map.set_time_signatures(timeline.time_signatures);
  }

  for (const auto& binding : timeline.mixer.automation_bindings) {
    const auto route = strip_for_track.find(binding.track_id);
    if (route == strip_for_track.end()) continue;
    SonareStrip* strip = sonare_mixer_strip_by_id(mixer, route->second.c_str());
    if (strip == nullptr) continue;
    const auto& lane = binding.lane;
    const auto& points = lane.points();
    if (points.empty()) continue;
    const float initial_value = lane.value_at(0.0);

    switch (lane.target_param_id()) {
      case sonare::engine::MixingRuntime::kFaderDb:
        strip->strip.set_fader_db(initial_value);
        break;
      case sonare::engine::MixingRuntime::kPan:
        strip->strip.set_pan(initial_value);
        break;
      case sonare::engine::MixingRuntime::kWidth:
        strip->strip.set_width(initial_value);
        break;
      default:
        break;
    }

    const auto schedule = [&](int64_t sample, float value,
                              sonare::mixing::AutomationCurveType curve) {
      switch (lane.target_param_id()) {
        case sonare::engine::MixingRuntime::kFaderDb:
          return strip->strip.schedule_fader_automation(sample, value, curve);
        case sonare::engine::MixingRuntime::kPan:
          return strip->strip.schedule_pan_automation(sample, value, curve);
        case sonare::engine::MixingRuntime::kWidth:
          return strip->strip.schedule_width_automation(sample, value, curve);
        default:
          return false;
      }
    };

    schedule(0, initial_value, sonare::mixing::AutomationCurveType::Hold);
    for (const auto& point : points) {
      const int64_t sample = std::max<int64_t>(0, tempo_map.ppq_to_sample(point.ppq));
      schedule(sample, point.value, to_mixing_curve(point.curve_to_next));
    }
  }
}

SonareMixer* create_timeline_mixer(const arr::CompiledTimeline& timeline,
                                   const MixerRouting& routing, double sample_rate, int block_size,
                                   const std::string& direct_strip_id = {}) {
  sonare::mixing::api::Scene scene = timeline.mixer.scene;
  if (!direct_strip_id.empty()) {
    sonare::mixing::api::Strip direct_strip;
    direct_strip.id = direct_strip_id;
    scene.strips.push_back(std::move(direct_strip));
  }
  const std::string scene_json = sonare::mixing::api::scene_to_json(scene);
  SonareMixer* mixer =
      sonare_mixer_from_scene_json(scene_json.c_str(), static_cast<int>(sample_rate), block_size);
  if (mixer == nullptr) return nullptr;
  sonare_c_mixing_detail::build_and_compile(mixer);
  schedule_mixer_automation(timeline, routing, sample_rate, mixer);

  // Snap each strip's fader/input-trim/width/pan smoothers to their steady-state
  // targets so the mixer summing pass opens at the configured gain instead of
  // fading in from the previous value over the first ~5 ms block. This keeps the
  // offline bounce deterministic; without it a non-default static fader ramps in
  // on the master. (See ChannelStrip::settle for the full set of snapped stages.)
  for (const std::string& strip_id : routing.strip_ids) {
    if (SonareStrip* strip = sonare_mixer_strip_by_id(mixer, strip_id.c_str())) {
      strip->strip.settle();
    }
  }
  if (!direct_strip_id.empty()) {
    if (SonareStrip* strip = sonare_mixer_strip_by_id(mixer, direct_strip_id.c_str())) {
      strip->strip.settle();
    }
  }
  return mixer;
}

struct MixerLatencyTail {
  int latency_samples = 0;
  int tail_samples = 0;
};

MixerLatencyTail mixer_latency_tail_for_timeline(const arr::CompiledTimeline& timeline,
                                                 const MixerRouting& routing, double sample_rate,
                                                 int block_size, MixerPtr* out_mixer = nullptr) {
  MixerPtr mixer(create_timeline_mixer(timeline, routing, sample_rate, block_size));
  if (!mixer) return {};
  MixerLatencyTail result;
  int latency = 0;
  int tail = 0;
  (void)sonare_mixer_latency_samples(mixer.get(), &latency);
  (void)sonare_mixer_tail_samples(mixer.get(), &tail);
  result.latency_samples = std::max(latency, 0);
  result.tail_samples = std::max(tail, 0);
  if (out_mixer != nullptr) {
    *out_mixer = std::move(mixer);
  }
  return result;
}

// Channel-strip bounce: renders each bound track as an isolated dry
// stereo stem and sums the stems through the scene's mixer so per-track EQ,
// inserts, pan, fader, sends and buses are applied. Tracks bound to no scene
// strip are rendered into a separate dry stem and summed straight into the
// master. `frames`/`pdc` are precomputed by the caller; stems are aligned to
// [0, frames) after dropping the leading PDC fill.
SonareError bounce_through_mixer(const arr::CompiledTimeline& timeline,
                                 const std::vector<HostedInstrument>& instruments,
                                 const MixerRouting& routing, double sample_rate, int block_size,
                                 int num_channels, int64_t frames, int64_t pdc,
                                 int64_t mixer_input_frames, float** out_interleaved,
                                 size_t* out_len, SonareMixer* prebuilt_mixer = nullptr) {
  MixerPtr mixer_owner(prebuilt_mixer);
  MixerRouting effective_routing = routing;
  bool shared_hosts_source_aware = true;
  const bool shared_midi_destination =
      has_shared_hosted_midi_destination(timeline, instruments, &shared_hosts_source_aware);
  // A shared-destination render has a destination-scoped residual target
  // (SF2 effects / native bodies). Reserve the direct identity strip for it
  // even when every authored track is bound to a scene strip.
  const bool route_direct =
      timeline_has_unbound_tracks(timeline, routing) || shared_midi_destination;
  const std::string direct_strip_id =
      route_direct ? unique_direct_strip_id(timeline.mixer.scene) : std::string();
  if (route_direct) {
    effective_routing.strip_ids.push_back(direct_strip_id);
    effective_routing.strip_tracks.emplace_back();
  }

  // Build the mixer before rendering stems so we know how many extra internal
  // frames are needed to compensate master-output latency.
  if (!mixer_owner) {
    mixer_owner.reset(
        create_timeline_mixer(timeline, routing, sample_rate, block_size, direct_strip_id));
  }
  if (!mixer_owner) return SONARE_ERROR_INVALID_STATE;
  int mixer_latency = 0;
  (void)sonare_mixer_latency_samples(mixer_owner.get(), &mixer_latency);
  mixer_latency = std::max(mixer_latency, 0);
  const int64_t mixer_render_frames = frames + static_cast<int64_t>(mixer_latency);

  const size_t strip_count = effective_routing.strip_ids.size();
  // Stems are stereo (the mixer is stereo): one per strip plus one direct stem.
  const uint64_t stem_floats = static_cast<uint64_t>(mixer_render_frames) * 2u * strip_count;
  if (stem_floats > kMaxBufferSize) return SONARE_ERROR_INVALID_PARAMETER;

  const int64_t render_frames = mixer_render_frames + pdc;
  std::unique_ptr<MidiSourceStemSink> midi_source_stems;
  if (shared_midi_destination) {
    if (!shared_hosts_source_aware || pdc != 0) {
      set_last_error(
          "a MIDI destination shared by channel strips requires source-aware, zero-latency "
          "instruments for project bounce");
      return SONARE_ERROR_NOT_SUPPORTED;
    }
    std::set<uint32_t> midi_tracks;
    for (const sonare::midi::MidiClipSchedule& clip : timeline.midi_clips) {
      if (clip.track_id != 0) midi_tracks.insert(clip.track_id);
    }
    midi_source_stems = std::make_unique<MidiSourceStemSink>(midi_tracks, 2, render_frames);
    if (!render_midi_source_stems(timeline, instruments, sample_rate, block_size, render_frames,
                                  midi_source_stems.get())) {
      set_last_error("could not render source-track MIDI stems for shared channel strips");
      return SONARE_ERROR_NOT_SUPPORTED;
    }
  }
  auto stem_aligned = [&](const std::function<bool(uint32_t)>& keep) {
    std::vector<std::vector<float>> ch;
    render_timeline(timeline, keep, instruments, sample_rate, block_size, /*num_channels=*/2,
                    render_frames, &ch, /*include_audio=*/true,
                    /*include_midi=*/midi_source_stems == nullptr);
    for (auto& c : ch) {
      if (pdc > 0) c.erase(c.begin(), c.begin() + pdc);
      c.resize(static_cast<size_t>(mixer_render_frames));
    }
    return ch;
  };

  // One stereo stem per strip (silent if the strip has no source track).
  std::vector<std::vector<std::vector<float>>> stems;
  stems.reserve(strip_count);
  for (size_t i = 0; i < effective_routing.strip_tracks.size(); ++i) {
    const std::set<uint32_t>& tracks = effective_routing.strip_tracks[i];
    if (tracks.empty()) {
      stems.emplace_back(2, std::vector<float>(static_cast<size_t>(mixer_render_frames), 0.0f));
      continue;
    }
    stems.push_back(stem_aligned([&tracks](uint32_t t) { return tracks.count(t) != 0; }));
    if (midi_source_stems) {
      for (uint32_t track_id : tracks) {
        if (const auto* stem = midi_source_stems->stem(track_id)) add_stem(&stems.back(), *stem);
      }
    }
  }

  // Direct stem: every track NOT bound to a scene strip (dry to master).
  if (route_direct) {
    const std::set<uint32_t>& bound = routing.bound_tracks;
    std::vector<std::vector<float>> direct =
        stem_aligned([&bound](uint32_t t) { return bound.count(t) == 0; });
    if (midi_source_stems) {
      std::set<uint32_t> direct_midi_tracks;
      for (const sonare::midi::MidiClipSchedule& clip : timeline.midi_clips) {
        if (bound.count(clip.track_id) == 0) direct_midi_tracks.insert(clip.track_id);
      }
      for (uint32_t track_id : direct_midi_tracks) {
        if (const auto* stem = midi_source_stems->stem(track_id)) add_stem(&direct, *stem);
      }
      add_stem(&direct, midi_source_stems->default_stem());
    }
    stems.back() = std::move(direct);
  }

  // Sum the strip stems through the mixer block by block.
  std::vector<float> master_l(static_cast<size_t>(mixer_render_frames), 0.0f);
  std::vector<float> master_r(static_cast<size_t>(mixer_render_frames), 0.0f);
  std::vector<const float*> in_l(strip_count, nullptr);
  std::vector<const float*> in_r(strip_count, nullptr);
  SonareError err = SONARE_OK;
  const int64_t input_frames = std::clamp<int64_t>(mixer_input_frames, 0, mixer_render_frames);
  for (int64_t off = 0; off < input_frames; off += block_size) {
    const size_t n = static_cast<size_t>(std::min<int64_t>(block_size, input_frames - off));
    for (size_t i = 0; i < strip_count; ++i) {
      in_l[i] = stems[i][0].data() + off;
      in_r[i] = stems[i][1].data() + off;
    }
    err = sonare_mixer_process_stereo(mixer_owner.get(), in_l.data(), in_r.data(), strip_count,
                                      master_l.data() + off, master_r.data() + off, n);
    if (err != SONARE_OK) break;
  }
  for (int64_t off = input_frames; err == SONARE_OK && off < mixer_render_frames;
       off += block_size) {
    const size_t n = static_cast<size_t>(std::min<int64_t>(block_size, mixer_render_frames - off));
    err = sonare_mixer_drain_tail_stereo(mixer_owner.get(), master_l.data() + off,
                                         master_r.data() + off, n);
  }
  if (err != SONARE_OK) return err;

  // Interleave into the requested channel count: mono downmixes the stereo
  // master; channels beyond stereo are left silent.
  const size_t total = static_cast<size_t>(frames) * static_cast<size_t>(num_channels);
  std::unique_ptr<float[]> interleaved(new float[total]);
  for (int64_t f = 0; f < frames; ++f) {
    const size_t source = static_cast<size_t>(f + mixer_latency);
    const float l = master_l[source];
    const float r = master_r[source];
    for (int ch = 0; ch < num_channels; ++ch) {
      float v = 0.0f;
      if (num_channels == 1) {
        v = 0.5f * (l + r);
      } else if (ch == 0) {
        v = l;
      } else if (ch == 1) {
        v = r;
      }
      interleaved[static_cast<size_t>(f) * num_channels + ch] = v;
    }
  }
  *out_interleaved = interleaved.release();
  *out_len = total;
  return SONARE_OK;
}
#endif  // SONARE_WITH_MIXING

// Shared bounce core: validates options, compiles, registers any hosted
// instruments per destination, renders offline, and writes the interleaved
// result. `instruments` may be empty for a silent MIDI bounce. When
// opts.total_frames <= 0 the render length is auto-derived from the compiled
// timeline (plus the longest hosted-instrument release tail) so a caller can
// bounce a MIDI-only arrangement without computing a length by hand. When the
// project routes tracks through mixer channel strips (under SONARE_WITH_MIXING)
// the render fans out into per-track stems summed through the scene's mixer so
// channel-strip FX are applied; otherwise a single offline render is used.
// Returns through the SONARE_C_TRY/CATCH guard of the caller.
SonareError do_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                              const std::vector<HostedInstrument>& instruments,
                              float** out_interleaved, size_t* out_len) {
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  if (!project || !out_interleaved || !out_len) return SONARE_ERROR_INVALID_PARAMETER;
  project->last_bounce_diagnostics.clear();
  project->last_bounce_has_timeline = false;

  SonareProjectBounceOptions opts{};
  if (options) opts = *options;
  const int block_size = opts.block_size > 0 ? opts.block_size : 128;
  const int num_channels = opts.num_channels > 0 ? opts.num_channels : 2;
  if (block_size <= 0 || num_channels <= 0) return SONARE_ERROR_INVALID_PARAMETER;
  // The project bounce sums to a stereo master and only writes a mono downmix or
  // the stereo pair; any wider count would leave the extra planes silent. Reject
  // unsupported widths up front, matching engine-bounce (which rejects channel
  // counts that do not map to a speaker layout) instead of emitting dead planes.
  if (num_channels != 1 && num_channels != 2) return SONARE_ERROR_INVALID_PARAMETER;
  const double project_sr = project->history.project().sample_rate();
  const double sample_rate =
      opts.sample_rate > 0 ? static_cast<double>(opts.sample_rate) : project_sr;
  if (!finite_positive(sample_rate) || sample_rate < kMinSampleRate ||
      sample_rate > kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (opts.sample_rate > 0 && std::abs(sample_rate - project_sr) > 1.0e-6) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (opts.instrument_latency_samples < 0) return SONARE_ERROR_INVALID_PARAMETER;
  for (const HostedInstrument& hosted : instruments) {
    if (hosted.instrument == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  }

  arr::CompileConfig config;
  config.instrument_latency_samples = opts.instrument_latency_samples;
  arr::CompileResult compiled = arr::compile(
      project->history.project(), project->history.midi_content(), project->audio, config);
  project->last_bounce_diagnostics = compiled.diagnostics;
  project->last_bounce_has_timeline = compiled.timeline.has_value();
  if (!compiled.timeline.has_value()) return SONARE_ERROR_INVALID_STATE;

#if defined(SONARE_WITH_MIXING)
  const MixerRouting routing = resolve_mixer_routing(*compiled.timeline);
  const bool mixer_route_direct = timeline_has_unbound_tracks(*compiled.timeline, routing);
  MixerPtr reusable_mixer;
#endif

  // Validate the hosted instruments and derive the project's PDC + longest tail
  // on a throwaway engine (latency depends only on the registered instruments,
  // not on the timeline), so both the single-render and the per-track-stem paths
  // share one render length and delay.
  int64_t instrument_tail = 0;
  int64_t pdc = 0;
  {
    sonare::engine::RealtimeEngine probe;
    probe.prepare(sample_rate, block_size);
    for (const HostedInstrument& hosted : instruments) {
      if (!probe.set_midi_instrument(hosted.destination_id, hosted.instrument)) {
        return SONARE_ERROR_INVALID_PARAMETER;  // more instruments than the rack holds
      }
      instrument_tail = std::max<int64_t>(instrument_tail, hosted.instrument->tail_samples());
    }
    pdc = static_cast<int64_t>(probe.midi_instrument_latency_samples());
    for (const HostedInstrument& hosted : instruments) {
      probe.set_midi_instrument(hosted.destination_id, nullptr);
    }
  }

  // Determine the render length: caller-supplied, or auto-derived from the
  // arrangement (musical end + the longest instrument release tail).
  const int64_t arrangement_frames = arrangement_end_frames(*compiled.timeline);
  int64_t mixer_input_frames = arrangement_frames;
  if (mixer_input_frames > 0) mixer_input_frames += instrument_tail;
  int64_t frames = opts.total_frames;
  if (frames <= 0) {
    frames = arrangement_frames;
    if (frames > 0) frames += instrument_tail;
#if defined(SONARE_WITH_MIXING)
    if (frames > 0 && !routing.bound_tracks.empty()) {
      MixerPtr* reusable = mixer_route_direct ? nullptr : &reusable_mixer;
      const MixerLatencyTail mixer_delay = mixer_latency_tail_for_timeline(
          *compiled.timeline, routing, sample_rate, block_size, reusable);
      frames += mixer_delay.tail_samples;
    }
#endif
  }
#if defined(SONARE_WITH_MIXING)
  // When the caller fixes the window with an explicit total_frames, feed the
  // per-track stems through the mixer across the whole window rather than
  // stopping at the arrangement's musical end. The drain-tail shortcut (zero
  // input past mixer_input_frames) is only valid for the auto-length branch
  // above, where everything past the arrangement plus the instrument release
  // tail is guaranteed silent; for an explicit length it would replace a real
  // instrument/stem tail with the mixer's decaying silence. bounce_through_mixer
  // clamps this to the render window, so this only ever lifts the input span.
  if (opts.total_frames > 0) {
    mixer_input_frames = std::max(mixer_input_frames, frames);
  }
#endif
  if (frames < 0 ||
      static_cast<uint64_t>(frames) >
          std::numeric_limits<size_t>::max() / static_cast<uint64_t>(num_channels) ||
      static_cast<uint64_t>(frames) * static_cast<uint64_t>(num_channels) > kMaxBufferSize) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  const size_t total = static_cast<size_t>(frames) * static_cast<size_t>(num_channels);
  if (frames == 0) {
    // Empty arrangement (or zero-length request): a valid empty render.
    *out_interleaved = new float[1];
    *out_len = 0;
    return SONARE_OK;
  }

#if defined(SONARE_WITH_MIXING)
  // Per-track channel-strip bounce when the project binds tracks to scene strips.
  if (!routing.bound_tracks.empty()) {
    return bounce_through_mixer(*compiled.timeline, instruments, routing, sample_rate, block_size,
                                num_channels, frames, pdc, mixer_input_frames, out_interleaved,
                                out_len, reusable_mixer.release());
  }
#endif

  // Single-render path: no channel strips bound (output identical to the legacy
  // bounce). Plugin-delay compensation renders `pdc` extra frames and drops the
  // leading delay-line fill so musical time [0, frames) aligns to output 0.
  const int64_t render_frames = frames + pdc;
  std::vector<std::vector<float>> channels;
  render_timeline(*compiled.timeline, /*keep=*/{}, instruments, sample_rate, block_size,
                  /*num_channels=*/2, render_frames, &channels);

  std::unique_ptr<float[]> interleaved(new float[total]);
  for (int64_t frame = 0; frame < frames; ++frame) {
    const float l = channels[0][static_cast<size_t>(frame + pdc)];
    const float r = channels[1][static_cast<size_t>(frame + pdc)];
    for (int ch = 0; ch < num_channels; ++ch) {
      float v = 0.0f;
      if (num_channels == 1) {
        v = 0.5f * (l + r);
      } else if (ch == 0) {
        v = l;
      } else if (ch == 1) {
        v = r;
      }
      interleaved[static_cast<size_t>(frame) * num_channels + ch] = v;
    }
  }
  *out_interleaved = interleaved.release();
  *out_len = total;
  return SONARE_OK;
}

// Maps the public built-in waveform ordinal to the core enum (out-of-range
// values fall back to sine via clamp_synth_config).
sonare::midi::BuiltinSynthConfig synth_config_from_c(const SonareBuiltinSynthConfig& c) noexcept {
  sonare::midi::BuiltinSynthConfig cfg;
  cfg.waveform = static_cast<sonare::midi::SynthWaveform>(c.waveform);
  cfg.gain = c.gain;
  cfg.attack_ms = c.attack_ms;
  cfg.decay_ms = c.decay_ms;
  cfg.sustain = c.sustain;
  cfg.release_ms = c.release_ms;
  cfg.polyphony = c.polyphony;
  return sonare::midi::clamp_synth_config(cfg);
}

// Maps the public versioned SF2 patch to the player config ("0 => default";
// struct_version 0/1 preserve the original layout; version 2 enables the
// model-first field. Anything newer is rejected by the
// caller). The player clamps polyphony itself.
sonare::midi::synth::Sf2PlayerConfig sf2_config_from_c(
    const SonareSf2InstrumentConfig& c) noexcept {
  sonare::midi::synth::Sf2PlayerConfig cfg;
  if (c.gain > 0.0f) cfg.gain = c.gain;
  if (c.polyphony > 0) cfg.polyphony = c.polyphony;
  if (c.struct_version >= 2) {
    cfg.prefer_model_for_modeled_families = c.prefer_model_for_modeled_families != 0;
  }
  // Wire the GS insertion-effect (EFX) path: the SF2 player never depends on the
  // mastering factory itself, so the host injects it. An EFX SysEx on the
  // compiled timeline then installs its inserts and rings through the per-part
  // bus. The bounce is single-threaded and offline, so pending EFX changes are
  // realised inline in process() (the allocation is safe off the audio thread).
  cfg.insert_factory = [](std::string_view name,
                          std::string_view json) -> std::unique_ptr<sonare::rt::ProcessorBase> {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  cfg.realize_efx_inline = true;
  return cfg;
}

}  // namespace
#endif

SonareError sonare_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                                  float** out_interleaved, size_t* out_len) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  return do_project_bounce(project, options, {}, out_interleaved, out_len);
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, options, out_interleaved, out_len);
#endif
}

SonareError sonare_project_bounce_with_instruments(SonareProject* project,
                                                   const SonareProjectBounceOptions* options,
                                                   const SonareInstrumentBinding* instruments,
                                                   size_t instrument_count, float** out_interleaved,
                                                   size_t* out_len) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  if (instrument_count > 0 && instruments == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  // Every callback instrument must supply a render function (the audio source).
  for (size_t i = 0; i < instrument_count; ++i) {
    if (instruments[i].callbacks.render == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  }
  std::vector<std::unique_ptr<CallbackInstrument>> owned;
  std::vector<HostedInstrument> hosted;
  owned.reserve(instrument_count);
  hosted.reserve(instrument_count);
  for (size_t i = 0; i < instrument_count; ++i) {
    owned.push_back(std::make_unique<CallbackInstrument>(instruments[i].callbacks));
    hosted.push_back({instruments[i].destination_id, owned.back().get()});
  }
  return do_project_bounce(project, options, hosted, out_interleaved, out_len);
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, options, instruments, instrument_count, out_interleaved,
                              out_len);
#endif
}

SonareError sonare_project_bounce_with_builtin_instruments(
    SonareProject* project, const SonareProjectBounceOptions* options,
    const SonareBuiltinInstrumentBinding* instruments, size_t instrument_count,
    float** out_interleaved, size_t* out_len) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  if (instrument_count > 0 && instruments == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  std::vector<std::unique_ptr<sonare::midi::BuiltinSynth>> owned;
  std::vector<HostedInstrument> hosted;
  owned.reserve(instrument_count);
  hosted.reserve(instrument_count);
  for (size_t i = 0; i < instrument_count; ++i) {
    owned.push_back(
        std::make_unique<sonare::midi::BuiltinSynth>(synth_config_from_c(instruments[i].config)));
    hosted.push_back({instruments[i].destination_id, owned.back().get()});
  }
  return do_project_bounce(project, options, hosted, out_interleaved, out_len);
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, options, instruments, instrument_count, out_interleaved,
                              out_len);
#endif
}

const char* sonare_synth_preset_names(void) {
#if defined(SONARE_WITH_ARRANGEMENT)
  static const std::string kNames = [] {
    std::string names;
    for (size_t i = 0; i < sonare::midi::synth::synth_preset_count(); ++i) {
      if (!names.empty()) names += '\n';
      names += sonare::midi::synth::synth_preset_at(i)->name;
    }
    return names;
  }();
  return kNames.c_str();
#else
  return "";
#endif
}

const char* sonare_synth_enum_names(int kind) {
#if defined(SONARE_WITH_ARRANGEMENT)
  static const std::string kEngineModes =
      "default\nsubtractive\nfm\nkarplus-strong\nmodal\nadditive\npercussion\npiano\npipe-organ\n"
      "bowed-string\nreed\nbrass\nflute\nplucked-string\nvocal\nfree-reed";
  static const std::string kWaveforms = "default\nsine\nsaw\nsquare\ntriangle\nnoise";
  static const std::string kFilterModels = "default\nsvf\nmoog-ladder\ndiode-ladder\nsallen-key";
  static const std::string kFilterOutputs = "default\nlowpass\nbandpass\nhighpass";
  static const std::string kBodyTypes =
      "default\nnone\nguitar\nviolin\nwood-tube\nbrass-bell\nvocal";
  static const std::string kModSources =
      "none\namp-env\nfilter-env\nlfo1\nlfo2\nvelocity\nkey-track\nmod-wheel\nrandom";
  static const std::string kModDestinations =
      "none\npitch-cents\ncutoff-cents\namp-gain\npan-units";

  switch (kind) {
    case SONARE_SYNTH_ENUM_ENGINE_MODE:
      return kEngineModes.c_str();
    case SONARE_SYNTH_ENUM_OSC_WAVEFORM:
      return kWaveforms.c_str();
    case SONARE_SYNTH_ENUM_FILTER_MODEL:
      return kFilterModels.c_str();
    case SONARE_SYNTH_ENUM_FILTER_OUTPUT:
      return kFilterOutputs.c_str();
    case SONARE_SYNTH_ENUM_BODY_TYPE:
      return kBodyTypes.c_str();
    case SONARE_SYNTH_ENUM_MOD_SOURCE:
      return kModSources.c_str();
    case SONARE_SYNTH_ENUM_MOD_DESTINATION:
      return kModDestinations.c_str();
    default:
      return "";
  }
#else
  (void)kind;
  return "";
#endif
}

SonareError sonare_synth_preset_patch(const char* name, SonareSynthPatch* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (!name || !out) return SONARE_ERROR_INVALID_PARAMETER;
  const sonare::midi::synth::SynthPreset* preset = sonare::midi::synth::find_synth_preset(name);
  if (preset == nullptr) {
    set_last_error("unknown synth preset name");
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  sonare_c_detail::synth_patch_to_c(*preset, out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(name, out);
#endif
}

SonareError sonare_project_bounce_with_synth_instruments(
    SonareProject* project, const SonareProjectBounceOptions* options,
    const SonareSynthInstrumentBinding* instruments, size_t instrument_count,
    float** out_interleaved, size_t* out_len) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  if (instrument_count > 0 && instruments == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  if (!project || !out_interleaved || !out_len) return SONARE_ERROR_INVALID_PARAMETER;
  std::vector<std::unique_ptr<sonare::midi::synth::NativeSynth>> owned;
  std::vector<HostedInstrument> hosted;
  owned.reserve(instrument_count);
  hosted.reserve(instrument_count);
  for (size_t i = 0; i < instrument_count; ++i) {
    sonare::midi::synth::NativeSynthConfig cfg;
    const char* error = nullptr;
    if (!sonare_c_detail::synth_config_from_patch_c(instruments[i].patch, &cfg, &error)) {
      set_last_error(error != nullptr ? error : "invalid synth patch");
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    owned.push_back(std::make_unique<sonare::midi::synth::NativeSynth>(cfg));
    hosted.push_back({instruments[i].destination_id, owned.back().get()});
  }
  return do_project_bounce(project, options, hosted, out_interleaved, out_len);
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, options, instruments, instrument_count, out_interleaved,
                              out_len);
#endif
}

SonareError sonare_project_bounce_with_sf2_instruments(
    SonareProject* project, const SonareProjectBounceOptions* options,
    const SonareSf2InstrumentBinding* instruments, size_t instrument_count, float** out_interleaved,
    size_t* out_len) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  if (out_interleaved) *out_interleaved = nullptr;
  if (out_len) *out_len = 0;
  if (instrument_count > 0 && instruments == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  if (!project) return SONARE_ERROR_INVALID_PARAMETER;
  // No loaded SoundFont is allowed: the player's NativeSynth GM fallback is
  // the data-free floor (every program still sounds; the manifest reports the
  // synth backend honestly).
  for (size_t i = 0; i < instrument_count; ++i) {
    if (instruments[i].config.struct_version > 2) return SONARE_ERROR_INVALID_PARAMETER;
  }
  std::vector<std::unique_ptr<sonare::midi::synth::Sf2Player>> owned;
  std::vector<HostedInstrument> hosted;
  owned.reserve(instrument_count);
  hosted.reserve(instrument_count);
  for (size_t i = 0; i < instrument_count; ++i) {
    auto player =
        std::make_unique<sonare::midi::synth::Sf2Player>(sf2_config_from_c(instruments[i].config));
    player->set_soundfont(project->soundfont);
    owned.push_back(std::move(player));
    hosted.push_back({instruments[i].destination_id, owned.back().get()});
  }
  return do_project_bounce(project, options, hosted, out_interleaved, out_len);
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, options, instruments, instrument_count, out_interleaved,
                              out_len);
#endif
}
