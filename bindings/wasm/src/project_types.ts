import type {
  Articulation,
  BuiltinSynthConfig,
  ExternalInstrument,
  ProjectInstrumentBinding,
  Sf2InstrumentConfig,
  SynthPatch,
} from './instrument_types.js';
import type { Project, ProjectTimeline } from './project_class.js';
import type { EngineMetronomeConfig, MidiFxConfig } from './public_types_engine.js';
import type { MidiBank } from './public_types_midi_messages.js';
import type { AutomationCurve } from './public_types_mixing.js';
import type { KeyMode, TimeSignature } from './public_types_music.js';

// ============================================================================
// Headless DAW Project
// ============================================================================

/**
 * Project ABI version this wrapper was written against; an internal mirror of
 * `SONARE_PROJECT_ABI_VERSION` the ABI tooling checks, not part of the package
 * entry.
 */
export const EXPECTED_PROJECT_ABI_VERSION = 3;

/**
 * Render options for {@link Project.bounce}.
 * Every field is optional: an omitted field takes its default. Fields with a
 * documented zero domain, such as `countInBars`, accept zero.
 */
export interface ProjectBounceOptions {
  /**
   * Render length in frames at the project sample rate. Omit to derive it from
   * the arrangement (an instrument-bound bounce adds the instruments' release
   * tail); an empty project yields no frames.
   */
  totalFrames?: number;
  /** Render block size in frames. Omit for the engine default (128). */
  blockSize?: number;
  /**
   * Output channel count: 1, 2, 6 or 8, and at most the width of the scene
   * master's layout (mono/stereo/no master allow 2). `1` folds a 2-channel
   * master to 0.5(L+R). Omit it for the master's own width (2 with no master);
   * the result's `channels.length` is the width used.
   */
  numChannels?: number;
  /** Host-instrument PDC (latency) fed to the compiler, in samples. */
  instrumentLatencySamples?: number;
  /** Click settings. Clicks require `enabled: true`; disabled settings render no click. */
  metronome?: EngineMetronomeConfig;
  /** Bars of pre-roll at the initial tempo and meter, from 0 through 16. */
  countInBars?: number;
  /** Controller profiles applied to hosted MIDI destinations for this render. */
  controllerProfiles?: ReadonlyArray<ProjectControllerProfileBinding>;
  /** Per-channel note articulation applied to hosted MIDI destinations for this render. */
  articulations?: ReadonlyArray<ProjectArticulationBinding>;
  /** Built-in oscillator synth bound to MIDI destinations. */
  builtinInstruments?: ReadonlyArray<ProjectInstrumentBinding<BuiltinSynthConfig>>;
  /** Patch-driven NativeSynth bound to MIDI destinations. */
  synthInstruments?: ReadonlyArray<ProjectInstrumentBinding<SynthPatch>>;
  /** GS-compatible SoundFont players bound to MIDI destinations. */
  sf2Instruments?: ReadonlyArray<ProjectSf2InstrumentBinding>;
  /** JavaScript instruments driven synchronously during the render. */
  instruments?: ReadonlyArray<ProjectInstrumentBinding<ExternalInstrument>>;
}

/** Selects a project SoundFont. Omit the URI to select the default SoundFont. */
export interface ProjectSoundFontOptions {
  /** URI of the SoundFont. Omit for the default SoundFont. */
  uri?: string;
}

/** A SoundFont player for a project bounce, with an optional SoundFont URI. */
export interface ProjectSf2InstrumentBinding extends ProjectInstrumentBinding<Sf2InstrumentConfig> {
  /** URI of the loaded SoundFont to use. Omit for the default SoundFont. */
  soundFontUri?: string;
}

/** A controller profile selected for one destination during a project bounce. */
export interface ProjectControllerProfileBinding {
  /** ID of a hosted instrument destination. */
  destinationId: number;
  /** Name returned by {@link controllerProfileNames}. */
  presetName: string;
}

interface ProjectSynthInstrument {
  /** NativeSynth instrument variant. */
  kind: 'synth';
  /** Sparse NativeSynth patch overrides; omitted fields keep the selected preset or engine base. */
  patch: SynthPatch;
  /** Follow incoming GM bank/program events instead of the fixed patch. */
  useGmPrograms?: boolean;
}

interface ProjectBuiltinInstrument {
  /** Built-in oscillator synth variant. */
  kind: 'builtin';
  /** Built-in synth settings. */
  config: BuiltinSynthConfig;
}

interface ProjectSf2Instrument {
  /** SoundFont player variant. */
  kind: 'sf2';
  /** SoundFont player settings. */
  config: Sf2InstrumentConfig;
  /** URI of the SoundFont bytes this binding uses; omitted selects the default font. */
  soundFontUri?: string;
}

/** A NativeSynth patch, built-in oscillator, or SoundFont player bound to a project destination. */
export type ProjectInstrument =
  | ProjectSynthInstrument
  | ProjectBuiltinInstrument
  | ProjectSf2Instrument;

/** Bank and program overrides for {@link Project.setDestinationInstrument}. */
export interface ProjectDestinationInstrumentOptions {
  /** Bank Select MSB and LSB; omit both to use each clip's bank. */
  bank?: MidiBank;
  /** Program number in [0, 127]; omit it to use each clip's program. */
  program?: number;
}

/** A stored instrument binding. Unlike a bounce input, its destination id is always present. */
export interface ProjectDestinationInstrumentBinding<T> extends ProjectInstrumentBinding<T> {
  /** MIDI destination id; always present in the stored-project reader. */
  destinationId: number;
  /** Stored Bank Select; absent when the clips supply their own bank. */
  bank?: MidiBank;
  /** Stored program number; absent when the clips supply their own program. */
  program?: number;
}

/** A note articulation selected for one destination and MIDI channel during a project bounce. */
export interface ProjectArticulationBinding {
  /** ID of a hosted instrument destination. */
  destinationId: number;
  /** MIDI channel in [0, 15]. */
  channel: number;
  /** Mode for overlapping notes on this channel. */
  articulation: Articulation;
}

/** Planar result of {@link Project.bounce}. */
export interface ProjectBounceResult {
  /** One `Float32Array` per output channel, all the same length. */
  readonly channels: Float32Array[];
  /** The project sample rate in Hz; a bounce is never resampled. */
  readonly sampleRate: number;
}

