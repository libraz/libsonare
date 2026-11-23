/// @file project_arrange.cpp
/// @brief Embind project facade: track + clip arrangement structure and warp,
/// plus the standalone take-alignment entry point that produces a warp map's
/// anchors.

#ifdef __EMSCRIPTEN__

#include <cmath>

#include "c_api/project_input_domains.h"
#include "feature/cqt.h"
#include "project_wasm.h"

#if defined(SONARE_WITH_ARRANGEMENT)

uint32_t ProjectWasm::addTrack(val desc) {
  SonareProjectTrackDesc d{};
  std::string name;
  if (!desc.isUndefined() && !desc.isNull()) {
    if (hasProperty(desc, "kind")) {
      val kind = desc["kind"];
      if (kind.typeOf().as<std::string>() == "string") {
        const std::string k = kind.as<std::string>();
        if (k == "audio") {
          d.kind = SONARE_TRACK_AUDIO;
        } else if (k == "midi") {
          d.kind = SONARE_TRACK_MIDI;
        } else if (k == "aux") {
          d.kind = SONARE_TRACK_AUX;
        } else {
          throw WasmRangeError("unknown project track kind");
        }
      } else if (kind.typeOf().as<std::string>() == "number") {
        const int ordinal = checkedIntFromVal(kind, "project track kind");
        requireOrdinalInRange(ordinal, SONARE_TRACK_AUDIO, SONARE_TRACK_AUX, "project track kind");
        d.kind = static_cast<uint32_t>(ordinal);
      } else {
        throw WasmTypeError("project track kind must be a string or number");
      }
    }
    if (hasProperty(desc, "name")) {
      name = stringFromVal(desc["name"], "name");
      d.name = wasmCString(name, "name");
    }
  }
  uint32_t out = 0;
  checkCodedCError(sonare_project_add_track(project_.get(), &d, &out));
  return out;
}

uint32_t ProjectWasm::addClip(val desc) {
  if (desc.isUndefined() || desc.isNull()) {
    throw WasmTypeError("expects a descriptor object");
  }
  SonareProjectClipDesc d{};
  std::vector<float> audio;
  std::string source_uri;
  d.track_id = uintProperty(desc, "trackId", 0);
  d.is_midi = boolProperty(desc, "isMidi", d.is_midi != 0) ? 1 : 0;
  d.start_ppq = doubleProperty(desc, "startPpq", 0.0);
  d.length_ppq = doubleProperty(desc, "lengthPpq", 0.0);
  d.source_offset_ppq = doubleProperty(desc, "sourceOffsetPpq", 0.0);
  d.gain = floatProperty(desc, "gain", 1.0f);
  d.audio_sample_rate = hasProperty(desc, "audioSampleRate")
                            ? checkedIntFromVal(desc["audioSampleRate"], "audioSampleRate")
                            : 0;
  if (hasProperty(desc, "audio")) {
    d.audio_channels = requireIntProperty(desc, "audioChannels", "descriptor");
    audio = float32ArrayToVector(desc["audio"], "audio");
    if (d.audio_channels <= 0 || audio.size() % static_cast<size_t>(d.audio_channels) != 0) {
      throw WasmRangeError("audio length must be a multiple of audioChannels");
    }
    d.audio_interleaved = elementData(audio);
    d.audio_frames = static_cast<int64_t>(audio.size()) / d.audio_channels;
  }
  if (hasProperty(desc, "sourceUri")) {
    source_uri = stringFromVal(desc["sourceUri"], "sourceUri");
    d.source_uri = wasmCString(source_uri, "sourceUri");
  }
  uint32_t out = 0;
  checkCodedCError(sonare_project_add_clip(project_.get(), &d, &out));
  return out;
}

