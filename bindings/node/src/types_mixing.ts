/**
 * Mixing types: the mixer's own pan, metering and automation vocabulary,
 * and the assistant that suggests settings over it.
 *
 * The assistant suggests; it does not apply. Every entry point returns
 * parameters and never audio, so realising a suggestion is the caller's own
 * second step through {@link Mixer.fromJson}.
 */

import type { MasteringInsertName, MasteringProcessorParams } from './types_mastering.js';
import type { MixBusRole } from './mixer.js';
import type { ChannelLayout } from './types_engine.js';
import type { EqBandInput } from './types_features.js';

/**
 * How a strip's pan position is applied to a stereo pair. A number is the raw C ABI ordinal
 * (0, 1, 2).
 *
 * - `'balance'` (default): keeps the stereo image and attenuates only the channel away from the
 *   pan direction. Centre is unity under every {@link PanLaw}.
 * - `'stereoPan'` (`'stereo-pan'`): folds the pair to mono, `0.5 * (L + R)`, and places it with
 *   the law's gains. The image collapses and centre takes the law's attenuation: -3.01, -4.52,
 *   -6.02 and 0 dB for `const3dB`, `const4.5dB`, `const6dB` and `linear0dB`.
 * - `'dualPan'` (`'dual-pan'`): pans the left and right inputs independently (default -1 and +1,
 *   which passes the pair through unchanged). Each input uses the law's gains, so one panned to
 *   centre takes the same attenuation as `'stereoPan'`.
 */
export type PanMode = 'balance' | 'stereoPan' | 'stereo-pan' | 'dualPan' | 'dual-pan' | number;

/**
 * Surround pan position for a strip feeding a >2-channel bus. Phase 1 honors
 * `azimuth`/`divergence`/`lfe`; `elevation`/`distance` are reserved. All fields
 * are optional and default to a centered point source. A value outside the
 * documented range is refused with a `RangeError`, not clamped.
 */
export interface SurroundPan {
  /** -180..180 deg, 0 = front-center, positive = right. */
  azimuth?: number;
  /** Reserved (no height beds in phase 1). */
  elevation?: number;
  /** 0 = point source, 1 = spread across the front. */
  divergence?: number;
  /** 0..1 scalar send into the LFE plane. */
  lfe?: number;
  /** Reserved (focus/spread), defaults to 1. */
  distance?: number;
}

export interface MixMeterSnapshot {
  peakDbL: number;
  peakDbR: number;
  rmsDbL: number;
  rmsDbR: number;
  correlation: number;
  monoCompatWidth: number;
  monoCompatPeak: number;
  monoCompatSideRms: number;
  likelyMonoCompatible: boolean;
  momentaryLufs: number;
  shortTermLufs: number;
  integratedLufs: number;
  gainReductionDb: number;
  /**
   * Left-channel inter-sample (true) peak in dBTP, from the ITU-R BS.1770-4
   * polyphase reconstruction at 4x. A streaming measurement that runs one
   * reconstruction-filter group delay (a few samples) behind the block, so every
   * value has its whole stencil and the reading does not depend on the block
   * size. Use `meteringTruePeakDb` over the whole signal for an exact dBTP number.
   */
  truePeakDbtpL: number;
  /** Right-channel inter-sample (true) peak in dBTP. See {@link truePeakDbtpL}. */
  truePeakDbtpR: number;
  /** Maximum inter-sample peak across channels in dBTP. See {@link truePeakDbtpL}. */
  maxTruePeakDbtp: number;
  seq: number;
  /** Number of valid surround planes (5.1/7.1); 0 before the meter sees audio. */
  numChannels: number;
  /** Per-plane peak dB, length numChannels; [0]/[1] mirror peakDbL/peakDbR. */
  peakDb: number[];
  /** Per-plane RMS dB, length numChannels; [0]/[1] mirror rmsDbL/rmsDbR. */
  rmsDb: number[];
  /**
   * Per-plane true-peak dBTP, length numChannels; [0]/[1] mirror
   * {@link truePeakDbtpL}/R and carry the same streaming caveat.
   */
  truePeakDbtp: number[];
}

export interface MixResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  meters: MixMeterSnapshot[];
}

/** Mixed stereo master returned by {@link Mixer.processStereo}. */
export interface MixerProcessResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
}

/**
 * Interpolation curve for scheduled automation events
 * (see {@link Mixer.scheduleInsertAutomation}).
 */
export type AutomationCurve = 'linear' | 'exponential' | 'hold' | 'sCurve' | 's-curve';

