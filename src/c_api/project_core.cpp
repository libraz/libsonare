#include "c_api/ppq_domains.h"
#include "c_api/project_internal.h"
#include "c_api/project_timeline_internal.h"
#if defined(SONARE_WITH_MASTERING) && defined(SONARE_WITH_ARRANGEMENT)
#include "c_api/checked_insert.h"
#endif
#include "mixing/api/scene.h"
#include "util/parameter_checks.h"
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

#if defined(SONARE_WITH_ARRANGEMENT)
namespace {

// Refuses an index at or past the number of @p what the project holds.
SonareError refuse_index_past(const char* what) {
  return run_validation([what] {
    sonare::util::check::refuse("index", std::string("must be less than the number of ") + what);
  });
}

// Refuses a source_id that names no audio source of the project.
SonareError refuse_unknown_audio_source() {
  return run_validation(
      [] { sonare::util::check::refuse_unknown_id("source_id", "an audio source", "project"); });
}

}  // namespace
#endif

SonareError sonare_project_create(SonareProject** out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out) *out = nullptr;
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  *out = new SonareProject{};
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out) *out = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, out);
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
  if (out_json) *out_json = nullptr;
  if (out_len) *out_len = 0;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_json);
  SONARE_C_TRY
  const std::string json = sonare::serialize::project_to_json(project->history.project(),
                                                              project->history.midi_content());
  *out_json = copy_string(json);
  if (out_len) *out_len = json.size();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_json) *out_json = {};
  if (out_len) *out_len = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out_json, out_len);
#endif
}

namespace {

#if defined(SONARE_WITH_ARRANGEMENT)
// One line per diagnostic: "<code>: <message>" for the diagnostics out-parameter, the message
// alone for the error text a refused load leaves, which names no internal code.
std::string serialize_deserialize_diagnostics(
    const std::vector<sonare::serialize::Diagnostic>& diagnostics, bool with_codes = true) {
  std::string joined;
  for (size_t i = 0; i < diagnostics.size(); ++i) {
    if (i > 0) joined += '\n';
    if (with_codes) {
      joined += diagnostics[i].code;
      joined += ": ";
    }
    joined += diagnostics[i].message;
  }
  return joined;
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

SonareError sonare_project_deserialize(const char* document, size_t len, SonareProject** out,
                                       char** out_diag) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_diag);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(document);
  SONARE_C_REQUIRE_NONNULL(out);
  if (len > sonare::resource::kDefaultProjectImportResourceLimits.max_json_bytes) {
    sonare_c_detail::set_last_error(SONARE_ERROR_INVALID_FORMAT, "JSON byte budget exceeded");
    return SONARE_ERROR_INVALID_FORMAT;
  }
  SONARE_C_TRY
  sonare::serialize::DeserializeResult result =
      sonare::serialize::project_from_json(std::string(document, len));
  if (!result.ok()) {
    std::unique_ptr<char[]> diagnostics;
    if (out_diag) {
      diagnostics.reset(copy_string(serialize_deserialize_diagnostics(result.diagnostics)));
    }
    const std::string message = serialize_deserialize_diagnostics(result.diagnostics, false);
    sonare_c_detail::set_last_error(SONARE_ERROR_INVALID_FORMAT, message.c_str(), result.reason);
    if (out_diag) *out_diag = diagnostics.release();
    return SONARE_ERROR_INVALID_FORMAT;
  }
#if defined(SONARE_WITH_MASTERING)
  if (const auto* scene = result.project->find_scene()) {
    try {
      sonare::mastering::api::validate_scene_insert_params(*scene, "scene",
                                                           result.project->sample_rate());
    } catch (const sonare::SonareException& error) {
      if (error.code() == sonare::ErrorCode::InvalidFormat && out_diag != nullptr) {
        *out_diag = copy_string(std::string("invalid_format: ") + error.what());
      }
      throw;
    }
  }
#endif
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
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, document,
                                                 len, out, out_diag);
#endif
}

#if defined(SONARE_WITH_ARRANGEMENT)
namespace {
void fill_audio_source(SonareProjectSource* out, const arr::AudioSourceRef& audio) {
  out->id = audio.id;
  out->num_channels = audio.channel_count;
  out->storage_handle_id = audio.storage_handle_id;
  out->sample_rate_hint = audio.sample_rate_hint;
  copy_utf8_prefix(out->name_or_uri, audio.uri);
}
}  // namespace
#endif

