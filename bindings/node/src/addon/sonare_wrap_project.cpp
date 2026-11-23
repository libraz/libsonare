#include "sonare_wrap_project.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "c_api/project_input_domains.h"
#include "project/common.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node::project;

namespace {

struct AudioSourceMetadataGuard {
  SonareProjectAudioSourceMetadata value{};

  ~AudioSourceMetadataGuard() { sonare_free_project_audio_source_metadata(&value); }
};

struct HeapStringGuard {
  char* value = nullptr;

  ~HeapStringGuard() {
    if (value != nullptr) sonare_free_string(value);
  }
};

// Builds the ProjectSource object from the descriptor and the source's full name or URI, which
// the caller reads through an untruncated getter rather than the descriptor's bounded field.
Napi::Value BuildProjectSource(Napi::Env env, const SonareProject* project,
                               const SonareProjectSource& source, const char* name_or_uri) {
  AudioSourceMetadataGuard metadata;
  if (source.kind == 0) {
    const SonareError metadata_error =
        sonare_project_get_audio_source_metadata(project, source.id, &metadata.value);
    if (metadata_error != SONARE_OK) {
      ThrowIfError(env, metadata_error);
      return env.Undefined();
    }
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("id", source.id);
  out.Set("kind", source.kind);
  out.Set("numChannels", source.num_channels);
  out.Set("storageHandleId", source.storage_handle_id);
  out.Set("sampleRateHint", source.sample_rate_hint);
  out.Set("nameOrUri", name_or_uri);
  out.Set("contentHash", metadata.value.content_hash != nullptr ? metadata.value.content_hash : "");
  out.Set("externalStemRole",
          metadata.value.external_stem_role != nullptr ? metadata.value.external_stem_role : "");
  return out;
}

}  // namespace

Napi::Object ProjectWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "Project",
      {
          InstanceMethod<&ProjectWrap::ToJson>("toJson", napi_default_method),
          InstanceMethod<&ProjectWrap::SetSampleRate>("setSampleRate", napi_default_method),
          InstanceMethod<&ProjectWrap::AddTrack>("addTrack", napi_default_method),
          InstanceMethod<&ProjectWrap::AddClip>("addClip", napi_default_method),
          InstanceMethod<&ProjectWrap::ImportExternalStems>("importExternalStems",
                                                            napi_default_method),
          InstanceMethod<&ProjectWrap::ApplyVocalEdit>("applyVocalEdit", napi_default_method),
          InstanceMethod<&ProjectWrap::GetVocalEditDependencies>("getVocalEditDependencies",
                                                                 napi_default_method),
          InstanceMethod<&ProjectWrap::RehydrateVocalEdits>("rehydrateVocalEdits",
                                                            napi_default_method),
          InstanceMethod<&ProjectWrap::AddLoopRecordingTakes>("addLoopRecordingTakes",
                                                              napi_default_method),
          InstanceMethod<&ProjectWrap::AddMidiClip>("addMidiClip", napi_default_method),
          InstanceMethod<&ProjectWrap::SplitClip>("splitClip", napi_default_method),
          InstanceMethod<&ProjectWrap::TrimClip>("trimClip", napi_default_method),
          InstanceMethod<&ProjectWrap::MoveClip>("moveClip", napi_default_method),
          InstanceMethod<&ProjectWrap::SetTrackKind>("setTrackKind", napi_default_method),
          InstanceMethod<&ProjectWrap::SetClipWarpRef>("setClipWarpRef", napi_default_method),
          InstanceMethod<&ProjectWrap::SetClipWarpMode>("setClipWarpMode", napi_default_method),
          InstanceMethod<&ProjectWrap::SetWarpMap>("setWarpMap", napi_default_method),
          InstanceMethod<&ProjectWrap::RemoveWarpMap>("removeWarpMap", napi_default_method),
          InstanceMethod<&ProjectWrap::SetTrackMidiDestination>("setTrackMidiDestination",
                                                                napi_default_method),
          InstanceMethod<&ProjectWrap::SetPartRig>("setPartRig", napi_default_method),
          InstanceMethod<&ProjectWrap::GetPartRig>("getPartRig", napi_default_method),
          InstanceMethod<&ProjectWrap::ClearPartRig>("clearPartRig", napi_default_method),
          InstanceMethod<&ProjectWrap::SetTrackGain>("setTrackGain", napi_default_method),
          InstanceMethod<&ProjectWrap::SetTrackMute>("setTrackMute", napi_default_method),
          InstanceMethod<&ProjectWrap::SetTrackSolo>("setTrackSolo", napi_default_method),
          InstanceMethod<&ProjectWrap::SetTrackPan>("setTrackPan", napi_default_method),
          InstanceMethod<&ProjectWrap::RemoveClip>("removeClip", napi_default_method),
          InstanceMethod<&ProjectWrap::SetClipGain>("setClipGain", napi_default_method),
          InstanceMethod<&ProjectWrap::SetClipFade>("setClipFade", napi_default_method),
          InstanceMethod<&ProjectWrap::SetClipTakes>("setClipTakes", napi_default_method),
          InstanceMethod<&ProjectWrap::SetClipCompSegments>("setClipCompSegments",
                                                            napi_default_method),
          InstanceMethod<&ProjectWrap::SetClipLoop>("setClipLoop", napi_default_method),
          InstanceMethod<&ProjectWrap::SetClipSource>("setClipSource", napi_default_method),
          InstanceMethod<&ProjectWrap::DuplicateClip>("duplicateClip", napi_default_method),
          InstanceMethod<&ProjectWrap::RemoveTrack>("removeTrack", napi_default_method),
          InstanceMethod<&ProjectWrap::RenameTrack>("renameTrack", napi_default_method),
          InstanceMethod<&ProjectWrap::SetTrackRoute>("setTrackRoute", napi_default_method),
          InstanceMethod<&ProjectWrap::AddAutomationLane>("addAutomationLane", napi_default_method),
          InstanceMethod<&ProjectWrap::EditAutomationLane>("editAutomationLane",
                                                           napi_default_method),
          InstanceMethod<&ProjectWrap::RemoveAutomationLane>("removeAutomationLane",
                                                             napi_default_method),
          InstanceMethod<&ProjectWrap::Undo>("undo", napi_default_method),
          InstanceMethod<&ProjectWrap::Redo>("redo", napi_default_method),
          InstanceMethod<&ProjectWrap::ClearHistory>("clearHistory", napi_default_method),
          InstanceMethod<&ProjectWrap::SetMaxUndoDepth>("setMaxUndoDepth", napi_default_method),
          InstanceMethod<&ProjectWrap::SetMaxHistoryBytes>("setMaxHistoryBytes",
                                                           napi_default_method),
          InstanceMethod<&ProjectWrap::SetMidiEvents>("setMidiEvents", napi_default_method),
          InstanceMethod<&ProjectWrap::ImportSmf>("importSmf", napi_default_method),
          InstanceMethod<&ProjectWrap::ExportSmf>("exportSmf", napi_default_method),
          InstanceMethod<&ProjectWrap::ImportClipFile>("importClipFile", napi_default_method),
          InstanceMethod<&ProjectWrap::ExportClipFile>("exportClipFile", napi_default_method),
          InstanceMethod<&ProjectWrap::SetProgram>("setProgram", napi_default_method),
          InstanceMethod<&ProjectWrap::BakeMidiFx>("bakeMidiFx", napi_default_method),
          InstanceMethod<&ProjectWrap::PreviewMidiFxCount>("previewMidiFxCount",
                                                           napi_default_method),
          InstanceMethod<&ProjectWrap::ValidateMidiNotes>("validateMidiNotes", napi_default_method),
          InstanceMethod<&ProjectWrap::TranscribeToClip>("transcribeToClip", napi_default_method),
          InstanceMethod<&ProjectWrap::AutoTempo>("autoTempo", napi_default_method),
          InstanceMethod<&ProjectWrap::AnalyzeTempo>("analyzeTempo", napi_default_method),
          InstanceMethod<&ProjectWrap::SnapToGrid>("snapToGrid", napi_default_method),
          InstanceMethod<&ProjectWrap::AnnotateKeys>("annotateKeys", napi_default_method),
          InstanceMethod<&ProjectWrap::AnnotateChords>("annotateChords", napi_default_method),
          InstanceMethod<&ProjectWrap::SetAssistSidecar>("setAssistSidecar", napi_default_method),
          InstanceMethod<&ProjectWrap::AssistSidecarCount>("assistSidecarCount",
                                                           napi_default_method),
          InstanceMethod<&ProjectWrap::GetAssistSidecar>("getAssistSidecar", napi_default_method),
          InstanceMethod<&ProjectWrap::AssistSidecars>("assistSidecars", napi_default_method),
          InstanceMethod<&ProjectWrap::Compile>("compile", napi_default_method),
          InstanceMethod<&ProjectWrap::LastBounceCompileResult>("lastBounceCompileResult",
                                                                napi_default_method),
          InstanceMethod<&ProjectWrap::Bounce>("bounce", napi_default_method),
          InstanceMethod<&ProjectWrap::LoadSoundFont>("loadSoundFont", napi_default_method),
          InstanceMethod<&ProjectWrap::ClearSoundFont>("clearSoundFont", napi_default_method),
          InstanceMethod<&ProjectWrap::SoundFontPresetCount>("soundFontPresetCount",
                                                             napi_default_method),
          InstanceMethod<&ProjectWrap::SoundFontManifest>("soundFontManifest", napi_default_method),
          InstanceMethod<&ProjectWrap::GetSampleRate>("getSampleRate", napi_default_method),
          InstanceMethod<&ProjectWrap::SetOverlapPolicy>("setOverlapPolicy", napi_default_method),
          InstanceMethod<&ProjectWrap::GetOverlapPolicy>("getOverlapPolicy", napi_default_method),
          InstanceMethod<&ProjectWrap::SetMixerSceneJson>("setMixerSceneJson", napi_default_method),
          InstanceMethod<&ProjectWrap::SetMarker>("setMarker", napi_default_method),
          InstanceMethod<&ProjectWrap::MarkerByIndex>("markerByIndex", napi_default_method),
          InstanceMethod<&ProjectWrap::TrackByIndex>("trackByIndex", napi_default_method),
          InstanceMethod<&ProjectWrap::ClipByIndex>("clipByIndex", napi_default_method),
          InstanceMethod<&ProjectWrap::SourceByIndex>("sourceByIndex", napi_default_method),
          InstanceMethod<&ProjectWrap::SetTempoSegments>("setTempoSegments", napi_default_method),
          InstanceMethod<&ProjectWrap::SetTimeSignatures>("setTimeSignatures", napi_default_method),
          InstanceMethod<&ProjectWrap::TrackCount>("trackCount", napi_default_method),
          InstanceMethod<&ProjectWrap::ClipCount>("clipCount", napi_default_method),
          InstanceMethod<&ProjectWrap::SourceCount>("sourceCount", napi_default_method),
          InstanceMethod<&ProjectWrap::UnresolvedAudioSourceIds>("unresolvedAudioSourceIds",
                                                                 napi_default_method),
          InstanceMethod<&ProjectWrap::UnresolvedAudioSources>("unresolvedAudioSources",
                                                               napi_default_method),
          InstanceMethod<&ProjectWrap::SetSourceAudio>("setSourceAudio", napi_default_method),
          InstanceMethod<&ProjectWrap::SetAudioSourceMetadata>("setAudioSourceMetadata",
                                                               napi_default_method),
          InstanceMethod<&ProjectWrap::TempoSegmentCount>("tempoSegmentCount", napi_default_method),
          InstanceMethod<&ProjectWrap::TempoSegmentByIndex>("tempoSegmentByIndex",
                                                            napi_default_method),
          InstanceMethod<&ProjectWrap::TimeSignatureByIndex>("timeSignatureByIndex",
                                                             napi_default_method),
          InstanceMethod<&ProjectWrap::TimeSignatureCount>("timeSignatureCount",
                                                           napi_default_method),
          InstanceMethod<&ProjectWrap::MarkerCount>("markerCount", napi_default_method),
          InstanceMethod<&ProjectWrap::Destroy>("destroy", napi_default_method),
          StaticMethod<&ProjectWrap::FromJson>("fromJson", napi_default_method),
      });
  exports.Set("Project", func);
  return exports;
}

