/// @file project.cpp
/// @brief Embind bindings for the headless DAW project API.

#ifdef __EMSCRIPTEN__

#include <sstream>
#include <vector>

#include "common.h"
#include "synth_patch_val.h"

#if defined(SONARE_WITH_ARRANGEMENT)
// ============================================================================
// Headless DAW project embind wrapper over the C ABI
// sonare_c_project.{h,cpp}. The wrapper owns an opaque SonareProject* (created
// in the constructor, destroyed in the destructor) and marshals the flat C
// surface into embind-friendly std::string / Float32Array shapes. The C-ABI
// translation unit (sonare_c_project.cpp) is compiled into this target under
// BUILD_ARRANGEMENT; the symbols are reached through the public sonare_c.h.
// ============================================================================

// Result of ProjectWasm::compile() surfaced to JS. Mirrors Node:
// { hasTimeline, messages, diagnostics }, while retaining diagnosticCount for
// existing callers.
val projectCompileResultToVal(const SonareProjectCompileResult& result) {
  val out = val::object();
  out.set("diagnosticCount", static_cast<double>(result.diagnostic_count));
  out.set("hasTimeline", result.has_timeline != 0);
  const std::string messages(result.messages != nullptr ? result.messages : "");
  out.set("messages", messages);
  std::vector<std::string> diagnostic_messages;
  std::stringstream message_stream(messages);
  std::string line;
  while (std::getline(message_stream, line)) {
    diagnostic_messages.push_back(line);
  }
  val diagnostics = val::array();
  for (size_t i = 0; i < result.diagnostic_count; ++i) {
    val diag = val::object();
    diag.set("code", static_cast<double>(result.diagnostics[i].code));
    diag.set("severity", static_cast<double>(result.diagnostics[i].severity));
    diag.set("targetId", static_cast<double>(result.diagnostics[i].target_id));
    diag.set("message", i < diagnostic_messages.size() ? diagnostic_messages[i] : std::string());
    diagnostics.set(static_cast<unsigned>(i), diag);
  }
  out.set("diagnostics", diagnostics);
  return out;
}