/**
 * Pan law applied by a strip's panner. Mapped to C enum ints `0=const3dB`, `1=const4.5dB`,
 * `2=const6dB`, `3=linear0dB`.
 *
 * In `'balance'` mode centre stays unity and the law changes only the far-channel taper; in
 * `'stereoPan'` and `'dualPan'` the law's centre attenuation is audible (see {@link PanMode}).
 * A mono source is unity at centre under every law: the standalone `Mixer` always feeds two
 * planes and engine lanes are unity at centre. Only a C++ `ChannelStrip` built without
 * `mono_pan_centre_unity` makes the law change a mono strip's centre gain.
 */
export type PanLaw =
  | 'const3Db'
  | 'const4.5Db'
  | 'const6Db'
  | 'linear0Db'
  | 'const3dB'
  | 'const4.5dB'
  | 'const6dB'
  | 'linear0dB';

/**
 * Accepted pan-law name aliases. Names are normalized case-insensitively with
 * underscores treated as hyphens at runtime; the canonical four spellings in
 * {@link PanLaw} remain the preferred TypeScript values.
 */
export type PanLawName =
  | PanLaw
  | 'const-3db'
  | '-3db'
  | 'const-4.5db'
  | '-4.5db'
  | 'const-6db'
  | '-6db'
  | 'linear-0db'
  | 'linear'
  | '0db';

/** Pan-law name or raw C ABI ordinal. */
export type PanLawInput = PanLawName | number;

/**
 * Meter tap point on a strip. Mapped to the C enum ints
 * `0=preFader`, `1=postFader`.
 */
export type MeterTap = 'preFader' | 'postFader';

/** Pre/post-fader send timing (see {@link Mixer.addSend}). */
export type SendTiming = 'preFader' | 'postFader';

/**
 * A reference to a strip in the {@link Mixer}: either a 0-based strip index or
 * the strip's string id.
 */
export type StripRef = number | string;

/** Single goniometer sample from {@link Mixer.readGoniometerLatest}. */
export interface GoniometerPoint {
  left: number;
  right: number;
}

/** One analysis band, in ascending frequency order. */
export type MixAnalysisBand = 'sub' | 'low' | 'lowMid' | 'mid' | 'highMid' | 'high' | 'air';

/**
 * Coarse source taxonomy the rule-based classifier resolves to. The runtime
 * list is {@link mixingAssistantSourceClassNames}; this union mirrors it.
 */
export type MixSourceClass =
  | 'unknown'
  | 'kick'
  | 'snare'
  | 'hiHat'
  | 'tom'
  | 'cymbal'
  | 'bass'
  | 'guitar'
  | 'keys'
  | 'strings'
  | 'lead'
  | 'vocal'
  | 'backing'
  | 'percussion'
  | 'fx'
  | 'drumKit';

/** Per-band energy share of one track, keyed by {@link MixAnalysisBand}. */
export type MixBandOccupancy = Record<MixAnalysisBand, number>;

/** One track handed to `mixingAssistantSuggest`. */
export interface MixAssistantTrack {
  /** Strip id the suggestion addresses. Must be non-empty and unique across the request. */
  id: string;
  /**
   * Display name, used as a source-classification hint.
   *
   * Naming the class the classifier measured raises its confidence. Naming
   * another class switches to it when the measurement does not contradict it:
   * a class the classifier measures needs its own feature rule satisfied by the
   * track, and one it cannot measure needs the track not to have been measured
   * as a drum (a drum is never renamed `keys` or `vocal`). For the six classes
   * it cannot separate by measurement — `keys`, `strings`, `lead`, `vocal`,
   * `backing` and `fx` — the name is the only thing that can supply the class,
   * so an unnamed voice, pad or lead line comes back `unknown`. A compound name
   * states its last hint word (`'Lead Vox'` is `vocal`, `'Synth Lead'` is
   * `lead`); hint words joined by anything else (`'Strings and Keys'`) state
   * none.
   */
  name?: string;
  /** Left/mono channel. */
  left: Float32Array;
  /** Right channel; omit for a mono track. Must match `left` in length. */
  right?: Float32Array;
  /**
   * Rate of this track, in Hz. Omitted: the request's `sampleRate`. A track at
   * another rate is resampled onto the request's rate before analysis.
   */
  sampleRate?: number;
}

/**
 * One processor slot on a scene strip or bus. Discriminated by `processor`, which selects the
 * parameter names and their types from the processor catalog.
 */
export type MixSceneInsert = {
  [P in MasteringInsertName]: {
    slot: string;
    processor: P;
    /** Parameters keyed by the names the processor's catalog entry declares. */
    params: MasteringProcessorParams<P>;
    /** Present only when the insert is keyed off another strip. */
    sidechainKey?: string;
  };
}[MasteringInsertName];