Napi::Object ProjectWrap::Wrap(const Napi::CallbackInfo& info, SonareProject* handle) {
  // Inject the already-created native handle through an External so the
  // constructor adopts it instead of allocating a fresh empty project.
  Napi::Env env = info.Env();
  Napi::External<SonareProject> external = Napi::External<SonareProject>::New(env, handle);
  return info.This().As<Napi::Function>().New({external});
}

ProjectWrap::ProjectWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<ProjectWrap>(info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  // Adoption path: fromJson() / Wrap() pass an External wrapping an existing
  // SonareProject* that was created (and ABI-checked) by the static factory.
  if (info.Length() > 0 && info[0].IsExternal()) {
    project_ = info[0].As<Napi::External<SonareProject>>().Data();
    return;
  }

  const uint32_t abi_version = sonare_project_abi_version();
  if (abi_version != kExpectedProjectAbiVersion) {
    sonare_node::ThrowSonareErrorMessage(
        env, SONARE_ERROR_ABI_MISMATCH,
        "libsonare project ABI mismatch: native binary reports version " +
            std::to_string(abi_version) + ", expected " +
            std::to_string(kExpectedProjectAbiVersion) +
            " (0 = arrangement support not compiled in). The prebuilt addon is "
            "incompatible with this binding.");
    return;
  }
  ThrowIfError(env, sonare_project_create(&project_));
  SONARE_NODE_CATCH_VOID(env)
}