struct ProjectWasm {
  ProjectWasm() {
    SonareProject* handle = nullptr;
    const SonareError err = sonare_project_create(&handle);
    if (err != SONARE_OK || handle == nullptr) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to create headless project");
    }
    project_ = std::shared_ptr<SonareProject>(handle, sonare_project_destroy);
  }

  // Adopts an already-created handle (used by the fromJson factory). The handle
  // must be non-null; ownership transfers to the shared_ptr.
  explicit ProjectWasm(SonareProject* adopted) : project_(adopted, sonare_project_destroy) {}

  // The handle is owned through a shared_ptr (deleter = sonare_project_destroy),
  // so the wrapper is freely copyable/movable. embind returns the fromJson
  // factory by value, which requires a copy/move-constructible holder; sharing
  // the refcounted handle keeps that safe (the C project is destroyed once, when
  // the last wrapper goes away).

  // Serializes the project (+ MIDI content) to deterministic JSON.
  std::string toJson() const {
    char* json = nullptr;
    size_t len = 0;
    const SonareError err = sonare_project_serialize(project_.get(), &json, &len);
    if (err != SONARE_OK || json == nullptr) {
      sonare_free_string(json);
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to serialize project");
    }
    std::string out(json, len);
    sonare_free_string(json);
    return out;
  }

  // Deserializes project JSON into a NEW project. Throws on malformed input,
  // surfacing the joined diagnostic messages to JS.
  static ProjectWasm fromJson(const std::string& json) {
    SonareProject* handle = nullptr;
    char* diag = nullptr;
    const SonareError err = sonare_project_deserialize(json.data(), json.size(), &handle, &diag);
    if (err != SONARE_OK || handle == nullptr) {
      std::string message =
          diag != nullptr ? std::string(diag) : std::string("invalid project JSON");
      sonare_free_string(diag);
      sonare_project_destroy(handle);
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat, message);
    }
    sonare_free_string(diag);
    return ProjectWasm(handle);
  }

  static val fromJsonWithDiagnostics(const std::string& json) {
    SonareProject* handle = nullptr;
    char* diag = nullptr;
    const SonareError err = sonare_project_deserialize(json.data(), json.size(), &handle, &diag);
    std::string diagnostics = diag != nullptr ? std::string(diag) : std::string();
    sonare_free_string(diag);
    if (err != SONARE_OK || handle == nullptr) {
      sonare_project_destroy(handle);
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidFormat,
          diagnostics.empty() ? std::string("invalid project JSON") : diagnostics);
    }
    val out = val::object();
    out.set("project", ProjectWasm(handle));
    out.set("diagnostics", diagnostics);
    return out;
  }

  // Sets the project sample rate (Hz). Must be > 0.
  void setSampleRate(double sample_rate) {
    const SonareError err = sonare_project_set_sample_rate(project_.get(), sample_rate);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "invalid project sample rate");
    }
  }

  uint32_t addTrack(val desc) {
    SonareProjectTrackDesc d{};
    std::string name;
    if (!desc.isUndefined() && !desc.isNull()) {
      if (hasProperty(desc, "kind")) {
        val kind = desc["kind"];
        if (kind.typeOf().as<std::string>() == "string") {
          const std::string k = kind.as<std::string>();
          d.kind = k == "midi" ? SONARE_TRACK_MIDI
                               : (k == "aux" ? SONARE_TRACK_AUX : SONARE_TRACK_AUDIO);
        } else {
          d.kind = kind.as<int>();
        }
      }
      if (hasProperty(desc, "name")) {
        name = desc["name"].as<std::string>();
        d.name = name.c_str();
      }
    }
    uint32_t out = 0;
    const SonareError err = sonare_project_add_track(project_.get(), &d, &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to add track");
    }
    return out;
  }

  uint32_t addClip(val desc) {
    if (desc.isUndefined() || desc.isNull()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "addClip expects a descriptor object");
    }
    SonareProjectClipDesc d{};
    std::vector<float> audio;
    std::string source_uri;
    d.track_id = hasProperty(desc, "trackId") ? desc["trackId"].as<uint32_t>() : 0;
    d.is_midi = hasProperty(desc, "isMidi") && desc["isMidi"].as<bool>() ? 1 : 0;
    d.start_ppq = hasProperty(desc, "startPpq") ? desc["startPpq"].as<double>() : 0.0;
    d.length_ppq = hasProperty(desc, "lengthPpq") ? desc["lengthPpq"].as<double>() : 0.0;
    d.source_offset_ppq =
        hasProperty(desc, "sourceOffsetPpq") ? desc["sourceOffsetPpq"].as<double>() : 0.0;
    d.gain = hasProperty(desc, "gain") ? desc["gain"].as<float>() : 1.0f;
    d.audio_channels = hasProperty(desc, "audioChannels") ? desc["audioChannels"].as<int>() : 1;
    d.audio_sample_rate =
        hasProperty(desc, "audioSampleRate") ? desc["audioSampleRate"].as<int>() : 0;
    if (hasProperty(desc, "audio")) {
      audio = float32ArrayToVector(desc["audio"]);
      d.audio_interleaved = audio.data();
      const int channels = d.audio_channels > 0 ? d.audio_channels : 1;
      d.audio_frames = static_cast<int64_t>(audio.size()) / channels;
    }
    if (hasProperty(desc, "sourceUri")) {
      source_uri = desc["sourceUri"].as<std::string>();
      d.source_uri = source_uri.c_str();
    }
    uint32_t out = 0;
    const SonareError err = sonare_project_add_clip(project_.get(), &d, &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to add clip");
    }
    return out;
  }

  val addMidiClip(double start_ppq, double length_ppq) {
    uint32_t track = 0;
    uint32_t clip = 0;
    const SonareError err =
        sonare_project_add_midi_clip(project_.get(), start_ppq, length_ppq, &track, &clip);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to add MIDI clip");
    }
    val out = val::object();
    out.set("trackId", track);
    out.set("clipId", clip);
    return out;
  }

  uint32_t splitClip(uint32_t clip_id, double split_ppq) {
    uint32_t out = 0;
    const SonareError err = sonare_project_split_clip(project_.get(), clip_id, split_ppq, &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to split clip");
    }
    return out;
  }

  void trimClip(uint32_t clip_id, double start_ppq, double length_ppq) {
    const SonareError err =
        sonare_project_trim_clip(project_.get(), clip_id, start_ppq, length_ppq);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to trim clip");
    }
  }

  void moveClip(uint32_t clip_id, double start_ppq, uint32_t track_id) {
    const SonareError err = sonare_project_move_clip(project_.get(), clip_id, start_ppq, track_id);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to move clip");
    }
  }

  void setTrackKind(uint32_t track_id, uint32_t kind) {
    const SonareError err = sonare_project_set_track_kind(project_.get(), track_id, kind);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to set track kind");
    }
  }

  void setClipWarpRef(uint32_t clip_id, uint32_t warp_ref_id) {
    const SonareError err = sonare_project_set_clip_warp_ref(project_.get(), clip_id, warp_ref_id);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to set clip warp reference");
    }
  }

  void setWarpMap(val desc) {
    std::vector<SonareProjectWarpAnchor> anchors;
    const size_t count = hasProperty(desc, "anchors") ? desc["anchors"]["length"].as<size_t>() : 0;
    anchors.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      val anchor = desc["anchors"][static_cast<unsigned>(i)];
      SonareProjectWarpAnchor out{};
      out.warp_sample = hasProperty(anchor, "warpSample") ? anchor["warpSample"].as<double>() : 0.0;
      out.source_sample =
          hasProperty(anchor, "sourceSample") ? anchor["sourceSample"].as<double>() : 0.0;
      anchors.push_back(out);
    }
    std::string name = hasProperty(desc, "name") ? desc["name"].as<std::string>() : "";
    SonareProjectWarpMapDesc cdesc{};
    cdesc.id = hasProperty(desc, "id") ? desc["id"].as<uint32_t>() : 0u;
    cdesc.name = name.empty() ? nullptr : name.c_str();
    cdesc.anchors = anchors.empty() ? nullptr : anchors.data();
    cdesc.anchor_count = anchors.size();
    const SonareError err = sonare_project_set_warp_map(project_.get(), &cdesc);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to set warp map");
    }
  }

  void removeWarpMap(uint32_t warp_ref_id) {
    const SonareError err = sonare_project_remove_warp_map(project_.get(), warp_ref_id);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to remove warp map");
    }
  }

  void setTrackMidiDestination(uint32_t track_id, uint32_t destination_id) {
    const SonareError err =
        sonare_project_set_track_midi_destination(project_.get(), track_id, destination_id);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to set track MIDI destination");
    }
  }

  void undo() {
    const SonareError err = sonare_project_undo(project_.get());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, "nothing to undo");
    }
  }

  void redo() {
    const SonareError err = sonare_project_redo(project_.get());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, "nothing to redo");
    }
  }

  void setMidiEvents(uint32_t clip_id, val events) {
    const size_t count =
        events.isUndefined() || events.isNull() ? 0 : events["length"].as<size_t>();
    std::vector<SonareMidiEventPod> pods(count);
    for (size_t i = 0; i < count; ++i) {
      val entry = events[i];
      if (val::global("Array").call<bool>("isArray", entry)) {
        pods[i].ppq = entry[0].as<double>();
        pods[i].data0 = entry[1].as<uint32_t>();
        pods[i].data1 = entry[2].as<uint32_t>();
      } else {
        pods[i].ppq = entry["ppq"].as<double>();
        pods[i].data0 = entry["data0"].as<uint32_t>();
        pods[i].data1 = hasProperty(entry, "data1") ? entry["data1"].as<uint32_t>() : 0;
      }
    }
    const SonareError err = sonare_project_set_midi_events(
        project_.get(), clip_id, pods.empty() ? nullptr : pods.data(), pods.size());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to set MIDI events");
    }
  }

  uint32_t importSmf(val data) {
    std::vector<uint8_t> bytes = uint8ArrayToVector(data);
    uint32_t first_clip = 0;
    const SonareError err = sonare_project_import_smf(
        project_.get(), bytes.empty() ? nullptr : bytes.data(), bytes.size(), &first_clip);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat, "failed to import SMF");
    }
    return first_clip;
  }

  val exportSmf() {
    uint8_t* bytes = nullptr;
    size_t len = 0;
    const SonareError err = sonare_project_export_smf(project_.get(), &bytes, &len);
    if (err != SONARE_OK) {
      sonare_free_bytes(bytes);
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to export SMF");
    }
    std::vector<uint8_t> out(bytes, bytes + len);
    sonare_free_bytes(bytes);
    return vectorToUint8Array(out);
  }

  uint32_t importClipFile(val data) {
    std::vector<uint8_t> bytes = uint8ArrayToVector(data);
    uint32_t first_clip = 0;
    const SonareError err = sonare_project_import_clip_file(
        project_.get(), bytes.empty() ? nullptr : bytes.data(), bytes.size(), &first_clip);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat,
                                    "failed to import MIDI Clip File");
    }
    return first_clip;
  }

  val exportClipFile() {
    uint8_t* bytes = nullptr;
    size_t len = 0;
    const SonareError err = sonare_project_export_clip_file(project_.get(), &bytes, &len);
    if (err != SONARE_OK) {
      sonare_free_bytes(bytes);
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to export MIDI Clip File");
    }
    std::vector<uint8_t> out(bytes, bytes + len);
    sonare_free_bytes(bytes);
    return vectorToUint8Array(out);
  }

  void setProgram(uint32_t clip_id, int program, int bank) {
    const SonareError err = sonare_project_set_program(project_.get(), clip_id, program, bank);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to set MIDI program");
    }
  }

  void setProgramOnChannel(uint32_t clip_id, uint32_t group, uint32_t channel, int program,
                           int bank) {
    const SonareError err =
        sonare_project_set_program_on_channel(project_.get(), clip_id, static_cast<uint8_t>(group),
                                              static_cast<uint8_t>(channel), program, bank);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to set MIDI program");
    }
  }

  void bakeMidiFx(uint32_t clip_id, const std::string& config_json) {
    const SonareError err =
        sonare_project_bake_midi_fx(project_.get(), clip_id, config_json.c_str());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to set MIDI FX");
    }
  }

  void setMidiFx(uint32_t clip_id, const std::string& config_json) {
    bakeMidiFx(clip_id, config_json);
  }

  // Pre-flight check for hanging / unmatched notes in a MIDI clip. Returns
  // { ok, unmatchedNoteOns, unmatchedNoteOffs }; throws if the clip id is
  // unknown or not a MIDI clip.
  val validateMidiNotes(uint32_t clip_id) {
    SonareNotePairValidation result{};
    const SonareError err = sonare_project_validate_midi_notes(project_.get(), clip_id, &result);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to validate MIDI notes");
    }
    val out = val::object();
    out.set("ok", result.ok != 0);
    out.set("unmatchedNoteOns", static_cast<double>(result.unmatched_note_ons));
    out.set("unmatchedNoteOffs", static_cast<double>(result.unmatched_note_offs));
    return out;
  }

  float autoTempo(val audio, int sample_rate) {
    std::vector<float> samples = float32ArrayToVector(audio);
    float bpm = 0.0f;
    const SonareError err = sonare_project_auto_tempo(project_.get(), samples.data(),
                                                      samples.size(), sample_rate, &bpm);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to detect project tempo");
    }
    return bpm;
  }

  double snapToGrid(double ppq, double strength) {
    double out = 0.0;
    const SonareError err = sonare_project_snap_to_grid(project_.get(), ppq, strength, &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to snap to grid");
    }
    return out;
  }

  // Compiles the project into a renderable timeline, returning a small JS
  // object { diagnosticCount, hasTimeline, messages }.
  val compile() {
    SonareProjectCompileResult result{};
    const SonareError err = sonare_project_compile(project_.get(), &result);
    if (err != SONARE_OK) {
      sonare_project_free_compile_result(&result);
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to compile project");
    }
    val out = projectCompileResultToVal(result);
    sonare_project_free_compile_result(&result);
    return out;
  }

  // Parses a JS bounce-options object into the flat C POD. A missing /
  // null/undefined object leaves the zero-init defaults (which the C ABI maps to
  // project sample rate, 2 channels, block 128, and auto-derived length).
  static SonareProjectBounceOptions bounceOptionsFromVal(val options) {
    SonareProjectBounceOptions opts{};
    if (!options.isUndefined() && !options.isNull()) {
      if (hasProperty(options, "totalFrames")) {
        opts.total_frames = static_cast<int64_t>(options["totalFrames"].as<double>());
      }
      if (hasProperty(options, "blockSize")) {
        opts.block_size = options["blockSize"].as<int>();
      }
      if (hasProperty(options, "numChannels")) {
        opts.num_channels = options["numChannels"].as<int>();
      }
      if (hasProperty(options, "sampleRate")) {
        opts.sample_rate = options["sampleRate"].as<int>();
      }
      if (hasProperty(options, "instrumentLatencySamples")) {
        opts.instrument_latency_samples = options["instrumentLatencySamples"].as<int>();
      }
    }
    return opts;
  }

  // Maps a waveform name to its SonareSynthWaveform ordinal, or -1 if unknown.
  // Mirrors the Node/Python accepted set: "sine", "saw"/"sawtooth", "square",
  // "triangle".
  static int waveformFromName(const std::string& name) {
    if (name == "sine") return SONARE_SYNTH_WAVEFORM_SINE;
    if (name == "saw" || name == "sawtooth") return SONARE_SYNTH_WAVEFORM_SAW;
    if (name == "square") return SONARE_SYNTH_WAVEFORM_SQUARE;
    if (name == "triangle") return SONARE_SYNTH_WAVEFORM_TRIANGLE;
    return -1;
  }

  // Reads a single { destinationId?, waveform?, gain?, attack?, decay?,
  // sustain?, release?, polyphony? } object into a built-in synth binding. Every
  // numeric synth field is "0 => default" per the C ABI, so unset fields stay 0.
  // `waveform` accepts the ordinal or a string ("sine"/"saw"/"sawtooth"/
  // "square"/"triangle"); an unknown name throws, matching Node/Python.
  static SonareBuiltinInstrumentBinding builtinBindingFromVal(val desc) {
    SonareBuiltinInstrumentBinding binding{};
    if (desc.isUndefined() || desc.isNull()) {
      return binding;
    }
    if (hasProperty(desc, "destinationId")) {
      binding.destination_id = desc["destinationId"].as<uint32_t>();
    }
    if (hasProperty(desc, "waveform")) {
      val wf = desc["waveform"];
      if (wf.typeOf().as<std::string>() == "string") {
        const std::string s = wf.as<std::string>();
        const int mapped = waveformFromName(s);
        if (mapped < 0) {
          throw sonare::SonareException(
              sonare::ErrorCode::InvalidParameter,
              "Unknown synth waveform name: '" + s +
                  "' (expected sine, saw, sawtooth, square, or triangle)");
        }
        binding.config.waveform = mapped;
      } else {
        binding.config.waveform = wf.as<int>();
      }
    }
    if (hasProperty(desc, "gain")) {
      binding.config.gain = desc["gain"].as<float>();
    }
    if (hasProperty(desc, "attackMs")) {
      binding.config.attack_ms = desc["attackMs"].as<float>();
    }
    if (hasProperty(desc, "decayMs")) {
      binding.config.decay_ms = desc["decayMs"].as<float>();
    }
    if (hasProperty(desc, "sustain")) {
      binding.config.sustain = desc["sustain"].as<float>();
    }
    if (hasProperty(desc, "releaseMs")) {
      binding.config.release_ms = desc["releaseMs"].as<float>();
    }
    if (hasProperty(desc, "polyphony")) {
      binding.config.polyphony = desc["polyphony"].as<int>();
    }
    return binding;
  }

  // Normalizes the bounceWithBuiltinInstrument `instrument(s)` argument into a
  // vector of bindings. Accepts an array of binding objects (an explicitly empty
  // array yields ZERO bindings -> silent bounce, matching Node/Python and the
  // documented contract), a single binding object (treated as one element), or
  // null/undefined (also zero bindings -> silent). The TS wrapper's default
  // argument of `{}` supplies the single default-sine patch for the no-arg call.
  static std::vector<SonareBuiltinInstrumentBinding> builtinBindingsFromVal(val bindings) {
    std::vector<SonareBuiltinInstrumentBinding> out;
    if (bindings.isUndefined() || bindings.isNull()) {
      return out;
    }
    if (val::global("Array").call<bool>("isArray", bindings)) {
      const size_t count = bindings["length"].as<size_t>();
      out.reserve(count);
      for (size_t i = 0; i < count; ++i) {
        out.push_back(builtinBindingFromVal(bindings[i]));
      }
      return out;
    }
    out.push_back(builtinBindingFromVal(bindings));
    return out;
  }

  // Compiles + renders the project offline to interleaved float audio (silent
  // for MIDI tracks, which have no instrument bound). Uses C-ABI defaults
  // (project sample rate, 2 channels, block 128) when no options are provided.
  // When totalFrames is omitted (or <= 0) the C ABI auto-derives the render
  // length from the arrangement, so an arrangement with content renders without
  // the caller computing a frame count; an empty project yields an empty buffer.
  // To make MIDI tracks audible use bounceWithBuiltinInstrument.
  val bounce(val options) {
    SonareProjectBounceOptions opts = bounceOptionsFromVal(options);
    float* interleaved = nullptr;
    size_t len = 0;
    const SonareError err = sonare_project_bounce(project_.get(), &opts, &interleaved, &len);
    if (err != SONARE_OK) {
      sonare_free_floats(interleaved);
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to bounce project");
    }
    std::vector<float> samples(interleaved, interleaved + len);
    sonare_free_floats(interleaved);
    return vectorToFloat32Array(samples);
  }

  // Compiles + renders the project, routing MIDI tracks through the built-in
  // oscillator synth so a MIDI-only arrangement bounces to audible audio.
  // @p bindings is an array of { destinationId?, ...synthConfig } objects (a
  // single object is accepted too); an explicitly empty array (or null /
  // undefined) produces zero bindings, so MIDI tracks render silently. Every
  // synth-config
  // field is optional and "0 => sensible default" per the C ABI. When
  // options.totalFrames is omitted the render length is auto-derived from the
  // arrangement plus the synth release tail.
  val bounceWithBuiltinInstrument(val bindings, val options) {
    std::vector<SonareBuiltinInstrumentBinding> synths = builtinBindingsFromVal(bindings);
    SonareProjectBounceOptions opts = bounceOptionsFromVal(options);
    float* interleaved = nullptr;
    size_t len = 0;
    const SonareError err = sonare_project_bounce_with_builtin_instruments(
        project_.get(), &opts, synths.empty() ? nullptr : synths.data(), synths.size(),
        &interleaved, &len);
    if (err != SONARE_OK) {
      sonare_free_floats(interleaved);
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to bounce project with built-in instrument");
    }
    std::vector<float> samples(interleaved, interleaved + len);
    sonare_free_floats(interleaved);
    return vectorToFloat32Array(samples);
  }

  // Compiles + renders the project, routing MIDI tracks through the
  // patch-driven NativeSynth (the full synthesizer; see the SynthPatch TS
  // type). @p bindings is a SynthPatch object, a preset-name string
  // ("saw-lead" / "va:saw-lead"), or an array of either (each entry may carry
  // a destinationId). An empty array / null / undefined produces zero
  // bindings. Unknown preset names throw.
  val bounceWithSynthInstrument(val bindings, val options) {
    std::vector<SonareSynthInstrumentBinding> synths;
    if (!bindings.isUndefined() && !bindings.isNull()) {
      auto bindingFromVal = [](val desc) {
        SonareSynthInstrumentBinding binding{};
        if (desc.typeOf().as<std::string>() == "object" && hasProperty(desc, "destinationId")) {
          binding.destination_id = desc["destinationId"].as<uint32_t>();
        }
        binding.patch = sonare_wasm_synth::synthPatchFromVal(desc);
        return binding;
      };
      if (val::global("Array").call<bool>("isArray", bindings)) {
        const size_t count = bindings["length"].as<size_t>();
        synths.reserve(count);
        for (size_t i = 0; i < count; ++i) synths.push_back(bindingFromVal(bindings[i]));
      } else {
        synths.push_back(bindingFromVal(bindings));
      }
    }
    SonareProjectBounceOptions opts = bounceOptionsFromVal(options);
    float* interleaved = nullptr;
    size_t len = 0;
    const SonareError err = sonare_project_bounce_with_synth_instruments(
        project_.get(), &opts, synths.empty() ? nullptr : synths.data(), synths.size(),
        &interleaved, &len);
    if (err != SONARE_OK) {
      sonare_free_floats(interleaved);
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to bounce project with synth instrument");
    }
    std::vector<float> samples(interleaved, interleaved + len);
    sonare_free_floats(interleaved);
    return vectorToFloat32Array(samples);
  }

  // Loads (parses) SoundFont 2 bytes into the project (presets / sample PCM),
  // replacing any previously loaded SoundFont. The host copies the .sf2 bytes
  // into linear memory as a Uint8Array (same convention as importSmf); they are
  // not referenced after the call. Throws on malformed input (the previous
  // SoundFont is kept).
  void loadSoundFont(val data) {
    std::vector<uint8_t> bytes = uint8ArrayToVector(data);
    const SonareError err = sonare_project_load_soundfont(
        project_.get(), bytes.empty() ? nullptr : bytes.data(), bytes.size());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidFormat, "failed to load SoundFont");
    }
  }

  // Releases the project's loaded SoundFont (no-op when none is loaded).
  void clearSoundFont() {
    const SonareError err = sonare_project_clear_soundfont(project_.get());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to clear SoundFont");
    }
  }

  // Number of presets in the loaded SoundFont (0 when none is loaded).
  size_t soundFontPresetCount() {
    size_t count = 0;
    const SonareError err = sonare_project_soundfont_preset_count(project_.get(), &count);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to query SoundFont preset count");
    }
    return count;
  }

  // Enumerates every (channel, bank, program) combination the arrangement
  // plays a note through, in first-use order, as an array of { channel, bank,
  // program, backend: 'sf2'|'synth', presetName } objects (matching Node).
  val soundFontManifest() {
    size_t total = 0;
    SonareError err = sonare_project_soundfont_manifest(project_.get(), nullptr, 0, &total);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to build SoundFont manifest");
    }
    std::vector<SonareSf2ProgramStatus> entries(total);
    if (total > 0) {
      err = sonare_project_soundfont_manifest(project_.get(), entries.data(), total, &total);
      if (err != SONARE_OK) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                      "failed to build SoundFont manifest");
      }
    }
    val out = val::array();
    for (size_t i = 0; i < entries.size(); ++i) {
      val entry = val::object();
      entry.set("channel", entries[i].channel);
      entry.set("bank", entries[i].bank);
      entry.set("program", entries[i].program);
      entry.set("backend",
                std::string(entries[i].backend == SONARE_SOURCE_BACKEND_SF2 ? "sf2" : "synth"));
      entry.set("presetName", std::string(entries[i].preset_name));
      out.set(i, entry);
    }
    return out;
  }

  // Reads a single { destinationId?, gain?, polyphony? } object into an SF2
  // instrument binding ("0 / omit => default" per the C ABI).
  static SonareSf2InstrumentBinding sf2BindingFromVal(val desc) {
    SonareSf2InstrumentBinding binding{};
    if (desc.isUndefined() || desc.isNull()) {
      return binding;
    }
    if (hasProperty(desc, "destinationId")) {
      binding.destination_id = desc["destinationId"].as<uint32_t>();
    }
    if (hasProperty(desc, "gain")) {
      binding.config.gain = desc["gain"].as<float>();
    }
    if (hasProperty(desc, "polyphony")) {
      binding.config.polyphony = desc["polyphony"].as<int>();
    }
    return binding;
  }

  // Like bounceWithBuiltinInstrument, but each bound destination renders
  // through a GS-compatible SoundFont player fed by the project's loaded
  // SoundFont. Programs the SoundFont does not cover — including bouncing
  // with no SoundFont loaded at all — play through the built-in synthesizer
  // GM fallback bank (the data-free floor). Accepts an array of binding
  // objects, a single object, or null/undefined (zero bindings -> silence).
  val bounceWithSf2Instrument(val bindings, val options) {
    std::vector<SonareSf2InstrumentBinding> players;
    if (!bindings.isUndefined() && !bindings.isNull()) {
      if (val::global("Array").call<bool>("isArray", bindings)) {
        const size_t count = bindings["length"].as<size_t>();
        players.reserve(count);
        for (size_t i = 0; i < count; ++i) {
          players.push_back(sf2BindingFromVal(bindings[i]));
        }
      } else {
        players.push_back(sf2BindingFromVal(bindings));
      }
    }
    SonareProjectBounceOptions opts = bounceOptionsFromVal(options);
    float* interleaved = nullptr;
    size_t len = 0;
    const SonareError err = sonare_project_bounce_with_sf2_instruments(
        project_.get(), &opts, players.empty() ? nullptr : players.data(), players.size(),
        &interleaved, &len);
    if (err != SONARE_OK) {
      sonare_free_floats(interleaved);
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to bounce project with SF2 instrument");
    }
    std::vector<float> samples(interleaved, interleaved + len);
    sonare_free_floats(interleaved);
    return vectorToFloat32Array(samples);
  }

  // --------------------------------------------------------------------------
  // Edit operations (undoable; route through EditHistory commands)
  // --------------------------------------------------------------------------

  void removeClip(uint32_t clip_id) {
    const SonareError err = sonare_project_remove_clip(project_.get(), clip_id);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to remove clip");
    }
  }

  void setClipGain(uint32_t clip_id, float gain) {
    const SonareError err = sonare_project_set_clip_gain(project_.get(), clip_id, gain);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to set clip gain");
    }
  }

  // Reads a { lengthPpq?, curve? } fade descriptor; curve accepts the ordinal or
  // a string ("linear"/"equalPower"/"exponential"/"logarithmic").
  static SonareProjectClipFade clipFadeFromVal(val desc) {
    SonareProjectClipFade fade{};
    if (desc.isUndefined() || desc.isNull()) {
      return fade;
    }
    if (hasProperty(desc, "lengthPpq")) {
      fade.length_ppq = desc["lengthPpq"].as<double>();
    }
    if (hasProperty(desc, "curve")) {
      val curve = desc["curve"];
      if (curve.typeOf().as<std::string>() == "string") {
        const std::string s = curve.as<std::string>();
        fade.curve = s == "equalPower"    ? SONARE_FADE_CURVE_EQUAL_POWER
                     : s == "exponential" ? SONARE_FADE_CURVE_EXPONENTIAL
                     : s == "logarithmic" ? SONARE_FADE_CURVE_LOGARITHMIC
                                          : SONARE_FADE_CURVE_LINEAR;
      } else {
        fade.curve = curve.as<uint32_t>();
      }
    }
    return fade;
  }

  void setClipFade(uint32_t clip_id, val fade_in, val fade_out) {
    SonareProjectClipFade in = clipFadeFromVal(fade_in);
    SonareProjectClipFade out = clipFadeFromVal(fade_out);
    const SonareError err = sonare_project_set_clip_fade(project_.get(), clip_id, &in, &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to set clip fade");
    }
  }

  void setClipLoop(uint32_t clip_id, int loop_mode, double loop_length_ppq) {
    const SonareError err =
        sonare_project_set_clip_loop(project_.get(), clip_id, loop_mode, loop_length_ppq);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to set clip loop");
    }
  }

  void setClipSource(uint32_t clip_id, uint32_t source_id) {
    const SonareError err = sonare_project_set_clip_source(project_.get(), clip_id, source_id);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to set clip source");
    }
  }

  uint32_t duplicateClip(uint32_t clip_id, double new_start_ppq) {
    uint32_t out = 0;
    const SonareError err =
        sonare_project_duplicate_clip(project_.get(), clip_id, new_start_ppq, &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to duplicate clip");
    }
    return out;
  }

  void removeTrack(uint32_t track_id) {
    const SonareError err = sonare_project_remove_track(project_.get(), track_id);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to remove track");
    }
  }

  void renameTrack(uint32_t track_id, const std::string& name) {
    const SonareError err = sonare_project_rename_track(project_.get(), track_id,
                                                        name.empty() ? nullptr : name.c_str());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to rename track");
    }
  }

  void setTrackRoute(uint32_t track_id, const std::string& channel_strip_ref,
                     const std::string& output_target) {
    const SonareError err = sonare_project_set_track_route(
        project_.get(), track_id, channel_strip_ref.empty() ? nullptr : channel_strip_ref.c_str(),
        output_target.empty() ? nullptr : output_target.c_str());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to set track route");
    }
  }

  // Reads a JS array of { ppq, value, curve? } automation breakpoints into the
  // shared SonareAutomationPoint POD. `curve` accepts the ordinal or a string
  // ("linear"/"exponential"/"hold"/"scurve"). The points are stored verbatim.
  static std::vector<SonareAutomationPoint> automationPointsFromVal(val points) {
    std::vector<SonareAutomationPoint> out;
    if (points.isUndefined() || points.isNull()) {
      return out;
    }
    if (!val::global("Array").call<bool>("isArray", points)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "automation points must be an array");
    }
    const size_t count = points["length"].as<size_t>();
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      val point = points[i];
      SonareAutomationPoint p{};
      p.ppq = hasProperty(point, "ppq") ? point["ppq"].as<double>() : 0.0;
      p.value = hasProperty(point, "value") ? point["value"].as<float>() : 0.0f;
      if (hasProperty(point, "curve")) {
        val curve = point["curve"];
        if (curve.typeOf().as<std::string>() == "string") {
          const std::string s = curve.as<std::string>();
          p.curve_to_next = s == "exponential" ? SONARE_CURVE_EXPONENTIAL
                            : s == "hold"      ? SONARE_CURVE_HOLD
                            : s == "scurve"    ? SONARE_CURVE_SCURVE
                                               : SONARE_CURVE_LINEAR;
        } else {
          p.curve_to_next = curve.as<int>();
        }
      } else {
        p.curve_to_next = SONARE_CURVE_LINEAR;
      }
      out.push_back(p);
    }
    return out;
  }

  static SonareAutomationLaneDesc automationLaneDescFromVal(
      val desc, std::vector<SonareAutomationPoint>* storage) {
    SonareAutomationLaneDesc d{};
    if (desc.isUndefined() || desc.isNull()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "automation lane descriptor required");
    }
    d.target_param_id =
        hasProperty(desc, "targetParamId") ? desc["targetParamId"].as<uint32_t>() : 0;
    *storage = automationPointsFromVal(hasProperty(desc, "points") ? desc["points"] : val::array());
    d.points = storage->empty() ? nullptr : storage->data();
    d.point_count = storage->size();
    return d;
  }

  double addAutomationLane(uint32_t track_id, val desc) {
    std::vector<SonareAutomationPoint> storage;
    SonareAutomationLaneDesc d = automationLaneDescFromVal(desc, &storage);
    size_t out = 0;
    const SonareError err = sonare_project_add_automation_lane(project_.get(), track_id, &d, &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to add automation lane");
    }
    return static_cast<double>(out);
  }

  void editAutomationLane(uint32_t track_id, double lane_index, val desc) {
    std::vector<SonareAutomationPoint> storage;
    SonareAutomationLaneDesc d = automationLaneDescFromVal(desc, &storage);
    const SonareError err = sonare_project_edit_automation_lane(
        project_.get(), track_id, static_cast<size_t>(lane_index), &d);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to edit automation lane");
    }
  }

  void removeAutomationLane(uint32_t track_id, double lane_index) {
    const SonareError err = sonare_project_remove_automation_lane(project_.get(), track_id,
                                                                  static_cast<size_t>(lane_index));
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to remove automation lane");
    }
  }

  // --------------------------------------------------------------------------
  // MIR annotation streams (undoable)
  // --------------------------------------------------------------------------

  void annotateKeys(val keys) {
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
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to annotate keys");
    }
  }

  void annotateChords(val chords) {
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
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to annotate chords");
    }
  }

  // --------------------------------------------------------------------------
  // Assist sidecars (opaque module state)
  // --------------------------------------------------------------------------

  void setAssistSidecar(const std::string& module_id, uint32_t schema_version,
                        uint32_t target_track_id, double region_start_ppq, double region_end_ppq,
                        val payload) {
    std::vector<uint8_t> bytes = uint8ArrayToVector(payload);
    const SonareError err = sonare_project_set_assist_sidecar(
        project_.get(), module_id.c_str(), schema_version, target_track_id, region_start_ppq,
        region_end_ppq, bytes.empty() ? nullptr : bytes.data(), bytes.size());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to set assist sidecar");
    }
  }

  double assistSidecarCount() const {
    return static_cast<double>(sonare_project_assist_sidecar_count(project_.get()));
  }

  val getAssistSidecar(double index) const {
    SonareProjectAssistSidecar sidecar{};
    const SonareError err =
        sonare_project_get_assist_sidecar(project_.get(), static_cast<size_t>(index), &sidecar);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to read assist sidecar");
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

  // --------------------------------------------------------------------------
  // Project-level configuration, counts, and timeline metadata
  // --------------------------------------------------------------------------

  void setOverlapPolicy(uint32_t policy) {
    const SonareError err = sonare_project_set_overlap_policy(project_.get(), policy);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to set overlap policy");
    }
  }

  uint32_t getOverlapPolicy() const {
    uint32_t out = 0;
    const SonareError err = sonare_project_get_overlap_policy(project_.get(), &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to read overlap policy");
    }
    return out;
  }

  double getSampleRate() const {
    double out = 0.0;
    const SonareError err = sonare_project_get_sample_rate(project_.get(), &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to read sample rate");
    }
    return out;
  }

  void setMixerSceneJson(const std::string& scene_json) {
    const SonareError err = sonare_project_set_mixer_scene_json(project_.get(), scene_json.c_str());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to set mixer scene JSON");
    }
  }

  uint32_t setMarker(uint32_t marker_id, double ppq, const std::string& name) {
    uint32_t out_id = 0;
    const SonareError err =
        sonare_project_set_marker(project_.get(), marker_id, ppq, name.c_str(), &out_id);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to set marker");
    }
    return out_id;
  }

  double trackCount() const {
    size_t out = 0;
    const SonareError err = sonare_project_track_count(project_.get(), &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to read track count");
    }
    return static_cast<double>(out);
  }

  double sourceCount() const {
    size_t out = 0;
    const SonareError err = sonare_project_source_count(project_.get(), &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState, "failed to read source count");
    }
    return static_cast<double>(out);
  }

  double tempoSegmentCount() const {
    size_t out = 0;
    const SonareError err = sonare_project_tempo_segment_count(project_.get(), &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to read tempo segment count");
    }
    return static_cast<double>(out);
  }

  double timeSignatureCount() const {
    size_t out = 0;
    const SonareError err = sonare_project_time_signature_count(project_.get(), &out);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to read time signature count");
    }
    return static_cast<double>(out);
  }

  // Replaces the project's tempo map from an array of { startPpq, bpm,
  // startSample?, endBpm? } segments. Missing numeric fields default to 0.0
  // (endBpm 0 = constant tempo), matching the C ABI.
  void setTempoSegments(val segments) {
    std::vector<SonareProjectTempoSegment> segs;
    if (!segments.isUndefined() && !segments.isNull()) {
      const unsigned count = segments["length"].as<unsigned>();
      segs.reserve(count);
      for (unsigned i = 0; i < count; ++i) {
        val entry = segments[i];
        SonareProjectTempoSegment seg{};
        seg.start_ppq = hasProperty(entry, "startPpq") ? entry["startPpq"].as<double>() : 0.0;
        seg.bpm = hasProperty(entry, "bpm") ? entry["bpm"].as<double>() : 0.0;
        seg.start_sample =
            hasProperty(entry, "startSample") ? entry["startSample"].as<double>() : 0.0;
        seg.end_bpm = hasProperty(entry, "endBpm") ? entry["endBpm"].as<double>() : 0.0;
        segs.push_back(seg);
      }
    }
    const SonareError err = sonare_project_set_tempo_segments(
        project_.get(), segs.empty() ? nullptr : segs.data(), segs.size());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to set tempo segments");
    }
  }

  // Replaces the project's time-signature map from an array of { startPpq,
  // numerator, denominator } segments.
  void setTimeSignatures(val segments) {
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
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to set time signatures");
    }
  }

  // Surfaces the compile diagnostics produced by the most recent bounce on this
  // project (e.g. MIDI clips rendering silently without a bound instrument).
  // Same shape as compile().
  val lastBounceCompileResult() const {
    SonareProjectCompileResult result{};
    const SonareError err = sonare_project_last_bounce_compile_result(project_.get(), &result);
    if (err != SONARE_OK) {
      sonare_project_free_compile_result(&result);
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "failed to read last bounce compile result");
    }
    val out = projectCompileResultToVal(result);
    sonare_project_free_compile_result(&result);
    return out;
  }

  std::shared_ptr<SonareProject> project_;
};