/** Stem layout of one {@link ExternalSeparatedStem}. */
export type ProjectStemLayout = 'mono' | 'stereo';

/** One decoded output from a host-owned source-separation model. */
export interface ExternalSeparatedStem {
  /** Stem name, used as the created track's name. */
  name: string;
  /** Optional host metadata; it does not change DSP. */
  role?: string;
  /** Channel layout; `channels` must hold one plane for `'mono'` and two for `'stereo'`. */
  layout: ProjectStemLayout;
  /** One `Float32Array` per channel, all the same length. */
  channels: Float32Array[];
  /** Absolute project-rate frame at which this stem begins. */
  startFrame?: number;
}

/** Request for {@link Project.importExternalStems}. */
export interface ExternalSeparatedStemImportRequest {
  /** Sample rate of every stem in Hz. */
  sampleRate: number;
  /** The stems to import, one track and clip each. */
  stems: ExternalSeparatedStem[];
}

/** Ids of the normal tracks and clips created by the import. */
export interface ExternalSeparatedStemImportResult {
  /** Ids of the tracks created, one per stem. */
  trackIds: number[];
  /** Ids of the clips created, one per stem. */
  clipIds: number[];
}

/** Marker kinds of a {@link ProjectMarker}. */
export type ProjectMarkerKind = 'marker' | 'text' | 'lyric' | 'cuePoint' | 'keySignature';

/** A project timeline marker as read back from {@link Project.markers}. */
export interface ProjectMarker {
  /** Stable marker id. */
  readonly id: number;
  /** Marker position in PPQ (quarter notes). */
  readonly ppq: number;
  /** Marker label; absent when unnamed. */
  readonly name?: string;
  /** Marker kind. */
  readonly kind: ProjectMarkerKind;
  /** Key-signature field: -7..7 (sharps positive); other kinds require `0`. */
  readonly keyFifths: number;
  /** Key-signature field: `false` = major, `true` = minor; other kinds require `false`. */
  readonly keyMinor: boolean;
}

/** Input of {@link Project.setMarker}. */
export interface ProjectMarkerInput {
  /** Id of the marker to replace. Omit to allocate a new marker; an explicit `0` is refused. */
  id?: number;
  /** Marker position in PPQ, in [0, 1e12]. */
  ppq: number;
  /** Marker label. Omit it for an unnamed marker; an empty string is refused. */
  name?: string;
  /** Marker kind. Defaults to `'marker'`. */
  kind?: ProjectMarkerKind;
  /** Key-signature field: -7..7 (sharps positive); other kinds require `0`. Defaults to `0`. */
  keyFifths?: number;
  /** Key-signature field: `false` = major (default), `true` = minor; other kinds require `false`. */
  keyMinor?: boolean;
}

/** Read-only stored project track returned by {@link Project.tracks}. */
export interface ProjectTrack {
  /** Stable track id. */
  readonly id: number;
  /** Track kind. */
  readonly kind: ProjectTrackKind;
  /** MIDI destination id the track's clips route to. */
  readonly midiDestinationId: number;
  /** Linear playback gain. */
  readonly gain: number;
  /** Stereo balance in [-1, 1]. */
  readonly pan: number;
  /** Whether the track is muted. */
  readonly mute: boolean;
  /** Whether the track is soloed. */
  readonly solo: boolean;
  /** Track name. */
  readonly name: string;
  /** Mixer strip address, when the track is routed to one. */
  readonly channelStripRef?: string;
  /** Output target token, when the track has one. */
  readonly outputTarget?: string;
}

/** Read-only stored project clip returned by {@link Project.clips}. */
export interface ProjectClip {
  /** Stable clip id. */
  readonly id: number;
  /** Id of the track holding the clip. */
  readonly trackId: number;
  /** Id of the source the clip plays. */
  readonly sourceId: number;
  /** Kind of the clip's source. */
  readonly sourceKind: ProjectSourceKind;
  /** Clip start in PPQ. */
  readonly startPpq: number;
  /** Clip length in PPQ. */
  readonly lengthPpq: number;
  /** Offset into the source in PPQ. */
  readonly sourceOffsetPpq: number;
  /** Linear playback gain. */
  readonly gain: number;
  /** Loop mode. */
  readonly loopMode: ProjectLoopMode;
  /** Loop length in PPQ; absent for a whole-clip loop or a clip that does not loop. */
  readonly loopLengthPpq?: number;
}

/** Kind of a project source. */
export type ProjectSourceKind = 'audio' | 'midi';

/** Read-only stored project source returned by {@link Project.sources}. */
export interface ProjectSource {
  /** Stable source id. */
  readonly id: number;
  /** Source kind. */
  readonly kind: ProjectSourceKind;
  /** Channel count of the source audio (`0` for an unresolved or MIDI source). */
  readonly numChannels: number;
  /** Id of the storage handle holding the PCM. */
  readonly storageHandleId: number;
  /** Sample rate hint in Hz. */
  readonly sampleRateHint: number;
  /** Source name or URI. */
  readonly nameOrUri: string;
  /** Owning content hash of an audio source; absent when none is set. */
  readonly contentHash?: string;
  /** External source-separation role of an audio source; absent when none is set. */
  readonly externalStemRole?: string;
}

/** Clip-overlap policy of a project. */
export type ProjectOverlapPolicy = 'disallow' | 'allow';

/** Clip fade curve for {@link Project.setClipFade}. */
export type ProjectFadeCurve = 'linear' | 'equalPower' | 'exponential' | 'logarithmic';

/** Input of {@link Project.setClipFade}: both fades in one flat object. Omitted fields keep no fade / `'linear'`. */
export interface ProjectClipFadeInput {
  /** Fade-in length in PPQ (in [0, 1e12]; omit or 0 for no fade). */
  fadeInPpq?: number;
  /** Fade-in curve. Defaults to `'linear'`. */
  fadeInCurve?: ProjectFadeCurve;
  /** Fade-out length in PPQ (in [0, 1e12]; omit or 0 for no fade). */
  fadeOutPpq?: number;
  /** Fade-out curve. Defaults to `'linear'`. */
  fadeOutCurve?: ProjectFadeCurve;
}

/** Options of {@link Project.moveClip}. */
export interface ProjectMoveClipOptions {
  /** Track to move the clip to. Omit to keep its track; an explicit `0` is refused. */
  trackId?: number;
}

