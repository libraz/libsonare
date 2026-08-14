/// @file project_meta.cpp
/// @brief Embind project facade: MIR annotation streams, assist sidecars,
/// project-level config / counts / markers / tempo / time-signature metadata,
/// last-bounce diagnostics, and the standalone free-function registration.

#ifdef __EMSCRIPTEN__

#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

namespace {

struct AudioSourceMetadataGuard {
  SonareProjectAudioSourceMetadata value{};

  ~AudioSourceMetadataGuard() { sonare_project_free_audio_source_metadata(&value); }
};

}  // namespace

void ProjectWasm::annotateKeys(val keys) {
  std::vector<SonareProjectKeySegment> segments;
  if (!keys.isUndefined() && !keys.isNull()) {
    const size_t count = keys["length"].as<size_t>();
    segments.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      val entry = keys[i];
      SonareProjectKeySegment seg{};
      seg.start_ppq = hasProperty(entry, "startPpq") ? entry["startPpq"].as<double>() : 0.0;
      seg.end_ppq = hasProperty(entry, "endPpq") ? entry["endPpq"].as<double>() : 0.0;
      seg.tonic_pc = hasProperty(entry, "tonicPc") ? entry["tonicPc"].as<uint32_t>() : 255u;
      seg.mode = hasProperty(entry, "mode") ? entry["mode"].as<uint32_t>() : 0u;
      segments.push_back(seg);
    }
  }
  const SonareError err = sonare_project_annotate_keys(
      project_.get(), segments.empty() ? nullptr : segments.data(), segments.size());
  if (err != SONARE_OK) {
    throwCError(err, "failed to annotate keys");
  }
}

void ProjectWasm::annotateChords(val chords) {
  std::vector<SonareProjectChordSymbol> symbols;
  // Keep the per-chord extensions / roman-numeral storage alive until the C
  // call returns (the POD holds borrowed pointers into these buffers).
  std::vector<std::vector<uint8_t>> ext_storage;
  std::vector<std::string> roman_storage;
  if (!chords.isUndefined() && !chords.isNull()) {
    const size_t count = chords["length"].as<size_t>();
    symbols.reserve(count);
    ext_storage.reserve(count);
    roman_storage.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      val entry = chords[i];
      SonareProjectChordSymbol sym{};
      sym.start_ppq = hasProperty(entry, "startPpq") ? entry["startPpq"].as<double>() : 0.0;
      sym.end_ppq = hasProperty(entry, "endPpq") ? entry["endPpq"].as<double>() : 0.0;
      sym.root_pc = hasProperty(entry, "rootPc") ? entry["rootPc"].as<uint32_t>() : 255u;
      sym.quality = hasProperty(entry, "quality") ? entry["quality"].as<uint32_t>() : 0u;
      sym.slash_bass_pc =
          hasProperty(entry, "slashBassPc") ? entry["slashBassPc"].as<uint32_t>() : 255u;
      sym.modulation_boundary =
          hasProperty(entry, "modulationBoundary") && entry["modulationBoundary"].as<bool>() ? 1
                                                                                             : 0;
      std::vector<uint8_t> exts;
      if (hasProperty(entry, "extensions")) {
        val ext_arr = entry["extensions"];
        if (val::global("Array").call<bool>("isArray", ext_arr)) {
          const size_t ec = ext_arr["length"].as<size_t>();
          exts.reserve(ec);
          for (size_t e = 0; e < ec; ++e) {
            exts.push_back(static_cast<uint8_t>(ext_arr[e].as<int>()));
          }
        }
      }
      ext_storage.push_back(std::move(exts));
      sym.extensions = ext_storage.back().empty() ? nullptr : ext_storage.back().data();
      sym.extension_count = ext_storage.back().size();
      roman_storage.push_back(hasProperty(entry, "romanNumeral")
                                  ? entry["romanNumeral"].as<std::string>()
                                  : std::string());
      sym.roman_numeral = roman_storage.back().empty() ? nullptr : roman_storage.back().c_str();
      symbols.push_back(sym);
    }
  }
  const SonareError err = sonare_project_annotate_chords(
      project_.get(), symbols.empty() ? nullptr : symbols.data(), symbols.size());
  if (err != SONARE_OK) {
    throwCError(err, "failed to annotate chords");
  }
}

void ProjectWasm::setAssistSidecar(const std::string& module_id, uint32_t schema_version,
                                   uint32_t target_track_id, double region_start_ppq,
                                   double region_end_ppq, val payload) {
  std::vector<uint8_t> bytes = uint8ArrayToVector(payload);
  const SonareError err = sonare_project_set_assist_sidecar(
      project_.get(), module_id.c_str(), schema_version, target_track_id, region_start_ppq,
      region_end_ppq, bytes.empty() ? nullptr : bytes.data(), bytes.size());
  if (err != SONARE_OK) {
    throwCError(err, "failed to set assist sidecar");
  }
}

