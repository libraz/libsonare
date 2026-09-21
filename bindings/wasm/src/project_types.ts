// Type-only, so the erased import adds no runtime edge to the codes module.
import type { PROJECT_AUTOMATION_CURVE_VALUES } from './codes';
import type { Project } from './project_class';

// ============================================================================
// Headless DAW Project
// ============================================================================

/**
 * Expected project ABI version. Mirrors `SONARE_PROJECT_ABI_VERSION` in
 * `src/sonare_c_project.h`; checked against {@link projectAbiVersion} to detect
 * a WASM build whose flat project POD layout has drifted from this wrapper.
 */
export const EXPECTED_PROJECT_ABI_VERSION = 2;

/** Render options for {@link Project.bounce}. All fields are optional. */
export interface ProjectBounceOptions {
  /** Render length in frames at the output sample rate. */
  totalFrames?: number;
  /** Render block size; <= 0 uses the engine default (128). */
  blockSize?: number;
  /** Output channel count; <= 0 uses the default (2). */
  numChannels?: number;
  /** Output sample rate; <= 0 uses the project sample rate. */
  sampleRate?: number;
  /** Host-instrument PDC (latency) fed to the compiler. */
  instrumentLatencySamples?: number;
}

/** One decoded output from a host-owned source-separation model. */
export interface ExternalSeparatedStem {
  name: string;
  /** Optional host metadata; it does not change DSP. */
  role?: string;
  layout: 'mono' | 'stereo' | 1 | 2;
  planarSamples: Float32Array[];
  /** Absolute project-rate frame at which this stem begins. */
  startFrame?: number;
}

/** Request for {@link Project.importExternalStems}. */
export interface ExternalSeparatedStemImportRequest {
  sampleRate: number;
  stems: ExternalSeparatedStem[];
}

/** Ids of the normal tracks and clips created by the import. */
export interface ExternalSeparatedStemImportResult {
  trackIds: number[];
  clipIds: number[];
}

/**
 * Marker kind ordinals. Mirrors `SonareMarkerKind` in `src/sonare_c_types.h`;
 * the values are part of the ABI and must not be renumbered.
 */
export const MarkerKind = {
  marker: 0,
  text: 1,
  lyric: 2,
  cuePoint: 3,
  keySignature: 4,
} as const;

/** A project timeline marker with its kind and (for key signatures) the key. */
export interface ProjectMarker {
  /** Stable marker id (0 when allocating a new id via {@link Project.setMarkerEx}). */
  id: number;
  /** Marker position in PPQ (quarter notes). */
  ppq: number;
  /** Marker label. */
  name?: string;
  /** {@link MarkerKind} ordinal (default 0 = marker). */
  kind?: number;
  /** Key signature only: -7..7 (sharps positive). */
  keyFifths?: number;
  /** Key signature only: false = major, true = minor. */
  keyMinor?: boolean;
}

/** Read-only stored project track returned by {@link Project.trackByIndex}. */
export interface ProjectTrack {
  id: number;
  kind: number;
  midiDestinationId: number;
  gain: number;
  pan: number;
  mute: boolean;
  solo: boolean;
  name: string;
}

/** Read-only stored project clip returned by {@link Project.clipByIndex}. */
export interface ProjectClip {
  id: number;
  trackId: number;
  sourceId: number;
  sourceKind: number;
  startPpq: number;
  lengthPpq: number;
  sourceOffsetPpq: number;
  gain: number;
  loopMode: number;
  loopLengthPpq: number;
}

/** Read-only stored project source returned by {@link Project.sourceByIndex}. */
export interface ProjectSource {
  id: number;
  kind: number;
  channelCount: number;
  storageHandleId: number;
  sampleRateHint: number;
  nameOrUri: string;
  /** Owning content hash for audio sources; empty for MIDI sources. */
  contentHash: string;
  /** External source-separation role for audio sources; empty for MIDI sources. */
  externalStemRole: string;
}

