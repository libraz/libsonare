/// @file project_meta.cpp
/// @brief Embind project facade: MIR annotation streams, assist sidecars,
/// project-level config / counts / markers / tempo / time-signature metadata,
/// last-bounce diagnostics, and the standalone free-function registration.

#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <array>

#include "c_api/project_input_domains.h"
#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

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

struct ProjectSectionListGuard {
  SonareProjectSectionList value{};

  ~ProjectSectionListGuard() { sonare_free_project_section_list(&value); }
};

struct ProjectKeyListGuard {
  SonareProjectKeySegmentList value{};

  ~ProjectKeyListGuard() { sonare_free_project_key_segment_list(&value); }
};

struct ProjectChordListGuard {
  SonareProjectChordSymbolList value{};

  ~ProjectChordListGuard() { sonare_free_project_chord_symbol_list(&value); }
};

struct ProjectStringListGuard {
  SonareStringList value{};

  ~ProjectStringListGuard() {
    if (value.items != nullptr) sonare_free_string_list(&value);
  }
};

template <size_t N>
void refuseUnknownAnnotationKeys(const val& row, const char* array_subject, size_t index,
                                 const std::array<const char*, N>& allowed) {
  if (row.isNull() || row.isUndefined()) return;
  const val names = val::global("Object").call<val>("keys", row);
  const size_t count = wasmArrayLength(names, array_subject);
  for (size_t i = 0; i < count; ++i) {
    const std::string key = names[i].as<std::string>();
    const bool known = std::any_of(allowed.begin(), allowed.end(),
                                   [&](const char* candidate) { return key == candidate; });
    if (!known) {
      std::string expected;
      for (const char* candidate : allowed) {
        if (!expected.empty()) expected += ", ";
        expected += candidate;
      }
      const std::string subject =
          std::string(array_subject) + "[" + std::to_string(index) + "]." + key;
      throw sonare::SonareParameterRefusal::unknown(sonare::ErrorCode::InvalidParameter, subject,
                                                    "", expected)
          .as_caller_spelled();
    }
  }
}

}  // namespace

void ProjectWasm::annotateKeys(val keys) {
  std::vector<SonareProjectKeySegment> segments;
  if (!keys.isUndefined() && !keys.isNull()) {
    const size_t count = wasmArrayLength(keys, "keys");
    segments.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      val entry = keys[i];
      static constexpr std::array<const char*, 4> kAllowed = {"endPpq", "mode", "startPpq",
                                                              "tonicPc"};
      refuseUnknownAnnotationKeys(entry, "keys", i, kAllowed);
      SonareProjectKeySegment seg{};
      seg.start_ppq = doubleProperty(entry, "startPpq", 0.0);
      seg.end_ppq = doubleProperty(entry, "endPpq", 0.0);
      seg.tonic_pc =
          static_cast<uint32_t>(intInRangeOrAbsentProperty(entry, "tonicPc", 0, 11, 255));
      seg.mode = uintProperty(entry, "mode", seg.mode);
      segments.push_back(seg);
    }
  }
  const SonareError err = sonare_project_annotate_keys(
      project_.get(), segments.empty() ? nullptr : segments.data(), segments.size());
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
}

val ProjectWasm::keyAnnotations() const {
  ProjectKeyListGuard list;
  const SonareError err = sonare_project_key_annotations(project_.get(), &list.value);
  if (err != SONARE_OK) throwCodedCError(err);
  if (list.value.count != 0 && list.value.items == nullptr) {
    list.value.count = 0;
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "keyAnnotations returned a missing key array");
  }

  val out = val::array();
  for (size_t i = 0; i < list.value.count; ++i) {
    const SonareProjectKeySegment& key = list.value.items[i];
    if (key.tonic_pc != 255 && key.tonic_pc > 11) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                    "keyAnnotations returned an invalid tonic pitch class");
    }
    val item = val::object();
    item.set("startPpq", key.start_ppq);
    item.set("endPpq", key.end_ppq);
    if (key.tonic_pc != 255) item.set("tonicPc", static_cast<double>(key.tonic_pc));
    item.set("mode", static_cast<double>(key.mode));
    out.set(static_cast<unsigned>(i), item);
  }
  return out;
}