double ProjectWasm::assistSidecarCount() const {
  return static_cast<double>(sonare_project_assist_sidecar_count(project_.get()));
}

val ProjectWasm::getAssistSidecar(double index) const {
  SonareProjectAssistSidecar sidecar{};
  const SonareError err =
      sonare_project_get_assist_sidecar(project_.get(), static_cast<size_t>(index), &sidecar);
  if (err != SONARE_OK) {
    throwCError(err, "failed to read assist sidecar");
  }
  val out = val::object();
  out.set("moduleId", std::string(sidecar.module_id != nullptr ? sidecar.module_id : ""));
  out.set("schemaVersion", static_cast<double>(sidecar.schema_version));
  out.set("targetTrackId", static_cast<double>(sidecar.target_track_id));
  out.set("regionStartPpq", sidecar.region_start_ppq);
  out.set("regionEndPpq", sidecar.region_end_ppq);
  std::vector<uint8_t> payload(sidecar.payload, sidecar.payload + sidecar.payload_len);
  out.set("payload", vectorToUint8Array(payload));
  sonare_project_free_assist_sidecar(&sidecar);
  return out;
}

void ProjectWasm::setOverlapPolicy(uint32_t policy) {
  const SonareError err = sonare_project_set_overlap_policy(project_.get(), policy);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set overlap policy");
  }
}

uint32_t ProjectWasm::getOverlapPolicy() const {
  uint32_t out = 0;
  const SonareError err = sonare_project_get_overlap_policy(project_.get(), &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to read overlap policy");
  }
  return out;
}

double ProjectWasm::getSampleRate() const {
  double out = 0.0;
  const SonareError err = sonare_project_get_sample_rate(project_.get(), &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to read sample rate");
  }
  return out;
}

void ProjectWasm::setMixerSceneJson(const std::string& scene_json) {
  const SonareError err = sonare_project_set_mixer_scene_json(project_.get(), scene_json.c_str());
  if (err != SONARE_OK) {
    throwCError(err, "failed to set mixer scene JSON");
  }
}

uint32_t ProjectWasm::setMarker(uint32_t marker_id, double ppq, const std::string& name) {
  uint32_t out_id = 0;
  const SonareError err =
      sonare_project_set_marker(project_.get(), marker_id, ppq, name.c_str(), &out_id);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set marker");
  }
  return out_id;
}

uint32_t ProjectWasm::setMarkerEx(val marker) {
  SonareProjectMarker desc{};
  desc.id = static_cast<uint32_t>(intProperty(marker, "id", 0));
  desc.kind = static_cast<uint8_t>(intProperty(marker, "kind", 0));
  desc.key_fifths = static_cast<int8_t>(intProperty(marker, "keyFifths", 0));
  desc.key_minor = static_cast<uint8_t>(boolProperty(marker, "keyMinor", false) ? 1 : 0);
  desc.ppq = objectProperty(marker, "ppq").as<double>();
  const std::string name = stringProperty(marker, "name", "");
  uint32_t out_id = 0;
  const SonareError err =
      sonare_project_set_marker_ex_name(project_.get(), &desc, name.c_str(), &out_id);
  if (err != SONARE_OK) {
    throwCError(err, "failed to set marker");
  }
  return out_id;
}

val ProjectWasm::markerByIndex(int index) const {
  SonareProjectMarker desc{};
  const SonareError err =
      sonare_project_marker_by_index(project_.get(), static_cast<size_t>(index), &desc);
  if (err != SONARE_OK) {
    throwCError(err, "marker index out of range");
  }
  char* full_name = nullptr;
  const SonareError name_err =
      sonare_project_marker_name_by_index(project_.get(), static_cast<size_t>(index), &full_name);
  if (name_err != SONARE_OK) throwCError(name_err, "failed to read marker name");
  val out = val::object();
  out.set("id", desc.id);
  out.set("ppq", desc.ppq);
  out.set("name", std::string(full_name));
  sonare_free_string(full_name);
  out.set("kind", static_cast<int>(desc.kind));
  out.set("keyFifths", static_cast<int>(desc.key_fifths));
  out.set("keyMinor", desc.key_minor != 0);
  return out;
}

val ProjectWasm::trackByIndex(int index) const {
  SonareProjectTrack d{};
  const SonareError err =
      sonare_project_track_by_index(project_.get(), static_cast<size_t>(index), &d);
  if (err != SONARE_OK) throwCError(err, "track index out of range");
  val out = val::object();
  out.set("id", d.id);
  out.set("kind", d.kind);
  out.set("midiDestinationId", d.midi_destination_id);
  out.set("gain", d.gain);
  out.set("pan", d.pan);
  out.set("mute", d.mute != 0);
  out.set("solo", d.solo != 0);
  out.set("name", std::string(d.name));
  return out;
}

