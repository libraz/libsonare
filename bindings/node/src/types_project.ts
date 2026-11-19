// Type-only, so the erased import adds no runtime edge: to the coercion module
// below, and to the bank module, which names these types back.
import type { ProjectTimeline } from './project.js';
import type { MidiFxConfig } from './types_engine.js';
import type { AutomationCurve } from './types_mixing.js';

/**
 * Automation breakpoint interpolation for {@link ProjectAutomationPoint}: the same
 * tokens as the mixer's {@link AutomationCurve}. An ordinal is a `TypeError`.
 */
export type ProjectAutomationCurve = AutomationCurve;

/**
 * Expected project ABI version, mirroring `SONARE_PROJECT_ABI_VERSION`
 * (src/sonare_c_project.h) and the other bindings' constant. A `projectAbiVersion()`
 * that differs means the native binary lays out the flat project PODs
 * differently than this binding expects (0 = arrangement support compiled out).
 */
export const EXPECTED_PROJECT_ABI_VERSION = 2;

/** What a track carries. */
export type ProjectTrackKind = 'audio' | 'midi' | 'aux';

/** What a source or a clip's source holds. */
export type ProjectSourceKind = 'audio' | 'midi';

/** Whether clips on one track may overlap in time. */
export type ProjectOverlapPolicy = 'disallow' | 'allow';

/** Descriptor for {@link Project.addTrack}. */
export interface ProjectTrackDesc {
  /** Track kind. Default `'audio'`. */
  kind?: ProjectTrackKind;
  /** Optional track name. Omit it for no name; an empty string is refused. */
  name?: string;
}

/** Read-only stored track descriptor. */
export interface ProjectTrack {
  /** Stable track id. */
  readonly id: number;
  /** What the track carries. */
  readonly kind: ProjectTrackKind;
  /** MIDI destination id the track's MIDI clips are routed to. */
  readonly midiDestinationId: number;
  /** Linear playback gain. */
  readonly gain: number;
  /** Stereo balance in `[-1, 1]`. */
  readonly pan: number;
  /** Whether the track is muted. */
  readonly mute: boolean;
  /** Whether the track is soloed. */
  readonly solo: boolean;
  /** Track name; empty when unnamed. */
  readonly name: string;
}

/** Read-only stored clip descriptor. */
export interface ProjectClip {
  /** Stable clip id. */
  readonly id: number;
  /** Owning track id. */
  readonly trackId: number;
  /** Id of the source the clip plays. */
  readonly sourceId: number;
  /** What the clip's source holds. */
  readonly sourceKind: ProjectSourceKind;
  /** Clip start in PPQ. */
  readonly startPpq: number;
  /** Clip length in PPQ. */
  readonly lengthPpq: number;
  /** Offset into the source content in PPQ. */
  readonly sourceOffsetPpq: number;
  /** Linear clip gain. */
  readonly gain: number;
  /** Whether the clip loops. */
  readonly loopMode: ProjectLoopMode;
  /** Loop length in PPQ; absent for a whole-clip loop or a clip that does not loop. */
  readonly loopLengthPpq?: number;
}

/** Read-only stored audio or MIDI source descriptor. */
export interface ProjectSource {
  /** Stable source id. */
  readonly id: number;
  /** What the source holds. */
  readonly kind: ProjectSourceKind;
  /** Channel count of the decoded audio; `0` for a MIDI source or one not yet resolved. */
  readonly numChannels: number;
  /** Id of the storage handle holding the decoded audio. */
  readonly storageHandleId: number;
  /** Sample rate the audio was recorded at, in Hz; `0` when unknown. */
  readonly sampleRateHint: number;
  /** Source name or host-local URI. */
  readonly nameOrUri: string;
  /** Owning content hash of an audio source; absent when none is set. */
  readonly contentHash?: string;
  /** External source-separation role of an audio source; absent when none is set. */
  readonly externalStemRole?: string;
}

/** What a {@link ProjectMarker} marks. */
export type ProjectMarkerKind = 'marker' | 'text' | 'lyric' | 'cuePoint' | 'keySignature';

/** A stored project marker. */
export interface ProjectMarker {
  /** Stable marker id. */
  readonly id: number;
  /** Position in PPQ. */
  readonly ppq: number;
  /** Marker text; absent when unnamed. */
  readonly name?: string;
  /** What the marker marks. */
  readonly kind: ProjectMarkerKind;
  /** Key signature only: -7..7 (sharps positive); `0` otherwise. */
  readonly keyFifths: number;
  /** Key signature only: `false` is major, `true` minor; `false` otherwise. */
  readonly keyMinor: boolean;
}