void ProjectWasm::annotateChords(val chords) {
  std::vector<SonareProjectChordSymbol> symbols;
  // Keep the per-chord extensions / roman-numeral storage alive until the C
  // call returns (the POD holds borrowed pointers into these buffers).
  std::vector<std::vector<uint8_t>> ext_storage;
  std::vector<std::string> roman_storage;
  if (!chords.isUndefined() && !chords.isNull()) {
    const size_t count = wasmArrayLength(chords, "chords");
    symbols.reserve(count);
    ext_storage.reserve(count);
    roman_storage.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      val entry = chords[i];
      static constexpr std::array<const char*, 8> kAllowed = {
          "endPpq",       "extensions", "modulationBoundary", "quality",
          "romanNumeral", "rootPc",     "slashBassPc",        "startPpq"};
      refuseUnknownAnnotationKeys(entry, "chords", i, kAllowed);
      SonareProjectChordSymbol sym{};
      sym.start_ppq = doubleProperty(entry, "startPpq", 0.0);
      sym.end_ppq = doubleProperty(entry, "endPpq", 0.0);
      sym.root_pc = static_cast<uint32_t>(intInRangeOrAbsentProperty(entry, "rootPc", 0, 11, 255));
      sym.quality = uintProperty(entry, "quality", sym.quality);
      sym.slash_bass_pc =
          static_cast<uint32_t>(intInRangeOrAbsentProperty(entry, "slashBassPc", 0, 11, 255));
      sym.modulation_boundary =
          boolProperty(entry, "modulationBoundary", sym.modulation_boundary != 0) ? 1 : 0;
      std::vector<uint8_t> exts;
      if (hasProperty(entry, "extensions")) {
        val ext_arr = entry["extensions"];
        if (!val::global("Array").call<bool>("isArray", ext_arr)) {
          throw WasmTypeError("chord extensions must be an array");
        }
        const size_t ec = wasmArrayLength(ext_arr, "extensions");
        exts.reserve(ec);
        for (size_t e = 0; e < ec; ++e) {
          exts.push_back(checkedByteFromVal(ext_arr[e], "extensions[i]"));
        }
      }
      ext_storage.push_back(std::move(exts));
      sym.extensions = ext_storage.back().empty() ? nullptr : ext_storage.back().data();
      sym.extension_count = ext_storage.back().size();
      roman_storage.push_back(stringProperty(entry, "romanNumeral", std::string()));
      sym.roman_numeral = roman_storage.back().empty()
                              ? nullptr
                              : wasmCString(roman_storage.back(), "romanNumeral");
      symbols.push_back(sym);
    }
  }
  const SonareError err = sonare_project_annotate_chords(
      project_.get(), symbols.empty() ? nullptr : symbols.data(), symbols.size());
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
}

val ProjectWasm::chordAnnotations() const {
  ProjectChordListGuard list;
  const SonareError err = sonare_project_chord_annotations(project_.get(), &list.value);
  if (err != SONARE_OK) throwCodedCError(err);
  if (list.value.count != 0 && list.value.items == nullptr) {
    list.value.count = 0;
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "chordAnnotations returned a missing chord array");
  }

  val out = val::array();
  for (size_t i = 0; i < list.value.count; ++i) {
    const SonareProjectChordSymbol& chord = list.value.items[i];
    if ((chord.root_pc != 255 && chord.root_pc > 11) ||
        (chord.slash_bass_pc != 255 && chord.slash_bass_pc > 11)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                    "chordAnnotations returned an invalid pitch class");
    }
    if (chord.extension_count > 32 || (chord.extension_count != 0 && chord.extensions == nullptr)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                    "chordAnnotations returned invalid extension bytes");
    }
    if (chord.modulation_boundary != 0 && chord.modulation_boundary != 1) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                    "chordAnnotations returned an invalid modulation flag");
    }

    val item = val::object();
    item.set("startPpq", chord.start_ppq);
    item.set("endPpq", chord.end_ppq);
    if (chord.root_pc != 255) item.set("rootPc", static_cast<double>(chord.root_pc));
    item.set("quality", static_cast<double>(chord.quality));
    val extensions = val::array();
    for (size_t extension = 0; extension < chord.extension_count; ++extension) {
      extensions.set(static_cast<unsigned>(extension),
                     static_cast<double>(chord.extensions[extension]));
    }
    item.set("extensions", extensions);
    if (chord.slash_bass_pc != 255) {
      item.set("slashBassPc", static_cast<double>(chord.slash_bass_pc));
    }
    if (chord.roman_numeral != nullptr && chord.roman_numeral[0] != '\0') {
      item.set("romanNumeral", std::string(chord.roman_numeral));
    }
    item.set("modulationBoundary", chord.modulation_boundary != 0);
    out.set(static_cast<unsigned>(i), item);
  }
  return out;
}

