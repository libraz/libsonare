#include "sonare_wrap_project.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <variant>
#include <vector>

#include "c_api/midi_input_domains.h"
#include "c_api/project_input_domains.h"
#include "c_api/synth_patch_fields.h"
#include "project/common.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_synth_patch.h"
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

struct StringListGuard {
  SonareStringList value{};

  ~StringListGuard() {
    if (value.count != 0 && value.items == nullptr) value.count = 0;
    sonare_free_string_list(&value);
  }
};

struct ProjectInstrumentListGuard {
  SonareProjectInstrumentList value{};

  ~ProjectInstrumentListGuard() {
    if (value.count != 0 && value.items == nullptr) value.count = 0;
    sonare_free_project_instrument_list(&value);
  }
};

bool ReadDestinationInstrumentOptions(Napi::Env env, const Napi::Value& value,
                                      SonareProjectInstrument* instrument) {
  if (value.IsUndefined() || value.IsNull()) return true;
  Napi::Object options;
  if (!RequiredObjectValue(env, value, "options", &options)) return false;
  const Napi::Value bank_value = options.Get("bank");
  if (!bank_value.IsUndefined() && !bank_value.IsNull()) {
    Napi::Object bank;
    if (!RequiredObjectValue(env, bank_value, "options.bank", &bank) ||
        !RequiredDomainValue(env, bank.Get("msb"), "options.bank.msb",
                             sonare_c_detail::kMidiDataByteDomain, &instrument->bank_msb) ||
        !RequiredDomainValue(env, bank.Get("lsb"), "options.bank.lsb",
                             sonare_c_detail::kMidiDataByteDomain, &instrument->bank_lsb)) {
      return false;
    }
  }
  const Napi::Value program = options.Get("program");
  if (!program.IsUndefined() && !program.IsNull() &&
      !RequiredDomainValue(env, program, "options.program", sonare_c_detail::kMidiDataByteDomain,
                           &instrument->program)) {
    return false;
  }
  return true;
}

bool ReadDestinationInstrument(Napi::Env env, const Napi::Object& object,
                               SonareProjectInstrument* instrument,
                               sonare_node::SynthPatchStorage* synth_storage,
                               SonareBuiltinSynthConfig* builtin, SonareSf2InstrumentConfig* sf2,
                               std::string* sound_font_uri) {
  std::string kind;
  if (!RequiredStringProperty(env, object, "kind", &kind)) return false;
  if (kind == "synth") {
    instrument->kind = SONARE_PROJECT_INSTRUMENT_SYNTH;
    if (!ReadSynthPatch(env, object.Get("patch"), synth_storage, "instrument.patch")) return false;
    instrument->synth = &synth_storage->patch;
    instrument->use_gm_programs = BoolProperty(object, "useGmPrograms", false) ? 1 : 0;
    return !env.IsExceptionPending();
  }
  if (kind == "builtin") {
    instrument->kind = SONARE_PROJECT_INSTRUMENT_BUILTIN;
    sonare_builtin_synth_config_init(builtin);
    Napi::Object config;
    if (!RequiredObjectValue(env, object.Get("config"), "instrument.config", &config) ||
        !ReadBuiltinWaveform(env, config.Get("waveform"), &builtin->waveform)) {
      return false;
    }
    builtin->gain = FloatProperty(config, "gain", builtin->gain);
    builtin->attack_ms = FloatProperty(config, "attackMs", builtin->attack_ms);
    builtin->decay_ms = FloatProperty(config, "decayMs", builtin->decay_ms);
    builtin->sustain = FloatProperty(config, "sustain", builtin->sustain);
    builtin->release_ms = FloatProperty(config, "releaseMs", builtin->release_ms);
    builtin->polyphony = IntProperty(config, "polyphony", builtin->polyphony);
    if (env.IsExceptionPending()) return false;
    instrument->builtin = builtin;
    return true;
  }
  if (kind == "sf2") {
    instrument->kind = SONARE_PROJECT_INSTRUMENT_SF2;
    sonare_sf2_instrument_config_init(sf2);
    Napi::Object config;
    if (!RequiredObjectValue(env, object.Get("config"), "instrument.config", &config)) {
      return false;
    }
    sf2->gain = FloatProperty(config, "gain", sf2->gain);
    sf2->polyphony = IntProperty(config, "polyphony", sf2->polyphony);
    sf2->prefer_model_for_modeled_families =
        BoolProperty(config, "preferModelForModeledFamilies",
                     sf2->prefer_model_for_modeled_families != 0)
            ? 1
            : 0;
    sf2->clear_bank_rig = BoolProperty(config, "clearBankRig", sf2->clear_bank_rig != 0) ? 1 : 0;
    sf2->gs_efx_realization = sonare_node::GsEfxRealizationProperty(config, "gsEfxRealization");
    if (env.IsExceptionPending()) return false;
    instrument->sf2 = sf2;
    const Napi::Value uri = object.Get("soundFontUri");
    if (!uri.IsUndefined() && !uri.IsNull()) {
      if (!RequiredStringValue(env, uri, "instrument.soundFontUri", sound_font_uri)) return false;
      instrument->sound_font_uri = sound_font_uri->c_str();
    }
    return true;
  }
  Napi::RangeError::New(env, "instrument.kind must be 'synth', 'builtin', or 'sf2'")
      .ThrowAsJavaScriptException();
  return false;
}