val ProjectWasm::clipByIndex(int index) const {
  SonareProjectClip d{};
  const SonareError err =
      sonare_project_clip_by_index(project_.get(), static_cast<size_t>(index), &d);
  if (err != SONARE_OK) throwCError(err, "clip index out of range");
  val out = val::object();
  out.set("id", d.id);
  out.set("trackId", d.track_id);
  out.set("sourceId", d.source_id);
  out.set("sourceKind", d.source_kind);
  out.set("startPpq", d.start_ppq);
  out.set("lengthPpq", d.length_ppq);
  out.set("sourceOffsetPpq", d.source_offset_ppq);
  out.set("gain", d.gain);
  out.set("loopMode", d.loop_mode);
  out.set("loopLengthPpq", d.loop_length_ppq);
  return out;
}

val ProjectWasm::sourceByIndex(int index) const {
  SonareProjectSource d{};
  const SonareError err =
      sonare_project_source_by_index(project_.get(), static_cast<size_t>(index), &d);
  if (err != SONARE_OK) throwCError(err, "source index out of range");
  AudioSourceMetadataGuard metadata;
  if (d.kind == 0) {
    const SonareError metadata_err =
        sonare_project_get_audio_source_metadata(project_.get(), d.id, &metadata.value);
    if (metadata_err != SONARE_OK) {
      throwCError(metadata_err, "failed to read audio source metadata");
    }
  }
  val out = val::object();
  out.set("id", d.id);
  out.set("kind", d.kind);
  out.set("channelCount", d.channel_count);
  out.set("storageHandleId", d.storage_handle_id);
  out.set("sampleRateHint", d.sample_rate_hint);
  out.set("nameOrUri", std::string(d.name_or_uri));
  out.set("contentHash",
          std::string(metadata.value.content_hash != nullptr ? metadata.value.content_hash : ""));
  out.set("externalStemRole", std::string(metadata.value.external_stem_role != nullptr
                                              ? metadata.value.external_stem_role
                                              : ""));
  return out;
}

double ProjectWasm::markerCount() const {
  size_t out = 0;
  const SonareError err = sonare_project_marker_count(project_.get(), &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to read marker count");
  }
  return static_cast<double>(out);
}

double ProjectWasm::trackCount() const {
  size_t out = 0;
  const SonareError err = sonare_project_track_count(project_.get(), &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to read track count");
  }
  return static_cast<double>(out);
}

double ProjectWasm::clipCount() const {
  size_t out = 0;
  const SonareError err = sonare_project_clip_count(project_.get(), &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to read clip count");
  }
  return static_cast<double>(out);
}

double ProjectWasm::sourceCount() const {
  size_t out = 0;
  const SonareError err = sonare_project_source_count(project_.get(), &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to read source count");
  }
  return static_cast<double>(out);
}

double ProjectWasm::tempoSegmentCount() const {
  size_t out = 0;
  const SonareError err = sonare_project_tempo_segment_count(project_.get(), &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to read tempo segment count");
  }
  return static_cast<double>(out);
}

double ProjectWasm::timeSignatureCount() const {
  size_t out = 0;
  const SonareError err = sonare_project_time_signature_count(project_.get(), &out);
  if (err != SONARE_OK) {
    throwCError(err, "failed to read time signature count");
  }
  return static_cast<double>(out);
}

void ProjectWasm::setTempoSegments(val segments) {
  std::vector<SonareProjectTempoSegment> segs;
  if (!segments.isUndefined() && !segments.isNull()) {
    const unsigned count = segments["length"].as<unsigned>();
    segs.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
      val entry = segments[i];
      SonareProjectTempoSegment seg{};
      seg.start_ppq = hasProperty(entry, "startPpq") ? entry["startPpq"].as<double>() : 0.0;
      seg.bpm = hasProperty(entry, "bpm") ? entry["bpm"].as<double>() : 0.0;
      seg.end_bpm = hasProperty(entry, "endBpm") ? entry["endBpm"].as<double>() : 0.0;
      segs.push_back(seg);
    }
  }
  const SonareError err = sonare_project_set_tempo_segments(
      project_.get(), segs.empty() ? nullptr : segs.data(), segs.size());
  if (err != SONARE_OK) {
    throwCError(err, "failed to set tempo segments");
  }
}