/** Clip fade-curve for {@link Project.setClipFade}. */
export type ProjectFadeCurve =
  | 'linear'
  | 'equal-power'
  | 'equal_power'
  | 'equalPower'
  | 'equalpower'
  | 'exponential'
  | 'exp'
  | 'logarithmic'
  | 'log'
  | 0
  | 1
  | 2
  | 3;

/** One clip fade region for {@link Project.setClipFade}. */
export interface ProjectClipFade {
  /** Fade length in PPQ (>= 0; 0 = no fade). */
  lengthPpq?: number;
  /** Fade curve (default `'linear'`). */
  curve?: ProjectFadeCurve;
}

/** One alternate take for {@link Project.setClipTakes}. */
export interface ProjectClipTake {
  id: number;
  sourceId?: number;
  sourceOffsetPpq?: number;
  name?: string;
}

/** One comp segment for {@link Project.setClipCompSegments}. */
export interface ProjectClipCompSegment {
  startPpq: number;
  endPpq: number;
  takeId?: number;
  /** Equal-power crossfade from the preceding segment, in PPQ (default 0 = butt join). */
  crossfadePpq?: number;
}

/** Descriptor for {@link Project.addLoopRecordingTakes}. */
export interface ProjectLoopRecordingDesc {
  trackId: number;
  startPpq?: number;
  loopLengthPpq: number;
  audio: Float32Array;
  audioChannels?: number;
  audioSampleRate?: number;
}

/** Result returned by {@link Project.addLoopRecordingTakes}. */
export interface ProjectLoopRecordingResult {
  clipId: number;
  takeCount: number;
}

/** Clip loop mode for {@link Project.setClipLoop}. */
export type ProjectLoopMode = 'off' | 'loop' | 0 | 1;
/**
 * How a clip follows its warp map.
 *
 * - `'off'` — no warping.
 * - `'repitch'` — resample along the map, so a rate change moves the pitch with
 *   the timing (tape-style).
 * - `'tempo-sync'` — control-thread bake against the tempo map.
 * - `'time-stretch'` — realtime pitch-preserving stretch. Follows the same map
 *   as `'repitch'` but overlap-adds instead of resampling, so a new anchor set
 *   takes effect from the next block with no re-bake. Falls back to `'repitch'`
 *   behaviour when the stretcher's voice budget is exhausted.
 */
export type ProjectWarpMode = 'off' | 'repitch' | 'tempo-sync' | 'time-stretch' | 0 | 1 | 2 | 3;

/**
 * Automation breakpoint interpolation for {@link ProjectAutomationPoint}.
 *
 * `'s-curve'` is the canonical spelling, matching the Node engine and the mixer
 * automation types. The legacy `'scurve'` remains accepted for compatibility.
 * The spellings are derived from the resolver's own table, so the documented
 * set and the accepted set cannot drift apart.
 */
export type ProjectAutomationCurve = 0 | 1 | 2 | 3 | keyof typeof PROJECT_AUTOMATION_CURVE_VALUES;

/** One automation breakpoint accepted by the automation-lane edit ops. */
export interface ProjectAutomationPoint {
  /** Breakpoint position in PPQ. */
  ppq: number;
  /** Breakpoint value. */
  value: number;
  /** Curve to the next breakpoint (default `'linear'`). */
  curve?: ProjectAutomationCurve;
  /** Alias of {@link ProjectAutomationPoint.curve}; `curve` wins when both are set. */
  curveToNext?: ProjectAutomationCurve;
}

/**
 * Persistent target classification for an automation lane.
 *
 * The numeric ordinals mirror `SonareAutomationTargetKind`; the string names
 * are the public WASM spellings accepted by the project facade.
 */
export const AutomationTargetKind = {
  opaque: 0,
  trackFaderDb: 1,
  trackPan: 2,
} as const;

/** Accepted automation target kind name or ABI ordinal. */
export type ProjectAutomationTargetKind = 'opaque' | 'track-fader-db' | 'track-pan' | 0 | 1 | 2;