SonareError sonare_project_unresolved_audio_source_count(const SonareProject* project,
                                                         size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_count) *out_count = 0;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_count);
  SONARE_C_TRY
  size_t count = 0;
  for (const arr::ClipSource& source : project->history.project().sources()) {
    if (const auto* audio = std::get_if<arr::AudioSourceRef>(&source);
        audio != nullptr && project->audio.find(audio->id) == nullptr) {
      ++count;
    }
  }
  *out_count = count;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_count) *out_count = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out_count);
#endif
}

SonareError sonare_project_unresolved_audio_source_id_by_index(const SonareProject* project,
                                                               size_t index,
                                                               uint32_t* out_source_id) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_source_id) *out_source_id = 0;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_source_id);
  SONARE_C_TRY
  size_t current = 0;
  for (const arr::ClipSource& source : project->history.project().sources()) {
    const auto* audio = std::get_if<arr::AudioSourceRef>(&source);
    if (audio == nullptr || project->audio.find(audio->id) != nullptr) continue;
    if (current++ == index) {
      *out_source_id = audio->id;
      return SONARE_OK;
    }
  }
  return refuse_index_past("unresolved audio sources");
  SONARE_C_CATCH
#else
  if (out_source_id) *out_source_id = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 index, out_source_id);
#endif
}

SonareError sonare_project_unresolved_audio_source_by_index(const SonareProject* project,
                                                            size_t index,
                                                            SonareProjectSource* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out) *out = {};
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  size_t current = 0;
  for (const arr::ClipSource& source : project->history.project().sources()) {
    const auto* audio = std::get_if<arr::AudioSourceRef>(&source);
    if (audio == nullptr || project->audio.find(audio->id) != nullptr) continue;
    if (current++ == index) {
      fill_audio_source(out, *audio);
      return SONARE_OK;
    }
  }
  return refuse_index_past("unresolved audio sources");
  SONARE_C_CATCH
#else
  if (out) *out = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 index, out);
#endif
}

SonareError sonare_project_set_sample_rate(SonareProject* project, int sample_rate) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_TRY
  if (!sonare::is_supported_sample_rate(static_cast<double>(sample_rate))) {
    // Name the rule, read from the constants the predicate checks, so a facade can
    // report why a rate it passed through was refused.
    sonare::util::check::refuse("sample_rate", sonare::util::refusal::integer_range_predicate(
                                                   kMinSampleRate, kMaxSampleRate));
  }
  // Through the history like every other setter. Set directly on the Project it
  // owns, this was the one piece of public project state undo could not reach:
  // addTrack() then setSampleRate(96000) then undo() removed the track and left
  // the rate at 96 kHz, and the wrong rate was then serialized into the saved
  // document. The facades all promise that every mutation routes through the
  // native EditHistory, so the sample rate has to be a command too.
  auto command = std::make_unique<arr::SetSampleRate>(static_cast<double>(sample_rate));
  if (!project->history.apply(std::move(command))) return SONARE_ERROR_INVALID_STATE;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 sample_rate);
#endif
}

SonareError sonare_project_get_sample_rate(const SonareProject* project, double* out_sample_rate) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_sample_rate) *out_sample_rate = 0.0;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_sample_rate);
  *out_sample_rate = project->history.project().sample_rate();
  return SONARE_OK;
#else
  if (out_sample_rate) *out_sample_rate = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out_sample_rate);
#endif
}

SonareError sonare_project_get_overlap_policy(const SonareProject* project,
                                              uint32_t* out_overlap_policy) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_overlap_policy) *out_overlap_policy = 0;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_overlap_policy);
  *out_overlap_policy = static_cast<uint32_t>(project->history.project().overlap_policy());
  return SONARE_OK;
#else
  if (out_overlap_policy) *out_overlap_policy = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out_overlap_policy);
#endif
}

namespace {

#if defined(SONARE_WITH_ARRANGEMENT)
template <typename CountFn>
SonareError project_count(const SonareProject* project, size_t* out_count, CountFn count_fn) {
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_count);
  *out_count = count_fn(project->history.project());
  return SONARE_OK;
}
#endif

}  // namespace

