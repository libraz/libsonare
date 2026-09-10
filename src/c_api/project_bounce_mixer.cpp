#include "c_api/project_bounce_mixer.h"

#if defined(SONARE_WITH_ARRANGEMENT) && defined(SONARE_WITH_MIXING)
#include <algorithm>
#include <functional>
#include <map>

#include "c_api/mixing_internal.h"
#include "c_api/project_bounce_stems.h"
#include "engine/mixing_runtime.h"
#include "mixing/api/scene.h"

namespace sonare_c_bounce_detail {
namespace {

void add_stem(std::vector<std::vector<float>>* destination,
              const std::vector<std::vector<float>>& source) {
  if (destination == nullptr) return;
  const size_t channels = std::min(destination->size(), source.size());
  for (size_t ch = 0; ch < channels; ++ch) {
    const size_t frames = std::min((*destination)[ch].size(), source[ch].size());
    for (size_t frame = 0; frame < frames; ++frame) (*destination)[ch][frame] += source[ch][frame];
  }
}

sonare::mixing::AutomationCurveType to_mixing_curve(sonare::automation::CurveType curve) noexcept {
  return static_cast<sonare::mixing::AutomationCurveType>(static_cast<int>(curve));
}

}  // namespace

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

namespace {

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

}  // namespace

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

bool has_error_diagnostic(const std::vector<arr::Diagnostic>& diagnostics) {
  return std::any_of(diagnostics.begin(), diagnostics.end(), [](const arr::Diagnostic& d) {
    return d.severity == arr::Diagnostic::Severity::kError;
  });
}

namespace {

// Installs the compiled opaque automation lanes on the scene strips.
//
// A strip automation lane is a bounded ring, so a project lane with more
// breakpoints than it holds cannot be installed in full. Every push result is
// inspected and a rejection becomes a diagnostic naming the lane: the curve
// would otherwise render frozen at the last accepted breakpoint with nothing to
// tell the caller the ramp was cut short. `out_diagnostics` may be null for the
// probe mixers whose only job is to report latency.
void schedule_mixer_automation(const arr::CompiledTimeline& timeline, const MixerRouting& routing,
                               double sample_rate, SonareMixer* mixer,
                               std::vector<arr::Diagnostic>* out_diagnostics) {
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
    // Typed track fader/pan lanes are applied once by TrackMixerRuntime through
    // the reserved engine namespace. Scheduling them on the scene strip too
    // would apply the same automation a second time (notably -6 dB -> -12 dB).
    // Legacy opaque ids retain their historical strip-scheduler behavior.
    if (lane.target_kind() != sonare::automation::AutomationTargetKind::kOpaque) continue;
    if (!sonare::engine::MixingRuntime::is_supported_parameter(lane.target_param_id())) continue;
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
          return strip->strip.schedule_fader_automation_result(sample, value, curve);
        case sonare::engine::MixingRuntime::kPan:
          return strip->strip.schedule_pan_automation_result(sample, value, curve);
        case sonare::engine::MixingRuntime::kWidth:
          return strip->strip.schedule_width_automation_result(sample, value, curve);
        default:
          return sonare::mixing::AutomationPushResult::NonMonotonic;
      }
    };

    bool lane_full = false;
    if (schedule(0, initial_value, sonare::mixing::AutomationCurveType::Hold) ==
        sonare::mixing::AutomationPushResult::Full) {
      lane_full = true;
    }
    for (const auto& point : points) {
      if (lane_full) break;
      const int64_t sample = std::max<int64_t>(0, tempo_map.ppq_to_sample(point.ppq));
      if (schedule(sample, point.value, to_mixing_curve(point.curve_to_next)) ==
          sonare::mixing::AutomationPushResult::Full) {
        lane_full = true;
      }
    }
    if (lane_full && out_diagnostics != nullptr) {
      const char* parameter = "automation";
      switch (lane.target_param_id()) {
        case sonare::engine::MixingRuntime::kFaderDb:
          parameter = "fader";
          break;
        case sonare::engine::MixingRuntime::kPan:
          parameter = "pan";
          break;
        case sonare::engine::MixingRuntime::kWidth:
          parameter = "width";
          break;
        default:
          break;
      }
      arr::Diagnostic diagnostic;
      diagnostic.code = arr::Diagnostic::Code::kAutomationLaneCapacity;
      diagnostic.severity = arr::Diagnostic::Severity::kError;
      diagnostic.target_id = binding.track_id;
      diagnostic.message = std::string("the ") + parameter + " automation lane on channel strip '" +
                           route->second + "' has " + std::to_string(points.size()) +
                           " breakpoints, more than the strip's automation lane holds";
      out_diagnostics->push_back(std::move(diagnostic));
    }
  }
}

