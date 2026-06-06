#include "arrangement/edit_compiler.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "core/resample.h"
#include "engine/tempo_sync.h"
#include "midi/midi_clip.h"

namespace sonare::arrangement {

namespace {

engine::FadeCurve to_engine_fade_curve(FadeCurve curve) noexcept {
  switch (curve) {
    case FadeCurve::kEqualPower:
      return engine::FadeCurve::EqualPower;
    case FadeCurve::kExponential:
      return engine::FadeCurve::Exponential;
    case FadeCurve::kLogarithmic:
      return engine::FadeCurve::Logarithmic;
    case FadeCurve::kLinear:
    default:
      return engine::FadeCurve::Linear;
  }
}

engine::WarpMode to_engine_warp_mode(WarpMode mode) noexcept {
  switch (mode) {
    case WarpMode::kTempoSync:
      return engine::WarpMode::kTempoSync;
    case WarpMode::kRepitch:
      return engine::WarpMode::kRepitch;
    case WarpMode::kOff:
    default:
      return engine::WarpMode::kOff;
  }
}

bool clip_matches_track_kind(const Project& project, const EditClip& clip,
                             const ClipSource& source) noexcept {
  const Track* track = project.find_track(clip.track_id);
  if (track == nullptr) return false;
  const SourceKind kind = source_kind(source);
  if (track->kind == Track::Kind::kAudio) return kind == SourceKind::kAudio;
  if (track->kind == Track::Kind::kMidi) return kind == SourceKind::kMidi;
  return false;
}

// Fills a deterministic transport::TempoMap from the project's plain segment
// data. When the project has no tempo segments a single 120 BPM segment is used
// so PPQ->sample conversion is always well defined (mirrors RealtimeEngine's
// default). No clock / random; pure value transform. TempoMap is non-copyable
// (it owns an rt::RtSnapshot), so it is filled in place rather than returned.
void fill_tempo_map(const Project& project, transport::TempoMap* map) {
  map->prepare(project.sample_rate());

  std::vector<transport::TempoSegment> segments = project.tempo_segments();
  if (segments.empty()) {
    segments.push_back(transport::TempoSegment{0.0, 120.0, 0.0});
  }
  map->set_segments(std::move(segments));

  std::vector<transport::TimeSignatureSegment> sigs = project.time_signatures();
  if (sigs.empty()) {
    sigs.push_back(transport::TimeSignatureSegment{0.0, transport::TimeSignature{4, 4}});
  }
  map->set_time_signatures(std::move(sigs));
}

// Resamples one channel from src_sr to dst_sr using the fixed, build-
// deterministic sonare::resample (r8brain 24-bit). Same-rate channels copy
// through unchanged.
std::vector<float> resample_channel(const std::vector<float>& in, double src_sr, double dst_sr) {
  const int s = static_cast<int>(std::lround(src_sr));
  const int d = static_cast<int>(std::lround(dst_sr));
  if (s == d || in.empty()) {
    return in;
  }
  return sonare::resample(in.data(), in.size(), s, d);
}

size_t rounded_nonnegative_sample(double sample) noexcept {
  if (!std::isfinite(sample) || sample <= 0.0) return 0;
  return static_cast<size_t>(std::llround(sample));
}

int64_t tempo_sync_latency_samples() noexcept {
  engine::TempoSyncWarpBakeConfig config;
  return config.n_fft / 2;
}

struct AudioClipPart {
  double start_ppq = 0.0;
  double end_ppq = 0.0;
  SourceId source_id = 0;
  double source_offset_ppq = 0.0;
};

const ClipTake* find_take(const EditClip& clip, TakeId id) noexcept {
  if (id == 0) return nullptr;
  const auto it = std::find_if(clip.takes.begin(), clip.takes.end(),
                               [id](const ClipTake& take) { return take.id == id; });
  return it == clip.takes.end() ? nullptr : &*it;
}

bool resolve_take_part(const EditClip& clip, TakeId take_id, double start_ppq, double end_ppq,
                       AudioClipPart* out) noexcept {
  if (out == nullptr) return false;
  const ClipTake* take = find_take(clip, take_id);
  if (take_id != 0 && take == nullptr) return false;
  const SourceId source_id =
      take != nullptr && take->source_id != 0 ? take->source_id : clip.source_id;
  const double source_offset_ppq =
      take != nullptr ? take->source_offset_ppq : clip.source_offset_ppq;
  out->start_ppq = start_ppq;
  out->end_ppq = end_ppq;
  out->source_id = source_id;
  out->source_offset_ppq = source_offset_ppq + start_ppq;
  return true;
}

std::vector<AudioClipPart> build_audio_clip_parts(const EditClip& clip, bool* ok) {
  if (ok != nullptr) *ok = true;
  std::vector<AudioClipPart> parts;
  const auto push_part = [&](TakeId take_id, double start_ppq, double end_ppq) {
    if (!(end_ppq > start_ppq)) return;
    AudioClipPart part;
    if (!resolve_take_part(clip, take_id, start_ppq, end_ppq, &part)) {
      if (ok != nullptr) *ok = false;
      return;
    }
    parts.push_back(part);
  };

  const TakeId fallback_take_id = clip.active_take_id;
  if (clip.comp_segments.empty()) {
    push_part(fallback_take_id, 0.0, clip.length_ppq);
    return parts;
  }

  double cursor = 0.0;
  for (const ClipCompSegment& segment : clip.comp_segments) {
    if (segment.start_ppq > cursor) {
      push_part(fallback_take_id, cursor, segment.start_ppq);
    }
    push_part(segment.take_id == 0 ? fallback_take_id : segment.take_id, segment.start_ppq,
              segment.end_ppq);
    cursor = segment.end_ppq;
  }
  if (cursor < clip.length_ppq) {
    push_part(fallback_take_id, cursor, clip.length_ppq);
  }
  return parts;
}

void add_diag(CompileResult* result, Diagnostic::Code code, Diagnostic::Severity sev,
              uint32_t target_id, std::string message) {
  result->diagnostics.push_back(Diagnostic{code, sev, target_id, std::move(message)});
}

// Validates tempo segments: bpm/end_bpm must be > 0 and start_ppq monotonic and
// non-negative. Returns false (with diagnostics appended) on any violation.
bool validate_tempo(const Project& project, CompileResult* result) {
  double prev_ppq = -1.0;
  for (const auto& seg : project.tempo_segments()) {
    if (seg.bpm <= 0.0 || (seg.end_bpm < 0.0)) {
      add_diag(result, Diagnostic::Code::kInvalidTempo, Diagnostic::Severity::kError, 0,
               "tempo segment has non-positive BPM");
      return false;
    }
    if (seg.start_ppq < 0.0 || seg.start_ppq <= prev_ppq) {
      add_diag(result, Diagnostic::Code::kInvalidTempo, Diagnostic::Severity::kError, 0,
               "tempo segment start_ppq is negative or non-monotonic");
      return false;
    }
    prev_ppq = seg.start_ppq;
  }
  return true;
}

void append_midi_render_events(const midi::MidiClip& midi_clip, const EditClip& clip,
                               const transport::TempoMap& tempo_map,
                               std::vector<midi::MidiEvent>* out) {
  if (out == nullptr) return;
  const double clip_end_ppq = clip.end_ppq();
  if (clip.loop_mode == LoopMode::kLoop && clip.loop_length_ppq > 0.0) {
    for (const midi::MidiClipEvent& ev : midi_clip.events()) {
      if (ev.ppq < 0.0 || ev.ppq >= clip.loop_length_ppq) continue;
      const double event_ppq = clip.start_ppq + ev.ppq;
      if (event_ppq >= clip_end_ppq) continue;
      midi::MidiEvent rendered;
      rendered.render_frame = tempo_map.ppq_to_sample(event_ppq);
      rendered.ump = ev.ump;
      rendered.sysex_payload = ev.sysex_payload;
      rendered.sysex_payload_size = ev.sysex_payload_size;
      out->push_back(rendered);
    }
  } else {
    for (const midi::MidiClipEvent& ev : midi_clip.events()) {
      if (ev.ppq < 0.0 || ev.ppq >= clip.length_ppq) continue;
      midi::MidiEvent rendered;
      rendered.render_frame = tempo_map.ppq_to_sample(clip.start_ppq + ev.ppq);
      rendered.ump = ev.ump;
      rendered.sysex_payload = ev.sysex_payload;
      rendered.sysex_payload_size = ev.sysex_payload_size;
      out->push_back(rendered);
    }
  }
}

uint8_t ump_word_count_from_word0(uint32_t word0) noexcept {
  const uint8_t mt = static_cast<uint8_t>((word0 >> 28u) & 0x0Fu);
  return mt == static_cast<uint8_t>(midi::UmpMessageType::kMidi2ChannelVoice) ? 2 : 1;
}

}  // namespace

void CompiledTimeline::copy_from(const CompiledTimeline& other) {
  audio_clips = other.audio_clips;
  midi_clips = other.midi_clips;
  automation_lanes = other.automation_lanes;
  graph = other.graph;
  mixer = other.mixer;
  tempo_segments = other.tempo_segments;
  time_signatures = other.time_signatures;
  latency = other.latency;

  // Copy the OWNED marker name storage first, then re-point each marker's
  // non-owning name pointer into THIS object's storage so the copy never
  // dangles into `other`. marker/marker_names are index-aligned by build().
  marker_names = other.marker_names;
  markers = other.markers;
  for (size_t i = 0; i < markers.size() && i < marker_names.size(); ++i) {
    markers[i].name = marker_names[i].c_str();
  }
}

CompileResult compile(const Project& project, const MidiContentStore& midi,
                      const AudioContentStore& audio, const CompileConfig& config) {
  CompileResult result;

  // ---- Global validation --------------------------------------------------
  const double project_sr = project.sample_rate();
  if (!(project_sr > 0.0)) {
    add_diag(&result, Diagnostic::Code::kInvalidSampleRate, Diagnostic::Severity::kError, 0,
             "project sample rate must be > 0");
  }
  validate_tempo(project, &result);

  // The TempoMap is needed for PPQ->sample conversion regardless of clip
  // validity; build it from whatever (validated) segments exist.
  transport::TempoMap tempo_map;
  fill_tempo_map(project, &tempo_map);

  CompiledTimeline timeline;

  // ---- Tempo / time-signature (carry full segment vectors) ----------------
  timeline.tempo_segments = project.tempo_segments();
  timeline.time_signatures = project.time_signatures();

  // ---- Markers (own the name strings; point into stable storage) ----------
  // Reserve so marker_names never reallocates after we take .c_str() pointers.
  timeline.marker_names.reserve(project.markers().size());
  timeline.markers.reserve(project.markers().size());
  for (const auto& m : project.markers()) {
    timeline.marker_names.push_back(m.name);
  }
  for (size_t i = 0; i < project.markers().size(); ++i) {
    const auto& m = project.markers()[i];
    transport::Marker out;
    out.ppq = m.ppq;
    out.id = m.id;
    out.name = timeline.marker_names[i].c_str();
    timeline.markers.push_back(out);
  }

  // ---- Automation lanes (flatten per-track lanes) --------------------------
  for (const auto& track : project.tracks()) {
    for (const auto& lane : track.automation_lanes) {
      timeline.automation_lanes.push_back(lane);
      timeline.mixer.automation_bindings.push_back(MixerAutomationBinding{track.id, lane});
    }
  }

  // ---- Mixer scene + Track->Strip bindings ---------------------------------
  timeline.mixer.scene = project.scene();
  for (const auto& track : project.tracks()) {
    if (!track.channel_strip_ref.empty()) {
      timeline.mixer.bindings.push_back(MixerStripBinding{track.id, track.channel_strip_ref});
    }
  }
#if !defined(SONARE_WITH_MIXING)
  if (!timeline.mixer.bindings.empty()) {
    timeline.mixer.unavailable_in_build = true;
    add_diag(&result, Diagnostic::Code::kUnsupportedMixing, Diagnostic::Severity::kWarning, 0,
             "mixer strip binding requested but SONARE_WITH_MIXING is disabled in this build");
  }
#endif

  // ---- Graph request -------------------------------------------------------
  // Nothing in the Project drives a graph swap yet, so graph.requested is false.
#if !defined(SONARE_WITH_GRAPH)
  if (timeline.graph.requested) {
    timeline.graph.unavailable_in_build = true;
    add_diag(&result, Diagnostic::Code::kUnsupportedGraph, Diagnostic::Severity::kWarning, 0,
             "graph swap requested but SONARE_WITH_GRAPH is disabled in this build");
  }
#endif

  // ---- Audio clips ---------------------------------------------------------
  // Cache baked storage per source+warp-ref so multiple clips share immutable
  // buffers, while a warped rendition does not alias the unwarped source.
  std::map<std::pair<SourceId, WarpRefId>, std::shared_ptr<const engine::ClipAudioStorage>> baked;

  for (const auto& clip : project.clips()) {
    const ClipSource* src = project.find_source(clip.source_id);
    if (src == nullptr) {
      add_diag(&result, Diagnostic::Code::kDanglingSourceRef, Diagnostic::Severity::kError, clip.id,
               "clip references a source id that is not registered");
      continue;
    }
    if (!clip_matches_track_kind(project, clip, *src)) {
      add_diag(&result, Diagnostic::Code::kSourceKindMismatch, Diagnostic::Severity::kError,
               clip.id, "clip source kind does not match its track kind");
      continue;
    }
    if (source_kind(*src) != SourceKind::kAudio) {
      continue;
    }

    // PPQ validation per clip.
    if (clip.length_ppq <= 0.0 || clip.start_ppq < 0.0 || clip.source_offset_ppq < 0.0) {
      add_diag(&result, Diagnostic::Code::kInvalidPpq, Diagnostic::Severity::kError, clip.id,
               "clip has non-positive length or negative start/offset PPQ");
      continue;
    }

    bool parts_ok = true;
    const std::vector<AudioClipPart> parts = build_audio_clip_parts(clip, &parts_ok);
    if (!parts_ok || parts.empty()) {
      add_diag(&result, Diagnostic::Code::kDanglingSourceRef, Diagnostic::Severity::kError, clip.id,
               "clip comp lane references a take that is not registered");
      continue;
    }

    const int64_t clip_start_sample = tempo_map.ppq_to_sample(clip.start_ppq);
    const int64_t clip_end_sample = tempo_map.ppq_to_sample(clip.end_ppq());
    const int64_t clip_length_samples = std::max<int64_t>(0, clip_end_sample - clip_start_sample);

    for (const AudioClipPart& part : parts) {
      const ClipSource* part_src = project.find_source(part.source_id);
      if (part_src == nullptr) {
        add_diag(&result, Diagnostic::Code::kDanglingSourceRef, Diagnostic::Severity::kError,
                 clip.id, "clip take references a source id that is not registered");
        continue;
      }
      if (!clip_matches_track_kind(project, clip, *part_src) ||
          source_kind(*part_src) != SourceKind::kAudio) {
        add_diag(&result, Diagnostic::Code::kSourceKindMismatch, Diagnostic::Severity::kError,
                 clip.id, "clip take source kind does not match its track kind");
        continue;
      }

      const AudioSourceSamples* samples =
          clip.warp_ref_id != 0 ? audio.find_warped(clip.warp_ref_id) : nullptr;
      const WarpRefId baked_warp_ref = samples != nullptr ? clip.warp_ref_id : 0;
      const WarpMapRef* rt_warp_map =
          (baked_warp_ref == 0 &&
           (clip.warp_mode == WarpMode::kRepitch || clip.warp_mode == WarpMode::kTempoSync) &&
           clip.warp_ref_id != 0)
              ? project.find_warp_map(clip.warp_ref_id)
              : nullptr;
      if (clip.warp_mode == WarpMode::kTempoSync && baked_warp_ref == 0 && clip.warp_ref_id == 0) {
        add_diag(&result, Diagnostic::Code::kDanglingSourceRef, Diagnostic::Severity::kError,
                 clip.id, "tempo-sync clip requires a warp map or pre-baked warped audio");
        continue;
      }
      if (clip.warp_mode == WarpMode::kTempoSync && clip.warp_ref_id != 0 && baked_warp_ref == 0 &&
          rt_warp_map == nullptr) {
        add_diag(&result, Diagnostic::Code::kDanglingSourceRef, Diagnostic::Severity::kError,
                 clip.id, "clip references a tempo-sync warp map that is not registered");
        continue;
      }
      if (clip.warp_mode == WarpMode::kRepitch && clip.warp_ref_id != 0 && baked_warp_ref == 0 &&
          rt_warp_map == nullptr) {
        add_diag(&result, Diagnostic::Code::kDanglingSourceRef, Diagnostic::Severity::kError,
                 clip.id, "clip references a repitch warp map that is not registered");
        continue;
      }
      if (samples == nullptr) {
        samples = audio.find(part.source_id);
      }
      if (samples == nullptr) {
        add_diag(&result, Diagnostic::Code::kDanglingSourceRef, Diagnostic::Severity::kError,
                 clip.id,
                 "audio source has no decoded samples registered in the AudioContentStore");
        continue;
      }
      if (samples->channels.empty() || samples->channels[0].empty()) {
        add_diag(&result, Diagnostic::Code::kEmptyAudioSource, Diagnostic::Severity::kError,
                 clip.id, "audio source is registered but contains no samples");
        continue;
      }
      if (!(samples->sample_rate > 0.0)) {
        add_diag(&result, Diagnostic::Code::kInvalidSampleRate, Diagnostic::Severity::kError,
                 clip.id, "audio source has an invalid native sample rate");
        continue;
      }

      // PPQ -> sample conversions through the deterministic TempoMap.
      const int64_t start_sample = tempo_map.ppq_to_sample(clip.start_ppq + part.start_ppq);
      const int64_t end_sample = tempo_map.ppq_to_sample(clip.start_ppq + part.end_ppq);
      const int64_t length_samples = std::max<int64_t>(0, end_sample - start_sample);
      // Source offset measured as a sample count in the (resampled) source domain:
      // the musical distance clip-start..clip-start+offset, in samples. Comp
      // fragments add their clip-local start to the selected take offset.
      const int64_t offset_end_sample =
          tempo_map.ppq_to_sample(clip.start_ppq + part.source_offset_ppq);
      const int64_t clip_offset_samples =
          std::max<int64_t>(0, offset_end_sample - clip_start_sample);

      // Bake (resample to project SR) once per source; share across clips.
      std::shared_ptr<const engine::ClipAudioStorage> storage;
      const bool compile_baked_tempo_sync =
          clip.warp_mode == WarpMode::kTempoSync && baked_warp_ref == 0 && rt_warp_map != nullptr;
      const auto cache_key = std::make_pair(
          part.source_id, compile_baked_tempo_sync ? clip.warp_ref_id : baked_warp_ref);
      const auto cached = baked.find(cache_key);
      if (cached != baked.end()) {
        storage = cached->second;
      } else {
        auto built = std::make_shared<engine::ClipAudioStorage>();
        built->channels.reserve(samples->channels.size());
        for (const auto& ch : samples->channels) {
          built->channels.push_back(resample_channel(ch, samples->sample_rate, project_sr));
        }
        if (compile_baked_tempo_sync) {
          engine::TempoSyncWarpBakeConfig bake_config;
          std::vector<engine::TempoSyncWarpSegment> segments;
          segments.reserve(rt_warp_map->anchors.size() > 1 ? rt_warp_map->anchors.size() - 1 : 0);
          size_t total_target_samples = 0;
          bool invalid_segment = rt_warp_map->anchors.size() < 2;
          for (size_t anchor_index = 1;
               !invalid_segment && anchor_index < rt_warp_map->anchors.size(); ++anchor_index) {
            const WarpAnchorRef& prev = rt_warp_map->anchors[anchor_index - 1];
            const WarpAnchorRef& next = rt_warp_map->anchors[anchor_index];
            const size_t source_offset = rounded_nonnegative_sample(prev.source_sample);
            const size_t source_end = rounded_nonnegative_sample(next.source_sample);
            const size_t target_start = rounded_nonnegative_sample(prev.warp_sample);
            const size_t target_end = rounded_nonnegative_sample(next.warp_sample);
            engine::TempoSyncWarpSegment segment;
            segment.source_offset = source_offset;
            segment.source_samples = source_end > source_offset ? source_end - source_offset : 0;
            segment.target_samples = target_end > target_start ? target_end - target_start : 0;
            invalid_segment = segment.target_samples == 0 ||
                              segment.source_samples < static_cast<size_t>(bake_config.hop_length);
            if (!invalid_segment) {
              total_target_samples += segment.target_samples;
              segments.push_back(segment);
            }
          }
          if (invalid_segment || total_target_samples == 0) {
            add_diag(&result, Diagnostic::Code::kDanglingSourceRef, Diagnostic::Severity::kError,
                     clip.id, "tempo-sync warp map has an unsupported source/target span");
            continue;
          }
          const int project_sample_rate = static_cast<int>(std::lround(project_sr));
          bake_config.sample_rate = project_sample_rate;
          bool span_exceeds_source = false;
          for (const auto& ch : built->channels) {
            for (const engine::TempoSyncWarpSegment& segment : segments) {
              if (segment.source_offset > ch.size() ||
                  segment.source_samples > ch.size() - segment.source_offset) {
                span_exceeds_source = true;
                break;
              }
            }
            if (span_exceeds_source) break;
          }
          if (span_exceeds_source) {
            add_diag(&result, Diagnostic::Code::kDanglingSourceRef, Diagnostic::Severity::kError,
                     clip.id, "tempo-sync warp map source span exceeds decoded audio length");
            continue;
          }
          std::vector<std::vector<float>> stretched;
          stretched.reserve(built->channels.size());
          for (const auto& ch : built->channels) {
            std::vector<float> channel =
                engine::bake_tempo_sync_warp_channel(ch.data(), ch.size(), segments, bake_config);
            stretched.push_back(std::move(channel));
          }
          built->channels = std::move(stretched);
        }
        built->channel_ptrs.reserve(built->channels.size());
        for (const auto& ch : built->channels) {
          built->channel_ptrs.push_back(ch.data());
        }
        storage = std::move(built);
        baked.emplace(cache_key, storage);
      }

      const int num_channels = static_cast<int>(storage->channels.size());
      const int64_t source_samples =
          num_channels > 0 ? static_cast<int64_t>(storage->channels[0].size()) : 0;

      const int64_t fade_in_samples = std::max<int64_t>(
          0, tempo_map.ppq_to_sample(clip.start_ppq + clip.fade_in.length_ppq) - clip_start_sample);
      const int64_t fade_out_samples = std::max<int64_t>(
          0, tempo_map.ppq_to_sample(clip.end_ppq()) -
                 tempo_map.ppq_to_sample(clip.end_ppq() - clip.fade_out.length_ppq));

      engine::ClipAudioBuffer buffer;
      buffer.channels = storage->channel_ptrs.data();
      buffer.num_channels = num_channels;
      buffer.num_samples = source_samples;

      const bool is_whole_clip = part.start_ppq == 0.0 && part.end_ppq == clip.length_ppq;
      const bool loop = is_whole_clip && clip.loop_mode == LoopMode::kLoop;
      const int64_t loop_length_samples =
          loop && clip.loop_length_ppq > 0.0
              ? std::max<int64_t>(
                    0,
                    tempo_map.ppq_to_sample(clip.start_ppq + clip.loop_length_ppq) - start_sample)
              : 0;
      const engine::FadeCurve fade_in_curve = to_engine_fade_curve(clip.fade_in.curve);
      const engine::FadeCurve fade_out_curve = to_engine_fade_curve(clip.fade_out.curve);

      engine::ClipSchedule sched(clip.id, buffer, clip.start_ppq + part.start_ppq, start_sample,
                                 clip_offset_samples, length_samples, loop, clip.gain,
                                 fade_in_samples, fade_out_samples, fade_in_curve, fade_out_curve,
                                 /*clip_has_separate_fade_out_curve=*/true);
      sched.loop_length_samples = loop_length_samples;
      sched.fade_reference_offset_samples = std::max<int64_t>(0, start_sample - clip_start_sample);
      sched.fade_reference_length_samples = clip_length_samples;
      sched.warp_ref_id = clip.warp_ref_id;
      sched.warp_mode = baked_warp_ref == 0 && !compile_baked_tempo_sync
                            ? to_engine_warp_mode(clip.warp_mode)
                            : engine::WarpMode::kOff;
      sched.warp_reference_offset_samples = std::max<int64_t>(0, start_sample - clip_start_sample);
      if (sched.warp_mode != engine::WarpMode::kOff && rt_warp_map != nullptr &&
          rt_warp_map->anchors.size() >= 2) {
        auto anchors = std::make_shared<std::vector<engine::WarpAnchor>>();
        anchors->reserve(rt_warp_map->anchors.size());
        for (const WarpAnchorRef& anchor : rt_warp_map->anchors) {
          anchors->push_back({anchor.warp_sample, anchor.source_sample});
        }
        sched.warp_anchors = std::move(anchors);
      }
      sched.track_id = clip.track_id;
      sched.storage = std::move(storage);
      timeline.audio_clips.push_back(std::move(sched));

      if (compile_baked_tempo_sync) {
        int64_t& source_latency = timeline.latency.per_source_samples[part.source_id];
        source_latency = std::max(source_latency, tempo_sync_latency_samples());
      }
    }
  }

  // ---- MIDI clips ----------------------------------------------------------
  uint32_t first_midi_clip_id = 0;
  // Bake each MIDI clip's PPQ-timed events (from the MidiContentStore) into a
  // midi::MidiClipSchedule with absolute render-frame UMP events. The store's
  // MidiClipEvent maps data0/data1 onto the first two UMP words and carries a
  // SysEx handle sidecar when needed. PPQ to frame conversion uses the same
  // deterministic TempoMap as audio.
  for (const auto& clip : project.clips()) {
    const ClipSource* src = project.find_source(clip.source_id);
    if (src == nullptr) {
      // Audio clips already reported dangling refs above; only report once for
      // MIDI-only references here (the audio loop `continue`s past non-audio).
      continue;
    }
    if (!clip_matches_track_kind(project, clip, *src)) {
      continue;
    }
    if (source_kind(*src) != SourceKind::kMidi) {
      continue;
    }
    if (clip.length_ppq <= 0.0 || clip.start_ppq < 0.0 || clip.source_offset_ppq < 0.0) {
      add_diag(&result, Diagnostic::Code::kInvalidPpq, Diagnostic::Severity::kError, clip.id,
               "MIDI clip has non-positive length or negative start/offset PPQ");
      continue;
    }

    midi::MidiClip midi_clip;
    const auto it = midi.events.find(clip.id);
    if (it != midi.events.end()) {
      for (const MidiClipEvent& ev : it->second) {
        // Drop events before the clip's source offset; rebase onto the clip.
        if (ev.ppq < clip.source_offset_ppq) continue;
        midi::MidiClipEvent out;
        out.ppq = ev.ppq - clip.source_offset_ppq;
        out.ump.words[0] = ev.data0;
        out.ump.words[1] = ev.data1;
        out.ump.word_count = ump_word_count_from_word0(ev.data0);
        out.ump.group = static_cast<uint8_t>((ev.data0 >> 24u) & 0x0Fu);
        out.ump.sysex_handle = ev.sysex_handle;
        if (ev.sysex_handle != 0) {
          const auto payload_it = midi.sysex_payloads.find(ev.sysex_handle);
          if (payload_it != midi.sysex_payloads.end() && !payload_it->second.empty()) {
            out.sysex_payload = payload_it->second.data();
            out.sysex_payload_size = payload_it->second.size();
          }
        }
        midi_clip.add_event(out);
      }
    }
    midi_clip.sort_stable();

    midi::MidiClipSchedule sched;
    sched.id = clip.id;
    sched.track_id = clip.track_id;
    // Route the clip's events to its track's MIDI destination. Without this the
    // sequencer fires every clip on destination 0, collapsing multi-track MIDI
    // onto a single instrument. 0 stays the default/null destination.
    if (const Track* clip_track = project.find_track(clip.track_id)) {
      sched.destination_id = clip_track->midi_destination_id;
    }
    sched.start_ppq = clip.start_ppq;
    sched.start_sample = tempo_map.ppq_to_sample(clip.start_ppq);
    sched.length_samples =
        std::max<int64_t>(0, tempo_map.ppq_to_sample(clip.end_ppq()) - sched.start_sample);
    sched.loop_mode = clip.loop_mode == LoopMode::kLoop ? midi::MidiLoopMode::kLoop
                                                        : midi::MidiLoopMode::kOneShot;
    if (sched.loop_mode == midi::MidiLoopMode::kLoop && clip.loop_length_ppq > 0.0) {
      sched.loop_length_samples = std::max<int64_t>(
          0, tempo_map.ppq_to_sample(clip.start_ppq + clip.loop_length_ppq) - sched.start_sample);
    }
    append_midi_render_events(midi_clip, clip, tempo_map, &sched.events);
    first_midi_clip_id = timeline.midi_clips.empty() ? sched.id : first_midi_clip_id;
    timeline.midi_clips.push_back(std::move(sched));

    // PDC / latency: a host instrument renders this MIDI source, so its
    // reported latency contributes to the timeline's compensation summary. Record
    // it per MIDI source id (deduplicated: multiple clips off one source share a
    // single instrument node, so the source's latency is counted once).
    if (config.instrument_latency_samples > 0) {
      timeline.latency.per_source_samples[clip.source_id] = config.instrument_latency_samples;
    }
  }

  // Best-effort hint: a compiled MIDI clip renders to silence unless the caller
  // binds an instrument at bounce time (the compiler runs before instruments are
  // registered, so it cannot know whether one will be — hence a warning, not an
  // error). Emitted once so a user who bounces with the plain sonare_project_bounce
  // path is nudged toward bounce_with_builtin_instruments / a bound destination.
  if (!timeline.midi_clips.empty()) {
    add_diag(&result, Diagnostic::Code::kMidiClipNoInstrument, Diagnostic::Severity::kWarning,
             first_midi_clip_id,
             "project contains MIDI clips; bounce is silent unless an instrument is bound");
  }

  // Total reported latency is the maximum per-source contribution (sources render
  // in parallel; the bus must wait for the slowest). With a single host instrument
  // node all entries are equal, so this is simply that node's latency.
  for (const auto& [source, samples] : timeline.latency.per_source_samples) {
    (void)source;
    timeline.latency.total_latency_samples =
        std::max(timeline.latency.total_latency_samples, samples);
  }

  // ---- Overlap policy ------------------------------------------------------
  if (config.enforce_overlap_policy && project.overlap_policy() == OverlapPolicy::kDisallow) {
    for (const auto& track : project.tracks()) {
      // Gather clip spans on this track (PPQ) for an O(n^2) pairwise check;
      // clip counts per track are small and this stays deterministic.
      std::vector<const EditClip*> on_track;
      for (const auto& clip : project.clips()) {
        if (clip.track_id == track.id && clip.length_ppq > 0.0) on_track.push_back(&clip);
      }
      for (size_t i = 0; i < on_track.size(); ++i) {
        for (size_t j = i + 1; j < on_track.size(); ++j) {
          const EditClip* a = on_track[i];
          const EditClip* b = on_track[j];
          const bool overlap = a->start_ppq < b->end_ppq() && b->start_ppq < a->end_ppq();
          if (overlap) {
            add_diag(&result, Diagnostic::Code::kClipOverlap, Diagnostic::Severity::kError, a->id,
                     "clip overlaps another clip on the same track under kDisallow policy");
          }
        }
      }
    }
  }

  // ---- Finalize ------------------------------------------------------------
  // ERROR diagnostics suppress the timeline; WARNING diagnostics do not.
  if (!result.has_errors()) {
    result.timeline = std::move(timeline);
  }
  return result;
}

void apply_to_engine(const CompiledTimeline& timeline, engine::RealtimeEngine& engine) {
  // Engine prescribed CONTROL-THREAD direct-setter order. All of these are
  // direct-setter / publisher installs, NOT push_command.

  // 1) Tempo / time signature. Install full segment vectors so transport,
  //    metronome, automation and clip rescheduling share the compiled map.
  if (!timeline.tempo_segments.empty()) {
    engine.set_tempo_segments(timeline.tempo_segments);
  }
  if (!timeline.time_signatures.empty()) {
    engine.set_time_signature_segments(timeline.time_signatures);
  }

  // 2) Markers. The CompiledTimeline owns the name strings; the transport::Marker
  //    name pointers point into that stable storage and stay valid while RT holds
  //    the snapshot (the caller must keep this CompiledTimeline alive).
  engine.set_markers(timeline.markers);

  // 3) Automation lanes.
  engine.automation().set_lanes(timeline.automation_lanes);

  // 4) Audio clips. The ClipSchedule::storage shared_ptr keeps the baked buffers
  //    alive across the RtPublisher swap; the engine's set_clips follows the
  //    retire protocol, so old buffers are released back to the control thread.
  engine.set_clips(timeline.audio_clips);

  // 4b) MIDI clips. set_midi_clips is a control-thread direct-setter that
  //     publishes through the MidiSequencer's RtPublisher (no rt::Command, no
  //     ABI bump). Only present when arrangement (and thus the sequencer member)
  //     is compiled in.
#if defined(SONARE_WITH_ARRANGEMENT)
  engine.set_midi_clips(timeline.midi_clips);
#endif

  // 5) Graph swap: no Project field currently requests a graph replacement, so
  //    timeline.graph.requested remains false. The mixer binding is value-only:
  //    the caller turns each MixerStripBinding into a live mixing::ChannelStrip
  //    and calls bind_mixing_strip itself, because the compiler must not own RT
  //    objects.
}

}  // namespace sonare::arrangement