SonareError sonare_project_track_count(const SonareProject* project, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  SONARE_C_TRY
#if defined(SONARE_WITH_ARRANGEMENT)
  return project_count(project, out_count, [](const arr::Project& p) { return p.tracks().size(); });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out_count);
#endif
  SONARE_C_CATCH
}

SonareError sonare_project_clip_count(const SonareProject* project, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  SONARE_C_TRY
#if defined(SONARE_WITH_ARRANGEMENT)
  return project_count(project, out_count, [](const arr::Project& p) { return p.clips().size(); });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out_count);
#endif
  SONARE_C_CATCH
}

SonareError sonare_project_source_count(const SonareProject* project, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  SONARE_C_TRY
#if defined(SONARE_WITH_ARRANGEMENT)
  return project_count(project, out_count,
                       [](const arr::Project& p) { return p.sources().size(); });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out_count);
#endif
  SONARE_C_CATCH
}

SonareError sonare_project_marker_count(const SonareProject* project, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  SONARE_C_TRY
#if defined(SONARE_WITH_ARRANGEMENT)
  return project_count(project, out_count,
                       [](const arr::Project& p) { return p.markers().size(); });
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out_count);
#endif
  SONARE_C_CATCH
}

SonareError sonare_project_marker_by_index(const SonareProject* project, size_t index,
                                           SonareProjectMarker* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out) *out = {};
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  const std::vector<arr::ProjectMarker>& markers = project->history.project().markers();
  if (index >= markers.size()) return refuse_index_past("markers");
  const arr::ProjectMarker& m = markers[index];
  out->id = m.id;
  out->kind = m.kind;
  out->key_fifths = m.key_fifths;
  out->key_minor = m.key_minor ? 1 : 0;
  out->ppq = m.ppq;
  return SONARE_OK;
#else
  if (out) *out = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 index, out);
#endif
}

SonareError sonare_project_track_by_index(const SonareProject* project, size_t index,
                                          SonareProjectTrack* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out) *out = {};
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  const auto& tracks = project->history.project().tracks();
  if (index >= tracks.size()) return refuse_index_past("tracks");
  const arr::Track& track = tracks[index];
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
  if (out) *out = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 index, out);
#endif
}

SonareError sonare_project_clip_by_index(const SonareProject* project, size_t index,
                                         SonareProjectClip* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out) *out = {};
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  const auto& clips = project->history.project().clips();
  if (index >= clips.size()) return refuse_index_past("clips");
  const arr::EditClip& clip = clips[index];
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
  if (out) *out = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 index, out);
#endif
}

SonareError sonare_project_track_name_by_index(const SonareProject* project, size_t index,
                                               char** out_name) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_name) *out_name = nullptr;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_name);
  const auto& tracks = project->history.project().tracks();
  if (index >= tracks.size()) return refuse_index_past("tracks");
  SONARE_C_TRY
  *out_name = copy_string(tracks[index].name);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_name) *out_name = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 index, out_name);
#endif
}

SonareError sonare_project_track_route_by_index(const SonareProject* project, size_t index,
                                                char** out_channel_strip_ref,
                                                char** out_output_target) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_channel_strip_ref, out_output_target);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_channel_strip_ref);
  SONARE_C_REQUIRE_NONNULL(out_output_target);
  const auto& tracks = project->history.project().tracks();
  if (index >= tracks.size()) return refuse_index_past("tracks");
  SONARE_C_TRY
  if (out_channel_strip_ref == out_output_target) {
    sonare::util::check::refuse_relation({"out_channel_strip_ref", "out_output_target"},
                                         "{0} and {1} must point to different outputs");
  }
  const arr::Track& track = tracks[index];
  std::unique_ptr<char[]> channel_strip_ref(
      track.channel_strip_ref.empty() ? nullptr : copy_string(track.channel_strip_ref));
  std::unique_ptr<char[]> output_target(
      track.output_target.empty() ? nullptr : copy_string(track.output_target));
  *out_channel_strip_ref = channel_strip_ref.release();
  *out_output_target = output_target.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 index, out_channel_strip_ref, out_output_target);
#endif
}

SonareError sonare_project_mixer_scene_json(const SonareProject* project, char** out_json) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_json);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_json);
  SONARE_C_TRY
  const sonare::mixing::api::Scene* scene = project->history.project().find_scene();
  if (scene != nullptr) *out_json = copy_string(sonare::mixing::api::scene_to_json(*scene));
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out_json);
#endif
}