SonareMixer* create_timeline_mixer(const arr::CompiledTimeline& timeline,
                                   const MixerRouting& routing, double sample_rate, int block_size,
                                   const std::string& direct_strip_id = {},
                                   std::vector<arr::Diagnostic>* out_diagnostics = nullptr) {
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
  schedule_mixer_automation(timeline, routing, sample_rate, mixer, out_diagnostics);

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

}  // namespace

MixerLatencyTail mixer_latency_tail_for_timeline(const arr::CompiledTimeline& timeline,
                                                 const MixerRouting& routing, double sample_rate,
                                                 int block_size, MixerPtr* out_mixer,
                                                 std::vector<arr::Diagnostic>* out_diagnostics) {
  MixerPtr mixer(create_timeline_mixer(timeline, routing, sample_rate, block_size,
                                       /*direct_strip_id=*/{}, out_diagnostics));
  if (!mixer) return {};
  MixerLatencyTail result;
  int latency = 0;
  int tail = 0;
  if (sonare_mixer_latency_samples(mixer.get(), &latency) != SONARE_OK ||
      sonare_mixer_tail_samples(mixer.get(), &tail) != SONARE_OK || latency < 0 || tail < 0) {
    return result;
  }
  result.latency_samples = latency;
  result.tail_samples = tail;
  result.valid = true;
  if (out_mixer != nullptr) {
    *out_mixer = std::move(mixer);
  }
  return result;
}

