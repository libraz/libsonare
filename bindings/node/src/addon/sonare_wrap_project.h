#ifndef SONARE_NODE_SONARE_WRAP_PROJECT_H_
#define SONARE_NODE_SONARE_WRAP_PROJECT_H_

#include <napi.h>

#include "sonare_c.h"

/// @brief N-API ObjectWrap over the opaque headless-project handle
///        (@ref SonareProject). Mirrors the @ref RealtimeEngineWrap pattern:
///        the constructor checks the project ABI version (1) and creates the
///        native handle; the destructor / @ref Destroy frees it. Every method
///        routes through the curated @c sonare_project_* C ABI
///        (src/sonare_c_project.h) and frees any heap buffers it returns.
class ProjectWrap : public Napi::ObjectWrap<ProjectWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  explicit ProjectWrap(const Napi::CallbackInfo& info);
  ~ProjectWrap();

 private:
  // -- serialization --
  Napi::Value ToJson(const Napi::CallbackInfo& info);
  static Napi::Value FromJson(const Napi::CallbackInfo& info);
  static Napi::Value FromJsonWithDiagnostics(const Napi::CallbackInfo& info);
  Napi::Value SetSampleRate(const Napi::CallbackInfo& info);
  Napi::Value GetSampleRate(const Napi::CallbackInfo& info);
  Napi::Value SetOverlapPolicy(const Napi::CallbackInfo& info);
  Napi::Value GetOverlapPolicy(const Napi::CallbackInfo& info);
  Napi::Value SetMixerSceneJson(const Napi::CallbackInfo& info);
  Napi::Value SetMarker(const Napi::CallbackInfo& info);
  Napi::Value SetTempoSegments(const Napi::CallbackInfo& info);
  Napi::Value SetTimeSignatures(const Napi::CallbackInfo& info);

  // -- value-model counts --
  Napi::Value TrackCount(const Napi::CallbackInfo& info);
  Napi::Value SourceCount(const Napi::CallbackInfo& info);
  Napi::Value TempoSegmentCount(const Napi::CallbackInfo& info);
  Napi::Value TimeSignatureCount(const Napi::CallbackInfo& info);

  // -- edit --
  Napi::Value AddTrack(const Napi::CallbackInfo& info);
  Napi::Value AddClip(const Napi::CallbackInfo& info);
  Napi::Value AddLoopRecordingTakes(const Napi::CallbackInfo& info);
  Napi::Value AddMidiClip(const Napi::CallbackInfo& info);
  Napi::Value SplitClip(const Napi::CallbackInfo& info);
  Napi::Value TrimClip(const Napi::CallbackInfo& info);
  Napi::Value MoveClip(const Napi::CallbackInfo& info);
  Napi::Value SetTrackKind(const Napi::CallbackInfo& info);
  Napi::Value SetClipWarpRef(const Napi::CallbackInfo& info);
  Napi::Value SetClipWarpMode(const Napi::CallbackInfo& info);
  Napi::Value SetWarpMap(const Napi::CallbackInfo& info);
  Napi::Value RemoveWarpMap(const Napi::CallbackInfo& info);
  Napi::Value SetTrackMidiDestination(const Napi::CallbackInfo& info);
  Napi::Value RemoveClip(const Napi::CallbackInfo& info);
  Napi::Value SetClipGain(const Napi::CallbackInfo& info);
  Napi::Value SetClipFade(const Napi::CallbackInfo& info);
  Napi::Value SetClipTakes(const Napi::CallbackInfo& info);
  Napi::Value SetClipCompSegments(const Napi::CallbackInfo& info);
  Napi::Value SetClipLoop(const Napi::CallbackInfo& info);
  Napi::Value SetClipSource(const Napi::CallbackInfo& info);
  Napi::Value DuplicateClip(const Napi::CallbackInfo& info);
  Napi::Value RemoveTrack(const Napi::CallbackInfo& info);
  Napi::Value RenameTrack(const Napi::CallbackInfo& info);
  Napi::Value SetTrackRoute(const Napi::CallbackInfo& info);
  Napi::Value AddAutomationLane(const Napi::CallbackInfo& info);
  Napi::Value EditAutomationLane(const Napi::CallbackInfo& info);
  Napi::Value RemoveAutomationLane(const Napi::CallbackInfo& info);
  Napi::Value Undo(const Napi::CallbackInfo& info);
  Napi::Value Redo(const Napi::CallbackInfo& info);

  // -- MIDI --
  Napi::Value SetMidiEvents(const Napi::CallbackInfo& info);
  Napi::Value ImportSmf(const Napi::CallbackInfo& info);
  Napi::Value ExportSmf(const Napi::CallbackInfo& info);
  Napi::Value ImportClipFile(const Napi::CallbackInfo& info);
  Napi::Value ExportClipFile(const Napi::CallbackInfo& info);
  Napi::Value SetProgram(const Napi::CallbackInfo& info);
  Napi::Value SetProgramOnChannel(const Napi::CallbackInfo& info);
  Napi::Value BakeMidiFx(const Napi::CallbackInfo& info);
  Napi::Value SetMidiFx(const Napi::CallbackInfo& info);
  Napi::Value ValidateMidiNotes(const Napi::CallbackInfo& info);

  // -- MIR --
  Napi::Value AutoTempo(const Napi::CallbackInfo& info);
  Napi::Value SnapToGrid(const Napi::CallbackInfo& info);
  Napi::Value AnnotateKeys(const Napi::CallbackInfo& info);
  Napi::Value AnnotateChords(const Napi::CallbackInfo& info);

  // -- assist sidecars --
  Napi::Value SetAssistSidecar(const Napi::CallbackInfo& info);
  Napi::Value AssistSidecarCount(const Napi::CallbackInfo& info);
  Napi::Value GetAssistSidecar(const Napi::CallbackInfo& info);
  Napi::Value AssistSidecars(const Napi::CallbackInfo& info);

  // -- compile / render --
  Napi::Value Compile(const Napi::CallbackInfo& info);
  Napi::Value LastBounceCompileResult(const Napi::CallbackInfo& info);
  Napi::Value Bounce(const Napi::CallbackInfo& info);
  Napi::Value BounceWithBuiltinInstruments(const Napi::CallbackInfo& info);
  Napi::Value BounceWithSynthInstruments(const Napi::CallbackInfo& info);

  // -- SoundFont (SF2) instrument --
  void LoadSoundFont(const Napi::CallbackInfo& info);
  void ClearSoundFont(const Napi::CallbackInfo& info);
  Napi::Value SoundFontPresetCount(const Napi::CallbackInfo& info);
  Napi::Value SoundFontManifest(const Napi::CallbackInfo& info);
  Napi::Value BounceWithSf2Instruments(const Napi::CallbackInfo& info);

  void Destroy(const Napi::CallbackInfo& info);

  SonareProject* project_ = nullptr;

  // Builds a ProjectWrap JS instance that adopts `handle` (used by fromJson()).
  static Napi::Object Wrap(Napi::Env env, SonareProject* handle);
  // Stored constructor reference so static factories can instantiate the class.
  static Napi::FunctionReference constructor;
};

#endif  // SONARE_NODE_SONARE_WRAP_PROJECT_H_