/** Input of {@link Project.setMarker}. */
export interface ProjectMarkerInput {
  /** Id of the marker to replace. Omitted, a new id is allocated; an explicit `0` is a `RangeError`. */
  id?: number;
  /** Position in PPQ. */
  ppq: number;
  /** Marker text. Omit it for an unnamed marker; an empty string is refused. */
  name?: string;
  /** What the marker marks. Default `'marker'`. */
  kind?: ProjectMarkerKind;
  /** Key signature only: -7..7 (sharps positive). Default `0`. */
  keyFifths?: number;
  /** Key signature only: `false` is major, `true` minor. Default `false`. */
  keyMinor?: boolean;
}

/**
 * One first-class warp-map anchor. Sample positions must be finite and monotonic.
 * Both axes are in samples at the project sample rate; anchors from
 * {@link alignTakeToReference} are in reference-rate samples, so scale both axes
 * by `projectRate / referenceRate` first.
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
 * Request form of {@link alignTakeToReference}.
 *
 * The reference is read at {@link sampleRate} and the take at
 * {@link takeSampleRate}; a take at another rate is resampled to the reference
 * rate before measuring, so the anchors and the alignment frame counts are in
 * reference-rate units.
 */
export interface AlignTakeToReferenceRequest {
  /**
   * The reference timeline — the guide take, or the backing track the takes were
   * sung against. Must not be empty.
   */
  reference: Float32Array;
  /** The mono take to be placed under {@link reference}. Must not be empty. */
  take: Float32Array;
  /** Sample rate of {@link reference}, in Hz; also the rate of {@link take} unless {@link takeSampleRate} is given. */
  sampleRate: number;
  /**
   * Sample rate of {@link take}, in Hz, validated like {@link sampleRate}. Omit
   * when the take was recorded at the reference rate.
   */
  takeSampleRate?: number;
  /**
   * Chroma hop in reference-rate samples, which sets the time resolution of the anchors: a
   * finer hop yields more of them. Omit for the library value — 0 is not a
   * meaningful hop, so the library reads it as "keep the default" and a
   * fractional value is refused rather than truncated onto one.
   */
  hopLength?: number;
  /**
   * Chroma bins per octave. Omit for the library value of 12, as with
   * {@link hopLength}.
   *
   * Must be a positive multiple of 12: the chromagram folds onto 12 pitch
   * classes, so `13` and `18` are refused as an invalid parameter rather than
   * rounded to one that fits. A multiple of 12 is necessary but not sufficient —
   * the CQT grid has to fit the analysis band as well, so how fine a value is
   * accepted depends on {@link sampleRate}, and one the rate cannot carry is
   * refused rather than analysed over a narrower range. 12 is the only value
   * accepted across the whole supported rate range; 24 and 36 hold from 16 kHz
   * to 48 kHz but not at 8 kHz.
   */
  binsPerOctave?: number;
}

/**
 * How well an alignment was conditioned, from {@link alignTakeToReference}.
 *
 * Every field is descriptive: none of them makes the call fail, and a caller
 * deciding what is acceptable supplies its own threshold.
 */
export interface TakeAlignment {
  /**
   * Mean absolute frame residual of the alignment path around its diagonal
   * trend. A coarse indicator of how far the alignment strayed from a constant
   * rate, not an error bound.
   */
  meanResidualFrames: number;
  /** Chroma hop frames the reference produced, at the reference rate. */
  referenceFrames: number;
  /**
   * Chroma hop frames the take produced, counted after any resampling to the
   * reference rate. Its ratio to {@link referenceFrames} is the
   * overall rate difference the anchors encode.
   */
  takeFrames: number;
}

/** Result of {@link alignTakeToReference}. */
export interface AlignTakeToReferenceResult {
  /**
   * At least two finite, strictly increasing anchors, both axes in samples at
   * the reference rate. Scale both by `projectRate / referenceRate`, then hand
   * them to {@link Project.setWarpMap} as a {@link ProjectWarpMapDesc}.
   */
  anchors: ProjectWarpAnchor[];
  /** How well the alignment was conditioned. */
  alignment: TakeAlignment;
}

/**
 * Descriptor for {@link Project.addClip}. All musical positions are PPQ
 * (quarter notes); `lengthPpq` must be > 0.
 */