/** Options of {@link Project.setClipTakes}. */
export interface ProjectClipTakesOptions {
  /** Take that plays. Omit for the clip's base source; an explicit `0` is refused. */
  activeTakeId?: number;
}

/** Input of {@link Project.setClipLoop}. */
export interface ProjectClipLoopInput {
  /** Loop mode. */
  loopMode: ProjectLoopMode;
  /**
   * Loop length in PPQ, in (0, 1e12]. Omit it to loop the entire clip (the
   * effective length is resolved from the clip's own duration); an explicit `0`
   * is refused.
   */
  loopLengthPpq?: number;
  /**
   * Equal-power crossfade at the loop seam in PPQ, in [0, 1e12]. Omit for a
   * hard loop; the engine clamps it to the clip's pre-roll and half the loop.
   */
  loopCrossfadePpq?: number;
}

/** Options of {@link Project.setProgram}. */
export interface ProjectProgramOptions {
  /** Bank Select to emit with the program change; omitted emits none. */
  bank?: MidiBank;
  /** UMP group, `0`..`15`; defaults to 0. */
  group?: number;
  /** MIDI channel, `0`..`15`; defaults to 0. */
  channel?: number;
}

/** Input of {@link Project.setSourceAudio}. */
export interface ProjectSourceAudioInput {
  /** Id of the existing audio source. */
  sourceId: number;
  /** One `Float32Array` per channel, all the same length. */
  channels: Float32Array[];
  /** Sample rate of `channels` in Hz, `[8000, 384000]`. */
  sampleRate: number;
}

/** Input of {@link Project.setAudioSourceMetadata}. */
export interface ProjectAudioSourceMetadataInput {
  /** Id of the existing audio source. */
  sourceId: number;
  /** Content hash. Omit to clear it. */
  contentHash?: string;
  /** External source-separation role. Omit to clear it. */
  externalStemRole?: string;
}

/** One alternate take for {@link Project.setClipTakes}. */
export interface ProjectClipTake {
  /** Take id unique within the clip. Required; `0` is refused. */
  id: number;
  /** Source id for this take. Omit it to reuse the clip's current source; `0` is refused. */
  sourceId?: number;
  /** Offset into the take source in PPQ, in [0, 1e12]. */
  sourceOffsetPpq?: number;
  /** Optional UI/display name. Omit it for an unnamed take; an empty string is refused. */
  name?: string;
}

/** One comp segment for {@link Project.setClipCompSegments}. */
export interface ProjectClipCompSegment {
  /** Segment start in PPQ, in [0, 1e12]. */
  startPpq: number;
  /** Segment end in PPQ, in (0, 1e12]. */
  endPpq: number;
  /** Take id to play in this range. Omit it to fall back to the clip's active take; `0` is refused. */
  takeId?: number;
  /** Equal-power crossfade from the preceding segment, in PPQ, in [0, 1e12] (default 0 = butt join). */
  crossfadePpq?: number;
}

/**
 * Descriptor for {@link Project.addLoopRecordingTakes}.
 *
 * Each loop-length span of the capture becomes a take and the last take is made
 * active, except that a partial last take (the capture does not fill the last
 * loop) is kept but not made active when an earlier complete take exists. A
 * capture shorter than one loop has only the partial take, which stays active.
 * A partial take is marked `"partial": true` in its `takes[]` entry of the project
 * JSON (`toJson()`).
 */
export type ProjectLoopRecordingDesc = ProjectLoopRecordingCommon &
  (
    | {
        /** Interleaved capture. */
        audio: Float32Array;
        /** Channel count of the interleaved `audio`. */
        audioChannels: number;
      }
    | {
        /** Planar capture: one `Float32Array` per channel, all the same length; their count is the channel count. */
        audio: Float32Array[];
        audioChannels?: never;
      }
  );

/** Fields of {@link ProjectLoopRecordingDesc} shared by interleaved and planar audio. */
export interface ProjectLoopRecordingCommon {
  /** Track the clip is added to. */
  trackId: number;
  /** Required clip start in PPQ, in [0, 1e12]. */
  startPpq: number;
  /** Length of one loop in PPQ; also the length of the added clip, in (0, 1e12]. */
  loopLengthPpq: number;
  /** Sample rate of `audio` in Hz. */
  audioSampleRate: number;
  /** `'keep'` (default) keeps a partial last take; `'drop'` does not create it. */
  partialTail?: 'keep' | 'drop';
}

/** Result returned by {@link Project.addLoopRecordingTakes}. */
export interface ProjectLoopRecordingResult {
  /** Id of the clip added to the track. */
  clipId: number;
  /** Number of takes created. */
  takeCount: number;
}

/** Clip loop mode for {@link Project.setClipLoop}. */
export type ProjectLoopMode = 'off' | 'loop';
/**
 * How a clip follows its warp map.
 *
 * - `'off'` — no warping.
 * - `'repitch'` — resample along the map, so a rate change moves the pitch with
 *   the timing (tape-style).
 * - `'tempoSync'` — control-thread bake against the tempo map.
 * - `'timeStretch'` — realtime pitch-preserving stretch. Follows the same map
 *   as `'repitch'` but overlap-adds instead of resampling, so a new anchor set
 *   takes effect from the next block with no re-bake. Falls back to `'repitch'`
 *   behaviour when the stretcher's voice budget is exhausted.
 */
export type ProjectWarpMode = 'off' | 'repitch' | 'tempoSync' | 'timeStretch';

/**
 * Automation breakpoint interpolation for {@link ProjectAutomationPoint}: the same
 * tokens as the mixer's {@link AutomationCurve}. An ordinal is a `TypeError`.
 */
export type ProjectAutomationCurve = AutomationCurve;

/** One automation breakpoint accepted by the automation-lane edit ops. */
export interface ProjectAutomationPoint {
  /** Breakpoint position in PPQ, in [0, 1e12]. */
  ppq: number;
  /** Breakpoint value. */
  value: number;
  /** Curve to the next breakpoint (default `'linear'`). */
  curveToNext?: ProjectAutomationCurve;
}

/**
 * Persistent target classification for an automation lane. Typed lanes target
 * a track fader, pan, send, width or insert parameter; an `'opaque'` lane
 * carries the host-defined `targetParamId` it drives.
 */
export type ProjectAutomationTargetKind =
  | 'opaque'
  | 'trackFaderDb'
  | 'trackPan'
  | 'trackSendDb'
  | 'trackWidth'
  | 'trackInsertParam';