void ProjectWasm::annotateSections(val sections) {
  std::vector<SonareProjectSection> values;
  std::vector<std::string> labels;
  if (!sections.isUndefined() && !sections.isNull()) {
    const size_t count = wasmArrayLength(sections, "sections");
    values.reserve(count);
    labels.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      val entry = sections[i];
      SonareProjectSection section{};
      section.start_ppq = doubleProperty(entry, "startPpq", 0.0);
      section.end_ppq = doubleProperty(entry, "endPpq", 0.0);
      labels.push_back(stringProperty(entry, "label", std::string()));
      section.label = wasmCString(labels.back(), "label");
      values.push_back(section);
    }
  }
  const SonareError err = sonare_project_annotate_sections(
      project_.get(), values.empty() ? nullptr : values.data(), values.size());
  if (err != SONARE_OK) throwCodedCError(err);
}

val ProjectWasm::sectionAnnotations() const {
  ProjectSectionListGuard list;
  const SonareError err = sonare_project_section_annotations(project_.get(), &list.value);
  if (err != SONARE_OK) throwCodedCError(err);
  if (list.value.count != 0 && list.value.items == nullptr) {
    list.value.count = 0;
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "sectionAnnotations returned a missing section array");
  }

  val out = val::array();
  for (size_t i = 0; i < list.value.count; ++i) {
    const SonareProjectSection& section = list.value.items[i];
    if (section.label == nullptr) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                    "sectionAnnotations returned a missing section label");
    }
    val item = val::object();
    item.set("startPpq", section.start_ppq);
    item.set("endPpq", section.end_ppq);
    item.set("label", std::string(section.label));
    out.set(static_cast<unsigned>(i), item);
  }
  return out;
}

void ProjectWasm::setAssistSidecar(const std::string& module_id, const val& schema_version_val,
                                   const val& target_track_id_val, const val& region_start_ppq_val,
                                   const val& region_end_ppq_val, val payload) {
  const double region_start_ppq = checkedDoubleFromVal(region_start_ppq_val, "regionStartPpq");
  const double region_end_ppq = checkedDoubleFromVal(region_end_ppq_val, "regionEndPpq");
  const uint32_t schema_version = checkedUintFromVal(schema_version_val, "schemaVersion");
  const uint32_t target_track_id = checkedUintFromVal(target_track_id_val, "targetTrackId");
  std::vector<uint8_t> bytes = uint8ArrayToVector(payload, "payload");
  const SonareError err = sonare_project_set_assist_sidecar(
      project_.get(), wasmCString(module_id, "moduleId"), schema_version, target_track_id,
      region_start_ppq, region_end_ppq, bytes.empty() ? nullptr : bytes.data(), bytes.size());
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
}

double ProjectWasm::assistSidecarCount() const {
  return static_cast<double>(sonare_project_assist_sidecar_count(project_.get()));
}

val ProjectWasm::getAssistSidecar(double index) const {
  SonareProjectAssistSidecar sidecar{};
  const SonareError err =
      sonare_project_get_assist_sidecar(project_.get(), wasmIndexArg(index, "index"), &sidecar);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  val out = val::object();
  out.set("moduleId", std::string(sidecar.module_id != nullptr ? sidecar.module_id : ""));
  out.set("schemaVersion", static_cast<double>(sidecar.schema_version));
  out.set("targetTrackId", static_cast<double>(sidecar.target_track_id));
  out.set("regionStartPpq", sidecar.region_start_ppq);
  out.set("regionEndPpq", sidecar.region_end_ppq);
  std::vector<uint8_t> payload(sidecar.payload, sidecar.payload + sidecar.payload_len);
  out.set("payload", vectorToUint8Array(payload));
  sonare_free_project_assist_sidecar(&sidecar);
  return out;
}

void ProjectWasm::setOverlapPolicy(const val& policy_val) {
  const uint32_t policy = checkedUintFromVal(policy_val, "policy");
  const SonareError err = sonare_project_set_overlap_policy(project_.get(), policy);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
}

uint32_t ProjectWasm::getOverlapPolicy() const {
  uint32_t out = 0;
  const SonareError err = sonare_project_get_overlap_policy(project_.get(), &out);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  return out;
}

double ProjectWasm::getSampleRate() const {
  double out = 0.0;
  const SonareError err = sonare_project_get_sample_rate(project_.get(), &out);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  return out;
}