ProjectWrap::~ProjectWrap() { DestroyNative(); }

void ProjectWrap::DestroyNative() noexcept {
  if (project_ != nullptr) {
    sonare_project_destroy(project_);
    project_ = nullptr;
  }
  destroy_requested_ = false;
}

Napi::Value ProjectWrap::ToJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  char* json = nullptr;
  size_t len = 0;
  ThrowIfError(env, sonare_project_serialize(project_, &json, &len));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::String out = Napi::String::New(env, json != nullptr ? json : "", len);
  if (json != nullptr) sonare_free_string(json);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::FromJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "json must be a string").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // A trailing `withDiagnostics` of true returns { project, diagnostics } instead of the project.
  bool with_diagnostics = false;
  if (!OptionalBoolArg(env, info, 1, "withDiagnostics", false, &with_diagnostics)) {
    return env.Undefined();
  }
  const uint32_t abi_version = sonare_project_abi_version();
  if (abi_version != kExpectedProjectAbiVersion) {
    sonare_node::ThrowSonareErrorMessage(
        env, SONARE_ERROR_ABI_MISMATCH,
        "libsonare project ABI mismatch: native binary reports version " +
            std::to_string(abi_version) + ", expected " +
            std::to_string(kExpectedProjectAbiVersion) +
            " (0 = arrangement support not compiled in).");
    return env.Undefined();
  }
  std::string json = sonare_node::node_narrow_string(env, info[0], "json");
  SonareProject* handle = nullptr;
  char* diag = nullptr;
  const SonareError err =
      sonare_project_deserialize(sonare_node::ElementData(json), json.size(), &handle, &diag);
  std::string diagnostics = diag != nullptr ? diag : "";
  if (diag != nullptr) sonare_free_string(diag);
  if (err != SONARE_OK) {
    sonare_project_destroy(handle);
    sonare_node::ThrowSonareErrorMessage(
        env, err, diagnostics.empty() ? "failed to deserialize project JSON" : diagnostics);
    return env.Undefined();
  }
  if (!with_diagnostics) return ProjectWrap::Wrap(info, handle);

  Napi::Object out = Napi::Object::New(env);
  out.Set("project", ProjectWrap::Wrap(info, handle));
  out.Set("diagnostics", diagnostics);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetSampleRate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  int sample_rate = 0;
  if (!sonare_node::ReadWholeHertzSampleRate(env, info[0], &sample_rate)) return env.Undefined();
  ThrowIfError(env, sonare_project_set_sample_rate(project_, sample_rate));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::GetSampleRate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  double out = 0.0;
  ThrowIfError(env, sonare_project_get_sample_rate(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetOverlapPolicy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t policy = 0;
  if (!OptionalUint32Arg(env, info, 0, "policy", 0, &policy)) return env.Undefined();
  ThrowIfError(env, sonare_project_set_overlap_policy(project_, policy));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::GetOverlapPolicy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t out = 0;
  ThrowIfError(env, sonare_project_get_overlap_policy(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, out);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetMixerSceneJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  std::string scene;
  if (!OptionalStringArg(env, info, 0, "sceneJson", "", &scene)) return env.Undefined();
  ThrowIfError(env, sonare_project_set_mixer_scene_json(project_, scene.c_str()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetMarker(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  if (info.Length() <= 0 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "expected a marker object").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object obj = info[0].As<Napi::Object>();
  SonareProjectMarker marker{};
  marker.id = Uint32Property(obj, "id", kZeroIsSentinel);
  marker.kind = sonare_node::MidiByteProperty(env, obj, "kind", SONARE_MARKER_KIND_MARKER);
  marker.key_fifths = sonare_node::DomainProperty<int8_t>(obj, "keyFifths", 0,
                                                          sonare_c_detail::kMarkerKeyFifthsDomain);
  marker.key_minor = BoolProperty(obj, "keyMinor", marker.key_minor != 0) ? 1 : 0;
  if (!RequiredDoubleProperty(env, obj, "ppq", &marker.ppq)) return env.Undefined();
  const std::string name = StringProperty(obj, "name", "");
  uint32_t out_id = 0;
  const SonareError err = sonare_project_set_marker(project_, &marker, name.c_str(), &out_id);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareErrorFlattening(env, err, "marker");
    return env.Undefined();
  }
  return Napi::Number::New(env, out_id);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::MarkerByIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  size_t index = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "index", &index)) return env.Undefined();
  SonareProjectMarker marker{};
  ThrowIfError(env, sonare_project_marker_by_index(project_, index, &marker));
  if (env.IsExceptionPending()) return env.Undefined();
  char* full_name = nullptr;
  ThrowIfError(env, sonare_project_marker_name_by_index(project_, index, &full_name));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  out.Set("id", Napi::Number::New(env, marker.id));
  out.Set("ppq", Napi::Number::New(env, marker.ppq));
  out.Set("name", Napi::String::New(env, full_name));
  sonare_free_string(full_name);
  out.Set("kind", Napi::Number::New(env, marker.kind));
  out.Set("keyFifths", Napi::Number::New(env, marker.key_fifths));
  out.Set("keyMinor", Napi::Boolean::New(env, marker.key_minor != 0));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::TrackByIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  size_t index = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "index", &index)) return env.Undefined();
  SonareProjectTrack track{};
  ThrowIfError(env, sonare_project_track_by_index(project_, index, &track));
  if (env.IsExceptionPending()) return env.Undefined();
  HeapStringGuard name;
  ThrowIfError(env, sonare_project_track_name_by_index(project_, index, &name.value));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  out.Set("id", track.id);
  out.Set("kind", track.kind);
  out.Set("midiDestinationId", track.midi_destination_id);
  out.Set("gain", track.gain);
  out.Set("pan", track.pan);
  out.Set("mute", track.mute != 0);
  out.Set("solo", track.solo != 0);
  out.Set("name", name.value);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::ClipByIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  size_t index = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "index", &index)) return env.Undefined();
  SonareProjectClip clip{};
  ThrowIfError(env, sonare_project_clip_by_index(project_, index, &clip));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  out.Set("id", clip.id);
  out.Set("trackId", clip.track_id);
  out.Set("sourceId", clip.source_id);
  out.Set("sourceKind", clip.source_kind);
  out.Set("startPpq", clip.start_ppq);
  out.Set("lengthPpq", clip.length_ppq);
  out.Set("sourceOffsetPpq", clip.source_offset_ppq);
  out.Set("gain", clip.gain);
  out.Set("loopMode", clip.loop_mode);
  out.Set("loopLengthPpq", clip.loop_length_ppq);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SourceByIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  size_t index = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "index", &index)) return env.Undefined();
  SonareProjectSource source{};
  ThrowIfError(env, sonare_project_source_by_index(project_, index, &source));
  if (env.IsExceptionPending()) return env.Undefined();
  HeapStringGuard name_or_uri;
  ThrowIfError(env,
               sonare_project_source_name_or_uri_by_index(project_, index, &name_or_uri.value));
  if (env.IsExceptionPending()) return env.Undefined();
  return BuildProjectSource(env, project_, source, name_or_uri.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::TempoSegmentByIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  size_t index = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "index", &index)) return env.Undefined();
  SonareProjectTempoSegment seg{};
  ThrowIfError(env, sonare_project_tempo_segment_by_index(project_, index, &seg));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  // The same three fields setTempoSegments accepts, so a segment read back can
  // be written straight to another project.
  out.Set("startPpq", Napi::Number::New(env, seg.start_ppq));
  out.Set("bpm", Napi::Number::New(env, seg.bpm));
  out.Set("endBpm", Napi::Number::New(env, seg.end_bpm));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::TimeSignatureByIndex(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  size_t index = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "index", &index)) return env.Undefined();
  SonareProjectTimeSignatureSegment seg{};
  ThrowIfError(env, sonare_project_time_signature_by_index(project_, index, &seg));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Object out = Napi::Object::New(env);
  out.Set("startPpq", Napi::Number::New(env, seg.start_ppq));
  out.Set("numerator", Napi::Number::New(env, seg.numerator));
  out.Set("denominator", Napi::Number::New(env, seg.denominator));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetTempoSegments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  std::vector<SonareProjectTempoSegment> segments;
  if (info.Length() > 0 && info[0].IsArray()) {
    Napi::Array input = info[0].As<Napi::Array>();
    const uint32_t input_count = sonare_node::node_list_length(input, "segments");
    segments.reserve(input_count);
    for (uint32_t i = 0; i < input_count; ++i) {
      Napi::Value entry = input.Get(i);
      if (!entry.IsObject()) {
        Napi::TypeError::New(env, "tempo segment must be an object").ThrowAsJavaScriptException();
        return env.Undefined();
      }
      Napi::Object obj = entry.As<Napi::Object>();
      SonareProjectTempoSegment seg{};
      if (!RequiredDoubleProperty(env, obj, "startPpq", &seg.start_ppq)) return env.Undefined();
      if (!RequiredDoubleProperty(env, obj, "bpm", &seg.bpm)) return env.Undefined();
      seg.end_bpm = DoubleProperty(obj, "endBpm", 0.0);
      if (env.IsExceptionPending()) return env.Undefined();
      segments.push_back(seg);
    }
  } else if (info.Length() > 0 && !info[0].IsUndefined() && !info[0].IsNull()) {
    Napi::TypeError::New(env, "segments must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_tempo_segments(
                        project_, segments.empty() ? nullptr : segments.data(), segments.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetTimeSignatures(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  std::vector<SonareProjectTimeSignatureSegment> segments;
  if (info.Length() > 0 && info[0].IsArray()) {
    Napi::Array input = info[0].As<Napi::Array>();
    const uint32_t input_count = sonare_node::node_list_length(input, "segments");
    segments.reserve(input_count);
    for (uint32_t i = 0; i < input_count; ++i) {
      Napi::Value entry = input.Get(i);
      if (!entry.IsObject()) {
        Napi::TypeError::New(env, "time-signature segment must be an object")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      Napi::Object obj = entry.As<Napi::Object>();
      SonareProjectTimeSignatureSegment seg{};
      if (!RequiredDoubleProperty(env, obj, "startPpq", &seg.start_ppq)) return env.Undefined();
      const Napi::Value numerator = obj.Get("numerator");
      const Napi::Value denominator = obj.Get("denominator");
      if (!RequiredDomainValue(env, numerator, "numerator",
                               sonare_c_detail::kTimeSignatureNumeratorDomain, &seg.numerator) ||
          !RequiredDomainValue(env, denominator, "denominator",
                               sonare_c_detail::kTimeSignatureDenominatorDomain,
                               &seg.denominator)) {
        return env.Undefined();
      }
      segments.push_back(seg);
    }
  } else if (info.Length() > 0 && !info[0].IsUndefined() && !info[0].IsNull()) {
    Napi::TypeError::New(env, "segments must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_time_signatures(
                        project_, segments.empty() ? nullptr : segments.data(), segments.size()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::TrackCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  size_t out = 0;
  ThrowIfError(env, sonare_project_track_count(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out));
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::ClipCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  size_t out = 0;
  ThrowIfError(env, sonare_project_clip_count(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out));
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SourceCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  size_t out = 0;
  ThrowIfError(env, sonare_project_source_count(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out));
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::UnresolvedAudioSourceIds(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  size_t count = 0;
  ThrowIfError(env, sonare_project_unresolved_audio_source_count(project_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Array ids = Napi::Array::New(env, count);
  for (size_t i = 0; i < count; ++i) {
    uint32_t source_id = 0;
    ThrowIfError(env, sonare_project_unresolved_audio_source_id_by_index(project_, i, &source_id));
    if (env.IsExceptionPending()) return env.Undefined();
    ids.Set(static_cast<uint32_t>(i), Napi::Number::New(env, source_id));
  }
  return ids;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::UnresolvedAudioSources(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  size_t count = 0;
  ThrowIfError(env, sonare_project_unresolved_audio_source_count(project_, &count));
  if (env.IsExceptionPending()) return env.Undefined();
  Napi::Array sources = Napi::Array::New(env, count);
  for (size_t i = 0; i < count; ++i) {
    SonareProjectSource source{};
    ThrowIfError(env, sonare_project_unresolved_audio_source_by_index(project_, i, &source));
    if (env.IsExceptionPending()) return env.Undefined();
    HeapStringGuard uri;
    ThrowIfError(env, sonare_project_get_audio_source_uri(project_, source.id, &uri.value));
    if (env.IsExceptionPending()) return env.Undefined();
    Napi::Value built = BuildProjectSource(env, project_, source, uri.value);
    if (env.IsExceptionPending()) return env.Undefined();
    sources.Set(static_cast<uint32_t>(i), built);
  }
  return sources;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetSourceAudio(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  if (!sonare_node::IsFloat32Array(info[1])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 3) {
    Napi::TypeError::New(env, "channels must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 4) {
    Napi::TypeError::New(env, "sampleRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  uint32_t source_id = 0;
  int channels = 0;
  int sample_rate = 0;
  if (!OptionalUint32Arg(env, info, 0, "sourceId", 0, &source_id) ||
      !Int32Arg(env, info, 2, "channels", 0, &channels) ||
      !Int32Arg(env, info, 3, "sampleRate", 0, &sample_rate)) {
    return env.Undefined();
  }
  Napi::Float32Array audio = info[1].As<Napi::Float32Array>();
  if (channels <= 0 || audio.ElementLength() % static_cast<size_t>(channels) != 0) {
    Napi::RangeError::New(env, "audio length must be a multiple of channels")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_set_source_audio(
                        project_, source_id, sonare_node::ElementData(audio),
                        static_cast<int64_t>(audio.ElementLength() / static_cast<size_t>(channels)),
                        channels, sample_rate));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::TempoSegmentCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  size_t out = 0;
  ThrowIfError(env, sonare_project_tempo_segment_count(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out));
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::TimeSignatureCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  size_t out = 0;
  ThrowIfError(env, sonare_project_time_signature_count(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out));
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::MarkerCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  size_t out = 0;
  ThrowIfError(env, sonare_project_marker_count(project_, &out));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(out));
  SONARE_NODE_CATCH(env)
}

void ProjectWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY(void) info;
  if (busy_call_count_ != 0) {
    destroy_requested_ = true;
  } else {
    DestroyNative();
  }
  SONARE_NODE_CATCH_VOID(env)
}