uint32_t js_project_abi_version() { return sonare_project_abi_version(); }

val js_nullable_string(const char* value) {
  return value != nullptr ? val(std::string(value)) : val::null();
}

val js_midi_gm_instrument_name(int program) {
  return js_nullable_string(sonare_midi_gm_instrument_name(program));
}

int js_midi_gm_program_for_name(const std::string& name) {
  return sonare_midi_gm_program_for_name(name.c_str());
}

val js_midi_gm_family_name(int family) {
  return js_nullable_string(sonare_midi_gm_family_name(family));
}

int js_midi_gm_family_first_program(int family) {
  return sonare_midi_gm_family_first_program(family);
}

val js_midi_gm2_instrument_name(int bank_lsb, int program) {
  return js_nullable_string(sonare_midi_gm2_instrument_name(bank_lsb, program));
}

val js_midi_gm_drum_name(int note) { return js_nullable_string(sonare_midi_gm_drum_name(note)); }

int js_midi_gm_drum_note_for_name(const std::string& name) {
  return sonare_midi_gm_drum_note_for_name(name.c_str());
}

val js_midi_gm2_drum_set_name(int bank_lsb) {
  return js_nullable_string(sonare_midi_gm2_drum_set_name(bank_lsb));
}

val js_midi_gm2_drum_name(int bank_lsb, int note) {
  return js_nullable_string(sonare_midi_gm2_drum_name(bank_lsb, note));
}