/**
 * Descriptor for {@link Project.addAutomationLane} /
 * {@link Project.editAutomationLane}.
 */
export interface ProjectAutomationLanePoints {
  /** Breakpoints. Omit to clear all breakpoints on edit. */
  points?: ReadonlyArray<ProjectAutomationPoint>;
}

/** Automation-lane descriptor for {@link Project.addAutomationLane}. */
export type ProjectAutomationLaneDesc = ProjectAutomationLanePoints &
  (
    | {
        /** Omit to add an opaque target. */
        targetKind?: 'opaque';
        /** Host-defined, nonzero parameter id for an opaque target. */
        targetParamId: number;
        /** Omitted for this target kind. */
        sendId?: never;
        /** Omitted for this target kind. */
        processor?: never;
        /** Omitted for this target kind. */
        paramName?: never;
        /** Omitted for this target kind. */
        insertIndex?: never;
      }
    | {
        /** Target kind token published by the library. */
        targetKind: 'trackFaderDb' | 'trackPan' | 'trackWidth';
        /** Omitted for this target kind. */
        targetParamId?: never;
        /** Omitted for this target kind. */
        sendId?: never;
        /** Omitted for this target kind. */
        processor?: never;
        /** Omitted for this target kind. */
        paramName?: never;
        /** Omitted for this target kind. */
        insertIndex?: never;
      }
    | {
        /** Target kind token published by the library. */
        targetKind: 'trackSendDb';
        /** Omitted for this target kind. */
        targetParamId?: never;
        /** Send id for a track send target. */
        sendId: string;
        /** Omitted for this target kind. */
        processor?: never;
        /** Omitted for this target kind. */
        paramName?: never;
        /** Omitted for this target kind. */
        insertIndex?: never;
      }
    | {
        /** Target kind token published by the library. */
        targetKind: 'trackInsertParam';
        /** Omitted for this target kind. */
        targetParamId?: never;
        /** Omitted for this target kind. */
        sendId?: never;
        /** Processor catalog id for an insert target. */
        processor: string;
        /** Processor parameter name for an insert target. */
        paramName: string;
        /** Zero-based strip insert position. */
        insertIndex: number;
      }
  );

/** Points-only edit; changing target identity requires an explicit `targetKind`. */
export type ProjectAutomationLaneEditDesc =
  | (ProjectAutomationLaneDesc & { targetKind: ProjectAutomationTargetKind })
  | (ProjectAutomationLanePoints & {
      /** Omit to keep the current target. */
      targetKind?: never;
      /** Omitted when keeping the current target. */
      targetParamId?: never;
      /** Omitted when keeping the current target. */
      sendId?: never;
      /** Omitted when keeping the current target. */
      processor?: never;
      /** Omitted when keeping the current target. */
      paramName?: never;
      /** Omitted when keeping the current target. */
      insertIndex?: never;
    });

/** Stored automation lane returned by {@link Project.automationLanes}. */
export type ProjectAutomationLane = {
  /** Id of the track that owns the lane. */
  readonly trackId: number;
  /** Stable lane id allocated by the project. */
  readonly laneId: number;
  /** Breakpoints in PPQ order. */
  readonly points: ReadonlyArray<ProjectAutomationPoint>;
} & (
  | {
      /** Target kind token published by the library. */
      readonly targetKind: 'opaque';
      /** Host-defined, nonzero parameter id for an opaque target. */
      readonly targetParamId: number;
    }
  | {
      /** Target kind token published by the library. */
      readonly targetKind: 'trackFaderDb' | 'trackPan' | 'trackWidth';
    }
  | {
      /** Target kind token published by the library. */
      readonly targetKind: 'trackSendDb';
      /** Send id for a track send target. */
      readonly sendId: string;
    }
  | {
      /** Target kind token published by the library. */
      readonly targetKind: 'trackInsertParam';
      /** Processor catalog id for an insert target. */
      readonly processor: string;
      /** Processor parameter name for an insert target. */
      readonly paramName: string;
      /** Zero-based strip insert position. */
      readonly insertIndex: number;
    }
);

/** One tempo segment for {@link Project.setTempoSegments}. */
export interface ProjectTempoSegment {
  /** Segment start in PPQ, in [0, 1e12]. */
  startPpq: number;
  /** Tempo in beats per minute at the segment start, in [1e-6, 100000]. */
  bpm: number;
  /**
   * Optional ramp end tempo in BPM, in [1e-6, 100000] when nonzero
   * (0 = constant tempo over the segment).
   */
  endBpm?: number;
}

/** One time-signature segment for {@link Project.setTimeSignatures}. */
export interface ProjectTimeSignatureSegment {
  /** Segment start in PPQ, in [0, 1e12]. */
  startPpq: number;
  /** Beats per bar (time-signature numerator). */
  numerator: number;
  /** Beat unit (time-signature denominator, e.g. 4 or 8). */
  denominator: number;
}

/**
 * Scoring options for the beat-analysis to tempo-map bridge, shared by
 * {@link Project.analyzeTempo} and {@link Project.autoTempo}.
 *
 * @remarks
 * Every field is optional and falls back to the native default. Pair the two
 * calls on the same options: `candidateIndex` indexes the ranking `analyzeTempo`
 * produced under whatever options it was given.
 */
export interface ProjectTempoOptions {
  /**
   * Whether beat tracking may follow a tempo that moves during the take
   * (default: `false`).
   *
   * @remarks
   * With it off the tracker fits one tempo to the whole take, so on a
   * performance that accelerates, slows or breathes every segment the bridge
   * emits sits near the take's average. Leave it off for material recorded to a
   * click; turn it on for a performance. Constant-tempo material still comes
   * back as a single segment either way.
   */
  adaptiveTempo?: boolean;
  /** Beats of context the local tempo estimate is read over. Used only when {@link adaptiveTempo} is on. */
  tempoUpdateIntervalBeats?: number;
  /**
   * Relative tempo change at which one segment closes and the next opens
   * (default: `0.02`). Smaller follows the performance more closely and emits
   * more segments; larger merges more of it into constant stretches.
   */
  rampThreshold?: number;
  /** Whether to rank the half- and double-tempo alternatives alongside the primary (default: `true`). */
  includeOctaveCandidates?: boolean;
}

