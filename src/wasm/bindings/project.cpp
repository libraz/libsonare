/// @file project.cpp
/// @brief Embind bindings for the headless DAW project API.

#ifdef __EMSCRIPTEN__

#include "common.h"

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
  out.set("messages", std::string(result.messages != nullptr ? result.messages : ""));
  val diagnostics = val::array();
  for (size_t i = 0; i < result.diagnostic_count; ++i) {
    val diag = val::object();
    diag.set("code", static_cast<double>(result.diagnostics[i].code));
    diag.set("severity", static_cast<double>(result.diagnostics[i].severity));
    diag.set("targetId", static_cast<double>(result.diagnostics[i].target_id));
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

  void setClipWarpRef(uint32_t clip_id, uint32_t warp_ref_id) {
    const SonareError err = sonare_project_set_clip_warp_ref(project_.get(), clip_id, warp_ref_id);
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "failed to set clip warp reference");
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

  void setMidiFx(uint32_t clip_id, const std::string& config_json) {
    const SonareError err =
        sonare_project_set_midi_fx(project_.get(), clip_id, config_json.c_str());
    if (err != SONARE_OK) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "failed to set MIDI FX");
    }
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

  // Reads a single { destinationId?, waveform?, gain?, attack?, decay?,
  // sustain?, release?, polyphony? } object into a built-in synth binding. Every
  // numeric synth field is "0 => default" per the C ABI, so unset fields stay 0.
  // `waveform` accepts the ordinal or a string ("sine"/"saw"/"square"/
  // "triangle").
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
        binding.config.waveform = s == "saw"        ? SONARE_SYNTH_WAVEFORM_SAW
                                  : s == "square"   ? SONARE_SYNTH_WAVEFORM_SQUARE
                                  : s == "triangle" ? SONARE_SYNTH_WAVEFORM_TRIANGLE
                                                    : SONARE_SYNTH_WAVEFORM_SINE;
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
  // vector of bindings. Accepts an array of binding objects, a single binding
  // object (treated as one element), or null/undefined (one default-destination
  // sine patch so a freshly built MIDI project still makes sound).
  static std::vector<SonareBuiltinInstrumentBinding> builtinBindingsFromVal(val bindings) {
    std::vector<SonareBuiltinInstrumentBinding> out;
    if (bindings.isUndefined() || bindings.isNull()) {
      out.push_back(SonareBuiltinInstrumentBinding{});
      return out;
    }
    if (val::global("Array").call<bool>("isArray", bindings)) {
      const size_t count = bindings["length"].as<size_t>();
      if (count == 0) {
        out.push_back(SonareBuiltinInstrumentBinding{});
        return out;
      }
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
  // single object is accepted too); a missing / empty array routes every MIDI
  // track through one default-destination (0) sine patch. Every synth-config
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

  std::shared_ptr<SonareProject> project_;
};

uint32_t js_project_abi_version() { return sonare_project_abi_version(); }
#endif  // SONARE_WITH_ARRANGEMENT

void registerProjectBindings() {
#if defined(SONARE_WITH_ARRANGEMENT)
  // Headless DAW project. fromJson is a static factory returning a
  // by-value Project; bounce takes an optional options object.
  class_<ProjectWasm>("Project")
      .constructor<>()
      .class_function("fromJson", &ProjectWasm::fromJson)
      .function("toJson", &ProjectWasm::toJson)
      .function("setSampleRate", &ProjectWasm::setSampleRate)
      .function("addTrack", &ProjectWasm::addTrack)
      .function("addClip", &ProjectWasm::addClip)
      .function("addMidiClip", &ProjectWasm::addMidiClip)
      .function("splitClip", &ProjectWasm::splitClip)
      .function("trimClip", &ProjectWasm::trimClip)
      .function("moveClip", &ProjectWasm::moveClip)
      .function("setClipWarpRef", &ProjectWasm::setClipWarpRef)
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
      .function("setMidiFx", &ProjectWasm::setMidiFx)
      .function("autoTempo", &ProjectWasm::autoTempo)
      .function("snapToGrid", &ProjectWasm::snapToGrid)
      .function("compile", &ProjectWasm::compile)
      .function("bounce", &ProjectWasm::bounce)
      .function("bounceWithBuiltinInstrument", &ProjectWasm::bounceWithBuiltinInstrument);
  function("projectAbiVersion", &js_project_abi_version);
#endif
}

#endif  // __EMSCRIPTEN__