export interface ProjectClipDesc {
  /** Owning track id (from {@link Project.addTrack}). */
  trackId: number;
  /** `true` for a MIDI clip, `false`/omitted for an audio clip. */
  isMidi?: boolean;
  /** Clip start position in PPQ (default 0). */
  startPpq?: number;
  /** Clip length in PPQ (must be > 0). */
  lengthPpq: number;
  /** Offset into the source content in PPQ (default 0). */
  sourceOffsetPpq?: number;
  /** Linear clip gain (default 1). */
  gain?: number;
  /**
   * Decoded audio for an audio clip, one `Float32Array` per channel, all the
   * same length; the array length is the channel count. When provided, the clip
   * is bound to a fresh renderable audio source; omit for a metadata-only source.
   */
  channels?: Float32Array[];
  /**
   * Sample rate of {@link channels} in Hz, within `[8000, 384000]`. Required
   * whenever `channels` is supplied; ignored for a metadata-only clip.
   */
  sampleRate?: number;
  /** Optional host-local source reference for a metadata-only audio source. */
  sourceUri?: string;
}

/** Channel layout of an {@link ExternalSeparatedStem}. */
export type ProjectStemLayout = 'mono' | 'stereo';

/** One decoded output of an external source-separation model. */
export interface ExternalSeparatedStem {
  /** Non-empty, unique host label. It is used as the new track name. */
  name: string;
  /** Optional semantic metadata such as `'vocals'`; it has no DSP effect. */
  role?: string;
  /** Mono or stereo layout. */
  layout: ProjectStemLayout;
  /** One Float32Array per layout channel, all with the same frame count. */
  channels: Float32Array[];
  /** Absolute project-rate frame where the stem begins (default 0). */
  startFrame?: number;
}

/** Request for {@link Project.importExternalStems}. */
export interface ExternalSeparatedStemImportRequest {
  /** Must exactly equal the target project's sample rate; no resampling occurs. */
  sampleRate: number;
  /** The stems to import, one track and clip each. */
  stems: ExternalSeparatedStem[];
}

/** Stable ids created by {@link Project.importExternalStems}. */
export interface ExternalSeparatedStemImportResult {
  /** Ids of the tracks created, one per stem. */
  trackIds: number[];
  /** Ids of the clips created, one per stem. */
  clipIds: number[];
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
  /** Clip start in PPQ. Default `0`. */
  startPpq?: number;
  /** Length of one loop in PPQ; also the length of the added clip. */
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

/** `(trackId, clipId)` returned by {@link Project.addMidiClip}. */
export interface ProjectMidiClipResult {
  /** Id of the MIDI track created for the clip. */
  trackId: number;
  /** Id of the clip created. */
  clipId: number;
}

/**
 * One MIDI event in a clip's list: a position plus the first two UMP words.
 *
 * Channel-voice messages only. A clip's SysEx payloads live beside the event
 * list and are reached by a handle this type does not carry — they survive
 * `Project.importSmf`, `Project.exportSmf` and project serialization, and are
 * destroyed by `Project.setMidiEvents`.
 */
export interface ProjectMidiEvent {
  /** Event position in PPQ. */
  ppq: number;
  /** First UMP word of the message. */
  word0: number;
  /** Second UMP word of the message. Default `0`. */
  word1?: number;
}

/** Options for {@link midiRouteEvents}. Omitted filter fields mean any group/channel and no remap. */
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
}

/** Whether a {@link ProjectDiagnostic} blocks the timeline. */
export type ProjectDiagnosticSeverity = 'error' | 'warning';

/** What a {@link ProjectDiagnostic} reports. */
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

/** Result of {@link Project.validateMidiNotes}. */
export interface ProjectNotePairValidation {
  /** `true` when every note-on has its note-off. */
  readonly ok: boolean;
  /** Note-ons with no matching note-off. */
  readonly unmatchedNoteOns: number;
  /** Note-offs with no matching note-on. */
  readonly unmatchedNoteOffs: number;
}

/** One compile diagnostic. */
export interface ProjectDiagnostic {
  /** What the diagnostic reports. */
  readonly code: ProjectDiagnosticCode;
  /** `'error'` means no timeline was produced. */
  readonly severity: ProjectDiagnosticSeverity;
  /** Affected clip / track / source id; absent when the diagnostic names none. */
  readonly targetId?: number;
  /** Human-readable message for this diagnostic. */
  readonly message: string;
}