/** Options of {@link Project.autoTempo} beyond the {@link ProjectTempoOptions} scoring fields. */
export interface ProjectAutoTempoOptions extends ProjectTempoOptions {
  /** Index into the ranking {@link Project.analyzeTempo} produced. Defaults to `0`. */
  candidateIndex?: number;
  /** Whether to also install the detected meter. Defaults to `false`. */
  applyTimeSignatures?: boolean;
}

/** Request form of {@link Project.autoTempo}. */
export interface ProjectAutoTempoRequest extends ProjectAutoTempoOptions {
  /** Mono source audio. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz, `[8000, 384000]`. */
  sampleRate: number;
}

/** Tempo and detected meter of the installed candidate. */
export interface ProjectAutoTempoResult {
  /** BPM of the first installed tempo segment. */
  readonly bpm: number;
  /** Confidence of the selected tempo candidate. */
  readonly bpmConfidence: number;
  /** Detected meter, including when it was not installed. */
  readonly timeSignature: Readonly<TimeSignature>;
}

/** Request form of {@link Project.analyzeTempo}. */
export interface ProjectAnalyzeTempoRequest extends ProjectTempoOptions {
  /** Mono source audio. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz, `[8000, 384000]`. */
  sampleRate: number;
}

/** A ranked primary/half/double tempo hypothesis returned by {@link Project.analyzeTempo}. */
export interface ProjectTempoCandidate {
  /** Tempo in beats per minute. */
  readonly bpm: number;
  /** Ranking confidence. */
  readonly confidence: number;
  /** Which hypothesis the candidate is. */
  readonly label: 'primary' | 'half' | 'double';
  /** Number of time-signature segments the detected meter has. */
  readonly timeSignatureCount: number;
  /** The detected meter's first time signature. */
  readonly firstTimeSignature: ProjectTimeSignatureSegment;
}

/** Mode of a {@link ProjectKeySegment}: a {@link KeyMode} or `'unknown'`. */
export type ProjectKeyMode = KeyMode | 'unknown';

/** Quality of a {@link ProjectChordSymbol}. */
export type ProjectChordQuality =
  | 'unknown'
  | 'major'
  | 'minor'
  | 'diminished'
  | 'augmented'
  | 'dominant'
  | 'halfDiminished'
  | 'suspended';

/** Key segment for {@link Project.annotateKeys}. */
export interface ProjectKeySegment {
  /** Segment start in PPQ, in [0, 1e12]. */
  startPpq: number;
  /** Segment end in PPQ, in [0, 1e12]. */
  endPpq: number;
  /** Tonic pitch class 0..11 (C=0). Omit when the tonic is unknown. */
  tonicPc?: number;
  /** Key mode. Default `'unknown'`. */
  mode?: ProjectKeyMode;
}

/** Chord symbol for {@link Project.annotateChords}. */
export interface ProjectChordSymbol {
  /** Chord start in PPQ, in [0, 1e12]. */
  startPpq: number;
  /** Chord end in PPQ, in [0, 1e12]. */
  endPpq: number;
  /** Root pitch class 0..11 (C=0). Omit when the root is unknown. */
  rootPc?: number;
  /** Chord quality. Default `'unknown'`. */
  quality?: ProjectChordQuality;
  /** Extensions as scale degrees, e.g. `[7, 9, 13]` (up to 32). */
  extensions?: ReadonlyArray<number>;
  /** Slash-bass pitch class 0..11. Omit when the chord has no slash bass. */
  slashBassPc?: number;
  /** Optional roman-numeral label. */
  romanNumeral?: string;
  /** True at a modulation boundary. */
  modulationBoundary?: boolean;
}

/** A labeled timeline section annotation. Sections may overlap. */
export interface ProjectSection {
  /** Section start in PPQ, in [0, 1e12]. */
  startPpq: number;
  /** Section end in PPQ, in [0, 1e12] and after `startPpq`. */
  endPpq: number;
  /** Nonblank UTF-8 section label. */
  label: string;
}

/** Descriptor accepted by {@link Project.setAssistSidecar}. */
export interface ProjectAssistSidecarInput {
  /** Non-empty module id key. */
  moduleId: string;
  /** Module-defined schema version. Defaults to `0`. */
  schemaVersion?: number;
  /** Target track id (`0` = project scope). Defaults to `0`. */
  targetTrackId?: number;
  /** Region start in PPQ, in [0, 1e12]. Defaults to `0`. */
  regionStartPpq?: number;
  /** Region end in PPQ, in [0, 1e12]. Defaults to `0`. */
  regionEndPpq?: number;
  /** Opaque module-owned payload bytes. Defaults to an empty payload. */
  payload?: Uint8Array;
}

/** Assist sidecar snapshot returned by {@link Project.assistSidecars}. */
export interface ProjectAssistSidecar {
  /** Module id key. */
  moduleId: string;
  /** Module-defined schema version. */
  schemaVersion: number;
  /** Target track id (`0` = project scope). */
  targetTrackId: number;
  /** Region start in PPQ. */
  regionStartPpq: number;
  /** Region end in PPQ. */
  regionEndPpq: number;
  /** Opaque module-owned payload bytes. */
  payload: Uint8Array;
}

/** Track kind for {@link Project.addTrack}. */
export type ProjectTrackKind = 'audio' | 'midi' | 'aux';

/** Descriptor for {@link Project.addTrack}. */
export interface ProjectTrackDesc {
  /** Track kind. Default `'audio'`. */
  kind?: ProjectTrackKind;
  /** Optional display name. Omit it for no name; an empty string is refused. */
  name?: string;
}

/**
 * One warp anchor. Both axes are in samples at the **project's** sample rate.
 * Anchors from {@link alignTakeToReference} are at the reference rate: scale both
 * fields by `projectRate / referenceRate` before {@link Project.setWarpMap}.
 */
export interface ProjectWarpAnchor {
  /** Position on the warped timeline, in samples at the project sample rate. */
  warpSample: number;
  /** Position in the clip source, in samples at the project sample rate. */
  sourceSample: number;
}

/** First-class project warp map referenced by clip `warpRefId`. */
export interface ProjectWarpMapDesc {
  /** Warp-map id, referenced by a clip's `warpRefId`. */
  id: number;
  /** Optional display name. Omit it for no name; an empty string is refused. */
  name?: string;
  /** Anchors of the map, in order. */
  anchors: ProjectWarpAnchor[];
}