val js_midi_cc_name(int controller) { return js_nullable_string(sonare_midi_cc_name(controller)); }

int js_midi_cc_index_for_name(const std::string& name) {
  return sonare_midi_cc_index_for_name(name.c_str());
}

val js_midi_per_note_controller_name(int index) {
  return js_nullable_string(sonare_midi_per_note_controller_name(index));
}

val js_midi_bank_program(double ppq, int group, int channel, int bank_msb, int bank_lsb,
                         int program) {
  SonareMidiEventPod events[3]{};
  size_t count = 0;
  const SonareError err =
      sonare_midi_bank_program(ppq, static_cast<uint8_t>(group), static_cast<uint8_t>(channel),
                               bank_msb, bank_lsb, program, events, 3, &count);
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid MIDI bank/program arguments");
  }
  val out = val::array();
  for (size_t i = 0; i < count; ++i) {
    val event = val::object();
    event.set("ppq", events[i].ppq);
    event.set("data0", static_cast<double>(events[i].data0));
    event.set("data1", static_cast<double>(events[i].data1));
    out.set(static_cast<unsigned>(i), event);
  }
  return out;
}

SonareMidiEventPod js_midi_event_from_val(val event) {
  SonareMidiEventPod out{};
  out.ppq = event["ppq"].as<double>();
  out.data0 = event["data0"].as<uint32_t>();
  out.data1 = hasProperty(event, "data1") ? event["data1"].as<uint32_t>() : 0;
  return out;
}

