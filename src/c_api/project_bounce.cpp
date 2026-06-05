#include "c_api/project_internal.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include <functional>
#include <memory>
#include <set>

#include "c_api/synth_patch_common.h"
#include "midi/builtin_synth.h"
#include "midi/synth/sf2_player.h"
#include "midi/synth/synth_presets.h"
#if defined(SONARE_WITH_MIXING)
#include "c_api/mixing_internal.h"
#include "engine/mixing_runtime.h"
#include "mixing/api/scene.h"
#include "sonare_c_mixing.h"
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
                     std::vector<std::vector<float>>* channels) {
  arr::CompiledTimeline filtered = timeline;  // copy re-points marker name pointers
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
  const bool route_direct = timeline_has_unbound_tracks(timeline, routing);
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
  auto stem_aligned = [&](const std::function<bool(uint32_t)>& keep) {
    std::vector<std::vector<float>> ch;
    render_timeline(timeline, keep, instruments, sample_rate, block_size, /*num_channels=*/2,
                    render_frames, &ch);
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
  }

  // Direct stem: every track NOT bound to a scene strip (dry to master).
  if (route_direct) {
    const std::set<uint32_t>& bound = routing.bound_tracks;
    std::vector<std::vector<float>> direct =
        stem_aligned([&bound](uint32_t t) { return bound.count(t) == 0; });
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
  const double project_sr = project->history.project().sample_rate();
  const double sample_rate =
      opts.sample_rate > 0 ? static_cast<double>(opts.sample_rate) : project_sr;
  if (!finite_positive(sample_rate) || sample_rate < kMinSampleRate ||
      sample_rate > kMaxSampleRate) {
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
// struct_version 0/1 are the current layout, anything newer is rejected by the
// caller). The player clamps polyphony itself.
sonare::midi::synth::Sf2PlayerConfig sf2_config_from_c(
    const SonareSf2InstrumentConfig& c) noexcept {
  sonare::midi::synth::Sf2PlayerConfig cfg;
  if (c.gain > 0.0f) cfg.gain = c.gain;
  if (c.polyphony > 0) cfg.polyphony = c.polyphony;
  return cfg;
}

}  // namespace
#endif

SonareError sonare_project_bounce(SonareProject* project, const SonareProjectBounceOptions* options,
                                  float** out_interleaved, size_t* out_len) {
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
      "default\nsubtractive\nfm\nkarplus-strong\nmodal\nadditive\npercussion\npiano";
  static const std::string kWaveforms = "default\nsine\nsaw\nsquare\ntriangle\nnoise";
  static const std::string kFilterModels = "default\nsvf\nmoog-ladder\ndiode-ladder\nsallen-key";
  static const std::string kFilterOutputs = "default\nlowpass\nbandpass\nhighpass";
  static const std::string kBodyTypes = "default\nnone\nguitar\nviolin\nwood-tube";
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
    if (instruments[i].config.struct_version > 1) return SONARE_ERROR_INVALID_PARAMETER;
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
