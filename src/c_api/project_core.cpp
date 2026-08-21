#include "c_api/project_internal.h"
#include "util/resource_limits.h"

// ============================================================================
// ABI version
// ============================================================================

#if defined(SONARE_WITH_ARRANGEMENT)
// Keep the C++ ABI constant and the C macro in lock-step. The check lives here
// (the C ABI bridge layer) rather than inside arrangement/ to preserve the layer
// rule "arrangement/ must not depend on the public C API header sonare_c.h".
static_assert(arr::kProjectAbiVersion == SONARE_PROJECT_ABI_VERSION,
              "C++ and C project ABI version constants drifted");
#endif

uint32_t sonare_project_abi_version(void) {
#if defined(SONARE_WITH_ARRANGEMENT)
  return arr::kProjectAbiVersion;
#else
  return 0u;
#endif
}

// ============================================================================
// Lifecycle / IO / render
// ============================================================================

SonareError sonare_project_create(SonareProject** out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  SONARE_C_TRY
  *out = new SonareProject{};
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(out);
#endif
}

void sonare_project_destroy(SonareProject* project) {
#if defined(SONARE_WITH_ARRANGEMENT)
  delete project;
#else
  (void)project;
#endif
}

SonareError sonare_project_serialize(const SonareProject* project, char** out_json,
                                     size_t* out_len) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out_json) return SONARE_ERROR_INVALID_PARAMETER;
  *out_json = nullptr;
  if (out_len) *out_len = 0;
  SONARE_C_TRY
  const std::string json = sonare::serialize::project_to_json(project->history.project(),
                                                              project->history.midi_content());
  *out_json = copy_string(json);
  if (out_len) *out_len = json.size();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out_json, out_len);
#endif
}

namespace {

#if defined(SONARE_WITH_ARRANGEMENT)
std::string serialize_deserialize_diagnostics(
    const std::vector<sonare::serialize::Diagnostic>& diagnostics) {
  std::ostringstream stream;
  for (size_t i = 0; i < diagnostics.size(); ++i) {
    if (i > 0) stream << '\n';
    stream << diagnostics[i].code << ": " << diagnostics[i].message;
  }
  return stream.str();
}
#endif

}  // namespace

namespace {

#if defined(SONARE_WITH_ARRANGEMENT)
template <size_t N>
void copy_utf8_prefix(char (&out)[N], const std::string& value) {
  size_t n = std::min(value.size(), N - 1u);
  while (n > 0 && n < value.size() && (static_cast<unsigned char>(value[n]) & 0xc0u) == 0x80u) {
    --n;
  }
  std::memcpy(out, value.data(), n);
  out[n] = '\0';
}
#endif

}  // namespace

SonareError sonare_project_deserialize(const char* json, size_t len, SonareProject** out,
                                       char** out_diag) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!json || !out) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;
  if (out_diag) *out_diag = nullptr;
  if (len > sonare::resource::kDefaultProjectImportResourceLimits.max_json_bytes) {
    return SONARE_ERROR_INVALID_FORMAT;
  }
  SONARE_C_TRY
  sonare::serialize::DeserializeResult result =
      sonare::serialize::project_from_json(std::string(json, len));
  if (!result.ok()) {
    if (out_diag) {
      *out_diag = copy_string(serialize_deserialize_diagnostics(result.diagnostics));
    }
    return SONARE_ERROR_INVALID_FORMAT;
  }
  auto handle = std::make_unique<SonareProject>();
  handle->history = arr::EditHistory(std::move(*result.project));
  handle->history.midi_content() = std::move(result.midi);
  if (out_diag && !result.diagnostics.empty()) {
    *out_diag = copy_string(serialize_deserialize_diagnostics(result.diagnostics));
  }
  *out = handle.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(json, len, out, out_diag);
#endif
}

SonareError sonare_project_unresolved_audio_source_count(const SonareProject* project,
                                                         size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  size_t count = 0;
  for (const arr::ClipSource& source : project->history.project().sources()) {
    if (const auto* audio = std::get_if<arr::AudioSourceRef>(&source);
        audio != nullptr && project->audio.find(audio->id) == nullptr) {
      ++count;
    }
  }
  *out_count = count;
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out_count);
#endif
}