val ProjectWasm::addLoopRecordingTakes(val desc) {
  if (desc.isUndefined() || desc.isNull()) {
    throw WasmTypeError("expects a descriptor object");
  }
  SonareProjectLoopRecordingDesc d{};
  std::vector<float> audio;
  d.track_id = uintProperty(desc, "trackId", 0);
  d.start_ppq = doubleProperty(desc, "startPpq", 0.0);
  d.loop_length_ppq = doubleProperty(desc, "loopLengthPpq", 0.0);
  // The capture's format is a fact about the recording, so neither has a default.
  d.audio_channels = requireIntProperty(desc, "audioChannels", "descriptor");
  d.audio_sample_rate = requireIntProperty(desc, "audioSampleRate", "descriptor");
  const std::string partial_tail = stringProperty(desc, "partialTail", "keep");
  if (partial_tail == "drop") {
    d.flags = SONARE_PROJECT_LOOP_RECORDING_DROP_PARTIAL_TAIL;
  } else if (partial_tail != "keep") {
    throw WasmRangeError("partialTail must be 'keep' or 'drop'");
  }
  if (hasProperty(desc, "audio")) {
    audio = float32ArrayToVector(desc["audio"], "audio");
    if (d.audio_channels <= 0 || audio.size() % static_cast<size_t>(d.audio_channels) != 0) {
      throw WasmRangeError("audio length must be a multiple of audioChannels");
    }
    d.audio_interleaved = elementData(audio);
    d.audio_frames = static_cast<int64_t>(audio.size()) / d.audio_channels;
  }
  uint32_t clip_id = 0;
  size_t take_count = 0;
  checkCodedCError(
      sonare_project_add_loop_recording_takes(project_.get(), &d, &clip_id, &take_count));
  val out = val::object();
  out.set("clipId", clip_id);
  out.set("takeCount", static_cast<double>(take_count));
  return out;
}

val ProjectWasm::addMidiClip(const val& start_ppq_val, const val& length_ppq_val) {
  const double start_ppq = checkedDoubleFromVal(start_ppq_val, "startPpq");
  const double length_ppq = checkedDoubleFromVal(length_ppq_val, "lengthPpq");
  uint32_t track = 0;
  uint32_t clip = 0;
  checkCodedCError(
      sonare_project_add_midi_clip(project_.get(), start_ppq, length_ppq, &track, &clip));
  val out = val::object();
  out.set("trackId", track);
  out.set("clipId", clip);
  return out;
}

uint32_t ProjectWasm::splitClip(const val& clip_id_val, const val& split_ppq_val) {
  const double split_ppq = checkedDoubleFromVal(split_ppq_val, "splitPpq");
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  uint32_t out = 0;
  checkCodedCError(sonare_project_split_clip(project_.get(), clip_id, split_ppq, &out));
  return out;
}

void ProjectWasm::trimClip(const val& clip_id_val, const val& start_ppq_val,
                           const val& length_ppq_val) {
  const double start_ppq = checkedDoubleFromVal(start_ppq_val, "newStartPpq");
  const double length_ppq = checkedDoubleFromVal(length_ppq_val, "newLengthPpq");
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  checkCodedCError(sonare_project_trim_clip(project_.get(), clip_id, start_ppq, length_ppq));
}

void ProjectWasm::moveClip(const val& clip_id_val, const val& start_ppq_val,
                           const val& track_id_val) {
  const double start_ppq = checkedDoubleFromVal(start_ppq_val, "newStartPpq");
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  // The facade spells the destination track "newTrackId"; the key names the
  // argument a caller wrote, not the C++ parameter.
  const uint32_t track_id = checkedUintFromVal(track_id_val, "newTrackId");
  checkCodedCError(sonare_project_move_clip(project_.get(), clip_id, start_ppq, track_id));
}

void ProjectWasm::setTrackKind(const val& track_id_val, const val& kind_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const uint32_t kind = checkedUintFromVal(kind_val, "kind");
  checkCodedCError(sonare_project_set_track_kind(project_.get(), track_id, kind));
}

