import { resolveEnumOrdinal } from './codes';
import { getSonareModule } from './module_state';
import type {
  BuiltinSynthBinding,
  ProjectAssistSidecar,
  ProjectAutomationPoint,
  ProjectBounceOptions,
  ProjectChordSymbol,
  ProjectClip,
  ProjectClipCompSegment,
  ProjectClipDesc,
  ProjectClipFade,
  ProjectClipTake,
  ProjectCompileResult,
  ProjectKeySegment,
  ProjectLoopMode,
  ProjectLoopRecordingDesc,
  ProjectLoopRecordingResult,
  ProjectMarker,
  ProjectMidiCcBinding,
  ProjectMidiClipResult,
  ProjectMidiEvent,
  ProjectMidiRouteConfig,
  ProjectMidiRouteResult,
  ProjectNotePairValidation,
  ProjectSource,
  ProjectTempoCandidate,
  ProjectTempoSegment,
  ProjectTimeSignatureSegment,
  ProjectTrack,
  ProjectTrackKind,
  ProjectWarpMapDesc,
  ProjectWarpMode,
  Sf2InstrumentConfig,
  Sf2ProgramStatus,
  SynthEnumTables,
  SynthPatch,
} from './project_types';

// Embind handle for the C++ `ProjectWasm` class. The generated `SonareModule`
// type only gains `Project` / `projectAbiVersion` after a WASM rebuild, so the
// module is cast through this shape here.
export interface WasmProject {
  toJson: () => string;
  setSampleRate: (sampleRate: number) => void;
  addTrack: (desc: { kind?: number | string; name?: string }) => number;
  addClip: (desc: ProjectClipDesc) => number;
  importExternalStems: (request: unknown) => { trackIds: number[]; clipIds: number[] };
  addLoopRecordingTakes: (desc: ProjectLoopRecordingDesc) => ProjectLoopRecordingResult;
  addMidiClip: (startPpq: number, lengthPpq: number) => ProjectMidiClipResult;
  splitClip: (clipId: number, splitPpq: number) => number;
  trimClip: (clipId: number, newStartPpq: number, newLengthPpq: number) => void;
  moveClip: (clipId: number, newStartPpq: number, newTrackId: number) => void;
  setTrackKind: (trackId: number, kind: number) => void;
  setClipWarpRef: (clipId: number, warpRefId: number) => void;
  setClipWarpMode: (clipId: number, mode: number) => void;
  setWarpMap: (map: ProjectWarpMapDesc) => void;
  removeWarpMap: (warpRefId: number) => void;
  setTrackMidiDestination: (trackId: number, destinationId: number) => void;
  setTrackGain: (trackId: number, gain: number) => void;
  setTrackMute: (trackId: number, mute: boolean) => void;
  setTrackSolo: (trackId: number, solo: boolean) => void;
  setTrackPan: (trackId: number, pan: number) => void;
  undo: () => void;
  redo: () => void;
  clearHistory: () => void;
  setMaxUndoDepth: (depth: number) => void;
  setMidiEvents: (
    clipId: number,
    events: ReadonlyArray<ProjectMidiEvent | readonly [number, number, number]>,
  ) => void;
  importSmf: (data: Uint8Array) => number;
  exportSmf: () => Uint8Array;
  importClipFile: (data: Uint8Array) => number;
  exportClipFile: () => Uint8Array;
  setProgram: (clipId: number, program: number, bank: number) => void;
  setProgramOnChannel: (
    clipId: number,
    group: number,
    channel: number,
    program: number,
    bank: number,
  ) => void;
  bakeMidiFx: (clipId: number, configJson: string) => void;
  setMidiFx: (clipId: number, configJson: string) => void;
  validateMidiNotes: (clipId: number) => ProjectNotePairValidation;
  analyzeTempo: (audio: Float32Array, sampleRate: number) => ProjectTempoCandidate[];
  autoTempo: (
    audio: Float32Array,
    sampleRate: number,
    candidateIndex: number,
    applyTimeSignatures: boolean,
  ) => number;
  snapToGrid: (ppq: number, strength: number, division: number) => number;
  compile: () => ProjectCompileResult;
  bounce: (options: ProjectBounceOptions) => Float32Array;
  bounceWithBuiltinInstrument: (
    bindings: BuiltinSynthBinding | ReadonlyArray<BuiltinSynthBinding> | undefined,
    options: ProjectBounceOptions,
  ) => Float32Array;
  bounceWithSynthInstrument: (
    bindings: SynthPatch | string | ReadonlyArray<SynthPatch | string> | undefined,
    options: ProjectBounceOptions,
  ) => Float32Array;
  loadSoundFont: (data: Uint8Array) => void;
  clearSoundFont: () => void;
  soundFontPresetCount: () => number;
  soundFontManifest: () => Sf2ProgramStatus[];
  bounceWithSf2Instrument: (
    bindings: Sf2InstrumentConfig | ReadonlyArray<Sf2InstrumentConfig> | undefined,
    options: ProjectBounceOptions,
  ) => Float32Array;
  removeClip: (clipId: number) => void;
  setClipGain: (clipId: number, gain: number) => void;
  setClipFade: (clipId: number, fadeIn: ProjectClipFade, fadeOut: ProjectClipFade) => void;
  unresolvedAudioSourceIds: () => number[];
  setSourceAudio: (
    sourceId: number,
    audio: Float32Array,
    channels: number,
    sampleRate: number,
  ) => void;
  setClipTakes: (
    clipId: number,
    takes: ReadonlyArray<ProjectClipTake>,
    activeTakeId: number,
  ) => void;
  setClipCompSegments: (clipId: number, segments: ReadonlyArray<ProjectClipCompSegment>) => void;
  setClipLoop: (
    clipId: number,
    loopMode: number,
    loopLengthPpq: number,
    loopCrossfadePpq: number,
  ) => void;
  setClipSource: (clipId: number, sourceId: number) => void;
  duplicateClip: (clipId: number, newStartPpq: number) => number;
  removeTrack: (trackId: number) => void;
  renameTrack: (trackId: number, name: string) => void;
  setTrackRoute: (trackId: number, channelStripRef: string, outputTarget: string) => void;
  addAutomationLane: (
    trackId: number,
    desc: { targetParamId: number; points: ReadonlyArray<ProjectAutomationPoint> },
  ) => number;
  editAutomationLane: (
    trackId: number,
    targetParamId: number,
    desc: { targetParamId: number; points: ReadonlyArray<ProjectAutomationPoint> },
  ) => void;
  removeAutomationLane: (trackId: number, targetParamId: number) => void;
  annotateKeys: (keys: ReadonlyArray<ProjectKeySegment>) => void;
  annotateChords: (chords: ReadonlyArray<ProjectChordSymbol>) => void;
  setAssistSidecar: (
    moduleId: string,
    schemaVersion: number,
    targetTrackId: number,
    regionStartPpq: number,
    regionEndPpq: number,
    payload: Uint8Array,
  ) => void;
  assistSidecarCount: () => number;
  getAssistSidecar: (index: number) => ProjectAssistSidecar;
  setOverlapPolicy: (policy: number) => void;
  getOverlapPolicy: () => number;
  getSampleRate: () => number;
  setMixerSceneJson: (sceneJson: string) => void;
  setMarker: (markerId: number, ppq: number, name: string) => number;
  setMarkerEx: (marker: ProjectMarker) => number;
  markerByIndex: (index: number) => ProjectMarker;
  trackByIndex: (index: number) => ProjectTrack;
  clipByIndex: (index: number) => ProjectClip;
  sourceByIndex: (index: number) => ProjectSource;
  markerCount: () => number;
  trackCount: () => number;
  clipCount: () => number;
  sourceCount: () => number;
  tempoSegmentCount: () => number;
  timeSignatureCount: () => number;
  setTempoSegments: (segments: ReadonlyArray<ProjectTempoSegment>) => void;
  setTimeSignatures: (segments: ReadonlyArray<ProjectTimeSignatureSegment>) => void;
  lastBounceCompileResult: () => ProjectCompileResult;
  delete: () => void;
}