SonareError sonare_project_unresolved_audio_source_id_by_index(const SonareProject* project,
                                                               size_t index,
                                                               uint32_t* out_source_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out_source_id) return SONARE_ERROR_INVALID_PARAMETER;
  size_t current = 0;
  for (const arr::ClipSource& source : project->history.project().sources()) {
    const auto* audio = std::get_if<arr::AudioSourceRef>(&source);
    if (audio == nullptr || project->audio.find(audio->id) != nullptr) continue;
    if (current++ == index) {
      *out_source_id = audio->id;
      return SONARE_OK;
    }
  }
  return SONARE_ERROR_INVALID_PARAMETER;
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, index, out_source_id);
#endif
}

SonareError sonare_project_set_sample_rate(SonareProject* project, double sample_rate) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !finite_positive(sample_rate) || sample_rate < kMinSampleRate ||
      sample_rate > kMaxSampleRate) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  // Through the history like every other setter. Set directly on the Project it
  // owns, this was the one piece of public project state undo could not reach:
  // addTrack() then setSampleRate(96000) then undo() removed the track and left
  // the rate at 96 kHz, and the wrong rate was then serialized into the saved
  // document. The facades all promise that every mutation routes through the
  // native EditHistory, so the sample rate has to be a command too.
  auto command = std::make_unique<arr::SetSampleRate>(sample_rate);
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, sample_rate);
#endif
}

SonareError sonare_project_get_sample_rate(const SonareProject* project, double* out_sample_rate) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out_sample_rate) return SONARE_ERROR_INVALID_PARAMETER;
  *out_sample_rate = project->history.project().sample_rate();
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out_sample_rate);
#endif
}

SonareError sonare_project_get_overlap_policy(const SonareProject* project,
                                              uint32_t* out_overlap_policy) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out_overlap_policy) return SONARE_ERROR_INVALID_PARAMETER;
  *out_overlap_policy = static_cast<uint32_t>(project->history.project().overlap_policy());
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out_overlap_policy);
#endif
}

namespace {

#if defined(SONARE_WITH_ARRANGEMENT)
template <typename CountFn>
SonareError project_count(const SonareProject* project, size_t* out_count, CountFn count_fn) {
  if (!project || !out_count) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = count_fn(project->history.project());
  return SONARE_OK;
}
#endif

}  // namespace

SonareError sonare_project_track_count(const SonareProject* project, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  return project_count(project, out_count, [](const arr::Project& p) { return p.tracks().size(); });
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out_count);
#endif
}

SonareError sonare_project_clip_count(const SonareProject* project, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  return project_count(project, out_count, [](const arr::Project& p) { return p.clips().size(); });
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out_count);
#endif
}

SonareError sonare_project_source_count(const SonareProject* project, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  return project_count(project, out_count,
                       [](const arr::Project& p) { return p.sources().size(); });
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out_count);
#endif
}

SonareError sonare_project_marker_count(const SonareProject* project, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  return project_count(project, out_count,
                       [](const arr::Project& p) { return p.markers().size(); });
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out_count);
#endif
}

SonareError sonare_project_marker_by_index(const SonareProject* project, size_t index,
                                           SonareProjectMarker* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out) return SONARE_ERROR_INVALID_PARAMETER;
  const std::vector<arr::ProjectMarker>& markers = project->history.project().markers();
  if (index >= markers.size()) return SONARE_ERROR_INVALID_PARAMETER;
  const arr::ProjectMarker& m = markers[index];
  out->id = m.id;
  out->kind = m.kind;
  out->key_fifths = m.key_fifths;
  out->key_minor = m.key_minor ? 1 : 0;
  out->ppq = m.ppq;
  size_t n = std::min(m.name.size(), sizeof(out->name) - 1u);
  while (n > 0 && n < m.name.size() && (static_cast<unsigned char>(m.name[n]) & 0xc0u) == 0x80u)
    --n;
  std::memcpy(out->name, m.name.data(), n);
  out->name[n] = '\0';
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, index, out);
#endif
}

SonareError sonare_project_tempo_segment_by_index(const SonareProject* project, size_t index,
                                                  SonareProjectTempoSegment* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out) return SONARE_ERROR_INVALID_PARAMETER;
  const auto& segments = project->history.project().tempo_segments();
  if (index >= segments.size()) return SONARE_ERROR_INVALID_PARAMETER;
  const sonare::transport::TempoSegment& seg = segments[index];
  *out = {};
  out->start_ppq = seg.start_ppq;
  out->bpm = seg.bpm;
  // Reported as stored. A project holds musical positions only; the sample
  // position is derived by tempo-map normalization at compile time, and the
  // core's own end_ppq is normalization state that the C struct has no field
  // for, so neither is invented here.
  out->start_sample = seg.start_sample;
  out->end_bpm = seg.end_bpm;
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, index, out);
#endif
}