void ProjectWasm::setClipWarpRef(const val& clip_id_val, const val& warp_ref_id_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const uint32_t warp_ref_id = checkedUintFromVal(warp_ref_id_val, "warpRefId");
  checkCodedCError(sonare_project_set_clip_warp_ref(project_.get(), clip_id, warp_ref_id));
}

void ProjectWasm::setClipWarpMode(const val& clip_id_val, val mode_val) {
  const uint32_t clip_id = checkedUintFromVal(clip_id_val, "clipId");
  const int mode_int = checkedIntFromVal(mode_val, "warp mode");
  if (mode_int < SONARE_PROJECT_WARP_MODE_OFF || mode_int > SONARE_PROJECT_WARP_MODE_TIME_STRETCH) {
    throw WasmRangeError("unknown warp mode");
  }
  const auto mode = static_cast<SonareProjectWarpMode>(mode_int);
  checkCodedCError(sonare_project_set_clip_warp_mode(project_.get(), clip_id, mode));
}

void ProjectWasm::setWarpMap(val desc) {
  std::vector<SonareProjectWarpAnchor> anchors;
  const size_t count =
      hasProperty(desc, "anchors") ? wasmArrayLength(desc["anchors"], "anchors") : 0;
  anchors.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    val anchor = desc["anchors"][static_cast<unsigned>(i)];
    SonareProjectWarpAnchor out{};
    out.warp_sample = doubleProperty(anchor, "warpSample", 0.0);
    out.source_sample = doubleProperty(anchor, "sourceSample", 0.0);
    anchors.push_back(out);
  }
  std::string name = stringProperty(desc, "name", "");
  SonareProjectWarpMapDesc cdesc{};
  cdesc.id = optionalDomainValue<uint32_t>(desc["id"], "id", sonare_c_detail::kWarpMapIdDomain)
                 .value_or(0u);
  cdesc.name = name.empty() ? nullptr : wasmCString(name, "name");
  cdesc.anchors = anchors.empty() ? nullptr : anchors.data();
  cdesc.anchor_count = anchors.size();
  const SonareError err = sonare_project_set_warp_map(project_.get(), &cdesc);
  if (err != SONARE_OK) throwCodedCErrorRenaming(err, kDerivedCountKeys);
}

void ProjectWasm::removeWarpMap(const val& warp_ref_id_val) {
  const uint32_t warp_ref_id = checkedUintFromVal(warp_ref_id_val, "warpRefId");
  checkCodedCError(sonare_project_remove_warp_map(project_.get(), warp_ref_id));
}

namespace {

val warpAnchorToVal(const SonareProjectWarpAnchor& anchor) {
  val out = val::object();
  out.set("warpSample", anchor.warp_sample);
  out.set("sourceSample", anchor.source_sample);
  return out;
}

}  // namespace

val js_align_take_to_reference(val reference, val take, const val& sample_rate_val,
                               const val& take_sample_rate_val, val config) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int take_sample_rate = checkedIntFromVal(take_sample_rate_val, "takeSampleRate");
  // An omitted key keeps the init value; a written one, 0 included, is read literally and a value
  // outside the domain is refused under its key.
  SonareTakeAlignConfig resolved;
  sonare_take_align_config_init(&resolved);
  if (hasProperty(config, "hopLength")) {
    resolved.hop_length =
        domainNumberFromVal<int>(config["hopLength"], "hopLength", sonare::kCqtHopLengthDomain);
  }
  if (hasProperty(config, "binsPerOctave")) {
    resolved.bins_per_octave =
        domainNumberFromVal<int>(config["binsPerOctave"], "binsPerOctave", sonare::kCqtCountDomain);
  }

  validateWasmFloat32ArrayPair(reference, "reference", take, "take", false);
  const std::vector<float> reference_buffer = float32ArrayToVector(reference, "reference");
  const std::vector<float> take_buffer = float32ArrayToVector(take, "take");

  SonareProjectWarpAnchor* rows = nullptr;
  size_t count = 0;
  SonareTakeAlignment alignment{};
  const SonareError err = sonare_align_take_to_reference(
      reference_buffer.empty() ? nullptr : reference_buffer.data(), reference_buffer.size(),
      sample_rate, take_buffer.empty() ? nullptr : take_buffer.data(), take_buffer.size(),
      take_sample_rate, &resolved, &rows, &count, &alignment);
  if (err != SONARE_OK) throwCodedCError(err);
  // Copied out and released before anything JS-facing is built, so a throw while
  // marshalling cannot leak the heap block.
  std::vector<SonareProjectWarpAnchor> anchors;
  if (rows != nullptr && count != 0) {
    anchors.assign(rows, rows + count);
  }
  sonare_free_project_warp_anchors(rows);

  val list = val::array();
  for (size_t i = 0; i < anchors.size(); ++i) {
    list.set(static_cast<unsigned>(i), warpAnchorToVal(anchors[i]));
  }
  val summary = val::object();
  summary.set("meanResidualFrames", alignment.mean_residual_frames);
  summary.set("referenceFrames", static_cast<double>(alignment.reference_frames));
  summary.set("takeFrames", static_cast<double>(alignment.take_frames));
  val out = val::object();
  out.set("anchors", list);
  out.set("alignment", summary);
  return out;
}