void ProjectWasm::setTimeSignatures(val segments) {
  std::vector<SonareProjectTimeSignatureSegment> segs;
  if (!segments.isUndefined() && !segments.isNull()) {
    const unsigned count = segments["length"].as<unsigned>();
    segs.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
      val entry = segments[i];
      SonareProjectTimeSignatureSegment seg{};
      seg.start_ppq = hasProperty(entry, "startPpq") ? entry["startPpq"].as<double>() : 0.0;
      seg.numerator = hasProperty(entry, "numerator") ? entry["numerator"].as<int>() : 0;
      seg.denominator = hasProperty(entry, "denominator") ? entry["denominator"].as<int>() : 0;
      segs.push_back(seg);
    }
  }
  const SonareError err = sonare_project_set_time_signatures(
      project_.get(), segs.empty() ? nullptr : segs.data(), segs.size());
  if (err != SONARE_OK) {
    throwCError(err, "failed to set time signatures");
  }
}

val ProjectWasm::lastBounceCompileResult() const {
  SonareProjectCompileResult result{};
  const SonareError err = sonare_project_last_bounce_compile_result(project_.get(), &result);
  if (err != SONARE_OK) {
    sonare_project_free_compile_result(&result);
    throwCError(err, "failed to read last bounce compile result");
  }
  val out = projectCompileResultToVal(result);
  sonare_project_free_compile_result(&result);
  return out;
}

void registerProjectMeta(class_<ProjectWasm>& cls) {
  cls.function("annotateKeys", &ProjectWasm::annotateKeys)
      .function("annotateChords", &ProjectWasm::annotateChords)
      .function("setAssistSidecar", &ProjectWasm::setAssistSidecar)
      .function("assistSidecarCount", &ProjectWasm::assistSidecarCount)
      .function("getAssistSidecar", &ProjectWasm::getAssistSidecar)
      .function("setOverlapPolicy", &ProjectWasm::setOverlapPolicy)
      .function("getOverlapPolicy", &ProjectWasm::getOverlapPolicy)
      .function("getSampleRate", &ProjectWasm::getSampleRate)
      .function("setMixerSceneJson", &ProjectWasm::setMixerSceneJson)
      .function("setMarker", &ProjectWasm::setMarker)
      .function("setMarkerEx", &ProjectWasm::setMarkerEx)
      .function("markerByIndex", &ProjectWasm::markerByIndex)
      .function("trackByIndex", &ProjectWasm::trackByIndex)
      .function("clipByIndex", &ProjectWasm::clipByIndex)
      .function("sourceByIndex", &ProjectWasm::sourceByIndex)
      .function("markerCount", &ProjectWasm::markerCount)
      .function("trackCount", &ProjectWasm::trackCount)
      .function("clipCount", &ProjectWasm::clipCount)
      .function("sourceCount", &ProjectWasm::sourceCount)
      .function("tempoSegmentCount", &ProjectWasm::tempoSegmentCount)
      .function("timeSignatureCount", &ProjectWasm::timeSignatureCount)
      .function("setTempoSegments", &ProjectWasm::setTempoSegments)
      .function("setTimeSignatures", &ProjectWasm::setTimeSignatures)
      .function("lastBounceCompileResult", &ProjectWasm::lastBounceCompileResult);
}

void registerProjectFreeFunctions() {
  function("projectAbiVersion", &js_project_abi_version);
  function("midiGmInstrumentName", &js_midi_gm_instrument_name);
  function("midiGmProgramForName", &js_midi_gm_program_for_name);
  function("midiGmFamilyName", &js_midi_gm_family_name);
  function("midiGmFamilyFirstProgram", &js_midi_gm_family_first_program);
  function("midiGm2InstrumentName", &js_midi_gm2_instrument_name);
  function("midiGmDrumName", &js_midi_gm_drum_name);
  function("midiGmDrumNoteForName", &js_midi_gm_drum_note_for_name);
  function("midiGm2DrumSetName", &js_midi_gm2_drum_set_name);
  function("midiGm2DrumName", &js_midi_gm2_drum_name);
  function("midiCcName", &js_midi_cc_name);
  function("midiCcIndexForName", &js_midi_cc_index_for_name);
  function("midiPerNoteControllerName", &js_midi_per_note_controller_name);
  function("midiBankProgram", &js_midi_bank_program);
  function("midiCcLearn", &js_midi_cc_learn);
  function("midiCcToBreakpoint", &js_midi_cc_to_breakpoint);
  function("midiParamToCc", &js_midi_param_to_cc);
  function("midiRouteEvents", &js_midi_route_events);
  function("synthPresetNames", &js_synth_preset_names);
  function("synthPresetPatch", &js_synth_preset_patch);
  function("_synthEnumTables", &js_synth_enum_tables);
  function("_synthPatchRoundTrip", &js_synth_patch_round_trip);
}

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