Napi::Object SparseSynthPatchToObject(Napi::Env env, const SonareSynthPatch& patch) {
  Napi::Object out = Napi::Object::New(env);
  const auto preset_end = std::find(std::begin(patch.preset), std::end(patch.preset), '\0');
  if (preset_end == std::end(patch.preset)) {
    throw sonare_node::MakeSonareError(env, SONARE_ERROR_INVALID_FORMAT,
                                       "stored synth preset name is not terminated");
  }
  if (preset_end != std::begin(patch.preset)) {
    out.Set("preset", Napi::String::New(env, std::string(std::begin(patch.preset), preset_end)));
  }
  if (patch.engine_mode != SONARE_SYNTH_ENGINE_DEFAULT) {
    out.Set("engineMode",
            sonare_node::SynthEnumName(env, patch.engine_mode, sonare_node::kSynthEngineModes,
                                       SONARE_SYNTH_ENGINE_MODE_COUNT));
  }
  for (const auto& row : sonare_c_detail::kSynthPatchEnumFields) {
    const int ordinal = patch.*row.member;
    if (ordinal == 0) continue;
    out.Set(row.name, sonare_node::SynthEnumName(env, ordinal, row.values.names,
                                                 static_cast<int>(row.values.count)));
  }
  for (const auto& row : sonare_c_detail::kSynthPatchNumericFields) {
    const uint32_t present = row.presence_fields2 ? patch.present_fields2 : patch.present_fields;
    const double value =
        std::visit([&](const auto member) { return double(patch.*member); }, row.member);
    if (value == 0.0 && (present & row.presence_bit) == 0u) continue;
    if (std::strcmp(row.name, "oneShot") == 0) {
      out.Set(row.name, Napi::Boolean::New(env, value != 0.0));
    } else {
      out.Set(row.name, Napi::Number::New(env, value));
    }
  }
  if (patch.engine_param_count != 0) {
    if (patch.engine_params == nullptr) {
      throw sonare_node::MakeSonareError(env, SONARE_ERROR_INVALID_FORMAT,
                                         "stored synth engine parameters are missing");
    }
    Napi::Object params = Napi::Object::New(env);
    for (size_t i = 0; i < patch.engine_param_count; ++i) {
      if (patch.engine_params[i].key == nullptr) {
        throw sonare_node::MakeSonareError(env, SONARE_ERROR_INVALID_FORMAT,
                                           "stored synth engine parameter has no key");
      }
      params.Set(patch.engine_params[i].key, patch.engine_params[i].value);
    }
    out.Set("engineParams", params);
  }
  if (patch.num_mod_routings < 0 || patch.num_mod_routings > SONARE_SYNTH_PATCH_MOD_ROUTINGS) {
    throw sonare_node::MakeSonareError(env, SONARE_ERROR_INVALID_FORMAT,
                                       "destination instrument has an invalid mod routing count");
  }
  if (patch.num_mod_routings > 0 ||
      (patch.present_fields & SONARE_SYNTH_FIELD_MOD_ROUTINGS) != 0u) {
    Napi::Array routings = Napi::Array::New(env, static_cast<uint32_t>(patch.num_mod_routings));
    for (int i = 0; i < patch.num_mod_routings; ++i) {
      const SonareSynthModRouting& route = patch.mod_routings[i];
      Napi::Object routing = Napi::Object::New(env);
      routing.Set("source",
                  sonare_node::SynthEnumName(env, route.source, sonare_node::kSynthModSources,
                                             SONARE_SYNTH_MOD_SOURCE_COUNT));
      routing.Set("destination", sonare_node::SynthEnumName(env, route.destination,
                                                            sonare_node::kSynthModDestinations,
                                                            SONARE_SYNTH_MOD_DESTINATION_COUNT));
      routing.Set("depth", route.depth);
      routings.Set(static_cast<uint32_t>(i), routing);
    }
    out.Set("modRoutings", routings);
  }
  return out;
}