void ProjectWasm::setTrackMidiDestination(const val& track_id_val, const val& destination_id_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  checkCodedCError(
      sonare_project_set_track_midi_destination(project_.get(), track_id, destination_id));
}

void ProjectWasm::setTrackGain(const val& track_id_val, const val& gain_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const float gain = checkedFloatFromVal(gain_val, "gain");
  checkCodedCError(sonare_project_set_track_gain(project_.get(), track_id, gain));
}

void ProjectWasm::setTrackMute(const val& track_id_val, bool mute) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  checkCodedCError(sonare_project_set_track_mute(project_.get(), track_id, mute ? 1 : 0));
}

void ProjectWasm::setTrackSolo(const val& track_id_val, bool solo) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  checkCodedCError(sonare_project_set_track_solo(project_.get(), track_id, solo ? 1 : 0));
}

void ProjectWasm::setTrackPan(const val& track_id_val, const val& pan_val) {
  const uint32_t track_id = checkedUintFromVal(track_id_val, "trackId");
  const float pan = checkedFloatFromVal(pan_val, "pan");
  checkCodedCError(sonare_project_set_track_pan(project_.get(), track_id, pan));
}

namespace {

// A part is 0-15 or SONARE_PART_RIG_ALL_PARTS; anything else is refused before
// the byte it would narrow into, so 256 cannot arrive as part 0.
uint8_t partRigPart(int part) {
  if (part != static_cast<int>(SONARE_PART_RIG_ALL_PARTS)) {
    requireOrdinalInRange(part, 0, 15, "part");
  }
  return static_cast<uint8_t>(part);
}

}  // namespace

void ProjectWasm::setPartRig(const val& destination_id_val, const val& part_val, val mode_val,
                             val inserts_json_val) {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  const uint8_t part = partRigPart(checkedIntFromVal(part_val, "part"));
  const int mode =
      sonare_wasm_synth::enumFromVal(mode_val, sonare_wasm_synth::kPartRigModes,
                                     sonare_wasm_synth::kPartRigModeCount, "part rig mode");
  std::string inserts_json;
  const bool has_inserts = !inserts_json_val.isUndefined() && !inserts_json_val.isNull();
  if (has_inserts) inserts_json = stringFromVal(inserts_json_val, "insertsJson");
  checkCodedCError(sonare_project_set_part_rig(
      project_.get(), destination_id, part, mode,
      has_inserts ? wasmCString(inserts_json, "insertsJson") : nullptr));
}