SonareError sonare_project_source_name_or_uri_by_index(const SonareProject* project, size_t index,
                                                       char** out_name_or_uri) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out_name_or_uri) *out_name_or_uri = nullptr;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_name_or_uri);
  SONARE_C_TRY
  const auto& sources = project->history.project().sources();
  if (index >= sources.size()) return refuse_index_past("sources");
  const arr::ClipSource& source = sources[index];
  if (const auto* audio = std::get_if<arr::AudioSourceRef>(&source)) {
    *out_name_or_uri = copy_string(audio->uri);
  } else {
    *out_name_or_uri = copy_string(std::get<arr::MidiSourceRef>(source).name);
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_name_or_uri) *out_name_or_uri = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 index, out_name_or_uri);
#endif
}

SonareError sonare_project_source_by_index(const SonareProject* project, size_t index,
                                           SonareProjectSource* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out) *out = {};
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  const auto& sources = project->history.project().sources();
  if (index >= sources.size()) return refuse_index_past("sources");
  const arr::ClipSource& source = sources[index];
  out->kind = static_cast<uint32_t>(arr::source_kind(source));
  if (const auto* audio = std::get_if<arr::AudioSourceRef>(&source)) {
    fill_audio_source(out, *audio);
  } else {
    const auto& midi = std::get<arr::MidiSourceRef>(source);
    out->id = midi.id;
    out->num_channels = midi.channel_hint;
    copy_utf8_prefix(out->name_or_uri, midi.name);
  }
  return SONARE_OK;
#else
  if (out) *out = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 index, out);
#endif
  SONARE_C_CATCH
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
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  const arr::ClipSource* source =
      source_id != 0 ? project->history.project().find_source(source_id) : nullptr;
  const auto* audio = source ? std::get_if<arr::AudioSourceRef>(source) : nullptr;
  if (audio == nullptr) return refuse_unknown_audio_source();
  SONARE_C_TRY
  std::unique_ptr<char[]> content_hash(copy_string(audio->content_hash));
  std::unique_ptr<char[]> external_stem_role(copy_string(audio->external_stem_role));
  out->content_hash = content_hash.release();
  out->external_stem_role = external_stem_role.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 source_id, out);
#endif
}

SonareError sonare_project_get_audio_source_uri(const SonareProject* project, uint32_t source_id,
                                                char** out_uri) {
  SONARE_C_API_ENTRY;
  if (out_uri) *out_uri = nullptr;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_uri);
  const arr::ClipSource* source =
      source_id != 0 ? project->history.project().find_source(source_id) : nullptr;
  const auto* audio = source ? std::get_if<arr::AudioSourceRef>(source) : nullptr;
  if (audio == nullptr) return refuse_unknown_audio_source();
  SONARE_C_TRY
  *out_uri = copy_string(audio->uri);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 source_id, out_uri);
#endif
}

void sonare_free_project_audio_source_metadata(SonareProjectAudioSourceMetadata* metadata) {
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
  if (out_name) *out_name = nullptr;
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out_name);
  const auto& markers = project->history.project().markers();
  if (index >= markers.size()) return refuse_index_past("markers");
  SONARE_C_TRY
  *out_name = copy_string(markers[index].name);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_name) *out_name = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 index, out_name);
#endif
}

void sonare_free_project_tempo_segment_list(SonareProjectTempoSegmentList* list) {
  if (list == nullptr) return;
  delete[] list->items;
  *list = {};
}

SonareError sonare_project_tempo_segments(const SonareProject* project,
                                          SonareProjectTempoSegmentList* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  const auto& segments = project->history.project().tempo_segments();
  sonare_c_detail::StagedOutput<SonareProjectTempoSegmentList,
                                &sonare_free_project_tempo_segment_list>
      staged;
  if (!segments.empty()) {
    staged->items = new SonareProjectTempoSegment[segments.size()]{};
    staged->count = segments.size();
    for (size_t i = 0; i < segments.size(); ++i) {
      const auto& segment = segments[i];
      staged->items[i] = {segment.start_ppq, segment.bpm, segment.end_bpm};
    }
  }
  staged.publish(out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out);
#endif
}