SonareError bounce_through_mixer(const arr::CompiledTimeline& timeline,
                                 const std::vector<HostedInstrument>& instruments,
                                 const MixerRouting& routing, double sample_rate, int block_size,
                                 int num_channels, int64_t frames, int64_t pdc,
                                 int64_t mixer_input_frames, float** out_interleaved,
                                 size_t* out_len, SonareMixer* prebuilt_mixer,
                                 std::vector<arr::Diagnostic>* out_diagnostics) {
  size_t total = 0;
  if (num_channels <= 0 ||
      !checked_frame_shape(frames, static_cast<size_t>(num_channels), &total) || pdc < 0 ||
      mixer_input_frames < 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
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
  // frames are needed to compensate master-output latency. A prebuilt mixer is
  // reusable only when it holds exactly the strips this routing feeds; anything
  // else is discarded and rebuilt so the input count handed to
  // sonare_mixer_process_stereo always matches the mixer's strip count.
  if (mixer_owner &&
      sonare_mixer_strip_count(mixer_owner.get()) != effective_routing.strip_ids.size()) {
    mixer_owner.reset();
  }
  if (!mixer_owner) {
    mixer_owner.reset(create_timeline_mixer(timeline, routing, sample_rate, block_size,
                                            direct_strip_id, out_diagnostics));
  }
  if (!mixer_owner) return SONARE_ERROR_INVALID_STATE;
  // An automation lane that did not fit its strip is caught here, before any
  // stem is rendered, so the caller gets the diagnostic instead of audio whose
  // automation curve froze partway through.
  if (out_diagnostics != nullptr && has_error_diagnostic(*out_diagnostics)) {
    return SONARE_ERROR_INVALID_STATE;
  }
  int mixer_latency = 0;
  if (sonare_mixer_latency_samples(mixer_owner.get(), &mixer_latency) != SONARE_OK ||
      mixer_latency < 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  int64_t mixer_render_frames = 0;
  if (!checked_nonnegative_add(frames, static_cast<int64_t>(mixer_latency), &mixer_render_frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  const size_t strip_count = effective_routing.strip_ids.size();
  // Stems are stereo (the mixer is stereo): one per strip plus one direct stem.
  size_t stem_floats = 0;
  if (!checked_frame_shape(mixer_render_frames, 2, strip_count, &stem_floats)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  int64_t render_frames = 0;
  if (!checked_nonnegative_add(mixer_render_frames, pdc, &render_frames)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  size_t render_floats = 0;
  if (!checked_frame_shape(render_frames, 2, &render_floats)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  size_t mixer_frame_count = 0;
  size_t master_floats = 0;
  if (!checked_frame_count(mixer_render_frames, &mixer_frame_count) ||
      !checked_frame_shape(mixer_render_frames, 2, &master_floats)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
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
    size_t midi_source_floats = 0;
    size_t render_frame_count = 0;
    if (!checked_midi_source_stem_shape(midi_tracks.size(), render_frames, &midi_source_floats) ||
        !checked_frame_count(render_frames, &render_frame_count)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    midi_source_stems = std::make_unique<MidiSourceStemSink>(
        midi_tracks, 2, render_frames, render_frame_count, timeline, sample_rate, block_size);
    if (!midi_source_stems->ready()) {
      set_last_error("could not prepare source-track mixer lanes");
      return SONARE_ERROR_NOT_SUPPORTED;
    }
    if (!render_midi_source_stems(timeline, instruments, sample_rate, block_size, render_frames,
                                  midi_source_stems.get())) {
      set_last_error("could not render source-track MIDI stems for shared channel strips");
      return SONARE_ERROR_NOT_SUPPORTED;
    }
  }
  auto stem_aligned = [&](const std::function<bool(uint32_t)>& keep,
                          std::vector<std::vector<float>>* out) {
    if (out == nullptr) return false;
    std::vector<std::vector<float>> ch;
    if (!render_timeline(timeline, keep, instruments, sample_rate, block_size,
                         /*num_channels=*/2, render_frames, &ch, /*include_audio=*/true,
                         /*include_midi=*/midi_source_stems == nullptr)) {
      return false;
    }
    for (auto& c : ch) {
      if (pdc > 0) c.erase(c.begin(), c.begin() + pdc);
      c.resize(mixer_frame_count);
    }
    *out = std::move(ch);
    return true;
  };

  // One stereo stem per strip (silent if the strip has no source track).
  std::vector<std::vector<std::vector<float>>> stems;
  stems.reserve(strip_count);
  for (size_t i = 0; i < effective_routing.strip_tracks.size(); ++i) {
    const std::set<uint32_t>& tracks = effective_routing.strip_tracks[i];
    if (tracks.empty()) {
      stems.emplace_back(2, std::vector<float>(mixer_frame_count, 0.0f));
      continue;
    }
    std::vector<std::vector<float>> stem;
    if (!stem_aligned([&tracks](uint32_t t) { return tracks.count(t) != 0; }, &stem)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    stems.push_back(std::move(stem));
    if (midi_source_stems) {
      for (uint32_t track_id : tracks) {
        if (const auto* stem = midi_source_stems->stem(track_id)) add_stem(&stems.back(), *stem);
      }
    }
  }

  // Direct stem: every track NOT bound to a scene strip (dry to master).
  if (route_direct) {
    const std::set<uint32_t>& bound = routing.bound_tracks;
    std::vector<std::vector<float>> direct;
    if (!stem_aligned([&bound](uint32_t t) { return bound.count(t) == 0; }, &direct)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
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
  std::vector<float> master_l(mixer_frame_count, 0.0f);
  std::vector<float> master_r(mixer_frame_count, 0.0f);
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
  const size_t mixer_latency_count = static_cast<size_t>(mixer_latency);
  std::unique_ptr<float[]> interleaved(new float[total]);
  for (int64_t f = 0; f < frames; ++f) {
    const size_t source = static_cast<size_t>(f) + mixer_latency_count;
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

}  // namespace sonare_c_bounce_detail

#endif  // SONARE_WITH_ARRANGEMENT && SONARE_WITH_MIXING