/** A strip send to a destination bus. */
export interface MixSceneSend {
  id: string;
  destinationBusId: string;
  sendDb: number;
  /** Send tap point. */
  timing: SendTiming;
}

/** A strip send as given to a scene document; omitted fields take the scene parser's defaults. */
export interface MixSceneSendInput {
  id: string;
  destinationBusId: string;
  /** Send level in dB. */
  sendDb?: number;
  /** Send tap point. */
  timing?: SendTiming;
}

/** A channel strip in a mixer scene. */
export interface MixSceneStrip {
  id: string;
  inputTrimDb: number;
  faderDb: number;
  vcaOffsetDb: number;
  pan: number;
  width: number;
  muted: boolean;
  soloed: boolean;
  soloSafe: boolean;
  panMode: PanMode;
  dualPanLeft: number;
  dualPanRight: number;
  polarityInvertLeft: boolean;
  polarityInvertRight: boolean;
  panLaw: PanLaw;
  channelDelaySamples: number;
  /** Present only for a non-stereo source. */
  sourceLayout?: ChannelLayout;
  /** Present only when the surround pan has moved off its centered default. */
  surroundPan?: Required<SurroundPan>;
  /**
   * Meter configuration for this strip's pre/post taps. Present only when the
   * strip has opted out of some of its metering; absent means the full default
   * (LUFS + true peak at 4x).
   *
   * Fixed when the mixer is built from the scene: a strip's meters size their
   * buffers up front, so there is no setter for this. A full meter costs about
   * 646 KB at 48 kHz and a strip carries two, so `lufs: false` (about 83 KB per
   * meter) or `enabled: false` (about 145 KB for the whole strip instead of
   * 1.4 MB) is worth setting for strips whose meters are never read.
   */
  metering?: Required<StripMeteringOptions>;
  inserts: MixSceneInsert[];
  sends: MixSceneSend[];
  /**
   * This strip's dedicated equalizer. Present only when it carries something
   * other than the identity (enabled with no bands set), so an existing scene
   * that never touched its EQ stays byte-identical.
   */
  eq?: {
    enabled?: boolean;
    bands: EqBandInput[];
  };
}

/**
 * Meter configuration of a strip, in the scene document's `strips[].metering` object and as the
 * argument of `Mixer.addStrip`. A strip's meters size their buffers when the strip is built, so
 * this is the only place the configuration can be chosen: there is no setter. A full meter costs
 * about 646 KB at 48 kHz and a strip carries two of them.
 */
export interface StripMeteringOptions {
  /** Both meters; `false` drops them (about 145 KB for the strip instead of 1.4 MB). Default `true`. */
  enabled?: boolean;
  /** LUFS measurement; `false` takes one meter to about 83 KB. Default `true`. */
  lufs?: boolean;
  /** Inter-sample (true) peak measurement. Default `true`. */
  truePeak?: boolean;
  /**
   * True-peak oversampling factor: `4` (the ITU-R BS.1770-4 minimum) or `8`. Omit for the library
   * default (4x); any other value, `0` included, is a `RangeError`.
   */
  truePeakOversample?: number;
}

/** A bus in a mixer scene. Defaulted fields are omitted from the document. */
export interface MixSceneBus {
  id: string;
  role: MixBusRole;
  layout?: ChannelLayout;
  inputTrimDb?: number;
  width?: number;
  polarityInvertLeft?: boolean;
  polarityInvertRight?: boolean;
  /**
   * Pan, same field names/defaults/ranges as a strip's. Rejected rather than
   * stored when this bus's layout carries more than two channels: a surround
   * bus has no pan of its own.
   */
  pan?: number;
  panMode?: PanMode;
  dualPanLeft?: number;
  dualPanRight?: number;
  panLaw?: PanLaw;
  inserts: MixSceneInsert[];
  /** This bus's dedicated equalizer, applied before its inserts. See {@link MixSceneStrip.eq}. */
  eq?: {
    enabled?: boolean;
    bands: EqBandInput[];
  };
}

/**
 * A strip as given to a scene document (`Mixer.fromJson`, engine strip setters): every field but
 * `id` is optional and the scene parser fills the defaults.
 */
export type MixSceneStripInput = Partial<
  Omit<MixSceneStrip, 'id' | 'sends' | 'surroundPan' | 'metering'>
> & {
  id: string;
  sends?: MixSceneSendInput[];
  surroundPan?: SurroundPan;
  metering?: StripMeteringOptions;
};