/** Numeric aliases matching the C-ABI enum names. */
export const PROJECT_AUTOMATION_TARGET_OPAQUE = AutomationTargetKind.opaque;
export const PROJECT_AUTOMATION_TARGET_TRACK_FADER_DB = AutomationTargetKind.trackFaderDb;
export const PROJECT_AUTOMATION_TARGET_TRACK_PAN = AutomationTargetKind.trackPan;

/** Automation-lane descriptor for {@link Project.addAutomationLane}. */
export interface ProjectAutomationLaneDesc {
  /** Host-defined, non-zero id of the parameter the lane drives (zero is reserved). */
  targetParamId: number;
  /** Breakpoints (stored verbatim). */
  points: ReadonlyArray<ProjectAutomationPoint>;
  /** Optional persistent target classification; omission retains the legacy opaque route. */
  targetKind?: ProjectAutomationTargetKind;
}

/** One tempo segment for {@link Project.setTempoSegments}. */
export interface ProjectTempoSegment {
  /** Segment start in PPQ. */
  startPpq: number;
  /** Tempo in beats per minute at the segment start. */
  bpm: number;
  /**
   * Derived segment start in samples. Accepted for compatibility and ignored on
   * input; never returned, because a project stores musical positions only and
   * sample positions are derived when it is compiled.
   */
  startSample?: number;
  /** Optional ramp end tempo in BPM (0 = constant tempo over the segment). */
  endBpm?: number;
}

/** One time-signature segment for {@link Project.setTimeSignatures}. */
export interface ProjectTimeSignatureSegment {
  /** Segment start in PPQ. */
  startPpq: number;
  /** Beats per bar (time-signature numerator). */
  numerator: number;
  /** Beat unit (time-signature denominator, e.g. 4 or 8). */
  denominator: number;
}

/** A ranked primary/half/double tempo hypothesis returned by {@link Project.analyzeTempo}. */
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

export interface ProjectTempoCandidate {
  bpm: number;
  confidence: number;
  label: 'primary' | 'half' | 'double';
  timeSignatureCount: number;
  timeSignature: ProjectTimeSignatureSegment;
}

/** Key segment for {@link Project.annotateKeys}. */
export interface ProjectKeySegment {
  startPpq: number;
  endPpq: number;
  /** Tonic pitch class 0..11 (C=0) or 255 for unknown. */
  tonicPc?: number;
  /** KeyMode ordinal (0 unknown, 1 major, 2 minor, 3 dorian, ...). */
  mode?: number;
}

/** Chord symbol for {@link Project.annotateChords}. */
export interface ProjectChordSymbol {
  startPpq: number;
  endPpq: number;
  /** Root pitch class 0..11 (C=0) or 255 for unknown. */
  rootPc?: number;
  /** ChordQuality ordinal (0 unknown, 1 major, 2 minor, ...). */
  quality?: number;
  /** Extension semitone offsets (up to 8). */
  extensions?: ReadonlyArray<number>;
  /** Slash-bass pitch class 0..11 or 255 for none. */
  slashBassPc?: number;
  /** Optional roman-numeral label. */
  romanNumeral?: string;
  /** True at a modulation boundary. */
  modulationBoundary?: boolean;
}

/** Descriptor accepted by {@link Project.setAssistSidecar}. */
export interface ProjectAssistSidecarInput {
  /** Non-empty module id key. */
  moduleId: string;
  /** Module-defined schema version. Defaults to `0`. */
  schemaVersion?: number;
  /** Target track id (`0` = project scope). Defaults to `0`. */
  targetTrackId?: number;
  /** Region start in PPQ. Defaults to `0`. */
  regionStartPpq?: number;
  /** Region end in PPQ. Defaults to `0`. */
  regionEndPpq?: number;
  /** Opaque module-owned payload bytes. Defaults to an empty payload. */
  payload?: Uint8Array;
}

/** Assist sidecar snapshot returned by {@link Project.getAssistSidecar}. */
export interface ProjectAssistSidecar {
  moduleId: string;
  schemaVersion: number;
  targetTrackId: number;
  regionStartPpq: number;
  regionEndPpq: number;
  payload: Uint8Array;
}