double ProjectWasm::countInFrames(const val& count_in_bars_val) const {
  const int32_t count_in_bars = checkedIntFromVal(count_in_bars_val, "countInBars");
  int64_t frames = 0;
  const SonareError err = sonare_project_count_in_frames(
      project_.get(), count_in_bars, SONARE_PROJECT_SAMPLE_RATE_PROJECT, &frames);
  if (err != SONARE_OK) throwCodedCError(err);
  constexpr int64_t kMaxSafeJsInteger = 9007199254740991LL;
  if (frames < 0 || frames > kMaxSafeJsInteger) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "countInFrames.frames is not exactly representable as a number");
  }
  return static_cast<double>(frames);
}

uint32_t ProjectWasm::addAudioSource(const std::string& uri) {
  uint32_t source_id = 0;
  const SonareError err =
      sonare_project_add_audio_source(project_.get(), wasmCString(uri, "uri"), &source_id);
  if (err != SONARE_OK) throwCodedCError(err);
  return source_id;
}

uint32_t ProjectWasm::relinkSource(const val& source_id_val, const std::string& uri) {
  const uint32_t source_id = checkedUintFromVal(source_id_val, "sourceId");
  const SonareError err =
      sonare_project_relink_source(project_.get(), source_id, wasmCString(uri, "uri"));
  if (err != SONARE_OK) throwCodedCError(err);
  return source_id;
}

double ProjectWasm::ppqToSec(const val& ppq_val) const {
  const double ppq = checkedDoubleFromVal(ppq_val, "ppq");
  double out = 0.0;
  const SonareError err = sonare_project_ppq_to_sec(project_.get(), ppq, &out);
  if (err != SONARE_OK) throwCodedCError(err);
  return out;
}

double ProjectWasm::secToPpq(const val& sec_val) const {
  const double sec = checkedDoubleFromVal(sec_val, "sec");
  double out = 0.0;
  const SonareError err = sonare_project_sec_to_ppq(project_.get(), sec, &out);
  if (err != SONARE_OK) throwCodedCError(err);
  return out;
}

val ProjectWasm::positionAtPpq(const val& ppq_val) const {
  const double ppq = checkedDoubleFromVal(ppq_val, "ppq");
  SonareProjectPosition position{};
  const SonareError err = sonare_project_position_at_ppq(project_.get(), ppq, &position);
  if (err != SONARE_OK) throwCodedCError(err);

  constexpr int64_t kMaxSafeJsInteger = 9007199254740991LL;
  if (position.bar_index < -kMaxSafeJsInteger || position.bar_index > kMaxSafeJsInteger) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "position.barIndex is not exactly representable as a number");
  }

  val time_signature = val::object();
  time_signature.set("startPpq", position.time_signature.start_ppq);
  time_signature.set("numerator", position.time_signature.numerator);
  time_signature.set("denominator", position.time_signature.denominator);

  val out = val::object();
  out.set("bpm", position.bpm);
  out.set("timeSignature", time_signature);
  out.set("barStartPpq", position.bar_start_ppq);
  out.set("barIndex", static_cast<double>(position.bar_index));
  out.set("beat", static_cast<double>(position.beat));
  out.set("beatFraction", position.beat_fraction);
  return out;
}

double ProjectWasm::barStartPpq(const val& bar_index_val) const {
  const double requested_bar_index = numberFromVal(bar_index_val, "barIndex");
  if (std::isfinite(requested_bar_index) &&
      std::trunc(requested_bar_index) != requested_bar_index) {
    throw WasmTypeError("barIndex must be an integer");
  }
  const int64_t bar_index = checkedInt64FromVal(bar_index_val, "barIndex");
  double out = 0.0;
  const SonareError err = sonare_project_bar_start_ppq(project_.get(), bar_index, &out);
  if (err != SONARE_OK) throwCodedCError(err);
  return out;
}

double ProjectWasm::endPpq() const {
  double out = 0.0;
  const SonareError err = sonare_project_end_ppq(project_.get(), &out);
  if (err != SONARE_OK) throwCodedCError(err);
  return out;
}

void ProjectWasm::setMixerSceneJson(const std::string& scene_json) {
  const SonareError err =
      sonare_project_set_mixer_scene_json(project_.get(), wasmCString(scene_json, "document"));
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
}

val ProjectWasm::mixerScene() const {
  HeapStringGuard document;
  const SonareError err = sonare_project_mixer_scene_json(project_.get(), &document.value);
  if (err != SONARE_OK) throwCodedCError(err, CErrorReasonMode::kCurrentEntry);
  if (document.value == nullptr) return val::null();
  return val(std::string(document.value));
}