SonareError sonare_project_time_signature_by_index(const SonareProject* project, size_t index,
                                                   SonareProjectTimeSignatureSegment* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out) return SONARE_ERROR_INVALID_PARAMETER;
  const auto& segments = project->history.project().time_signatures();
  if (index >= segments.size()) return SONARE_ERROR_INVALID_PARAMETER;
  const sonare::transport::TimeSignatureSegment& seg = segments[index];
  *out = {};
  out->start_ppq = seg.start_ppq;
  out->numerator = seg.time_sig.numerator;
  out->denominator = seg.time_sig.denominator;
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, index, out);
#endif
}

SonareError sonare_project_track_by_index(const SonareProject* project, size_t index,
                                          SonareProjectTrack* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out) return SONARE_ERROR_INVALID_PARAMETER;
  const auto& tracks = project->history.project().tracks();
  if (index >= tracks.size()) return SONARE_ERROR_INVALID_PARAMETER;
  const arr::Track& track = tracks[index];
  *out = {};
  out->id = track.id;
  out->kind = static_cast<uint32_t>(track.kind);
  out->midi_destination_id = track.midi_destination_id;
  out->gain = track.gain;
  out->pan = track.pan;
  out->mute = track.mute ? 1 : 0;
  out->solo = track.solo ? 1 : 0;
  copy_utf8_prefix(out->name, track.name);
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, index, out);
#endif
}

SonareError sonare_project_clip_by_index(const SonareProject* project, size_t index,
                                         SonareProjectClip* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out) return SONARE_ERROR_INVALID_PARAMETER;
  const auto& clips = project->history.project().clips();
  if (index >= clips.size()) return SONARE_ERROR_INVALID_PARAMETER;
  const arr::EditClip& clip = clips[index];
  *out = {};
  out->id = clip.id;
  out->track_id = clip.track_id;
  out->source_id = clip.source_id;
  if (const arr::ClipSource* source = project->history.project().find_source(clip.source_id)) {
    out->source_kind = static_cast<uint32_t>(arr::source_kind(*source));
  }
  out->start_ppq = clip.start_ppq;
  out->length_ppq = clip.length_ppq;
  out->source_offset_ppq = clip.source_offset_ppq;
  out->gain = clip.gain;
  out->loop_mode = static_cast<uint32_t>(clip.loop_mode);
  out->loop_length_ppq = clip.loop_length_ppq;
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, index, out);
#endif
}

SonareError sonare_project_source_by_index(const SonareProject* project, size_t index,
                                           SonareProjectSource* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out) return SONARE_ERROR_INVALID_PARAMETER;
  const auto& sources = project->history.project().sources();
  if (index >= sources.size()) return SONARE_ERROR_INVALID_PARAMETER;
  const arr::ClipSource& source = sources[index];
  *out = {};
  out->kind = static_cast<uint32_t>(arr::source_kind(source));
  if (const auto* audio = std::get_if<arr::AudioSourceRef>(&source)) {
    out->id = audio->id;
    out->channel_count = audio->channel_count;
    out->storage_handle_id = audio->storage_handle_id;
    out->sample_rate_hint = audio->sample_rate_hint;
    copy_utf8_prefix(out->name_or_uri, audio->uri);
  } else {
    const auto& midi = std::get<arr::MidiSourceRef>(source);
    out->id = midi.id;
    out->channel_count = midi.channel_hint;
    copy_utf8_prefix(out->name_or_uri, midi.name);
  }
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, index, out);
#endif
}

SonareError sonare_project_get_audio_source_metadata(const SonareProject* project,
                                                     uint32_t source_id,
                                                     SonareProjectAudioSourceMetadata* out) {
  SONARE_C_API_ENTRY;
  // The caller may safely pass an uninitialized output and then call the free
  // helper regardless of the return code. Keep this invariant on every path,
  // including builds without the arrangement subsystem.
  if (out) *out = {};
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out || source_id == 0) return SONARE_ERROR_INVALID_PARAMETER;
  const arr::ClipSource* source = project->history.project().find_source(source_id);
  const auto* audio = source ? std::get_if<arr::AudioSourceRef>(source) : nullptr;
  if (audio == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  std::unique_ptr<char[]> content_hash(copy_string(audio->content_hash));
  std::unique_ptr<char[]> external_stem_role(copy_string(audio->external_stem_role));
  out->content_hash = content_hash.release();
  out->external_stem_role = external_stem_role.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, source_id, out);
#endif
}