/** Result of {@link Project.compile}. */
export interface ProjectCompileResult {
  /** Every diagnostic the compile produced; empty when it was clean. */
  readonly diagnostics: readonly ProjectDiagnostic[];
}

/** Result of {@link Project.compileTimeline}. */
export interface ProjectCompileTimelineResult {
  /** Every diagnostic the compile produced; empty when it was clean. */
  readonly diagnostics: readonly ProjectDiagnostic[];
  /**
   * The compiled playback snapshot; absent when compilation produced none
   * (see `diagnostics`). Owned by the caller: destroy it when done.
   */
  readonly timeline?: ProjectTimeline;
}

/**
 * One tempo segment, as {@link Project.setTempoSegments} accepts it and
 * {@link Project.tempoSegments} returns it.
 *
 * @remarks
 * A segment read back carries the fields the setter reads, so it can be written
 * straight to another project. A project stores musical positions only, so there
 * is no sample position: it is derived when the project is compiled.
 */
export interface ProjectTempoSegment {
  /** Segment start position in PPQ. */
  startPpq: number;
  /** Tempo in BPM at the segment start. */
  bpm: number;
  /** Tempo in BPM at the segment end for a ramp; `0` / omitted = constant tempo. */
  endBpm?: number;
}

/**
 * One time-signature segment, as {@link Project.setTimeSignatures} accepts it
 * and {@link Project.timeSignatures} returns it.
 */