val ProjectWasm::unresolvedSoundFontUris() const {
  ProjectStringListGuard list;
  const SonareError err = sonare_project_unresolved_sound_font_uris(project_.get(), &list.value);
  if (err != SONARE_OK) throwCodedCError(err, CErrorReasonMode::kCurrentEntry);
  if (list.value.count != 0 && list.value.items == nullptr) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                  "unresolvedSoundFontUris returned a missing URI array");
  }
  val out = val::array();
  for (size_t i = 0; i < list.value.count; ++i) {
    if (list.value.items[i] == nullptr) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                    "unresolvedSoundFontUris returned a null URI");
    }
    out.set(static_cast<unsigned>(i), std::string(list.value.items[i]));
  }
  return out;
}

uint32_t ProjectWasm::setMarker(val marker) {
  SonareProjectMarker desc{};
  desc.id = uintProperty(marker, "id", desc.id);
  desc.kind = static_cast<uint8_t>(intProperty(marker, "kind", desc.kind));
  desc.key_fifths = optionalDomainValue<int8_t>(marker["keyFifths"], "keyFifths",
                                                sonare_c_detail::kMarkerKeyFifthsDomain)
                        .value_or(desc.key_fifths);
  desc.key_minor =
      static_cast<uint8_t>(boolProperty(marker, "keyMinor", desc.key_minor != 0) ? 1 : 0);
  desc.ppq = numberFromVal(objectProperty(marker, "ppq"), "ppq");
  const std::string name = stringProperty(marker, "name", "");
  uint32_t out_id = 0;
  const SonareError err =
      sonare_project_set_marker(project_.get(), &desc, wasmCString(name, "name"), &out_id);
  if (err != SONARE_OK) {
    throwCodedCErrorFlattening(err, "marker");
  }
  return out_id;
}