val js_midi_event_to_val(const SonareMidiEventPod& event) {
  val out = val::object();
  out.set("ppq", event.ppq);
  out.set("data0", static_cast<double>(event.data0));
  out.set("data1", static_cast<double>(event.data1));
  return out;
}

SonareMidiCcBinding js_cc_binding_from_val(val object) {
  SonareMidiCcBinding out{};
  out.cc_number = object["ccNumber"].as<uint8_t>();
  out.channel = hasProperty(object, "channel") && !object["channel"].isNull()
                    ? object["channel"].as<uint8_t>()
                    : 0xffu;
  out.kind = hasProperty(object, "kind") ? object["kind"].as<uint8_t>() : 0u;
  out.cc_lsb_number = hasProperty(object, "ccLsbNumber") ? object["ccLsbNumber"].as<uint8_t>() : 0u;
  out.selector_msb = hasProperty(object, "selectorMsb") ? object["selectorMsb"].as<uint8_t>() : 0u;
  out.selector_lsb = hasProperty(object, "selectorLsb") ? object["selectorLsb"].as<uint8_t>() : 0u;
  out.param_id = object["paramId"].as<uint32_t>();
  out.min_value = hasProperty(object, "minValue") ? object["minValue"].as<float>() : 0.0f;
  out.max_value = hasProperty(object, "maxValue") ? object["maxValue"].as<float>() : 1.0f;
  return out;
}