/** Track kind for {@link Project.addTrack}. */
export type ProjectTrackKind = 'audio' | 'midi' | 'aux' | 0 | 1 | 2;

/** Descriptor for {@link Project.addTrack}. */
export interface ProjectTrackDesc {
  kind?: ProjectTrackKind;
  name?: string;
}

export interface ProjectWarpAnchor {
  warpSample: number;
  sourceSample: number;
}

export interface ProjectWarpMapDesc {
  id: number;
  name?: string;
  anchors: ProjectWarpAnchor[];
}

/**
 * Canonical request form for {@link alignTakeToReference}.
 *
 * Both resolution fields are optional and omitting one takes the library value.
 * A `0` is **refused** rather than read as a request for the default: neither
 * field has a meaning at 0, so omission is already how you ask for the default,
 * and a substituted value is indistinguishable downstream from one you chose.
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
   * Sample rate of **both** buffers in Hz, `[8000, 384000]`. Resample first if
   * they differ: the alignment does no rate conversion.
   */
  sampleRate: number;
  /**
   * Chroma hop in samples, which sets the time resolution of the anchors — a
   * smaller hop measures more frames and yields more anchors. Default `512`;
   * must be a positive integer.
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
  /** Chroma frames the reference produced. */
  referenceFrames: number;
  /**
   * Chroma frames the take produced. Its ratio to `referenceFrames` is the
   * overall rate difference the anchors encode.
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
   * whose source is that take needs.
   */
  anchors: ProjectWarpAnchor[];
  /** How well the alignment was conditioned. */
  alignment: TakeAlignment;
}

/** Descriptor for {@link Project.addClip}. */
export interface ProjectClipDesc {
  trackId: number;
  isMidi?: boolean;
  startPpq?: number;
  lengthPpq: number;
  sourceOffsetPpq?: number;
  gain?: number;
  audio?: Float32Array;
  audioChannels?: number;
  /**
   * Sample rate of `audio` in Hz. Required whenever `audio` is supplied, and
   * must be in `[8000, 384000]`: omitting it sends 0, which the native side
   * rejects. Ignored for a metadata-only clip.
   */
  audioSampleRate?: number;
  sourceUri?: string;
}

/** Result returned by {@link Project.addMidiClip}. */
export interface ProjectMidiClipResult {
  trackId: number;
  clipId: number;
}

/** Flat MIDI event accepted by {@link Project.setMidiEvents}. */
/**
 * One MIDI event in a clip's list: a position plus the first two UMP words.
 *
 * @remarks
 * Channel-voice messages only. A clip's SysEx payloads live beside the event
 * list and are reached by a handle this type does not carry — they survive
 * {@link Project.importSmf}, {@link Project.exportSmf} and project
 * serialization, and are destroyed by {@link Project.setMidiEvents}.
 */
export interface ProjectMidiEvent {
  ppq: number;
  data0: number;
  data1?: number;
}

/** Options for {@link Project.midiRouteEvents}. `null`/omitted filter fields mean any/no remap. */
export interface ProjectMidiRouteConfig {
  filterGroup?: number | null;
  filterChannel?: number | null;
  remapChannel?: number | null;
  thru?: boolean;
}

/** Result of {@link Project.midiRouteEvents}. */
export interface ProjectMidiRouteResult {
  events: ProjectMidiEvent[];
  overflowed: boolean;
  overflowCount: number;
}

export type ProjectMidiCcBindingKind = 0 | 1 | 2 | 3;

/** Options for {@link Project.midiCcLearn}. All fields are optional. */
export interface MidiCcLearnOptions {
  /** Lower end of the mapped parameter range. Default `0`. */
  minValue?: number;
  /** Upper end of the mapped parameter range. Default `1`. */
  maxValue?: number;
  /** Minimum normalized CC movement required to learn a binding. Default `0`. */
  minMovement?: number;
}