val ProjectWasm::markerByIndex(const val& index_val) const {
  const int index = checkedIntFromVal(index_val, "index");
  SonareProjectMarker desc{};
  const SonareError err =
      sonare_project_marker_by_index(project_.get(), static_cast<size_t>(index), &desc);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  char* full_name = nullptr;
  const SonareError name_err =
      sonare_project_marker_name_by_index(project_.get(), static_cast<size_t>(index), &full_name);
  if (name_err != SONARE_OK) throwCodedCError(name_err);
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

val ProjectWasm::trackByIndex(const val& index_val) const {
  const int index = checkedIntFromVal(index_val, "index");
  SonareProjectTrack d{};
  const SonareError err =
      sonare_project_track_by_index(project_.get(), static_cast<size_t>(index), &d);
  if (err != SONARE_OK) throwCodedCError(err, CErrorReasonMode::kCurrentEntry);
  HeapStringGuard name;
  const SonareError name_err =
      sonare_project_track_name_by_index(project_.get(), static_cast<size_t>(index), &name.value);
  if (name_err != SONARE_OK) throwCodedCError(name_err, CErrorReasonMode::kCurrentEntry);
  HeapStringGuard channel_strip_ref;
  HeapStringGuard output_target;
  const SonareError route_err = sonare_project_track_route_by_index(
      project_.get(), static_cast<size_t>(index), &channel_strip_ref.value, &output_target.value);
  if (route_err != SONARE_OK) throwCodedCError(route_err, CErrorReasonMode::kCurrentEntry);
  val out = val::object();
  out.set("id", d.id);
  out.set("kind", d.kind);
  out.set("midiDestinationId", d.midi_destination_id);
  out.set("gain", d.gain);
  out.set("pan", d.pan);
  out.set("mute", d.mute != 0);
  out.set("solo", d.solo != 0);
  out.set("name", std::string(name.value));
  if (channel_strip_ref.value != nullptr) {
    out.set("channelStripRef", std::string(channel_strip_ref.value));
  }
  if (output_target.value != nullptr) out.set("outputTarget", std::string(output_target.value));
  return out;
}

val ProjectWasm::clipByIndex(const val& index_val) const {
  const int index = checkedIntFromVal(index_val, "index");
  SonareProjectClip d{};
  const SonareError err =
      sonare_project_clip_by_index(project_.get(), static_cast<size_t>(index), &d);
  if (err != SONARE_OK) throwCodedCError(err);
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

val ProjectWasm::sourceByIndex(const val& index_val) const {
  const int index = checkedIntFromVal(index_val, "index");
  SonareProjectSource d{};
  const SonareError err =
      sonare_project_source_by_index(project_.get(), static_cast<size_t>(index), &d);
  if (err != SONARE_OK) throwCodedCError(err);
  HeapStringGuard name_or_uri;
  const SonareError name_err = sonare_project_source_name_or_uri_by_index(
      project_.get(), static_cast<size_t>(index), &name_or_uri.value);
  if (name_err != SONARE_OK) throwCodedCError(name_err);
  return sourceToVal(d, name_or_uri.value);
}

val ProjectWasm::sourceToVal(const SonareProjectSource& d, const char* name_or_uri) const {
  AudioSourceMetadataGuard metadata;
  if (d.kind == 0) {
    const SonareError metadata_err =
        sonare_project_get_audio_source_metadata(project_.get(), d.id, &metadata.value);
    if (metadata_err != SONARE_OK) {
      throwCodedCError(metadata_err);
    }
  }
  val out = val::object();
  out.set("id", d.id);
  out.set("kind", d.kind);
  out.set("numChannels", d.num_channels);
  out.set("storageHandleId", d.storage_handle_id);
  out.set("sampleRateHint", d.sample_rate_hint);
  out.set("nameOrUri", std::string(name_or_uri));
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
    throwCodedCError(err);
  }
  return static_cast<double>(out);
}

double ProjectWasm::trackCount() const {
  size_t out = 0;
  const SonareError err = sonare_project_track_count(project_.get(), &out);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  return static_cast<double>(out);
}

double ProjectWasm::clipCount() const {
  size_t out = 0;
  const SonareError err = sonare_project_clip_count(project_.get(), &out);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  return static_cast<double>(out);
}

double ProjectWasm::sourceCount() const {
  size_t out = 0;
  const SonareError err = sonare_project_source_count(project_.get(), &out);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  return static_cast<double>(out);
}

double ProjectWasm::tempoSegmentCount() const {
  size_t out = 0;
  const SonareError err = sonare_project_tempo_segment_count(project_.get(), &out);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  return static_cast<double>(out);
}

double ProjectWasm::timeSignatureCount() const {
  size_t out = 0;
  const SonareError err = sonare_project_time_signature_count(project_.get(), &out);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  return static_cast<double>(out);
}

val ProjectWasm::tempoSegmentByIndex(const val& index_val) const {
  const int index = checkedIntFromVal(index_val, "index");
  SonareProjectTempoSegment seg{};
  const SonareError err =
      sonare_project_tempo_segment_by_index(project_.get(), static_cast<size_t>(index), &seg);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  val out = val::object();
  // The same three fields setTempoSegments reads, so a segment read back can be
  // written straight to another project.
  out.set("startPpq", seg.start_ppq);
  out.set("bpm", seg.bpm);
  out.set("endBpm", seg.end_bpm);
  return out;
}

val ProjectWasm::timeSignatureByIndex(const val& index_val) const {
  const int index = checkedIntFromVal(index_val, "index");
  SonareProjectTimeSignatureSegment seg{};
  const SonareError err =
      sonare_project_time_signature_by_index(project_.get(), static_cast<size_t>(index), &seg);
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
  val out = val::object();
  out.set("startPpq", seg.start_ppq);
  out.set("numerator", seg.numerator);
  out.set("denominator", seg.denominator);
  return out;
}

void ProjectWasm::setTempoSegments(val segments) {
  std::vector<SonareProjectTempoSegment> segs;
  if (!segments.isUndefined() && !segments.isNull()) {
    const unsigned count = static_cast<unsigned>(wasmArrayLength(segments, "segments"));
    segs.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
      // Presence and type here, each domain in the C entry's text under the same index.
      const val entry = requiredElementObject(segments, i, "segments");
      const std::string at = elementAt("segments", i);
      SonareProjectTempoSegment seg{};
      seg.start_ppq = requiredDomainDoubleProperty(entry, "startPpq", at);
      seg.bpm = requiredDomainDoubleProperty(entry, "bpm", at);
      seg.end_bpm = optionalDomainDoubleProperty(entry, "endBpm", seg.end_bpm, at);
      segs.push_back(seg);
    }
  }
  const SonareError err = sonare_project_set_tempo_segments(
      project_.get(), segs.empty() ? nullptr : segs.data(), segs.size());
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
}