val js_cc_binding_to_val(const SonareMidiCcBinding& binding) {
  val out = val::object();
  out.set("ccNumber", static_cast<double>(binding.cc_number));
  out.set("channel", static_cast<double>(binding.channel));
  out.set("kind", static_cast<double>(binding.kind));
  out.set("ccLsbNumber", static_cast<double>(binding.cc_lsb_number));
  out.set("selectorMsb", static_cast<double>(binding.selector_msb));
  out.set("selectorLsb", static_cast<double>(binding.selector_lsb));
  out.set("paramId", static_cast<double>(binding.param_id));
  out.set("minValue", binding.min_value);
  out.set("maxValue", binding.max_value);
  return out;
}

std::vector<SonareMidiCcBinding> js_cc_bindings_from_val(val bindings) {
  const size_t count =
      bindings.isUndefined() || bindings.isNull() ? 0 : bindings["length"].as<size_t>();
  std::vector<SonareMidiCcBinding> out(count);
  for (size_t i = 0; i < count; ++i) {
    out[i] = js_cc_binding_from_val(bindings[i]);
  }
  return out;
}

val js_midi_cc_learn(val events, uint32_t param_id, float min_value, float max_value,
                     int min_movement) {
  const size_t count = events.isUndefined() || events.isNull() ? 0 : events["length"].as<size_t>();
  std::vector<SonareMidiEventPod> pods(count);
  for (size_t i = 0; i < count; ++i) {
    pods[i] = js_midi_event_from_val(events[i]);
  }
  SonareMidiCcBinding learned{};
  const SonareError err =
      sonare_midi_cc_learn(pods.empty() ? nullptr : pods.data(), pods.size(), param_id, min_value,
                           max_value, static_cast<uint8_t>(min_movement), &learned);
  if (err == SONARE_ERROR_INVALID_STATE) return val::null();
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid MIDI CC learn arguments");
  }
  return js_cc_binding_to_val(learned);
}