export interface ProjectTimeSignatureSegment {
  /** Segment start position in PPQ. */
  startPpq: number;
  /** Beats per bar. */
  numerator: number;
  /** Beat unit (e.g. `4` for quarter note). */
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

/** A ranked primary/half/double tempo hypothesis from {@link Project.analyzeTempo}. */
export interface ProjectTempoCandidate {
  /** Tempo in BPM. */
  readonly bpm: number;
  /** Ranking confidence in `[0, 1]`. */
  readonly confidence: number;
  /** Which tempo octave the candidate is. */
  readonly label: 'primary' | 'half' | 'double';
  /** Number of time-signature segments the candidate would install. */
  readonly timeSignatureCount: number;
  /** The first time-signature segment the candidate would install. */
  readonly firstTimeSignature: ProjectTimeSignatureSegment;
}

/**
 * Request form of {@link Project.analyzeTempo}; it is the positional
 * `(samples, sampleRate, options?)` call spelt as one object.
 */
export interface ProjectAnalyzeTempoRequest extends ProjectTempoOptions {
  /** Mono audio to analyse. */
  samples: Float32Array;
  /** Sample rate of {@link samples}, in Hz. */
  sampleRate: number;
}

/**
 * Request form of {@link Project.autoTempo}; it is the positional
 * `(samples, sampleRate, options?)` call spelt as one object.
 */
export interface ProjectAutoTempoRequest extends ProjectAnalyzeTempoRequest {
  /** Index into the ranking {@link Project.analyzeTempo} produced. Default `0`. */
  candidateIndex?: number;
  /** Also install the candidate's detected time signatures. Default `false`. */
  applyTimeSignatures?: boolean;
}

/** Options of the positional form of {@link Project.autoTempo}. */
export type ProjectAutoTempoOptions = Omit<ProjectAutoTempoRequest, 'samples' | 'sampleRate'>;

/**
 * Options for {@link Project.bounce} and the `bounceWith*Instruments` methods.
 * Omit a field for its default; an explicit `0` on `totalFrames`, `blockSize` or
 * `numChannels` is a `RangeError`.
 */
export interface ProjectBounceOptions {
  /**
   * Render length in frames at the project sample rate. Omitted, the length is
   * derived from the arrangement (musical end plus any instrument release tail);
   * it is never an empty render.
   */
  totalFrames?: number;
  /** Render block size in frames. Default 128. */
  blockSize?: number;
  /**
   * Output channel count: 1, 2, 6 or 8, and at most the width of the scene
   * master's layout (mono/stereo/no master allow 2). The master is mixed at
   * this width; 1 folds a 2-channel master to 0.5(L+R). Omit it for the
   * master's own width (2 with no master); the result's `channels.length` is the width used.
   */
  numChannels?: number;
  /** Host-instrument plugin delay compensation, in samples, fed to the compiler. Default 0. */
  instrumentLatencySamples?: number;
}

/** Planar audio rendered by {@link Project.bounce}. */
export interface ProjectBounceResult {
  /** One plane per output channel, all the same length. */
  readonly channels: Float32Array[];
  /** Sample rate of the render in Hz; always the project's own. */
  readonly sampleRate: number;
}

/**
 * One instrument bound to a MIDI destination for a `bounceWith*Instruments`
 * call. An omitted `destinationId` is destination `0`, which is a real id.
 */
export interface ProjectInstrumentBinding<T> {
  /** MIDI destination id the instrument renders (see {@link Project.setTrackMidiDestination}). Default `0`. */
  destinationId?: number;
  /** The instrument that renders the destination. */
  instrument: T;
}

/** Clip fade-curve names. */
export type ProjectFadeCurve = 'linear' | 'equalPower' | 'exponential' | 'logarithmic';

/** Whether a clip loops. */
export type ProjectLoopMode = 'off' | 'loop';

/**
 * Persistent target classification for a project automation lane: a typed
 * lane drives its track's fader or pan; an `'opaque'` lane drives the exact `targetParamId` it carries — a host-registered engine parameter, a graph-node parameter binding or strip width (3) — and a reserved mixer, insert or instrument id is refused.
 */
export type ProjectAutomationTargetKind = 'opaque' | 'trackFaderDb' | 'trackPan';

/**
 * Fade regions of {@link Project.setClipFade}. An omitted side, or an omitted
 * length, is a zero-length linear fade.
 */
export interface ProjectClipFadeRequest {
  /** Fade-in length in PPQ; finite and >= 0. Default `0` (no fade). */
  fadeInPpq?: number;
  /** Fade-in curve. Default `'linear'`. */
  fadeInCurve?: ProjectFadeCurve;
  /** Fade-out length in PPQ; finite and >= 0. Default `0` (no fade). */
  fadeOutPpq?: number;
  /** Fade-out curve. Default `'linear'`. */
  fadeOutCurve?: ProjectFadeCurve;
}

/** Options of {@link Project.moveClip}. */
export interface ProjectClipMoveOptions {
  /** Track to move the clip to. Omitted, the clip stays on its track. */
  trackId?: number;
}

/** Options of {@link Project.setClipTakes}. */
export interface ProjectClipTakesOptions {
  /** Take that plays. Omitted, the clip's base source plays. */
  activeTakeId?: number;
}

/** Request of {@link Project.setClipLoop}. */
export interface ProjectClipLoopRequest {
  /** Whether the clip loops. */
  loopMode: ProjectLoopMode;
  /**
   * Loop length in PPQ, finite and > 0. Omit it to loop the entire clip (the
   * effective length is resolved from the clip's own duration); an explicit `0`
   * is refused.
   */
  loopLengthPpq?: number;
  /**
   * Equal-power crossfade at the loop seam in PPQ, finite and >= 0; omitted or
   * `0` is a hard loop. The engine clamps it to the clip's pre-roll and half the loop.
   */
  loopCrossfadePpq?: number;
}

/** Request of {@link Project.setSourceAudio}. */
export interface ProjectSourceAudioRequest {
  /** Id of an existing audio source. */
  sourceId: number;
  /** Decoded audio, one `Float32Array` per channel, all the same length. */
  channels: Float32Array[];
  /** Sample rate of {@link channels} in Hz. */
  sampleRate: number;
}

/**
 * Request of {@link Project.setAudioSourceMetadata}. Both strings are replaced;
 * an omitted one is cleared, and an empty string is refused.
 */
export interface ProjectAudioSourceMetadataRequest {
  /** Id of an existing audio source. */
  sourceId: number;
  /** Owning content hash. Omit it to clear it. */
  contentHash?: string;
  /** External source-separation role such as `'vocals'`. Omit it to clear it. */
  externalStemRole?: string;
}

/** Request of {@link Project.setProgramOnChannel}. */
export interface ProjectProgramRequest {
  /** UMP group, `0`..`15`. */
  group: number;
  /** MIDI channel, `0`..`15`. */
  channel: number;
  /** Program number, `0`..`127`. */
  program: number;
  /** Bank Select to emit before the program. Omitted, none is emitted. */
  bank?: ProjectProgramBank;
}

/** Bank Select of a {@link ProjectProgramRequest}. */
export interface ProjectProgramBank {
  /** Bank Select MSB (CC 0), `0`..`127`. */
  msb: number;
  /** Bank Select LSB (CC 32), `0`..`127`. */
  lsb: number;
}

/** One alternate take for {@link Project.setClipTakes}. */
export interface ProjectClipTake {
  /** Take id unique within the clip. Required; `0` is refused. */
  id: number;
  /** Source id for this take. Omit it to reuse the clip's current source; `0` is refused. */
  sourceId?: number;
  /** Offset into the take source in PPQ. */
  sourceOffsetPpq?: number;
  /** Optional UI/display name. Omit it for an unnamed take; an empty string is refused. */
  name?: string;
}

/** One comp segment for {@link Project.setClipCompSegments}. */
export interface ProjectClipCompSegment {
  /** Segment start in PPQ. */
  startPpq: number;
  /** Segment end in PPQ. */
  endPpq: number;
  /** Take id to play in this range. Omit it to fall back to the clip's active take; `0` is refused. */
  takeId?: number;
  /**
   * Equal-power crossfade length in PPQ, fading this segment in over the part
   * of the preceding segment taken from before `startPpq` (the clip's length
   * does not move). Finite and >= 0; default `0` (a butt join).
   */
  crossfadePpq?: number;
}

/**
 * One automation breakpoint for {@link Project.addAutomationLane} /
 * {@link Project.editAutomationLane}. Any key other than `ppq`, `value` and
 * `curveToNext` is a `TypeError`.
 */
export interface ProjectAutomationPoint {
  /** Breakpoint position in PPQ. */
  ppq: number;
  /** Parameter value at the breakpoint. */
  value: number;
  /** Curve to the next breakpoint (default `'linear'`). */
  curveToNext?: ProjectAutomationCurve;
}

/**
 * Descriptor for {@link Project.addAutomationLane} /
 * {@link Project.editAutomationLane}.
 */
export interface ProjectAutomationLaneDesc {
  /** Host-defined, non-zero target parameter id the lane drives (zero is reserved). */
  targetParamId: number;
  /**
   * Persistent target classification. On add, omitted is `'opaque'`; on edit,
   * omitted keeps the lane's current kind.
   */
  targetKind?: ProjectAutomationTargetKind;
  /** Breakpoints (stored verbatim; need not be pre-sorted). */
  points: ProjectAutomationPoint[];
}

/** Mode of a {@link ProjectKeySegment}. */
export type ProjectKeyMode =
  | 'unknown'
  | 'major'
  | 'minor'
  | 'dorian'
  | 'phrygian'
  | 'lydian'
  | 'mixolydian'
  | 'locrian';

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

/** One key segment for {@link Project.annotateKeys}. */
export interface ProjectKeySegment {
  /** Segment start in PPQ. */
  startPpq: number;
  /** Segment end in PPQ. */
  endPpq: number;
  /** Tonic pitch class 0..11 (C=0). Omit when the tonic is unknown. */
  tonicPc?: number;
  /** Key mode. Default `'unknown'`. */
  mode?: ProjectKeyMode;
}

/** One chord symbol for {@link Project.annotateChords}. */
export interface ProjectChordSymbol {
  /** Chord start in PPQ. */
  startPpq: number;
  /** Chord end in PPQ. */
  endPpq: number;
  /** Root pitch class 0..11 (C=0). Omit when the root is unknown. */
  rootPc?: number;
  /** Chord quality. Default `'unknown'`. */
  quality?: ProjectChordQuality;
  /** Extension scale degrees (for example 7, 9, 13; 2 or 4 for a suspension), up to 32. */
  extensions?: number[];
  /** Slash-bass pitch class 0..11. Omit when the chord has no slash bass. */
  slashBassPc?: number;
  /** Optional roman-numeral label. */
  romanNumeral?: string;
  /** Marks a modulation boundary. Default false. */
  modulationBoundary?: boolean;
}

/** Descriptor for {@link Project.setAssistSidecar}. */
export interface ProjectAssistSidecarInput {
  /** Non-empty module id key. */
  moduleId: string;
  /** Module-defined schema version. Default 0. */
  schemaVersion?: number;
  /** Target track id (0 = project scope). Default 0. */
  targetTrackId?: number;
  /** Region start in PPQ. Default 0. */
  regionStartPpq?: number;
  /** Region end in PPQ. Default 0. */
  regionEndPpq?: number;
  /** Opaque module-owned payload bytes. */
  payload?: Uint8Array;
}

/** A stored assist sidecar returned by {@link Project.assistSidecars}. */
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
 * What a transcription reads and how it converts what it finds.
 *
 * Shared by the one-shot {@link transcribe} and by
 * {@link Project.transcribeToClip}; every field is optional and omitting one
 * takes the documented default.
 *
 * Omission is the only way to ask for a default. A field whose documented
 * domain excludes `0` refuses a written `0` instead of silently substituting
 * its default, so an answer measured against a value nobody asked for is not
 * something a caller can receive. `group` and `channel` are the exceptions:
 * their `0` is a value a caller can mean. Python and WASM refuse the same set.
 *
 * Three neighbouring jobs are deliberately NOT done here, because the library
 * already does each of them somewhere else:
 *
 * - Quantizing to a grid — {@link Project.bakeMidiFx}'s `quantize_ppq` /
 *   `quantize_strength`.
 * - Detecting and installing a tempo map — {@link Project.autoTempo}.
 * - Annotating key and chords — {@link Project.annotateKeys} /
 *   {@link Project.annotateChords}.
 *
 * The tuning reference is the caller's by default: pass {@link referenceHz}, or
 * convert a measured tuning with {@link tuningToReferenceHz}. A take recorded
 * away from A440 can instead pass `referenceHz: 'auto'`, which measures it from
 * the audio before any pitch is tracked; {@link TranscribeResult.tuningCents}
 * reports what was used either way, so a wrong transcription can be traced to it.
 */
export interface TranscribeOptions {
  /**
   * `true` reads the multi-F0 chain, which finds overlapping notes at the cost
   * of a full STFT and a mask per tracked ridge. `false` reads pYIN cut into
   * notes, which follows one line at a time. Default `false`.
   *
   * Working memory on the polyphonic path grows with the input: about 0.75 MB
   * per second of audio at any rate, plus about 3 MB, plus about 32 bytes per
   * sample of the longest note while it is measured. The input itself is extra.
   */
  polyphonic?: boolean;
  /**
   * Tuning reference the MIDI note numbers are measured against, in Hz, or
   * `'auto'` to measure it from the audio. A number must be finite and
   * positive; `0` is refused rather than read as the default, which is what
   * omitting the field asks for. Default `440`.
   */
  referenceHz?: number | 'auto';
  /**
   * Low end of the F0 tracker's range, in Hz. Both paths use this bound; the
   * polyphonic path applies it to the salience estimator's F0 axis while its
   * cent-spectrum bounds remain independent. When omitted, the monophonic
   * default is `65` and the polyphonic default is `55`. An explicit value must
   * be finite and positive, and below {@link f0MaxHz}.
   */
  f0MinHz?: number;
  /**
   * High end of the F0 tracker's range, in Hz. Both paths use this bound; the
   * polyphonic path applies it to the salience estimator's F0 axis while its
   * cent-spectrum bounds remain independent. When omitted, the monophonic
   * default is `2093` and the polyphonic default is `1760`. An explicit value
   * must be finite and above {@link f0MinHz}.
   */
  f0MaxHz?: number;
  /**
   * Shortest span kept as a note, in milliseconds. Must be finite and
   * positive. Refused together with {@link minNoteDivision}. When neither is
   * given the default is `30` for the monophonic `'segments'` model, `45` for
   * `'singing'` and `60` for the polyphonic tracker; neither depends on the tempo.
   */
  minNoteMs?: number;
  /**
   * Shortest note kept, as a note value: `n` is a 1/n note at the transcription
   * tempo (`32` is a thirty-second note). An integer in `[1, 128]`; a written
   * `0` is refused. The tempo is the one given (or detected) for
   * {@link transcribe} and the project's tempo at its start for
   * {@link Project.transcribeToClip}. Applies to both the monophonic and the
   * polyphonic tracker. Refused together with {@link minNoteMs}.
   */
  minNoteDivision?: number;
  /**
   * Pitch movement threshold, in cents, of the monophonic tracker. The
   * `'segments'` model ends one note and starts the next where the pitch moves
   * this far; `'singing'` reads it as the scale of the pitch movement kept
   * within one note. Must be finite and positive. Default `50`.
   */
  segmentationThresholdCents?: number;
  /**
   * Level mapped to velocity 1, in dBFS. A note's peak per-frame RMS of the
   * input over its span (other notes sounding then included) is taken in dBFS
   * and mapped linearly from `[velocityFloorDb, 0]` onto `[1, 127]`, clamped at
   * both ends. It does not drop notes: a note more than 36 dB under the take's
   * loudest notes, or under -80 dBFS, is dropped whatever this is set to.
   * Must be finite and negative; `0` is refused rather than read as the
   * default. Default `-48`.
   */
  velocityFloorDb?: number;
  /**
   * Gives every note this velocity instead of the mapped one; the level gate
   * still drops tails and ghosts. Must be an integer in `[1, 127]`; `0`
   * is not a MIDI velocity and is refused rather than read as "measure". Omit
   * the field to map the level, which is the default.
   */
  fixedVelocity?: number;
  /**
   * Voices a frame may carry, an integer in `[1, 64]`. Polyphonic only; a
   * monophonic request that sets it is refused. A written `0` is refused rather
   * than read as the default. Default `10`.
   */
  maxPolyphony?: number;
  /**
   * Share of a frame's peak salience below which voice extraction stops, in
   * `[0, 1]`. Polyphonic only. `0` is a real zero (no cutoff), not the default.
   * Default `0.20`.
   */
  minFramePeakRatio?: number;
  /**
   * A fade below this share of a ridge's own peak ends the note, in `[0, 1]`.
   * Polyphonic only. `0` is a real zero (never ends on a fade), not the default.
   * Default `0.10`.
   */
  minRidgePeakRatio?: number;
  /**
   * A re-strike whose level rises to this multiple of the preceding trough
   * splits the note. Polyphonic only. Greater than `1`; an explicit `0`, a value
   * in `(0, 1]` and a negative one are refused with a `RangeError`. Absent leaves the split off.
   */
  reattackRatio?: number;
  /**
   * Monophonic segmentation model. `'segments'` (the default) cuts the pitch
   * track where it moves and splits brief level dips; it is accurate on
   * instrument lines, including short notes. `'singing'` decodes notes jointly
   * over pitch bins and holds vibrato and intonation drift within a note; it
   * suits sung takes and can merge very short notes at fast tempos. Monophonic
   * only: `'singing'` with {@link polyphonic} is refused. An unknown name is a
   * `RangeError`.
   */
  monophonicModel?: 'segments' | 'singing';
  /** UMP group the events are emitted on, `0`..`15`. Default `0`. */
  group?: number;
  /** MIDI channel the events are emitted on, `0`..`15`. Default `0`. */
  channel?: number;
}

/** Request form of {@link transcribe}. */
export interface TranscribeRequest extends TranscribeOptions {
  /** Mono source audio. Must not be empty. */
  samples: Float32Array;
  /** Sample rate of {@link samples}, in Hz. */
  sampleRate: number;
  /**
   * The tempo the PPQ grid is built on, in BPM. Omit it to have it detected
   * from {@link samples}, which costs an onset/tempo pass. A value `<= 0` is a
   * `RangeError`.
   * The one setting that reads it is {@link TranscribeOptions.minNoteDivision};
   * with a grid tempo that is not the music's, give `minNoteMs` instead.
   */
  tempoBpm?: number;
}

/** Result of {@link transcribe}. */
export interface TranscribeResult {
  /**
   * Note-on / note-off pairs in canonical PPQ order, ready to hand to
   * {@link Project.setMidiEvents}. Ordering is `(ppq, note-off before
   * note-on)`, so a note-off sharing a tick with the next note's on comes
   * first. Empty when nothing was found, which is not an error.
   */
  events: ProjectMidiEvent[];
  /** Number of notes, which is half `events.length`. */
  noteCount: number;
  /** The tempo the PPQ coordinates were built on — the supplied value when one was given, the detected one otherwise. */
  tempoBpm: number;
  /**
   * Tuning the note numbers were measured against, in cents from concert A440:
   * the measured value under `referenceHz: 'auto'`, otherwise the one
   * `referenceHz` amounts to ({@link referenceHzToTuning}).
   */
  tuningCents: number;
}

/**
 * Request form of {@link Project.transcribeToClip}.
 *
 * It carries no tempo: the PPQ grid is the PROJECT's own tempo map, so a
 * project whose tempo was installed by {@link Project.autoTempo} transcribes
 * onto that map rather than onto a second, separately detected one.
 */
export interface ProjectTranscribeRequest extends TranscribeOptions {
  /** Target MIDI clip id. Its entire event list is replaced. */
  clipId: number;
  /** Mono source audio. Must not be empty. */
  samples: Float32Array;
  /** Sample rate of {@link samples}, in Hz. */
  sampleRate: number;
}