void ProjectWasm::setTimeSignatures(val segments) {
  std::vector<SonareProjectTimeSignatureSegment> segs;
  if (!segments.isUndefined() && !segments.isNull()) {
    const unsigned count = static_cast<unsigned>(wasmArrayLength(segments, "segments"));
    segs.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
      const val entry = requiredElementObject(segments, i, "segments");
      const std::string at = elementAt("segments", i);
      SonareProjectTimeSignatureSegment seg{};
      seg.start_ppq = requiredDomainDoubleProperty(entry, "startPpq", at);
      seg.numerator =
          domainNumberFromVal<int>(objectProperty(entry, "numerator"), (at + ".numerator").c_str(),
                                   sonare_c_detail::kTimeSignatureNumeratorDomain);
      seg.denominator = domainNumberFromVal<int>(objectProperty(entry, "denominator"),
                                                 (at + ".denominator").c_str(),
                                                 sonare_c_detail::kTimeSignatureDenominatorDomain);
      segs.push_back(seg);
    }
  }
  const SonareError err = sonare_project_set_time_signatures(
      project_.get(), segs.empty() ? nullptr : segs.data(), segs.size());
  if (err != SONARE_OK) {
    throwCodedCError(err);
  }
}

val ProjectWasm::lastBounceCompileResult() const {
  SonareProjectCompileResult result{};
  const SonareError err = sonare_project_last_bounce_compile_result(project_.get(), &result);
  if (err != SONARE_OK) {
    sonare_free_project_compile_result(&result);
    throwCodedCError(err);
  }
  val out = val::undefined();
  try {
    out = projectCompileResultToVal(result);
  } catch (...) {
    sonare_free_project_compile_result(&result);
    throw;
  }
  sonare_free_project_compile_result(&result);
  return out;
}

void registerProjectMeta(class_<ProjectWasm>& cls) {
  cls.function("annotateKeys", &ProjectWasm::annotateKeys)
      .function("annotateChords", &ProjectWasm::annotateChords)
      .function("annotateSections", &ProjectWasm::annotateSections)
      .function("keyAnnotations", &ProjectWasm::keyAnnotations)
      .function("chordAnnotations", &ProjectWasm::chordAnnotations)
      .function("sectionAnnotations", &ProjectWasm::sectionAnnotations)
      .function("setAssistSidecar", &ProjectWasm::setAssistSidecar)
      .function("assistSidecarCount", &ProjectWasm::assistSidecarCount)
      .function("getAssistSidecar", &ProjectWasm::getAssistSidecar)
      .function("setOverlapPolicy", &ProjectWasm::setOverlapPolicy)
      .function("getOverlapPolicy", &ProjectWasm::getOverlapPolicy)
      .function("getSampleRate", &ProjectWasm::getSampleRate)
      .function("countInFrames", &ProjectWasm::countInFrames)
      .function("addAudioSource", &ProjectWasm::addAudioSource)
      .function("relinkSource", &ProjectWasm::relinkSource)
      .function("ppqToSec", &ProjectWasm::ppqToSec)
      .function("secToPpq", &ProjectWasm::secToPpq)
      .function("positionAtPpq", &ProjectWasm::positionAtPpq)
      .function("barStartPpq", &ProjectWasm::barStartPpq)
      .function("endPpq", &ProjectWasm::endPpq)
      .function("setMixerSceneJson", &ProjectWasm::setMixerSceneJson)
      .function("mixerScene", &ProjectWasm::mixerScene)
      .function("unresolvedSoundFontUris", &ProjectWasm::unresolvedSoundFontUris)
      .function("setMarker", &ProjectWasm::setMarker)
      .function("markerByIndex", &ProjectWasm::markerByIndex)
      .function("trackByIndex", &ProjectWasm::trackByIndex)
      .function("clipByIndex", &ProjectWasm::clipByIndex)
      .function("sourceByIndex", &ProjectWasm::sourceByIndex)
      .function("markerCount", &ProjectWasm::markerCount)
      .function("trackCount", &ProjectWasm::trackCount)
      .function("clipCount", &ProjectWasm::clipCount)
      .function("sourceCount", &ProjectWasm::sourceCount)
      .function("tempoSegmentCount", &ProjectWasm::tempoSegmentCount)
      .function("tempoSegmentByIndex", &ProjectWasm::tempoSegmentByIndex)
      .function("timeSignatureByIndex", &ProjectWasm::timeSignatureByIndex)
      .function("timeSignatureCount", &ProjectWasm::timeSignatureCount)
      .function("setTempoSegments", &ProjectWasm::setTempoSegments)
      .function("setTimeSignatures", &ProjectWasm::setTimeSignatures)
      .function("lastBounceCompileResult", &ProjectWasm::lastBounceCompileResult);
}