val js_midi_cc_to_breakpoint(val bindings, val event) {
  std::vector<SonareMidiCcBinding> cc_bindings = js_cc_bindings_from_val(bindings);
  SonareMidiEventPod pod = js_midi_event_from_val(event);
  SonareAutomationPoint point{};
  const SonareError err = sonare_midi_cc_to_breakpoint(
      cc_bindings.empty() ? nullptr : cc_bindings.data(), cc_bindings.size(), &pod, &point);
  if (err == SONARE_ERROR_INVALID_STATE) return val::null();
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid MIDI CC breakpoint arguments");
  }
  val out = val::object();
  out.set("ppq", point.ppq);
  out.set("value", point.value);
  out.set("curveToNext", static_cast<double>(point.curve_to_next));
  return out;
}

val js_midi_param_to_cc(val bindings, uint32_t param_id, float unit_value, int group, double ppq) {
  std::vector<SonareMidiCcBinding> cc_bindings = js_cc_bindings_from_val(bindings);
  SonareMidiEventPod event{};
  const SonareError err = sonare_midi_param_to_cc(
      cc_bindings.empty() ? nullptr : cc_bindings.data(), cc_bindings.size(), param_id, unit_value,
      static_cast<uint8_t>(group), ppq, &event);
  if (err == SONARE_ERROR_INVALID_STATE) return val::null();
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid MIDI param-to-CC arguments");
  }
  return js_midi_event_to_val(event);
}

val js_midi_route_events(val events, val config) {
  const size_t count = events.isUndefined() || events.isNull() ? 0 : events["length"].as<size_t>();
  std::vector<SonareMidiEventPod> input(count);
  for (size_t i = 0; i < count; ++i) {
    val entry = events[i];
    input[i].ppq = entry["ppq"].as<double>();
    input[i].data0 = entry["data0"].as<uint32_t>();
    input[i].data1 = hasProperty(entry, "data1") ? entry["data1"].as<uint32_t>() : 0;
  }

  SonareMidiRouteConfig route{-1, -1, -1, 1};
  if (!config.isUndefined() && !config.isNull()) {
    if (hasProperty(config, "filterGroup") && !config["filterGroup"].isNull()) {
      route.filter_group = config["filterGroup"].as<int>();
    }
    if (hasProperty(config, "filterChannel") && !config["filterChannel"].isNull()) {
      route.filter_channel = config["filterChannel"].as<int>();
    }
    if (hasProperty(config, "remapChannel") && !config["remapChannel"].isNull()) {
      route.remap_channel = config["remapChannel"].as<int>();
    }
    if (hasProperty(config, "thru")) {
      route.thru = config["thru"].as<bool>() ? 1 : 0;
    }
  }

  std::vector<SonareMidiEventPod> output(input.size());
  size_t output_count = 0;
  int overflowed = 0;
  uint32_t overflow_count = 0;
  const SonareError err =
      sonare_midi_route_events(input.empty() ? nullptr : input.data(), input.size(), &route,
                               output.empty() ? nullptr : output.data(), output.size(),
                               &output_count, &overflowed, &overflow_count);
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid MIDI route arguments");
  }

  val out = val::object();
  val routed = val::array();
  for (size_t i = 0; i < output_count; ++i) {
    val event = val::object();
    event.set("ppq", output[i].ppq);
    event.set("data0", static_cast<double>(output[i].data0));
    event.set("data1", static_cast<double>(output[i].data1));
    routed.set(static_cast<unsigned>(i), event);
  }
  out.set("events", routed);
  out.set("overflowed", overflowed != 0);
  out.set("overflowCount", static_cast<double>(overflow_count));
  return out;
}
#endif  // SONARE_WITH_ARRANGEMENT

