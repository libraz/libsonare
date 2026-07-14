/// @file project_wasm.h
/// @brief Shared declaration of the embind headless-DAW project facade.
///
/// The facade is large enough that its method implementations are split across
/// several translation units (project.cpp + project_*.cpp), one per domain. They
/// all define members of the single ProjectWasm class declared here, and each
/// contributes its slice of the embind class_<> via a registerProject*() helper
/// that the core TU calls while building the one class_ handle. Splitting the
/// registration this way keeps every domain's JS-facing surface unchanged (same
/// class name, same method names, same free-function names).

#pragma once

#ifdef __EMSCRIPTEN__

#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "wasm/bindings/common/common.h"
#include "wasm/bindings/common/synth_patch_val.h"

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
inline val projectCompileResultToVal(const SonareProjectCompileResult& result) {
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

inline sonare::ErrorCode codeFromCError(SonareError err) {
  switch (err) {
    case SONARE_ERROR_FILE_NOT_FOUND:
      return sonare::ErrorCode::FileNotFound;
    case SONARE_ERROR_INVALID_FORMAT:
      return sonare::ErrorCode::InvalidFormat;
    case SONARE_ERROR_DECODE_FAILED:
      return sonare::ErrorCode::DecodeFailed;
    case SONARE_ERROR_INVALID_PARAMETER:
      return sonare::ErrorCode::InvalidParameter;
    case SONARE_ERROR_OUT_OF_MEMORY:
      return sonare::ErrorCode::OutOfMemory;
    case SONARE_ERROR_NOT_SUPPORTED:
      return sonare::ErrorCode::NotImplemented;
    case SONARE_ERROR_INVALID_STATE:
      return sonare::ErrorCode::InvalidState;
    case SONARE_OK:
    case SONARE_ERROR_UNKNOWN:
    default:
      return sonare::ErrorCode::InvalidState;
  }
}

[[noreturn]] inline void throwCError(SonareError err, const char* context) {
  const char* detail = sonare_last_error_message();
  const char* fallback = sonare_error_message(err);
  std::string message = context != nullptr ? context : "C API call failed";
  message += ": ";
  message += detail != nullptr && detail[0] != '\0' ? detail : fallback;
  throw sonare::SonareException(codeFromCError(err), message);
}

struct ProjectWasm {
  ProjectWasm();

  // Adopts an already-created handle (used by the fromJson factory). The handle
  // must be non-null; ownership transfers to the shared_ptr.
  explicit ProjectWasm(SonareProject* adopted);

  // The handle is owned through a shared_ptr (deleter = sonare_project_destroy),
  // so the wrapper is freely copyable/movable. embind returns the fromJson
  // factory by value, which requires a copy/move-constructible holder; sharing
  // the refcounted handle keeps that safe (the C project is destroyed once, when
  // the last wrapper goes away).

  // Serializes the project (+ MIDI content) to deterministic JSON.
  std::string toJson() const;

  // Deserializes project JSON into a NEW project. Throws on malformed input,
  // surfacing the joined diagnostic messages to JS.
  static ProjectWasm fromJson(const std::string& json);

  static val fromJsonWithDiagnostics(const std::string& json);

  // Sets the project sample rate (Hz). Must be > 0.
  void setSampleRate(double sample_rate);

  uint32_t addTrack(val desc);
  uint32_t addClip(val desc);
  val addLoopRecordingTakes(val desc);
  val addMidiClip(double start_ppq, double length_ppq);
  uint32_t splitClip(uint32_t clip_id, double split_ppq);
  void trimClip(uint32_t clip_id, double start_ppq, double length_ppq);
  void moveClip(uint32_t clip_id, double start_ppq, uint32_t track_id);
  void setTrackKind(uint32_t track_id, uint32_t kind);
  void setClipWarpRef(uint32_t clip_id, uint32_t warp_ref_id);
  void setClipWarpMode(uint32_t clip_id, val mode_val);
  void setWarpMap(val desc);
  void removeWarpMap(uint32_t warp_ref_id);
  void setTrackMidiDestination(uint32_t track_id, uint32_t destination_id);
  void setTrackGain(uint32_t track_id, float gain);
  void setTrackMute(uint32_t track_id, bool mute);
  void setTrackSolo(uint32_t track_id, bool solo);
  void setTrackPan(uint32_t track_id, float pan);
  void undo();
  void redo();

  void setMidiEvents(uint32_t clip_id, val events);
  uint32_t importSmf(val data);
  val exportSmf();
  uint32_t importClipFile(val data);
  val exportClipFile();
  void setProgram(uint32_t clip_id, int program, int bank);
  void setProgramOnChannel(uint32_t clip_id, uint32_t group, uint32_t channel, int program,
                           int bank);
  void bakeMidiFx(uint32_t clip_id, const std::string& config_json);
  void setMidiFx(uint32_t clip_id, const std::string& config_json);

  // Pre-flight check for hanging / unmatched notes in a MIDI clip. Returns
  // { ok, unmatchedNoteOns, unmatchedNoteOffs }; throws if the clip id is
  // unknown or not a MIDI clip.
  val validateMidiNotes(uint32_t clip_id);
  float autoTempo(val audio, int sample_rate);
  double snapToGrid(double ppq, double strength);

  // Compiles the project into a renderable timeline, returning a small JS
  // object { diagnosticCount, hasTimeline, messages }.
  val compile();

  // Parses a JS bounce-options object into the flat C POD. A missing /
  // null/undefined object leaves the zero-init defaults (which the C ABI maps to
  // project sample rate, 2 channels, block 128, and auto-derived length).
  static SonareProjectBounceOptions bounceOptionsFromVal(val options);

  // Maps a waveform name to its SonareSynthWaveform ordinal, or -1 if unknown.
  // Mirrors the Node/Python accepted set: "sine", "saw"/"sawtooth", "square",
  // "triangle".
  static int waveformFromName(const std::string& name);

  // Reads a single { destinationId?, waveform?, gain?, attack?, decay?,
  // sustain?, release?, polyphony? } object into a built-in synth binding. Every
  // numeric synth field is "0 => default" per the C ABI, so unset fields stay 0.
  // `waveform` accepts the ordinal or a string ("sine"/"saw"/"sawtooth"/
  // "square"/"triangle"); an unknown name throws, matching Node/Python.
  static SonareBuiltinInstrumentBinding builtinBindingFromVal(val desc);

  // Normalizes the bounceWithBuiltinInstrument `instrument(s)` argument into a
  // vector of bindings. Accepts an array of binding objects (an explicitly empty
  // array yields ZERO bindings -> silent bounce, matching Node/Python and the
  // documented contract), a single binding object (treated as one element), or
  // null/undefined (also zero bindings -> silent). The TS wrapper's default
  // argument of `{}` supplies the single default-sine patch for the no-arg call.
  static std::vector<SonareBuiltinInstrumentBinding> builtinBindingsFromVal(val bindings);

  // Compiles + renders the project offline to interleaved float audio (silent
  // for MIDI tracks, which have no instrument bound). Uses C-ABI defaults
  // (project sample rate, 2 channels, block 128) when no options are provided.
  // When totalFrames is omitted (or <= 0) the C ABI auto-derives the render
  // length from the arrangement, so an arrangement with content renders without
  // the caller computing a frame count; an empty project yields an empty buffer.
  // To make MIDI tracks audible use bounceWithBuiltinInstrument.
  val bounce(val options);

  // Compiles + renders the project, routing MIDI tracks through the built-in
  // oscillator synth so a MIDI-only arrangement bounces to audible audio.
  // @p bindings is an array of { destinationId?, ...synthConfig } objects (a
  // single object is accepted too); an explicitly empty array (or null /
  // undefined) produces zero bindings, so MIDI tracks render silently. Every
  // synth-config
  // field is optional and "0 => sensible default" per the C ABI. When
  // options.totalFrames is omitted the render length is auto-derived from the
  // arrangement plus the synth release tail.
  val bounceWithBuiltinInstrument(val bindings, val options);

  // Compiles + renders the project, routing MIDI tracks through the
  // patch-driven NativeSynth (the full synthesizer; see the SynthPatch TS
  // type). @p bindings is a SynthPatch object, a preset-name string
  // ("saw-lead" / "va:saw-lead"), or an array of either (each entry may carry
  // a destinationId). An empty array / null / undefined produces zero
  // bindings. Unknown preset names throw.
  val bounceWithSynthInstrument(val bindings, val options);

  // Loads (parses) SoundFont 2 bytes into the project (presets / sample PCM),
  // replacing any previously loaded SoundFont. The host copies the .sf2 bytes
  // into linear memory as a Uint8Array (same convention as importSmf); they are
  // not referenced after the call. Throws on malformed input (the previous
  // SoundFont is kept).
  void loadSoundFont(val data);

  // Releases the project's loaded SoundFont (no-op when none is loaded).
  void clearSoundFont();

  // Number of presets in the loaded SoundFont (0 when none is loaded).
  size_t soundFontPresetCount();

  // Enumerates every (channel, bank, program) combination the arrangement
  // plays a note through, in first-use order, as an array of { channel, bank,
  // program, backend: 'sf2'|'synth', presetName } objects (matching Node).
  val soundFontManifest();

  // Reads a single { destinationId?, gain?, polyphony? } object into an SF2
  // instrument binding ("0 / omit => default" per the C ABI).
  static SonareSf2InstrumentBinding sf2BindingFromVal(val desc);

  // Like bounceWithBuiltinInstrument, but each bound destination renders
  // through a GS-compatible SoundFont player fed by the project's loaded
  // SoundFont. Programs the SoundFont does not cover — including bouncing
  // with no SoundFont loaded at all — play through the built-in synthesizer
  // GM fallback bank (the data-free floor). Accepts an array of binding
  // objects, a single object, or null/undefined (zero bindings -> silence).
  val bounceWithSf2Instrument(val bindings, val options);

  // --------------------------------------------------------------------------
  // Edit operations (undoable; route through EditHistory commands)
  // --------------------------------------------------------------------------

  void removeClip(uint32_t clip_id);
  void setClipGain(uint32_t clip_id, float gain);

  // Reads a { lengthPpq?, curve? } fade descriptor; curve accepts the ordinal or
  // a string ("linear"/"equal-power"/"equalPower"/"equal_power"/...).
  static SonareProjectClipFade clipFadeFromVal(val desc);

  void setClipFade(uint32_t clip_id, val fade_in, val fade_out);
  void setClipTakes(uint32_t clip_id, val takes_val, uint32_t active_take_id);
  void setClipCompSegments(uint32_t clip_id, val segments_val);
  void setClipLoop(uint32_t clip_id, int loop_mode, double loop_length_ppq,
                   double loop_crossfade_ppq);
  void setClipSource(uint32_t clip_id, uint32_t source_id);
  uint32_t duplicateClip(uint32_t clip_id, double new_start_ppq);
  void removeTrack(uint32_t track_id);
  void renameTrack(uint32_t track_id, const std::string& name);
  void setTrackRoute(uint32_t track_id, const std::string& channel_strip_ref,
                     const std::string& output_target);

  // Reads a JS array of { ppq, value, curve? } automation breakpoints into the
  // shared SonareAutomationPoint POD. `curve` accepts the ordinal or a string
  // ("linear"/"exponential"/"hold"/"scurve"). The points are stored verbatim.
  static std::vector<SonareAutomationPoint> automationPointsFromVal(val points);

  static SonareAutomationLaneDesc automationLaneDescFromVal(
      val desc, std::vector<SonareAutomationPoint>* storage);

  double addAutomationLane(uint32_t track_id, val desc);
  void editAutomationLane(uint32_t track_id, double lane_index, val desc);
  void removeAutomationLane(uint32_t track_id, double lane_index);

  // --------------------------------------------------------------------------
  // MIR annotation streams (undoable)
  // --------------------------------------------------------------------------

  void annotateKeys(val keys);
  void annotateChords(val chords);

  // --------------------------------------------------------------------------
  // Assist sidecars (opaque module state)
  // --------------------------------------------------------------------------

  void setAssistSidecar(const std::string& module_id, uint32_t schema_version,
                        uint32_t target_track_id, double region_start_ppq, double region_end_ppq,
                        val payload);
  double assistSidecarCount() const;
  val getAssistSidecar(double index) const;

  // --------------------------------------------------------------------------
  // Project-level configuration, counts, and timeline metadata
  // --------------------------------------------------------------------------

  void setOverlapPolicy(uint32_t policy);
  uint32_t getOverlapPolicy() const;
  double getSampleRate() const;
  void setMixerSceneJson(const std::string& scene_json);
  uint32_t setMarker(uint32_t marker_id, double ppq, const std::string& name);
  uint32_t setMarkerEx(val marker);
  val markerByIndex(int index) const;
  double markerCount() const;
  double trackCount() const;
  double clipCount() const;
  double sourceCount() const;
  double tempoSegmentCount() const;
  double timeSignatureCount() const;

  // Replaces the project's tempo map from an array of { startPpq, bpm,
  // startSample?, endBpm? } segments. startSample is accepted for ABI/source
  // compatibility but ignored; sample positions are derived during normalization.
  void setTempoSegments(val segments);

  // Replaces the project's time-signature map from an array of { startPpq,
  // numerator, denominator } segments.
  void setTimeSignatures(val segments);

  // Surfaces the compile diagnostics produced by the most recent bounce on this
  // project (e.g. MIDI clips rendering silently without a bound instrument).
  // Same shape as compile().
  val lastBounceCompileResult() const;

  std::shared_ptr<SonareProject> project_;
};

// Standalone MIDI helper / GM-table free functions (bodies in project_midi.cpp).
uint32_t js_project_abi_version();
val js_nullable_string(const char* value);
val js_midi_gm_instrument_name(int program);
int js_midi_gm_program_for_name(const std::string& name);
val js_midi_gm_family_name(int family);
int js_midi_gm_family_first_program(int family);
val js_midi_gm2_instrument_name(int bank_lsb, int program);
val js_midi_gm_drum_name(int note);
int js_midi_gm_drum_note_for_name(const std::string& name);
val js_midi_gm2_drum_set_name(int bank_lsb);
val js_midi_gm2_drum_name(int bank_lsb, int note);
val js_midi_cc_name(int controller);
int js_midi_cc_index_for_name(const std::string& name);
val js_midi_per_note_controller_name(int index);
val js_midi_bank_program(double ppq, int group, int channel, int bank_msb, int bank_lsb,
                         int program);
SonareMidiEventPod js_midi_event_from_val(val event);
val js_midi_event_to_val(const SonareMidiEventPod& event);
SonareMidiCcBinding js_cc_binding_from_val(val object);
val js_cc_binding_to_val(const SonareMidiCcBinding& binding);
std::vector<SonareMidiCcBinding> js_cc_bindings_from_val(val bindings);
val js_midi_cc_learn(val events, uint32_t param_id, float min_value, float max_value,
                     int min_movement);
val js_midi_cc_to_breakpoint(val bindings, val event);
val js_midi_param_to_cc(val bindings, uint32_t param_id, float unit_value, int group, double ppq);
val js_midi_route_events(val events, val config);

// NativeSynth preset / enum free functions (bodies in project_bounce.cpp).
val js_synth_preset_names();
val js_synth_preset_patch(const std::string& name);
val js_synth_enum_tables();
val js_synth_patch_round_trip(val desc);

// Each domain TU registers its slice of the single Project class_ handle. The
// core TU (project.cpp) creates the handle, registers lifecycle, and calls these
// in turn from registerProjectBindings(); registerProjectFreeFunctions() emits
// the standalone function(...) registrations.
void registerProjectArrange(emscripten::class_<ProjectWasm>& cls);
void registerProjectEdit(emscripten::class_<ProjectWasm>& cls);
void registerProjectMidi(emscripten::class_<ProjectWasm>& cls);
void registerProjectBounce(emscripten::class_<ProjectWasm>& cls);
void registerProjectMeta(emscripten::class_<ProjectWasm>& cls);
void registerProjectFreeFunctions();

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