void sonare_free_project_time_signature_list(SonareProjectTimeSignatureList* list) {
  if (list == nullptr) return;
  delete[] list->items;
  *list = {};
}

SonareError sonare_project_time_signatures(const SonareProject* project,
                                           SonareProjectTimeSignatureList* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  const auto& segments = project->history.project().time_signatures();
  sonare_c_detail::StagedOutput<SonareProjectTimeSignatureList,
                                &sonare_free_project_time_signature_list>
      staged;
  if (!segments.empty()) {
    staged->items = new SonareProjectTimeSignatureSegment[segments.size()]{};
    staged->count = segments.size();
    for (size_t i = 0; i < segments.size(); ++i) {
      const auto& segment = segments[i];
      staged->items[i] = {segment.start_ppq, segment.time_sig.numerator,
                          segment.time_sig.denominator};
    }
  }
  staged.publish(out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out);
#endif
}

const char* sonare_project_diagnostic_code_name(int code) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (code < 0) return nullptr;
  return sonare::arrangement::diagnostic_code_name(
      static_cast<sonare::arrangement::Diagnostic::Code>(code));
#else
  (void)code;
  return nullptr;
#endif
}

const char* sonare_project_diagnostic_severity_name(int severity) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (severity < 0) return nullptr;
  return sonare::arrangement::diagnostic_severity_name(
      static_cast<sonare::arrangement::Diagnostic::Severity>(severity));
#else
  (void)severity;
  return nullptr;
#endif
}

SonareError sonare_project_compile(SonareProject* project, SonareProjectCompileResult* out,
                                   SonareProjectTimeline** out_timeline) {
  SONARE_C_API_ENTRY;
  if (out) *out = {};
  if (out_timeline) *out_timeline = nullptr;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  arr::CompileResult result =
      arr::compile(project->history.project(), project->history.midi_content(), project->audio);
  std::unique_ptr<SonareProjectTimeline> handle;
  if (out_timeline != nullptr && result.timeline.has_value()) {
    handle = std::make_unique<SonareProjectTimeline>();
    handle->timeline = std::make_shared<const arr::CompiledTimeline>(std::move(*result.timeline));
  }
  fill_compile_result_from_diagnostics(result.diagnostics, result.timeline.has_value(), out);
  if (out_timeline != nullptr) *out_timeline = handle.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out, out_timeline);
#endif
}

SonareError sonare_project_last_bounce_compile_result(const SonareProject* project,
                                                      SonareProjectCompileResult* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  if (out) *out = {};
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  fill_compile_result_from_diagnostics(project->last_bounce_diagnostics,
                                       project->last_bounce_has_timeline, out);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out) *out = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 out);
#endif
}

void sonare_free_project_compile_result(SonareProjectCompileResult* result) {
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
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(module_id);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(payload, payload_len);
  if (const SonareError err = run_validation([&] {
        namespace check = sonare::util::check;
        check::non_empty(module_id, "module_id");
        check::within(region_start_ppq, sonare_c_detail::kPublicPpqDomain, "region_start_ppq");
        check::within(region_end_ppq, sonare_c_detail::kPublicPpqDomain, "region_end_ppq");
        check::integer_in_range(static_cast<long long>(payload_len), 0,
                                static_cast<long long>(kMaxBufferSize), "payload_len");
      });
      err != SONARE_OK) {
    return err;
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
  return sonare_c_detail::refuse_not_compiled_in(
      sonare_c_detail::Feature::kArrangement, project, module_id, schema_version, target_track_id,
      region_start_ppq, region_end_ppq, payload, payload_len);
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
  SONARE_C_REQUIRE_NONNULL(project);
  SONARE_C_REQUIRE_NONNULL(out);
  const auto& sidecars = project->history.project().assist_sidecars();
  if (index >= sidecars.size()) return refuse_index_past("assist sidecars");
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
  if (out) *out = {};
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kArrangement, project,
                                                 index, out);
#endif
}

void sonare_free_project_assist_sidecar(SonareProjectAssistSidecar* sidecar) {
#if defined(SONARE_WITH_ARRANGEMENT)
  if (!sidecar) return;
  delete[] sidecar->module_id;
  delete[] sidecar->payload;
  *sidecar = {};
#else
  (void)sidecar;
#endif
}