/** A bus as given to a scene document: every field but `id` and `role` is optional. */
export type MixSceneBusInput = Partial<Omit<MixSceneBus, 'id' | 'role'>> & {
  id: string;
  role: MixBusRole;
};

/** A VCA group in a mixer scene. */
export interface MixSceneVcaGroup {
  id: string;
  gainDb: number;
  members: string[];
}

/** A VCA group as given to a scene document; omitted fields take the scene parser's defaults. */
export interface MixSceneVcaGroupInput {
  id: string;
  /** Group gain in dB. */
  gainDb?: number;
  members?: string[];
}

/** A routing edge in a mixer scene. */
export interface MixSceneConnection {
  source: string;
  destination: string;
}

/** A mixer scene document as returned by `mixingScenePreset` and `mixingAssistantSuggest`. */
export interface MixSceneDocument {
  version: number;
  strips: MixSceneStrip[];
  buses: MixSceneBus[];
  vcaGroups: MixSceneVcaGroup[];
  connections: MixSceneConnection[];
}

/**
 * A mixer scene document as given to `Mixer.fromJson` and the engine; omitted fields take the
 * scene parser's defaults. A {@link MixSceneDocument} is accepted as is.
 */
export interface MixSceneDocumentInput {
  version?: number;
  strips?: MixSceneStripInput[];
  buses?: MixSceneBusInput[];
  vcaGroups?: MixSceneVcaGroupInput[];
  connections?: MixSceneConnection[];
}

/** Per-track measurements and the class the track was resolved to. */
export interface MixAssistantTrackProfile {
  stripId: string;
  name: string;
  source: MixSourceClass;
  sourceConfidence: number;
  /**
   * False when the track could not be measured: it has no samples, a
   * non-positive sample rate, a NaN or Inf sample, is shorter than a gated
   * loudness needs, is silent, or has no energy in the analysis bands. An
   * excluded track gets no suggestions at all rather than suggestions of zero,
   * and the call still succeeds — `exclusionReason` names which it was. A
   * non-finite sample is reported as itself rather than as silence, so the
   * reason describes the buffer instead of the material.
   */
  usable: boolean;
  /** Why the track was excluded; empty when `usable` is true. */
  exclusionReason: string;
  numChannels: number;
  durationSec: number;
  /**
   * BS.1770 integrated loudness in LUFS, or `null` for a track with no gated
   * block to measure — a silent stem, or one muted before it was handed over.
   * The measurement is `-Infinity` there, which JSON has no number for, so the
   * document carries `null` rather than a finite value that would read as a
   * real level. Such a track always carries an `exclusionReason` as well.
   */
  integratedLufs: number | null;
  truePeakDbtp: number;
  crestFactorDb: number;
  spectralCentroidHz: number;
  spectralFlatness: number;
  /** Onsets per second. */
  attackPerSecond: number;
  sustainRatio: number;
  bandOccupancy: MixBandOccupancy;
}

/**
 * One masker/maskee band pair. Only informative pairs are reported, so the
 * array is far shorter than the full matrix.
 */
export interface MixBandDominance {
  masker: string;
  maskee: string;
  band: MixAnalysisBand;
  ratio: number;
  validFrames: number;
}

/** A related track pair and the time/polarity relationship measured between them. */
export interface MixTrackAlignment {
  reference: string;
  target: string;
  lagSamples: number;
  correlation: number;
  polarityOpposed: boolean;
}

/** A band carrying more simultaneous sources than the image comfortably holds. */
export interface MixCrowdedBand {
  band: MixAnalysisBand;
  crowding: number;
}

/** A track whose stereo image is likely to collapse in mono. */
export interface MixMonoRisk {
  stripId: string;
  correlation: number;
  width: number;
  wideLowEnd: boolean;
}

/** Cross-track measurements the decisions were made from. */
export interface MixAssistantMixProfile {
  trackCount: number;
  bandDominance: MixBandDominance[];
  alignment: MixTrackAlignment[];
  crowdedBands: MixCrowdedBand[];
  monoRisks: MixMonoRisk[];
}

/** What `mixingAssistantSuggest` produces. */
export interface MixAssistantResult {
  /** The suggested scene. Nothing has been applied; feed it to {@link Mixer}. */
  scene: MixSceneDocument;
  /** One entry per input track, in input order. */
  tracks: MixAssistantTrackProfile[];
  /** Cross-track measurements behind the suggestion. */
  mix: MixAssistantMixProfile;
  /**
   * Human-readable reasons in the order the changes were applied, so reading
   * top to bottom retraces how the scene was built. Empty when every decision
   * domain is disabled or no track is usable.
   */
  explanation: string[];
}