namespace {

// Analysis chord -> annotation fields (root, quality, bass as ordinals the
// facade already resolved). Domain errors are the C ABI's.
val js_chord_symbol_from_analysis(const val& root_val, const val& quality_val,
                                  const val& bass_val) {
  const int root = checkedIntFromVal(root_val, "root");
  const int quality = checkedIntFromVal(quality_val, "quality");
  const int bass = checkedIntFromVal(bass_val, "bass");
  uint32_t root_pc = 0;
  uint32_t out_quality = 0;
  uint32_t slash_bass_pc = 0;
  uint8_t extensions[32] = {};
  size_t extension_count = 0;
  checkCodedCError(sonare_chord_symbol_from_analysis(
      static_cast<SonarePitchClass>(root), static_cast<SonareChordQuality>(quality),
      static_cast<SonarePitchClass>(bass), &root_pc, &out_quality, extensions, sizeof(extensions),
      &extension_count, &slash_bass_pc));
  val list = val::array();
  for (size_t i = 0; i < extension_count && i < sizeof(extensions); ++i) {
    list.set(static_cast<unsigned>(i), static_cast<unsigned>(extensions[i]));
  }
  val out = val::object();
  out.set("rootPc", root_pc);
  out.set("quality", out_quality);
  out.set("extensions", list);
  out.set("slashBassPc", slash_bass_pc);
  return out;
}

unsigned js_key_mode_from_analysis(const val& mode_val) {
  const int mode = checkedIntFromVal(mode_val, "mode");
  uint32_t out_mode = 0;
  checkCodedCError(sonare_key_mode_from_analysis(static_cast<SonareMode>(mode), &out_mode));
  return out_mode;
}

// The C ABI's descriptor JSON for an engine's section / the wrapper fields; the
// facade parses it. A refusal (out-of-range mode) carries the C message.
// Copies a library-allocated string out and releases it.
std::string takeCString(char* value) {
  std::string out(value);
  sonare_free_string(value);
  return out;
}

std::string js_synth_engine_param_info(const val& engine_mode) {
  char* json = nullptr;
  const SonareError err =
      sonare_synth_engine_param_info(checkedIntFromVal(engine_mode, "engineMode"), &json);
  if (err != SONARE_OK) throwCodedCError(err);
  return takeCString(json);
}

std::string js_synth_patch_param_info() {
  char* json = nullptr;
  const SonareError err = sonare_synth_patch_param_info(&json);
  if (err != SONARE_OK) throwCodedCError(err);
  return takeCString(json);
}

val js_automation_target_kind_names() {
  const std::vector<std::string> tokens = projectAutomationTargetKindTokens();
  val out = val::array();
  for (size_t i = 0; i < tokens.size(); ++i) out.set(static_cast<unsigned>(i), tokens[i]);
  return out;
}

}  // namespace

void registerProjectFreeFunctions() {
  function("projectAbiVersion", &js_project_abi_version);
  function("automationTargetKindNames", &js_automation_target_kind_names);
  function("midiVelocityToMidi2", &js_midi_velocity_to_midi2);
  function("midiNoteOnVelocityToMidi2", &js_midi_note_on_velocity_to_midi2);
  function("midi2VelocityToMidi1", &js_midi2_velocity_to_midi1);
  function("midi2NoteOnVelocityToMidi1", &js_midi2_note_on_velocity_to_midi1);
  function("midiControlToMidi2", &js_midi_control_to_midi2);
  function("midiControl14ToMidi2", &js_midi_control14_to_midi2);
  function("midi2ControlToMidi1", &js_midi2_control_to_midi1);
  function("midi2ControlToMidi1Control14", &js_midi2_control_to_midi1_control14);
  function("midiPitchBendToMidi2", &js_midi_pitch_bend_to_midi2);
  function("midi2PitchBendToMidi1", &js_midi2_pitch_bend_to_midi1);
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
  function("transcribe", &js_transcribe);
  function("alignTakeToReference", &js_align_take_to_reference);
  function("chordSymbolFromAnalysis", &js_chord_symbol_from_analysis);
  function("keyModeFromAnalysis", &js_key_mode_from_analysis);
  function("synthPresetNames", &js_synth_preset_names);
  function("synthPresetPatch", &js_synth_preset_patch);
  function("synthEngineParamInfo", &js_synth_engine_param_info);
  function("synthPatchParamInfo", &js_synth_patch_param_info);
  function("synthGsDrumKitName", &js_synth_gs_drum_kit_name);
  function("synthGsDrumKitIsVoicedApart", &js_synth_gs_drum_kit_is_voiced_apart);
  function("synthGsVariationIsVoicedApart", &js_synth_gs_variation_is_voiced_apart);
  function("controllerProfileNames", &js_controller_profile_names);
  function("_synthEnumTables", &js_synth_enum_tables);
  function("_synthPatchRoundTrip", &js_synth_patch_round_trip);
}

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