Napi::Value BuiltinWaveformToName(Napi::Env env, int waveform) {
  const char* name = nullptr;
  switch (waveform) {
    case SONARE_SYNTH_WAVEFORM_SINE:
      name = sonare::c_api::synth_names::kBuiltinWaveforms[0];
      break;
    case SONARE_SYNTH_WAVEFORM_SAW:
      name = sonare::c_api::synth_names::kBuiltinWaveforms[1];
      break;
    case SONARE_SYNTH_WAVEFORM_SQUARE:
      name = sonare::c_api::synth_names::kBuiltinWaveforms[3];
      break;
    case SONARE_SYNTH_WAVEFORM_TRIANGLE:
      name = sonare::c_api::synth_names::kBuiltinWaveforms[4];
      break;
    default:
      throw sonare_node::MakeSonareError(
          env, SONARE_ERROR_INVALID_FORMAT,
          "unknown built-in synth waveform ordinal: " + std::to_string(waveform));
  }
  return Napi::String::New(env, name);
}

Napi::Object ProjectInstrumentToObject(Napi::Env env, const SonareProjectInstrument& instrument) {
  Napi::Object out = Napi::Object::New(env);
  if (instrument.kind == SONARE_PROJECT_INSTRUMENT_SYNTH) {
    if (instrument.synth == nullptr) {
      throw sonare_node::MakeSonareError(env, SONARE_ERROR_INVALID_FORMAT,
                                         "stored synth instrument has no patch");
    }
    out.Set("kind", "synth");
    out.Set("patch", SparseSynthPatchToObject(env, *instrument.synth));
    out.Set("useGmPrograms", Napi::Boolean::New(env, instrument.use_gm_programs != 0));
  } else if (instrument.kind == SONARE_PROJECT_INSTRUMENT_BUILTIN) {
    if (instrument.builtin == nullptr) {
      throw sonare_node::MakeSonareError(env, SONARE_ERROR_INVALID_FORMAT,
                                         "stored built-in instrument has no config");
    }
    const SonareBuiltinSynthConfig& config = *instrument.builtin;
    Napi::Object values = Napi::Object::New(env);
    values.Set("waveform", BuiltinWaveformToName(env, config.waveform));
    values.Set("gain", config.gain);
    values.Set("attackMs", config.attack_ms);
    values.Set("decayMs", config.decay_ms);
    values.Set("sustain", config.sustain);
    values.Set("releaseMs", config.release_ms);
    values.Set("polyphony", config.polyphony);
    out.Set("kind", "builtin");
    out.Set("config", values);
  } else if (instrument.kind == SONARE_PROJECT_INSTRUMENT_SF2) {
    if (instrument.sf2 == nullptr) {
      throw sonare_node::MakeSonareError(env, SONARE_ERROR_INVALID_FORMAT,
                                         "stored SoundFont instrument has no config");
    }
    const SonareSf2InstrumentConfig& config = *instrument.sf2;
    if (config.gs_efx_realization != 0 && config.gs_efx_realization != 1) {
      throw sonare_node::MakeSonareError(
          env, SONARE_ERROR_INVALID_FORMAT,
          "unknown GS effects realization ordinal: " + std::to_string(config.gs_efx_realization));
    }
    Napi::Object values = Napi::Object::New(env);
    values.Set("gain", config.gain);
    values.Set("polyphony", config.polyphony);
    values.Set("preferModelForModeledFamilies",
               Napi::Boolean::New(env, config.prefer_model_for_modeled_families != 0));
    values.Set("clearBankRig", Napi::Boolean::New(env, config.clear_bank_rig != 0));
    values.Set("gsEfxRealization", config.gs_efx_realization == 0 ? "modern" : "classic");
    out.Set("kind", "sf2");
    out.Set("config", values);
    if (instrument.sound_font_uri != nullptr) {
      out.Set("soundFontUri", Napi::String::New(env, instrument.sound_font_uri));
    }
  } else {
    throw sonare_node::MakeSonareError(
        env, SONARE_ERROR_INVALID_FORMAT,
        "unknown project instrument kind ordinal: " + std::to_string(instrument.kind));
  }
  return out;
}