export interface ProjectModule {
  Project: {
    new (): WasmProject;
    fromJson: (json: string) => WasmProject;
    fromJsonWithDiagnostics: (json: string) => { project: WasmProject; diagnostics: string };
  };
  projectAbiVersion: () => number;
  synthPresetNames: () => string[];
  synthPresetPatch: (name: string) => SynthPatch;
  _synthEnumTables: () => SynthEnumTables;
  _synthPatchRoundTrip: (patch: SynthPatch) => SynthPatch;
  midiGmInstrumentName: (program: number) => string | null;
  midiGmProgramForName: (name: string) => number;
  midiGmFamilyName: (family: number) => string | null;
  midiGmFamilyFirstProgram: (family: number) => number;
  midiGm2InstrumentName: (bankLsb: number, program: number) => string | null;
  midiGmDrumName: (note: number) => string | null;
  midiGmDrumNoteForName: (name: string) => number;
  midiGm2DrumSetName: (bankLsb: number) => string | null;
  midiGm2DrumName: (bankLsb: number, note: number) => string | null;
  midiCcName: (controller: number) => string | null;
  midiCcIndexForName: (name: string) => number;
  midiPerNoteControllerName: (index: number) => string | null;
  midiBankProgram: (
    ppq: number,
    group: number,
    channel: number,
    bankMsb: number,
    bankLsb: number,
    program: number,
  ) => ProjectMidiEvent[];
  midiRouteEvents: (
    events: ReadonlyArray<ProjectMidiEvent>,
    config: ProjectMidiRouteConfig,
  ) => ProjectMidiRouteResult;
  midiCcLearn: (
    events: ReadonlyArray<ProjectMidiEvent>,
    paramId: number,
    minValue: number,
    maxValue: number,
    minMovement: number,
  ) => ProjectMidiCcBinding | null;
  midiCcToBreakpoint: (
    bindings: ReadonlyArray<ProjectMidiCcBinding>,
    event: ProjectMidiEvent,
  ) => ProjectAutomationPoint | null;
  midiParamToCc: (
    bindings: ReadonlyArray<ProjectMidiCcBinding>,
    paramId: number,
    unitValue: number,
    group: number,
    ppq: number,
  ) => ProjectMidiEvent | null;
}