/**
 * Canonical request form for {@link alignTakeToReference}.
 *
 * Both resolution fields (`hopLength`, `binsPerOctave`) are optional, and
 * omitting one takes the library value. A value outside the domain is refused: a
 * `0` or negative, a `binsPerOctave` that is not a multiple of 12. The sample
 * rates are never defaulted from `0`:
 * `sampleRate: 0` and `takeSampleRate: 0` are refused.
 */
export interface AlignTakeToReferenceRequest {
  /**
   * The reference timeline — the guide take, or the backing track the takes were
   * sung against. Must be non-empty and all-finite.
   */
  reference: Float32Array;
  /** The take to be placed under it. Must be non-empty and all-finite. */
  take: Float32Array;
  /**
   * Sample rate of `reference` in Hz, `[8000, 384000]`. Also the rate of `take`
   * unless `takeSampleRate` says otherwise. Anchors, `hopLength` and the
   * alignment's frame counts are all expressed at this rate.
   */
  sampleRate: number;
  /**
   * Sample rate of `take` in Hz, `[8000, 384000]`. Defaults to `sampleRate`. A
   * take at another rate is resampled to `sampleRate` before it is measured.
   */
  takeSampleRate?: number;
  /**
   * Chroma hop in samples at `sampleRate`, which sets the time resolution of the
   * anchors — a smaller hop measures more frames and yields more anchors.
   * Default `512`; must be a positive integer.
   */
  hopLength?: number;
  /**
   * Chroma bins per octave — the CQT resolution the twelve pitch classes are
   * folded from. Default `12`; must be a positive **multiple of 12**, since each
   * pitch class takes the mean of a whole number of CQT bins.
   */
  binsPerOctave?: number;
}

/**
 * How well an alignment was conditioned, reported by
 * {@link AlignTakeToReferenceResult}.
 *
 * Every field is descriptive: none of them makes the call fail, and a caller
 * deciding what is acceptable supplies its own threshold.
 */
export interface TakeAlignment {
  /**
   * Mean absolute frame residual of the path around its diagonal trend. A coarse
   * indicator of how far the alignment strayed from a constant rate, not an error
   * bound.
   */
  meanResidualFrames: number;
  /** Chroma frames the reference produced, at the reference rate's hop. */
  referenceFrames: number;
  /**
   * Chroma frames the take produced, counted after it was resampled to the
   * reference rate. Its ratio to `referenceFrames` is the overall rate difference
   * the anchors encode.
   */
  takeFrames: number;
}

/** Result of {@link alignTakeToReference}. */
export interface AlignTakeToReferenceResult {
  /**
   * At least two finite, strictly increasing anchors, ready to hand to
   * {@link Project.setWarpMap} as the take clip's own warp map.
   *
   * `warpSample` is a position on the **reference** timeline and `sourceSample`
   * the corresponding position in the **take**, which is the direction a clip
   * whose source is that take needs. Both are in samples at the **reference**
   * rate (`sampleRate`), unrounded.
   */
  anchors: ProjectWarpAnchor[];
  /** How well the alignment was conditioned. */
  alignment: TakeAlignment;
}

/** Descriptor for {@link Project.addClip}. */
export interface ProjectClipDesc {
  /** Track the clip is added to. */
  trackId: number;
  /** Whether the clip is a MIDI clip. Defaults to `false`. */
  isMidi?: boolean;
  /** Required clip start position in PPQ, in [0, 1e12]. */
  startPpq: number;
  /** Clip length in PPQ, in (0, 1e12]. */
  lengthPpq: number;
  /** Offset into the source in PPQ, in [0, 1e12]. Defaults to `0`. */
  sourceOffsetPpq?: number;
  /** Linear playback gain. Defaults to `1`. */
  gain?: number;
  /** Planar audio, one `Float32Array` per channel, all the same length. Needs `sampleRate`. */
  channels?: Float32Array[];
  /** Sample rate of `channels` in Hz, `[8000, 384000]`. Required with `channels`. */
  sampleRate?: number;
  /** Source URI for a metadata-only audio clip the host fills later. */
  sourceUri?: string;
}

/** Result returned by {@link Project.addMidiClip}. */
export interface ProjectMidiClipResult {
  /** Id of the MIDI track created for the clip. */
  trackId: number;
  /** Id of the clip created. */
  clipId: number;
}

/** Effective tempo, meter and bar position at a project PPQ coordinate. */
export interface ProjectPosition {
  /** Instantaneous tempo in beats per minute. */
  readonly bpm: number;
  /** Normalized time-signature segment active at this position. */
  readonly timeSignature: ProjectTimeSignatureSegment;
  /** Start of the containing bar, in PPQ. */
  readonly barStartPpq: number;
  /** Zero-based bar index; exactly representable as a JavaScript number. */
  readonly barIndex: number;
  /** One-based beat within the bar. */
  readonly beat: number;
  /** Position within the beat, in [0, 1). */
  readonly beatFraction: number;
}

/** One MIDI event in a clip's list: a position, UMP words and optional SysEx bytes. */
export interface ProjectMidiEvent {
  /** Event position in PPQ, in [0, 1e12]. */
  ppq: number;
  /** First UMP word of the message. */
  word0: number;
  /** Second UMP word of the message. Default `0`. */
  word1?: number;
  /** Owned SysEx bytes for a Data64 event. Embedded zeros are data. Omitted for channel-voice messages. */
  sysex?: Uint8Array;
}

/** Options for {@link midiRouteEvents}. Omitted filter fields mean any/no remap; `null` reads as omitted. */
export interface ProjectMidiRouteConfig {
  /** Only events on this UMP group (0-15) pass. Omitted: any group. */
  filterGroup?: number;
  /** Only events on this MIDI channel (0-15) pass. Omitted: any channel. */
  filterChannel?: number;
  /** Rewrites a channel-voice message's channel to this value (0-15). Omitted: no remap. */
  remapChannel?: number;
  /** When `false`, nothing is emitted. Default `true`. */
  thru?: boolean;
}

/** Result of {@link midiRouteEvents}. */
export interface ProjectMidiRouteResult {
  /** The events that passed the filter, remapped as configured. */
  events: ProjectMidiEvent[];
  /** `true` when the output was too long and events were dropped. */
  overflowed: boolean;
  /** Number of events dropped. */
  overflowCount: number;
}