Napi::Value AutomationTargetKindNames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return sonare_node::JoinedNamesToArray(env, sonare_automation_target_kind_names(),
                                         "automation target kind names");
  SONARE_NODE_CATCH(env)
}

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
          InstanceMethod<&ProjectWrap::SetDestinationInstrument>("setDestinationInstrument",
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
          InstanceMethod<&ProjectWrap::AutomationLanes>("automationLanes", napi_default_method),
          InstanceMethod<&ProjectWrap::Undo>("undo", napi_default_method),
          InstanceMethod<&ProjectWrap::Redo>("redo", napi_default_method),
          InstanceMethod<&ProjectWrap::ClearHistory>("clearHistory", napi_default_method),
          InstanceMethod<&ProjectWrap::SetMaxUndoDepth>("setMaxUndoDepth", napi_default_method),
          InstanceMethod<&ProjectWrap::SetMaxHistoryBytes>("setMaxHistoryBytes",
                                                           napi_default_method),
          InstanceMethod<&ProjectWrap::SetMidiEvents>("setMidiEvents", napi_default_method),
          InstanceMethod<&ProjectWrap::MidiEvents>("midiEvents", napi_default_method),
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
          InstanceMethod<&ProjectWrap::KeyAnnotations>("keyAnnotations", napi_default_method),
          InstanceMethod<&ProjectWrap::ChordAnnotations>("chordAnnotations", napi_default_method),
          InstanceMethod<&ProjectWrap::AnnotateSections>("annotateSections", napi_default_method),
          InstanceMethod<&ProjectWrap::SectionAnnotations>("sectionAnnotations",
                                                           napi_default_method),
          InstanceMethod<&ProjectWrap::SetAssistSidecar>("setAssistSidecar", napi_default_method),
          InstanceMethod<&ProjectWrap::AssistSidecarCount>("assistSidecarCount",
                                                           napi_default_method),
          InstanceMethod<&ProjectWrap::GetAssistSidecar>("getAssistSidecar", napi_default_method),
          InstanceMethod<&ProjectWrap::AssistSidecars>("assistSidecars", napi_default_method),
          InstanceMethod<&ProjectWrap::Compile>("compile", napi_default_method),
          InstanceMethod<&ProjectWrap::LastBounceCompileResult>("lastBounceCompileResult",
                                                                napi_default_method),
          InstanceMethod<&ProjectWrap::Bounce>("bounce", napi_default_method),
          InstanceMethod<&ProjectWrap::CountInFrames>("countInFrames", napi_default_method),
          InstanceMethod<&ProjectWrap::LoadSoundFont>("loadSoundFont", napi_default_method),
          InstanceMethod<&ProjectWrap::ClearSoundFont>("clearSoundFont", napi_default_method),
          InstanceMethod<&ProjectWrap::SoundFontPresetCount>("soundFontPresetCount",
                                                             napi_default_method),
          InstanceMethod<&ProjectWrap::SoundFontManifest>("soundFontManifest", napi_default_method),
          InstanceMethod<&ProjectWrap::GetSampleRate>("getSampleRate", napi_default_method),
          InstanceMethod<&ProjectWrap::SetOverlapPolicy>("setOverlapPolicy", napi_default_method),
          InstanceMethod<&ProjectWrap::GetOverlapPolicy>("getOverlapPolicy", napi_default_method),
          InstanceMethod<&ProjectWrap::SetMixerSceneJson>("setMixerSceneJson", napi_default_method),
          InstanceMethod<&ProjectWrap::MixerSceneJson>("mixerSceneJson", napi_default_method),
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
          InstanceMethod<&ProjectWrap::UnresolvedSoundFontUris>("unresolvedSoundFontUris",
                                                                napi_default_method),
          InstanceMethod<&ProjectWrap::DestinationInstruments>("destinationInstruments",
                                                               napi_default_method),
          InstanceMethod<&ProjectWrap::AddAudioSource>("addAudioSource", napi_default_method),
          InstanceMethod<&ProjectWrap::RelinkSource>("relinkSource", napi_default_method),
          InstanceMethod<&ProjectWrap::SetSourceAudio>("setSourceAudio", napi_default_method),
          InstanceMethod<&ProjectWrap::SetAudioSourceMetadata>("setAudioSourceMetadata",
                                                               napi_default_method),
          InstanceMethod<&ProjectWrap::TempoSegmentCount>("tempoSegmentCount", napi_default_method),
          InstanceMethod<&ProjectWrap::TempoSegmentByIndex>("tempoSegmentByIndex",
                                                            napi_default_method),
          InstanceMethod<&ProjectWrap::TimeSignatureByIndex>("timeSignatureByIndex",
                                                             napi_default_method),
          InstanceMethod<&ProjectWrap::PpqToSec>("ppqToSec", napi_default_method),
          InstanceMethod<&ProjectWrap::SecToPpq>("secToPpq", napi_default_method),
          InstanceMethod<&ProjectWrap::PositionAtPpq>("positionAtPpq", napi_default_method),
          InstanceMethod<&ProjectWrap::BarStartPpq>("barStartPpq", napi_default_method),
          InstanceMethod<&ProjectWrap::EndPpq>("endPpq", napi_default_method),
          InstanceMethod<&ProjectWrap::TimeSignatureCount>("timeSignatureCount",
                                                           napi_default_method),
          InstanceMethod<&ProjectWrap::MarkerCount>("markerCount", napi_default_method),
          InstanceMethod<&ProjectWrap::Destroy>("destroy", napi_default_method),
          StaticMethod<&ProjectWrap::FromJson>("fromJson", napi_default_method),
      });
  exports.Set("Project", func);
  exports.Set("automationTargetKindNames",
              Napi::Function::New(env, AutomationTargetKindNames, "automationTargetKindNames"));
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
    Napi::TypeError::New(env, "document must be a string").ThrowAsJavaScriptException();
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
  std::string json = sonare_node::node_narrow_string(env, info[0], "document");
  SonareProject* handle = nullptr;
  char* diag = nullptr;
  const SonareError err =
      sonare_project_deserialize(sonare_node::ElementData(json), json.size(), &handle, &diag);
  std::string diagnostics = diag != nullptr ? diag : "";
  if (diag != nullptr) sonare_free_string(diag);
  if (err != SONARE_OK) {
    sonare_project_destroy(handle);
    // The last error carries the diagnostics without their codes; the codes stay in diagnostics.
    sonare_node::ThrowSonareError(env, err);
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
  if (!OptionalStringArg(env, info, 0, "document", "", &scene)) return env.Undefined();
  ThrowIfError(env, sonare_project_set_mixer_scene_json(project_, scene.c_str()));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::MixerSceneJson(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  HeapStringGuard document;
  ThrowIfError(env, sonare_project_mixer_scene_json(project_, &document.value));
  if (env.IsExceptionPending()) return env.Undefined();
  if (document.value == nullptr) return env.Null();
  return Napi::String::New(env, document.value);
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
  HeapStringGuard channel_strip_ref;
  HeapStringGuard output_target;
  ThrowIfError(env, sonare_project_track_route_by_index(project_, index, &channel_strip_ref.value,
                                                        &output_target.value));
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
  if (channel_strip_ref.value != nullptr) out.Set("channelStripRef", channel_strip_ref.value);
  if (output_target.value != nullptr) out.Set("outputTarget", output_target.value);
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

Napi::Value ProjectWrap::PpqToSec(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  double ppq = 0.0;
  if (!sonare_node::RequiredDoubleArg(env, info, 0, "ppq", &ppq)) return env.Undefined();
  double sec = 0.0;
  ThrowIfError(env, sonare_project_ppq_to_sec(project_, ppq, &sec));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, sec);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SecToPpq(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  double sec = 0.0;
  if (!sonare_node::RequiredDoubleArg(env, info, 0, "sec", &sec)) return env.Undefined();
  double ppq = 0.0;
  ThrowIfError(env, sonare_project_sec_to_ppq(project_, sec, &ppq));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, ppq);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::PositionAtPpq(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  double ppq = 0.0;
  if (!sonare_node::RequiredDoubleArg(env, info, 0, "ppq", &ppq)) return env.Undefined();
  SonareProjectPosition position{};
  ThrowIfError(env, sonare_project_position_at_ppq(project_, ppq, &position));
  if (env.IsExceptionPending()) return env.Undefined();
  constexpr int64_t kMaxSafeBarIndex = static_cast<int64_t>(sonare_node::kMaxSafeInteger);
  if (position.bar_index < -kMaxSafeBarIndex || position.bar_index > kMaxSafeBarIndex) {
    sonare_node::ThrowSonareErrorMessage(
        env, SONARE_ERROR_INVALID_FORMAT,
        "position.barIndex is not exactly representable as a number");
    return env.Undefined();
  }
  Napi::Object time_signature = Napi::Object::New(env);
  time_signature.Set("startPpq", Napi::Number::New(env, position.time_signature.start_ppq));
  time_signature.Set("numerator", Napi::Number::New(env, position.time_signature.numerator));
  time_signature.Set("denominator", Napi::Number::New(env, position.time_signature.denominator));
  Napi::Object out = Napi::Object::New(env);
  out.Set("bpm", Napi::Number::New(env, position.bpm));
  out.Set("timeSignature", time_signature);
  out.Set("barStartPpq", Napi::Number::New(env, position.bar_start_ppq));
  out.Set("barIndex", Napi::Number::New(env, static_cast<double>(position.bar_index)));
  out.Set("beat", Napi::Number::New(env, static_cast<double>(position.beat)));
  out.Set("beatFraction", Napi::Number::New(env, position.beat_fraction));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::BarStartPpq(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  int64_t bar_index = 0;
  if (!RequiredSafeIntegerValue(env, info[0], "barIndex", &bar_index)) return env.Undefined();
  double ppq = 0.0;
  ThrowIfError(env, sonare_project_bar_start_ppq(project_, bar_index, &ppq));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, ppq);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::EndPpq(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  double end_ppq = 0.0;
  ThrowIfError(env, sonare_project_end_ppq(project_, &end_ppq));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, end_ppq);
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
      Napi::Object obj;
      std::string at;
      SonareProjectTempoSegment seg{};
      // Presence and type here, each domain in the C entry's text under the same index.
      if (!RequiredElementObject(env, input, i, "segments", &obj, &at) ||
          !RequiredDomainDoubleValue(env, obj.Get("startPpq"), at + ".startPpq", &seg.start_ppq) ||
          !RequiredDomainDoubleValue(env, obj.Get("bpm"), at + ".bpm", &seg.bpm)) {
        return env.Undefined();
      }
      seg.end_bpm = DomainDoubleProperty(obj, "endBpm", seg.end_bpm, at);
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
      Napi::Object obj;
      std::string at;
      SonareProjectTimeSignatureSegment seg{};
      if (!RequiredElementObject(env, input, i, "segments", &obj, &at) ||
          !RequiredDomainDoubleValue(env, obj.Get("startPpq"), at + ".startPpq", &seg.start_ppq) ||
          !RequiredDomainValue(env, obj.Get("numerator"), (at + ".numerator").c_str(),
                               sonare_c_detail::kTimeSignatureNumeratorDomain, &seg.numerator) ||
          !RequiredDomainValue(env, obj.Get("denominator"), (at + ".denominator").c_str(),
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

Napi::Value ProjectWrap::UnresolvedSoundFontUris(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  StringListGuard list;
  ThrowIfError(env, sonare_project_unresolved_sound_font_uris(project_, &list.value));
  if (env.IsExceptionPending()) return env.Undefined();
  if (list.value.count != 0 && list.value.items == nullptr) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_FORMAT,
                                         "invalid unresolved SoundFont URI list");
    return env.Undefined();
  }
  Napi::Array uris = Napi::Array::New(env, list.value.count);
  for (size_t i = 0; i < list.value.count; ++i) {
    if (list.value.items[i] == nullptr) {
      sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_FORMAT,
                                           "unresolved SoundFont URI list contains a null item");
      return env.Undefined();
    }
    uris.Set(static_cast<uint32_t>(i), Napi::String::New(env, list.value.items[i]));
  }
  return uris;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::SetDestinationInstrument(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t destination_id = 0;
  if (!sonare_node::RequiredUint32Arg(env, info, 0, "destinationId", &destination_id)) {
    return env.Undefined();
  }
  SonareProjectInstrument instrument{};
  sonare_project_instrument_init(&instrument);
  if (!ReadDestinationInstrumentOptions(env, info[2], &instrument)) return env.Undefined();
  const Napi::Value input = info[1];
  if (input.IsUndefined() || input.IsNull()) {
    if (instrument.bank_msb != SONARE_PROJECT_INSTRUMENT_FROM_CLIP ||
        instrument.bank_lsb != SONARE_PROJECT_INSTRUMENT_FROM_CLIP ||
        instrument.program != SONARE_PROJECT_INSTRUMENT_FROM_CLIP) {
      Napi::RangeError::New(env, "bank and program require an instrument")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    ThrowIfError(env, sonare_project_set_destination_instrument(project_, destination_id, nullptr));
    return env.Undefined();
  }
  Napi::Object object;
  if (!RequiredObjectValue(env, input, "instrument", &object)) return env.Undefined();
  sonare_node::SynthPatchStorage synth_storage;
  SonareBuiltinSynthConfig builtin{};
  SonareSf2InstrumentConfig sf2{};
  std::string sound_font_uri;
  if (!ReadDestinationInstrument(env, object, &instrument, &synth_storage, &builtin, &sf2,
                                 &sound_font_uri)) {
    return env.Undefined();
  }
  ThrowIfError(env,
               sonare_project_set_destination_instrument(project_, destination_id, &instrument));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::DestinationInstruments(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  ProjectInstrumentListGuard list;
  ThrowIfError(env, sonare_project_destination_instruments(project_, &list.value));
  if (env.IsExceptionPending()) return env.Undefined();
  if (list.value.count != 0 && list.value.items == nullptr) {
    sonare_node::ThrowSonareErrorMessage(env, SONARE_ERROR_INVALID_FORMAT,
                                         "invalid project destination instrument list");
    return env.Undefined();
  }
  Napi::Array result = Napi::Array::New(env, list.value.count);
  for (size_t i = 0; i < list.value.count; ++i) {
    const SonareProjectInstrumentBinding& item = list.value.items[i];
    if (item.reserved != 0) {
      throw sonare_node::MakeSonareError(env, SONARE_ERROR_INVALID_FORMAT,
                                         "project instrument binding reserved field is nonzero");
    }
    const SonareProjectInstrument& instrument = item.instrument;
    Napi::Object binding = Napi::Object::New(env);
    binding.Set("destinationId", item.destination_id);
    binding.Set("instrument", ProjectInstrumentToObject(env, instrument));
    const int absent = SONARE_PROJECT_INSTRUMENT_FROM_CLIP;
    if ((instrument.bank_msb == absent) != (instrument.bank_lsb == absent)) {
      throw sonare_node::MakeSonareError(env, SONARE_ERROR_INVALID_FORMAT,
                                         "stored project instrument has a partial bank");
    }
    if (instrument.bank_msb != absent) {
      if (instrument.bank_msb < 0 || instrument.bank_msb > 127 || instrument.bank_lsb < 0 ||
          instrument.bank_lsb > 127) {
        throw sonare_node::MakeSonareError(env, SONARE_ERROR_INVALID_FORMAT,
                                           "destination instrument has an invalid bank override");
      }
      Napi::Object bank = Napi::Object::New(env);
      bank.Set("msb", instrument.bank_msb);
      bank.Set("lsb", instrument.bank_lsb);
      binding.Set("bank", bank);
    }
    if (instrument.program != absent) {
      if (instrument.program < 0 || instrument.program > 127) {
        throw sonare_node::MakeSonareError(
            env, SONARE_ERROR_INVALID_FORMAT,
            "destination instrument has an invalid program override");
      }
      binding.Set("program", instrument.program);
    }
    result.Set(static_cast<uint32_t>(i), binding);
  }
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::AddAudioSource(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  std::string uri;
  if (!sonare_node::RequiredStringValue(env, info[0], "uri", &uri)) return env.Undefined();
  uint32_t source_id = 0;
  ThrowIfError(env, sonare_project_add_audio_source(project_, uri.c_str(), &source_id));
  if (env.IsExceptionPending()) return env.Undefined();
  return Napi::Number::New(env, source_id);
  SONARE_NODE_CATCH(env)
}

Napi::Value ProjectWrap::RelinkSource(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, project_, "Project");
  uint32_t source_id = 0;
  std::string uri;
  if (!sonare_node::RequiredUint32Arg(env, info, 0, "sourceId", &source_id) ||
      !sonare_node::RequiredStringValue(env, info[1], "uri", &uri)) {
    return env.Undefined();
  }
  ThrowIfError(env, sonare_project_relink_source(project_, source_id, uri.c_str()));
  return env.Undefined();
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