export function projectModule(): ProjectModule {
  const candidate = getSonareModule() as unknown as Partial<ProjectModule>;
  if (typeof candidate.projectAbiVersion !== 'function' || candidate.Project === undefined) {
    throw new Error('libsonare was built without arrangement (headless DAW) support');
  }
  return candidate as ProjectModule;
}

export function assertProjectU7(fnName: string, value: number, argName: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 127) {
    throw new RangeError(`${fnName}: ${argName} must be an integer in [0, 127]`);
  }
  return value;
}

export function assertProjectNibble(fnName: string, value: number, argName: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 15) {
    throw new RangeError(`${fnName}: ${argName} must be an integer in [0, 15]`);
  }
  return value;
}

export function projectMidi1Event(
  fnName: string,
  ppq: number,
  group: number,
  status: number,
  channel: number,
  data1: number,
  data2 = 0,
): ProjectMidiEvent {
  if (!Number.isFinite(ppq) || ppq < 0) {
    throw new RangeError(`${fnName}: ppq must be a non-negative finite number`);
  }
  const g = assertProjectNibble(fnName, group, 'group');
  const ch = assertProjectNibble(fnName, channel, 'channel');
  const d1 = assertProjectU7(fnName, data1, 'data1');
  const d2 = assertProjectU7(fnName, data2, 'data2');
  // UMP MIDI-1.0 channel-voice word (message type 0x2). Canonical layout is
  // sonare::midi::make_midi1_* (C-ABI sonare_midi_*, which Python delegates to);
  // this hand-written copy is locked against those words by the golden vectors
  // in project.test.ts (mirrored in the Node suite) so it cannot silently drift.
  const word = ((0x2 << 28) | (g << 24) | (status << 20) | (ch << 16) | (d1 << 8) | d2) >>> 0;
  return { ppq, data0: word, data1: 0 };
}

export function assertProjectU32(fnName: string, value: number, argName: string): void {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) {
    throw new RangeError(`${fnName}: ${argName} must be an integer in [0, 4294967295]`);
  }
}

export function assertProjectMidiEvents(
  fnName: string,
  events: ReadonlyArray<ProjectMidiEvent | readonly [number, number, number]>,
): void {
  if (!Array.isArray(events)) {
    throw new TypeError(`${fnName}: events must be an array`);
  }
  events.forEach((event, index) => {
    const prefix = `events[${index}]`;
    if (Array.isArray(event)) {
      if (event.length < 3) {
        throw new TypeError(`${fnName}: ${prefix} must contain [ppq, data0, data1]`);
      }
      if (!Number.isFinite(event[0]) || event[0] < 0) {
        throw new RangeError(`${fnName}: ${prefix}.ppq must be a non-negative finite number`);
      }
      assertProjectU32(fnName, event[1], `${prefix}.data0`);
      assertProjectU32(fnName, event[2], `${prefix}.data1`);
      return;
    }
    if (event === null || typeof event !== 'object') {
      throw new TypeError(`${fnName}: ${prefix} must be a MIDI event object or tuple`);
    }
    if (!Number.isFinite(event.ppq) || event.ppq < 0) {
      throw new RangeError(`${fnName}: ${prefix}.ppq must be a non-negative finite number`);
    }
    assertProjectU32(fnName, event.data0, `${prefix}.data0`);
    if (event.data1 !== undefined) {
      assertProjectU32(fnName, event.data1, `${prefix}.data1`);
    }
  });
}

export function projectTrackKindValue(kind: ProjectTrackKind | undefined): number {
  return resolveEnumOrdinal(kind ?? 'audio', { audio: 0, midi: 1, aux: 2 }, 'project track kind');
}

export function projectWarpModeValue(mode: ProjectWarpMode | undefined): number {
  return resolveEnumOrdinal(
    mode ?? 'off',
    { off: 0, repitch: 1, 'tempo-sync': 2 },
    'project warp mode',
  );
}

export function projectLoopModeValue(mode: ProjectLoopMode | undefined): number {
  return resolveEnumOrdinal(mode ?? 'off', { off: 0, loop: 1 }, 'project loop mode');
}