#if defined(SONARE_WITH_ARRANGEMENT)
// NativeSynth preset catalog ('\n'-joined program-lifetime string from the C
// ABI) split into a JS string[].
val js_synth_preset_names() {
  val out = val::array();
  const char* joined = sonare_synth_preset_names();
  if (joined == nullptr || joined[0] == '\0') return out;
  std::string names(joined);
  size_t start = 0;
  while (start <= names.size()) {
    const size_t end = names.find('\n', start);
    if (end == std::string::npos) {
      out.call<void>("push", names.substr(start));
      break;
    }
    out.call<void>("push", names.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

// Fetches a named catalog preset as a SynthPatch object (the preset name plus
// its wrapper-section values). A "va:" routing prefix is accepted; unknown
// names throw.
val js_synth_preset_patch(const std::string& name) {
  const std::string bare = name.rfind("va:", 0) == 0 ? name.substr(3) : name;
  SonareSynthPatch patch{};
  if (sonare_synth_preset_patch(bare.c_str(), &patch) != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "unknown synth preset name: '" + name + "'");
  }
  return sonare_wasm_synth::synthPatchToVal(patch);
}

val js_synth_enum_tables() { return sonare_wasm_synth::synthEnumTablesToVal(); }

val js_synth_patch_round_trip(val desc) {
  return sonare_wasm_synth::synthPatchToVal(sonare_wasm_synth::synthPatchFromVal(desc));
}
#endif  // SONARE_WITH_ARRANGEMENT

void registerProjectBindings() {
#if defined(SONARE_WITH_ARRANGEMENT)
  // Headless DAW project. fromJson is a static factory returning a
  // by-value Project; bounce takes an optional options object.
  class_<ProjectWasm>("Project")
      .constructor<>()
      .class_function("fromJson", &ProjectWasm::fromJson)
      .class_function("fromJsonWithDiagnostics", &ProjectWasm::fromJsonWithDiagnostics)
      .function("toJson", &ProjectWasm::toJson)
      .function("setSampleRate", &ProjectWasm::setSampleRate)
      .function("addTrack", &ProjectWasm::addTrack)
      .function("addClip", &ProjectWasm::addClip)
      .function("addMidiClip", &ProjectWasm::addMidiClip)
      .function("splitClip", &ProjectWasm::splitClip)
      .function("trimClip", &ProjectWasm::trimClip)
      .function("moveClip", &ProjectWasm::moveClip)
      .function("setTrackKind", &ProjectWasm::setTrackKind)
      .function("setClipWarpRef", &ProjectWasm::setClipWarpRef)
      .function("setWarpMap", &ProjectWasm::setWarpMap)
      .function("removeWarpMap", &ProjectWasm::removeWarpMap)
      .function("setTrackMidiDestination", &ProjectWasm::setTrackMidiDestination)
      .function("undo", &ProjectWasm::undo)
      .function("redo", &ProjectWasm::redo)
      .function("setMidiEvents", &ProjectWasm::setMidiEvents)
      .function("importSmf", &ProjectWasm::importSmf)
      .function("exportSmf", &ProjectWasm::exportSmf)
      .function("importClipFile", &ProjectWasm::importClipFile)
      .function("exportClipFile", &ProjectWasm::exportClipFile)
      .function("setProgram", &ProjectWasm::setProgram)
      .function("setProgramOnChannel", &ProjectWasm::setProgramOnChannel)
      .function("bakeMidiFx", &ProjectWasm::bakeMidiFx)
      .function("setMidiFx", &ProjectWasm::setMidiFx)
      .function("validateMidiNotes", &ProjectWasm::validateMidiNotes)
      .function("autoTempo", &ProjectWasm::autoTempo)
      .function("snapToGrid", &ProjectWasm::snapToGrid)
      .function("compile", &ProjectWasm::compile)
      .function("bounce", &ProjectWasm::bounce)
      .function("bounceWithBuiltinInstrument", &ProjectWasm::bounceWithBuiltinInstrument)
      .function("bounceWithSynthInstrument", &ProjectWasm::bounceWithSynthInstrument)
      .function("loadSoundFont", &ProjectWasm::loadSoundFont)
      .function("clearSoundFont", &ProjectWasm::clearSoundFont)
      .function("soundFontPresetCount", &ProjectWasm::soundFontPresetCount)
      .function("soundFontManifest", &ProjectWasm::soundFontManifest)
      .function("bounceWithSf2Instrument", &ProjectWasm::bounceWithSf2Instrument)
      .function("removeClip", &ProjectWasm::removeClip)
      .function("setClipGain", &ProjectWasm::setClipGain)
      .function("setClipFade", &ProjectWasm::setClipFade)
      .function("setClipLoop", &ProjectWasm::setClipLoop)
      .function("setClipSource", &ProjectWasm::setClipSource)
      .function("duplicateClip", &ProjectWasm::duplicateClip)
      .function("removeTrack", &ProjectWasm::removeTrack)
      .function("renameTrack", &ProjectWasm::renameTrack)
      .function("setTrackRoute", &ProjectWasm::setTrackRoute)
      .function("addAutomationLane", &ProjectWasm::addAutomationLane)
      .function("editAutomationLane", &ProjectWasm::editAutomationLane)
      .function("removeAutomationLane", &ProjectWasm::removeAutomationLane)
      .function("annotateKeys", &ProjectWasm::annotateKeys)
      .function("annotateChords", &ProjectWasm::annotateChords)
      .function("setAssistSidecar", &ProjectWasm::setAssistSidecar)
      .function("assistSidecarCount", &ProjectWasm::assistSidecarCount)
      .function("getAssistSidecar", &ProjectWasm::getAssistSidecar)
      .function("setOverlapPolicy", &ProjectWasm::setOverlapPolicy)
      .function("getOverlapPolicy", &ProjectWasm::getOverlapPolicy)
      .function("getSampleRate", &ProjectWasm::getSampleRate)
      .function("setMixerSceneJson", &ProjectWasm::setMixerSceneJson)
      .function("setMarker", &ProjectWasm::setMarker)
      .function("trackCount", &ProjectWasm::trackCount)
      .function("sourceCount", &ProjectWasm::sourceCount)
      .function("tempoSegmentCount", &ProjectWasm::tempoSegmentCount)
      .function("timeSignatureCount", &ProjectWasm::timeSignatureCount)
      .function("setTempoSegments", &ProjectWasm::setTempoSegments)
      .function("setTimeSignatures", &ProjectWasm::setTimeSignatures)
      .function("lastBounceCompileResult", &ProjectWasm::lastBounceCompileResult);
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
#endif
}

#endif  // __EMSCRIPTEN__