void sonare_project_free_audio_source_metadata(SonareProjectAudioSourceMetadata* metadata) {
  if (!metadata) return;
#if defined(SONARE_WITH_ARRANGEMENT)
  delete[] metadata->content_hash;
  delete[] metadata->external_stem_role;
#endif
  *metadata = {};
}

SonareError sonare_project_marker_name_by_index(const SonareProject* project, size_t index,
                                                char** out_name) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !out_name) return SONARE_ERROR_INVALID_PARAMETER;
  *out_name = nullptr;
  const auto& markers = project->history.project().markers();
  if (index >= markers.size()) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_name = copy_string(markers[index].name);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, index, out_name);
#endif
}

SonareError sonare_project_tempo_segment_count(const SonareProject* project, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  return project_count(project, out_count,
                       [](const arr::Project& p) { return p.tempo_segments().size(); });
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out_count);
#endif
}

SonareError sonare_project_time_signature_count(const SonareProject* project, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  return project_count(project, out_count,
                       [](const arr::Project& p) { return p.time_signatures().size(); });
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out_count);
#endif
}

SonareError sonare_project_compile(SonareProject* project, SonareProjectCompileResult* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out) *out = {};
  if (!project || !out) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  arr::CompileResult result =
      arr::compile(project->history.project(), project->history.midi_content(), project->audio);
  fill_compile_result_from_diagnostics(result.diagnostics, result.timeline.has_value(), out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out);
#endif
}

SonareError sonare_project_last_bounce_compile_result(const SonareProject* project,
                                                      SonareProjectCompileResult* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out) *out = {};
  if (!project || !out) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  fill_compile_result_from_diagnostics(project->last_bounce_diagnostics,
                                       project->last_bounce_has_timeline, out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, out);
#endif
}

void sonare_project_free_compile_result(SonareProjectCompileResult* result) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!result) return;
  delete[] result->diagnostics;
  delete[] result->messages;
  *result = {};
#else
  (void)result;
#endif
}

SonareError sonare_project_set_assist_sidecar(SonareProject* project, const char* module_id,
                                              uint32_t schema_version, uint32_t target_track_id,
                                              double region_start_ppq, double region_end_ppq,
                                              const uint8_t* payload, size_t payload_len) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project || !module_id || module_id[0] == '\0' || (payload_len > 0 && !payload) ||
      payload_len > kMaxBufferSize || !finite_non_negative(region_start_ppq) ||
      !finite_non_negative(region_end_ppq)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  arr::AssistSidecar sidecar;
  sidecar.module_id = module_id;
  sidecar.schema_version = schema_version;
  sidecar.target_track_id = target_track_id;
  sidecar.region_start_ppq = region_start_ppq;
  sidecar.region_end_ppq = region_end_ppq;
  if (payload_len > 0) {
    sidecar.payload.assign(payload, payload + payload_len);
  }
  auto command = std::make_unique<arr::SetAssistSidecar>(std::move(sidecar));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, module_id, schema_version, target_track_id, region_start_ppq,
                              region_end_ppq, payload, payload_len);
#endif
}

size_t sonare_project_assist_sidecar_count(const SonareProject* project) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!project) return 0;
  return project->history.project().assist_sidecars().size();
#else
  (void)project;
  return 0;
#endif
}

SonareError sonare_project_get_assist_sidecar(const SonareProject* project, size_t index,
                                              SonareProjectAssistSidecar* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out) *out = {};
  if (!project || !out) return SONARE_ERROR_INVALID_PARAMETER;
  const auto& sidecars = project->history.project().assist_sidecars();
  if (index >= sidecars.size()) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  const arr::AssistSidecar& sidecar = sidecars[index];
  std::unique_ptr<char[]> module_id(copy_string(sidecar.module_id));
  std::unique_ptr<uint8_t[]> payload;
  if (!sidecar.payload.empty()) {
    payload = std::make_unique<uint8_t[]>(sidecar.payload.size());
    std::memcpy(payload.get(), sidecar.payload.data(), sidecar.payload.size());
  }
  out->module_id = module_id.release();
  out->schema_version = sidecar.schema_version;
  out->target_track_id = sidecar.target_track_id;
  out->region_start_ppq = sidecar.region_start_ppq;
  out->region_end_ppq = sidecar.region_end_ppq;
  out->payload = payload.release();
  out->payload_len = sidecar.payload.size();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(project, index, out);
#endif
}

void sonare_project_free_assist_sidecar(SonareProjectAssistSidecar* sidecar) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!sidecar) return;
  delete[] sidecar->module_id;
  delete[] sidecar->payload;
  *sidecar = {};
#else
  (void)sidecar;
#endif
}