/** MIDI CC <-> automation binding descriptor used by CC learn/conversion helpers. */
export interface ProjectMidiCcBinding {
  ccNumber: number;
  /** MIDI channel 0..15, or 255 for any channel. Omit for the any-channel sentinel `255`. */
  channel?: number;
  /** 0 = 7-bit CC, 1 = 14-bit CC, 2 = RPN, 3 = NRPN. Default `0`. */
  kind?: ProjectMidiCcBindingKind;
  ccLsbNumber?: number;
  selectorMsb?: number;
  selectorLsb?: number;
  paramId: number;
  /** Lower end of the mapped parameter range. Default `0`. */
  minValue?: number;
  /** Upper end of the mapped parameter range. Default `1`. */
  maxValue?: number;
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

/**
 * Request form of {@link Project.bakeMidiFx}. The positional
 * `(clipId, configJson)` call stays supported and normalizes to this shape.
 */
export interface ProjectMidiFxBakeRequest {
  /** Target MIDI clip id. */
  clipId: number;
  /** MIDI-FX chain configuration as JSON. */
  configJson: string;
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
  /** MIDI-FX chain configuration as JSON. */
  configJson: string;
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
 * `fmin`, `fmax`, `minNoteMs`, `segmentationThresholdCents`,
 * `velocityFloorDb`, `fixedVelocity`): omitting the field is how you ask for
 * the default, so a `0` you wrote is a value, and it is out of domain. Only
 * `group` and `channel` accept `0` — there it is a value you can mean.
 */
export interface TranscribeOptions {
  /**
   * `true` reads the multi-F0 chain, which finds overlapping notes at the cost
   * of a full STFT and a mask per tracked ridge. `false` (the default) reads
   * pYIN cut into notes, which follows one line at a time.
   */
  polyphonic?: boolean;
  /**
   * Tuning reference in Hz the MIDI note numbers are measured against.
   * Default `440`; must be finite and positive.
   *
   * **It is not measured for you.** A take recorded away from A440 should have
   * its reference measured first — run `pitchPyin` and feed its F0 array to
   * `pitchTuning` — and the answer passed in here. Measuring it internally
   * would track the pitch twice and hide which of the two answers a wrong
   * transcription came from.
   */
  referenceHz?: number;
  /**
   * Monophonic tracker range in Hz. Defaults `65` and `2093`; both must be
   * finite and positive, and `fmax` must exceed `fmin`. The polyphonic chain
   * sets its own range and reads neither.
   */
  fmin?: number;
  /** Upper end of the monophonic tracker range in Hz. Default `2093`. */
  fmax?: number;
  /** Shortest span kept as a note, in milliseconds. Default `30`; must be positive. */
  minNoteMs?: number;
  /**
   * Pitch movement, in cents, that ends one note and starts the next.
   * Default `50`; must be positive.
   */
  segmentationThresholdCents?: number;
  /**
   * Level mapped to velocity 1, in dBFS. Default `-48`; **must be negative**.
   *
   * A note's peak per-frame RMS is taken in dBFS and mapped linearly from
   * `[velocityFloorDb, 0]` onto `[1, 127]`, clamped at both ends.
   */
  velocityFloorDb?: number;
  /**
   * An integer in `[1, 127]` gives every note that velocity and skips the level
   * measurement. **Omit the field to measure** — `0` is refused, because it is
   * not a MIDI velocity and omission already says "measure".
   */
  fixedVelocity?: number;
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

/** One compile diagnostic (mirrors SonareProjectDiagnostic). */
export interface ProjectDiagnostic {
  code: number;
  /** 0 = error, 1 = warning. */
  severity: number;
  /** Affected clip / track / source id (0 = n/a). */
  targetId: number;
  /** Human-readable message for this diagnostic. */
  message: string;
}

/** Diagnostics summary returned by {@link Project.compile}. */
export interface ProjectCompileResult {
  /** Number of diagnostics surfaced by the compile. Kept for backward compatibility. */
  diagnosticCount: number;
  /** True when compilation produced a renderable timeline (no error diagnostics). */
  hasTimeline: boolean;
  /** Newline-joined human-readable detail of every diagnostic. */
  messages: string;
  diagnostics: ProjectDiagnostic[];
}

export interface ProjectDeserializeResult {
  project: Project;
  diagnostics: string;
}