/** Controller shape of a {@link ProjectMidiCcBinding}: a 7-bit CC, a 14-bit CC, an RPN or an NRPN. */
export type ProjectMidiCcBindingKind = 'controlChange7' | 'controlChange14' | 'rpn' | 'nrpn';

/** MIDI CC <-> automation binding descriptor used by CC learn/conversion helpers. */
export interface ProjectMidiCcBinding {
  /** Controller number (the MSB controller for a 14-bit CC). */
  ccNumber: number;
  /** MIDI channel 0..15. Omit to match any channel; a learned any-channel binding comes back without it. */
  channel?: number;
  /** Controller shape. Default `'controlChange7'`. */
  kind?: ProjectMidiCcBindingKind;
  /** LSB controller number of a 14-bit CC (`ccNumber` + 32). Default `0`. */
  ccLsbNumber?: number;
  /** Selector MSB of an RPN/NRPN binding. Default `0`. */
  selectorMsb?: number;
  /** Selector LSB of an RPN/NRPN binding. Default `0`. */
  selectorLsb?: number;
  /** Automation parameter id the controller drives. */
  paramId: number;
  /** Lower end of the mapped parameter range. Default `0`. */
  minValue?: number;
  /** Upper end of the mapped parameter range. Default `1`. */
  maxValue?: number;
  /** Stops a matched MIDI message after it updates the parameter. Default `false`. */
  consume?: boolean;
}

/** Result of {@link Project.validateMidiNotes}. */
export interface ProjectNotePairValidation {
  /** True when every note-on has a matching note-off (and vice versa). */
  ok: boolean;
  /** Count of note-ons that never received a matching note-off. */
  unmatchedNoteOns: number;
  /** Count of note-offs with no preceding matching note-on. */
  unmatchedNoteOffs: number;
}

/** Request of {@link Project.bakeMidiFx}. */
export interface ProjectMidiFxBakeRequest {
  /** Target MIDI clip id. */
  clipId: number;
  /** MIDI-FX chain configuration. */
  config: MidiFxConfig;
  /** Return per-event provenance in the result. Default false. */
  withSourceIndex?: boolean;
}

/** Result of the request form of {@link Project.bakeMidiFx}. */
export interface ProjectMidiFxBakeResult {
  /**
   * One entry per transformed event in canonical order: the index of the input
   * event it derives from, or -1 for an event with no originating input. Chord
   * and arpeggiator fan-out makes several outputs share one source index, so a
   * caller that treats the first output per index as the same event and the
   * rest as newly generated can carry a selection across the bake. Present only
   * when the request set `withSourceIndex`.
   */
  sourceIndex?: Int32Array;
}

/** Request form of {@link Project.previewMidiFxCount}. */
export interface ProjectMidiFxPreviewRequest {
  /** Target MIDI clip id. */
  clipId: number;
  /** MIDI-FX chain configuration. */
  config: MidiFxConfig;
}

/**
 * Detector settings shared by {@link transcribe} and
 * {@link Project.transcribeToClip}.
 *
 * Every field is optional and omitting one takes the documented default. A
 * value outside a field's domain is **refused**, never silently replaced — a
 * substituted default is indistinguishable downstream from one you chose.
 *
 * That includes `0` on the fields whose domain excludes it (`referenceHz`,
 * `f0MinHz`, `f0MaxHz`, `minNoteMs`, `segmentationThresholdCents`,
 * `velocityFloorDb`, `fixedVelocity`, `maxPolyphony`, `minNoteDivision`): omitting the field is how
 * you ask for the default, so a `0` you wrote is a value, and it is out of
 * domain. Only `group` and `channel` accept `0` — there it is a value you can
 * mean. On `minFramePeakRatio` and `minRidgePeakRatio` a written `0` is
 * accepted and means "none" (a real 0), not the default. `reattackRatio` is the
 * opposite: omitting it leaves re-attack splitting off and a written `0` is refused.
 *
 * `maxPolyphony`, `minFramePeakRatio`, `minRidgePeakRatio` and `reattackRatio`
 * apply to the polyphonic path only; with `polyphonic` off, writing any of them
 * is refused.
 */