// Returns null when no entry is stored for (destinationId, part); otherwise
// { mode, insertsJson } with insertsJson null for every mode but chain.
val ProjectWasm::getPartRig(const val& destination_id_val, const val& part_val) const {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  const uint8_t part = partRigPart(checkedIntFromVal(part_val, "part"));
  int mode = 0;
  int present = 0;
  char* inserts = nullptr;
  checkCodedCError(
      sonare_project_get_part_rig(project_.get(), destination_id, part, &mode, &inserts, &present));
  std::string inserts_json;
  const bool has_inserts = inserts != nullptr;
  if (has_inserts) {
    inserts_json = inserts;
    sonare_free_string(inserts);
  }
  if (!present) return val::null();
  val out = val::object();
  out.set("mode", sonare_wasm_synth::enumNameVal(mode, sonare_wasm_synth::kPartRigModes,
                                                 sonare_wasm_synth::kPartRigModeCount));
  out.set("insertsJson", has_inserts ? val(inserts_json) : val::null());
  return out;
}

void ProjectWasm::clearPartRig(const val& destination_id_val, const val& part_val) {
  const uint32_t destination_id = checkedUintFromVal(destination_id_val, "destinationId");
  const uint8_t part = partRigPart(checkedIntFromVal(part_val, "part"));
  checkCodedCError(sonare_project_clear_part_rig(project_.get(), destination_id, part));
}

void ProjectWasm::undo() {
  const SonareError err = sonare_project_undo(project_.get());
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "nothing to undo");
  }
}

void ProjectWasm::redo() {
  const SonareError err = sonare_project_redo(project_.get());
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, "nothing to redo");
  }
}

void ProjectWasm::clearHistory() { checkCodedCError(sonare_project_clear_history(project_.get())); }

void ProjectWasm::setMaxUndoDepth(const val& depth_val) {
  const std::size_t depth = static_cast<std::size_t>(checkedUintFromVal(depth_val, "depth"));
  checkCodedCError(sonare_project_set_max_undo_depth(project_.get(), depth));
}

void ProjectWasm::setMaxHistoryBytes(const val& bytes_val) {
  const std::size_t bytes = static_cast<std::size_t>(checkedUintFromVal(bytes_val, "bytes"));
  checkCodedCError(sonare_project_set_max_history_bytes(project_.get(), bytes));
}

void registerProjectArrange(class_<ProjectWasm>& cls) {
  cls.function("addTrack", &ProjectWasm::addTrack)
      .function("addClip", &ProjectWasm::addClip)
      .function("addLoopRecordingTakes", &ProjectWasm::addLoopRecordingTakes)
      .function("addMidiClip", &ProjectWasm::addMidiClip)
      .function("splitClip", &ProjectWasm::splitClip)
      .function("trimClip", &ProjectWasm::trimClip)
      .function("moveClip", &ProjectWasm::moveClip)
      .function("setTrackKind", &ProjectWasm::setTrackKind)
      .function("setClipWarpRef", &ProjectWasm::setClipWarpRef)
      .function("setClipWarpMode", &ProjectWasm::setClipWarpMode)
      .function("setWarpMap", &ProjectWasm::setWarpMap)
      .function("removeWarpMap", &ProjectWasm::removeWarpMap)
      .function("setTrackMidiDestination", &ProjectWasm::setTrackMidiDestination)
      .function("setTrackGain", &ProjectWasm::setTrackGain)
      .function("setTrackMute", &ProjectWasm::setTrackMute)
      .function("setTrackSolo", &ProjectWasm::setTrackSolo)
      .function("setTrackPan", &ProjectWasm::setTrackPan)
      // destinationId, part, mode, insertsJson -- the C ABI's own argument order
      // minus the project handle.
      .function("setPartRig", &ProjectWasm::setPartRig)
      .function("getPartRig", &ProjectWasm::getPartRig)
      .function("clearPartRig", &ProjectWasm::clearPartRig)
      .function("undo", &ProjectWasm::undo)
      .function("redo", &ProjectWasm::redo)
      .function("clearHistory", &ProjectWasm::clearHistory)
      .function("setMaxUndoDepth", &ProjectWasm::setMaxUndoDepth)
      .function("setMaxHistoryBytes", &ProjectWasm::setMaxHistoryBytes);
}

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