export interface TranscribeOptions {
  /**
   * `true` reads the multi-F0 chain, which finds overlapping notes at the cost
   * of a full STFT and a mask per tracked ridge. `false` (the default) reads
   * pYIN cut into notes, which follows one line at a time.
   *
   * Working memory on the polyphonic path grows with the input: about 0.75 MB
   * per second of audio at any rate, plus about 3 MB, plus about 32 bytes per
   * sample of the longest note while it is measured. The input itself is extra.
   */
  polyphonic?: boolean;
  /**
   * Tuning reference in Hz the MIDI note numbers are measured against, or
   * `'auto'` to measure it from the audio before any pitch is tracked. Default
   * `440`; a number must be finite and positive. {@link tuningToReferenceHz}
   * converts a measured tuning to Hz, and {@link TranscribeResult.tuningCents}
   * reports what was used either way, so a wrong transcription can be traced to it.
   */
  referenceHz?: number | 'auto';
  /**
   * F0 tracker range in Hz. Both paths use these bounds; the polyphonic path
   * applies them to the salience estimator's F0 axis while its cent-spectrum
   * bounds remain independent. When omitted, the monophonic defaults are `65`
   * and `2093`, while the polyphonic defaults are `55` and `1760`. Explicit
   * values must be finite and positive, and `f0MaxHz` must exceed `f0MinHz`.
   */
  f0MinHz?: number;
  /** Upper end of the F0 tracker range in Hz; omitted defaults to `2093` mono or `1760` poly. */
  f0MaxHz?: number;
  /**
   * Shortest span kept as a note, in milliseconds. Default `30` monophonic
   * (segments), `45` monophonic (singing) and `60` polyphonic, where it is the
   * shortest ridge the tracker keeps; no default depends on the tempo.
   * Must be positive, and cannot be written together with `minNoteDivision`.
   */
  minNoteMs?: number;
  /**
   * Shortest note kept, as a note value: `n` is a `1/n` note at the
   * transcription tempo (`32` is a thirty-second note). An integer in
   * `[1, 128]`; `0` is refused. Applies to both paths. The tempo is the given
   * or detected one for {@link transcribe} and the project's tempo at its start
   * for {@link Project.transcribeToClip}. Cannot be written together with `minNoteMs`.
   */
  minNoteDivision?: number;
  /**
   * Monophonic only. With the `'segments'` model, the pitch movement in cents at
   * which one note ends and the next starts; with `'singing'`, the scale of the
   * pitch movement kept within one note. Default `50`; must be positive.
   */
  segmentationThresholdCents?: number;
  /**
   * Monophonic only. `'segments'` (default) cuts the pitch track where it moves
   * and splits brief level dips; it is accurate on instrument lines, including
   * short notes. `'singing'` decodes notes jointly over pitch bins and holds
   * vibrato and intonation drift within a note; it suits sung takes and can
   * merge very short notes at fast tempos. Any other string is refused, and
   * with `polyphonic` set a non-default model is refused.
   */
  monophonicModel?: 'segments' | 'singing';
  /**
   * Level mapped to velocity 1, in dBFS. Default `-48`; **must be negative**.
   *
   * A note's peak per-frame RMS of the input over its span (other notes
   * sounding then included) is taken in dBFS and mapped linearly from
   * `[velocityFloorDb, 0]` onto `[1, 127]`, clamped at both ends. It does not
   * drop notes: a note more than 36 dB under the take's loudest notes, or under
   * -80 dBFS, is dropped whatever this is set to.
   */
  velocityFloorDb?: number;
  /**
   * An integer in `[1, 127]` gives every note that velocity instead of the
   * mapped one; the level gate still drops tails and ghosts. **Omit the
   * field to map the level** — `0` is refused, because it is not a MIDI
   * velocity and omission already says "measure".
   */
  fixedVelocity?: number;
  /**
   * Polyphonic only. Voices one frame may hold: an integer in `[1, 64]`.
   * Default `10`; `0` is refused.
   */
  maxPolyphony?: number;
  /**
   * Polyphonic only. Stops a frame's search below this share of its first peak,
   * in `[0, 1]`. Absent takes the library default `0.20`; `0` is a real 0 (no cut); a negative is refused.
   */
  minFramePeakRatio?: number;
  /**
   * Polyphonic only. Ends a ridge below this share of its own running peak, in
   * `[0, 1]`. Absent takes the library default `0.10`; `0` is a real 0 (no cut); a negative is refused.
   */
  minRidgePeakRatio?: number;
  /**
   * Polyphonic only. Splits a ridge where its salience climbs past this
   * multiple of the level just before, i.e. the same pitch struck again while
   * it sounds. Must be above `1`. Absent leaves the split off; `0`, a negative and a
   * non-finite value are refused with a `RangeError`.
   */
  reattackRatio?: number;
  /** UMP group the events are emitted on, `0..15`. Default `0`. */
  group?: number;
  /** MIDI channel the events are emitted on, `0..15`. Default `0`. */
  channel?: number;
}

/** Result of {@link transcribe}. */
export interface TranscribeResult {
  /**
   * Note-on / note-off pairs in canonical PPQ order, ready to hand straight to
   * {@link Project.setMidiEvents}.
   *
   * Ordering is `(ppq, note-off before note-on)`. A note-off sharing a tick
   * with the next note's on comes first, so a consumer playing the events in
   * order does not start a legato repeat of the same pitch and immediately
   * stop it.
   */
  events: ProjectMidiEvent[];
  /** Number of notes, which is always half `events.length`. */
  noteCount: number;
  /** The tempo the PPQ coordinates were built on — yours when you gave one, the detected one otherwise. */
  tempoBpm: number;
  /**
   * Tuning the note numbers were measured against, in cents from concert A440,
   * in [-50, 50): the measured value under `referenceHz: 'auto'`, otherwise the
   * one `referenceHz` amounts to ({@link referenceHzToTuning}).
   */
  tuningCents: number;
}

/**
 * Request form of {@link Project.transcribeToClip}.
 *
 * No `tempoBpm`: the PPQ grid is the **project's own tempo map**, so a project
 * whose tempo was installed by {@link Project.autoTempo} transcribes onto that
 * map rather than onto a second, separately detected tempo.
 */
export interface ProjectTranscribeRequest extends TranscribeOptions {
  /** Target MIDI clip id. Its entire event list is replaced. */
  clipId: number;
  /** Mono source audio. Must be non-empty and all-finite. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz, `[8000, 384000]`. */
  sampleRate: number;
}

/** Diagnostic codes of a project compile. */
export type ProjectDiagnosticCode =
  | 'ok'
  | 'danglingSourceRef'
  | 'clipOverlap'
  | 'invalidTempo'
  | 'invalidPpq'
  | 'unsupportedGraph'
  | 'unsupportedMixing'
  | 'emptyAudioSource'
  | 'invalidSampleRate'
  | 'sourceKindMismatch'
  | 'midiClipNoInstrument'
  | 'raggedAudioSource'
  | 'loopCrossfadeUnavailable'
  | 'sharedChannelStrip'
  | 'automationLaneConflict'
  | 'invalidAutomationTargetKind'
  | 'automationLaneCapacity'
  | 'reservedAutomationRouteConflict'
  | 'automationTargetUnrouted';

/** One compile diagnostic. */
export interface ProjectDiagnostic {
  /** What the diagnostic reports. */
  readonly code: ProjectDiagnosticCode;
  /** `'error'` means nothing compiled; `'warning'` leaves the result usable. */
  readonly severity: 'error' | 'warning';
  /** Affected clip / track / source id; absent when the diagnostic is not tied to one. */
  readonly targetId?: number;
  /** Human-readable message for this diagnostic. */
  readonly message: string;
}

/** Options of {@link Project.compile}. */
export interface ProjectCompileOptions {
  /** Also keep the compiled timeline in the result's `timeline`. Omit for `false`. */
  withTimeline?: boolean;
}

/** Result of {@link Project.compile}. */
export interface ProjectCompileResult {
  /** Every diagnostic the compile surfaced; empty when it was clean. */
  readonly diagnostics: readonly ProjectDiagnostic[];
  /**
   * The compiled timeline; present only when `withTimeline` was requested and
   * compilation produced one (an error diagnostic means none; read
   * {@link ProjectCompileResult.diagnostics}). Release it with
   * {@link ProjectTimeline.destroy}.
   */
  readonly timeline?: ProjectTimeline;
}

/** Result of {@link Project.fromJsonWithDiagnostics}. */
export interface ProjectDeserializeResult {
  /** The deserialized project. */
  project: Project;
  /** Native warning diagnostics emitted by a successful load. */
  diagnostics: string;
}
