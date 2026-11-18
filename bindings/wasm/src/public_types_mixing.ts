import type { ChannelLayout } from './audio.js';
import type { MixBusRole } from './mixer.js';
import type { MasteringInsertName, MasteringProcessorParams } from './public_types_mastering.js';
import type { EqBandInput } from './public_types_realtime.js';

/**
 * How a strip's pan position is applied to a stereo pair.
 *
 * - `'balance'` (default): keeps the stereo image and attenuates only the channel away from the
 *   pan direction. Centre is unity under every {@link PanLaw}.
 * - `'stereoPan'`: folds the pair to mono, `0.5 * (L + R)`, and places it with
 *   the law's gains. The image collapses and centre takes the law's attenuation: -3.01, -4.52,
 *   -6.02 and 0 dB for `const3Db`, `const4.5Db`, `const6Db` and `linear0Db`.
 * - `'dualPan'`: pans the left and right inputs independently (default -1 and +1,
 *   which passes the pair through unchanged). Each input uses the law's gains, so one panned to
 *   centre takes the same attenuation as `'stereoPan'`.
 */
export type PanMode = 'balance' | 'stereoPan' | 'dualPan';

/**
 * Surround pan position for a strip feeding a >2-channel bus. Phase 1 honors
 * `azimuth`/`divergence`/`lfe`; `elevation`/`distance` are reserved. All fields
 * are optional and default to a centered point source. A value outside the stated
 * range is refused with a RangeError, never clamped.
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

/** Master-output meter reading for a mix. */
export interface MixMeterSnapshot {
  /** Left-channel sample peak in dBFS. */
  peakDbL: number;
  /** Right-channel sample peak in dBFS. */
  peakDbR: number;
  /** Left-channel RMS level in dBFS. */
  rmsDbL: number;
  /** Right-channel RMS level in dBFS. */
  rmsDbR: number;
  /** Inter-channel correlation in `[-1, 1]`. */
  correlation: number;
  /** Stereo width measured by the mono-compatibility check. */
  monoCompatWidth: number;
  /** Peak level of the mono sum used by the mono-compatibility check. */
  monoCompatPeak: number;
  /** RMS level of the side signal used by the mono-compatibility check. */
  monoCompatSideRms: number;
  /** True when the mono-compatibility check finds no collapse risk. */
  likelyMonoCompatible: boolean;
  /** Momentary loudness in LUFS. */
  momentaryLufs: number;
  /** Short-term loudness in LUFS. */
  shortTermLufs: number;
  /** Integrated loudness in LUFS. */
  integratedLufs: number;
  /** Gain reduction applied by the dynamics stages, in dB. */
  gainReductionDb: number;
  /**
   * Left-channel inter-sample (true) peak in dBTP, from the ITU-R BS.1770-4
   * polyphase reconstruction at 4x. A streaming measurement that runs one
   * reconstruction-filter group delay (a few samples) behind the block, so every
   * value has its whole stencil and the reading does not depend on the block
   * size. Use `meteringTruePeakDbtp` over the whole signal for an exact dBTP number.
   */
  truePeakDbtpL: number;
  /** Right-channel inter-sample (true) peak in dBTP. See {@link truePeakDbtpL}. */
  truePeakDbtpR: number;
  /** Maximum inter-sample peak across channels in dBTP. See {@link truePeakDbtpL}. */
  maxTruePeakDbtp: number;
  /** Sequence counter; increments each time a new snapshot is published. */
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

/** A mixed stereo master and its meter readings. */
export interface MixResult {
  /** Left channel of the mixed master. */
  left: Float32Array;
  /** Right channel of the mixed master. */
  right: Float32Array;
  /** Sample rate of the mixed master in Hz. */
  sampleRate: number;
  /** Meter readings taken while mixing. */
  meters: MixMeterSnapshot[];
}

/** Mixed stereo master returned by {@link Mixer.processStereo}. */
export interface MixerProcessResult {
  /** Left channel of the mixed master. */
  left: Float32Array;
  /** Right channel of the mixed master. */
  right: Float32Array;
  /** Sample rate of the mixed master in Hz. */
  sampleRate: number;
}

/**
 * Interpolation curve for scheduled automation events
 * (see {@link Mixer.scheduleInsertAutomation}).
 */
export type AutomationCurve = 'linear' | 'exponential' | 'hold' | 'sCurve';

/**
 * Pan law applied when computing left/right gains from a pan position
 * (see {@link Mixer.setPanLaw}). Mapped to C enum ints `0=const3Db`, `1=const4.5Db`,
 * `2=const6Db`, `3=linear0Db`.
 *
 * In `'balance'` mode centre stays unity and the law changes only the far-channel taper; in
 * `'stereoPan'` and `'dualPan'` the law's centre attenuation is audible (see {@link PanMode}).
 * A mono source is unity at centre under every law: the standalone `Mixer` always feeds two
 * planes and engine lanes are unity at centre. Only a C++ `ChannelStrip` built without
 * `mono_pan_centre_unity` makes the law change a mono strip's centre gain.
 */
export type PanLaw = 'const3Db' | 'const4.5Db' | 'const6Db' | 'linear0Db';

/**
 * Meter tap point for reading a strip's meter snapshot
 * (see {@link Mixer.meterTap} and {@link Mixer.stripMeter}).
 */
export type MeterTap = 'preFader' | 'postFader';

/** Pre/post-fader send timing (see {@link Mixer.addSend}). */
export type SendTiming = 'preFader' | 'postFader';

/**
 * Source a bus or master insert's sidechain key is taken from: a track lane's
 * post-strip signal, or a bus's signal before its `gainDb`.
 */
export type SidechainSourceKind = 'track' | 'bus';

/** A single goniometer (left/right) sample returned by {@link Mixer.readGoniometerLatest}. */
export interface GoniometerPoint {
  /** Left-channel sample value. */
  left: number;
  /** Right-channel sample value. */
  right: number;
}

/** Source taxonomy the assistant's classifier resolves to, in native ordinal order. */
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

/** One analysis band of the mixing assistant's shared 7-band split. */
export type MixAnalysisBand = 'sub' | 'low' | 'lowMid' | 'mid' | 'highMid' | 'high' | 'air';

/** Share of a track's energy in each analysis band; sums to 1, or to 0 when silent. */
export type MixBandOccupancy = Record<MixAnalysisBand, number>;

/**
 * One track handed to `mixingAssistantSuggest`.
 *
 * Planar and per-track: tracks in one call may differ in length, and each is
 * mono (one channel) or stereo (two channels of equal length) independently of
 * the others.
 */
export interface MixAssistantTrack {
  /** Strip id the suggestion is written against. Must be unique and non-empty. */
  id: string;
  /**
   * Optional display name, used as a source-classification hint.
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
  /** One plane for a mono track, two (left, right) of equal length for a stereo one. */
  channels: Float32Array[];
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
    /** Where the insert sits relative to the fader: `'pre'` before it, `'post'` after it. */
    slot: 'pre' | 'post';
    /** Processor catalog name; selects the `params` keys. */
    processor: P;
    /** Parameters keyed by the names the processor's catalog entry declares. */
    params: MasteringProcessorParams<P>;
    /** Present only when the insert is keyed off another strip. */
    sidechainKey?: string;
  };
}[MasteringInsertName];

/** A strip send to a destination bus. */
export interface MixSceneSend {
  /** Send id. */
  id: string;
  /** Id of the bus the send feeds. */
  destinationBusId: string;
  /** Send level in dB. */
  sendDb: number;
  /** Send tap point. */
  timing: SendTiming;
}

/** A strip send as given to a scene document; omitted fields take the scene parser's defaults. */
export interface MixSceneSendInput {
  /** Send id. */
  id: string;
  /** Id of the bus the send feeds. */
  destinationBusId: string;
  /** Send level in dB. */
  sendDb?: number;
  /** Send tap point. */
  timing?: SendTiming;
}

/** A channel strip in a mixer scene. */
export interface MixSceneStrip {
  /** Strip id, unique across the scene. */
  id: string;
  /** Input trim in dB, applied ahead of the inserts. */
  inputTrimDb: number;
  /** Fader level in dB. */
  faderDb: number;
  /** Direct VCA trim in dB. */
  vcaOffsetDb: number;
  /** Pan position in `[-1, 1]`; -1 is full left. */
  pan: number;
  /** Stereo width in `[0, 2]`: 0 mono, 1 unchanged, 2 widest. */
  width: number;
  /** True when the strip is muted. */
  muted: boolean;
  /** True when the strip is soloed. */
  soloed: boolean;
  /** True when the strip stays audible while another strip is soloed. */
  soloSafe: boolean;
  /** How the pan position is applied; see {@link PanMode}. */
  panMode: PanMode;
  /** Left input's pan position in `'dualPan'` mode, `[-1, 1]`. */
  dualPanLeft: number;
  /** Right input's pan position in `'dualPan'` mode, `[-1, 1]`. */
  dualPanRight: number;
  /** True when the left channel polarity is inverted. */
  polarityInvertLeft: boolean;
  /** True when the right channel polarity is inverted. */
  polarityInvertRight: boolean;
  /** Pan law applied by the panner; see {@link PanLaw}. */
  panLaw: PanLaw;
  /** Delay applied to the strip's channels, in samples. */
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
  /** Processor slots, in signal order. */
  inserts: MixSceneInsert[];
  /** Sends to destination buses. */
  sends: MixSceneSend[];
  /**
   * This strip's dedicated equalizer. Present only when it carries something
   * other than the identity (enabled with no bands set), so an existing scene
   * that never touched its EQ stays byte-identical.
   */
  eq?: {
    /** False bypasses the equalizer and keeps its bands. Default `true`. */
    enabled?: boolean;
    /** The equalizer's bands. */
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
  /** Bus id, unique across the scene. */
  id: string;
  /** Role of the bus. */
  role: MixBusRole;
  /** Channel layout of the bus. */
  layout?: ChannelLayout;
  /** Input trim in dB, applied ahead of the inserts. */
  inputTrimDb?: number;
  /** Stereo width in `[0, 2]`: 0 mono, 1 unchanged, 2 widest. */
  width?: number;
  /** True when the left channel polarity is inverted. */
  polarityInvertLeft?: boolean;
  /** True when the right channel polarity is inverted. */
  polarityInvertRight?: boolean;
  /**
   * Pan, same field names/defaults/ranges as a strip's. Rejected rather than
   * stored when this bus's layout carries more than two channels: a surround
   * bus has no pan of its own.
   */
  pan?: number;
  /** How the pan position is applied; see {@link PanMode}. */
  panMode?: PanMode;
  /** Left input's pan position in `'dualPan'` mode, `[-1, 1]`. */
  dualPanLeft?: number;
  /** Right input's pan position in `'dualPan'` mode, `[-1, 1]`. */
  dualPanRight?: number;
  /** Pan law applied by the panner; see {@link PanLaw}. */
  panLaw?: PanLaw;
  /** Processor slots, in signal order. */
  inserts: MixSceneInsert[];
  /** This bus's dedicated equalizer, applied before its inserts. See {@link MixSceneStrip.eq}. */
  eq?: {
    /** False bypasses the equalizer and keeps its bands. Default `true`. */
    enabled?: boolean;
    /** The equalizer's bands. */
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
  /** Strip id, unique across the scene. */
  id: string;
  /** Sends to destination buses; omitted fields take the scene parser defaults. */
  sends?: MixSceneSendInput[];
  /** Surround pan position for a strip feeding a surround bus. */
  surroundPan?: SurroundPan;
  /** Meter configuration; see {@link StripMeteringOptions}. */
  metering?: StripMeteringOptions;
};

/** A bus as given to a scene document: every field but `id` and `role` is optional. */
export type MixSceneBusInput = Partial<Omit<MixSceneBus, 'id' | 'role'>> & {
  /** Bus id, unique across the scene. */
  id: string;
  /** Role of the bus. */
  role: MixBusRole;
};

/** A VCA group in a mixer scene. */
export interface MixSceneVcaGroup {
  /** VCA group id. */
  id: string;
  /** Group gain in dB. */
  gainDb: number;
  /** Ids of the member strips. */
  members: string[];
}

/** A VCA group as given to a scene document; omitted fields take the scene parser's defaults. */
export interface MixSceneVcaGroupInput {
  /** VCA group id. */
  id: string;
  /** Group gain in dB. */
  gainDb?: number;
  /** Ids of the member strips. */
  members?: string[];
}

/** A routing edge in a mixer scene. */
export interface MixSceneConnection {
  /** Id of the source strip or bus. */
  source: string;
  /** Id of the destination bus. */
  destination: string;
}

/** A mixer scene document as returned by `mixingScenePreset` and `mixingAssistantSuggest`. */
export interface MixSceneDocument {
  /** Scene document schema version. */
  version: number;
  /** Channel strips in the scene. */
  strips: MixSceneStrip[];
  /** Buses in the scene. */
  buses: MixSceneBus[];
  /** VCA groups in the scene. */
  vcaGroups: MixSceneVcaGroup[];
  /** Routing edges between strips and buses. */
  connections: MixSceneConnection[];
}

/**
 * A mixer scene document as given to `Mixer.fromJson` and the engine; omitted fields take the
 * scene parser's defaults. A {@link MixSceneDocument} is accepted as is.
 */
export interface MixSceneDocumentInput {
  /** Scene document schema version. */
  version?: number;
  /** Channel strips in the scene. */
  strips?: MixSceneStripInput[];
  /** Buses in the scene. */
  buses?: MixSceneBusInput[];
  /** VCA groups in the scene. */
  vcaGroups?: MixSceneVcaGroupInput[];
  /** Routing edges between strips and buses. */
  connections?: MixSceneConnection[];
}

/** What the assistant measured about one input track. */
export interface MixAssistantTrackProfile {
  /** Id of the strip the track is addressed by. */
  stripId: string;
  /** Display name the track was given. */
  name: string;
  /** Source class identifier; one of {@link MixSourceClass}. */
  source: MixSourceClass;
  /** Classifier confidence in `[0, 1]`. */
  sourceConfidence: number;
  /**
   * False when the track could not be measured: it has no samples, a
   * non-positive sample rate, a NaN or Inf sample, is shorter than a gated
   * loudness needs, is silent, or has no energy in the analysis bands. An
   * excluded track gets no suggestions at all rather than suggestions of zero,
   * and the call still succeeds — {@link exclusionReason} names which it was.
   * A non-finite sample is reported as itself rather than as silence, so the
   * reason describes the buffer instead of the material.
   */
  usable: boolean;
  /** Why the track was excluded; empty when {@link usable} is true. */
  exclusionReason: string;
  /** Channel count of the track: 1 for mono, 2 for stereo. */
  numChannels: number;
  /** Track duration in seconds. */
  durationSec: number;
  /**
   * BS.1770 integrated loudness in LUFS; absent for a track with no gated
   * block to measure — a silent stem, or one muted before it was handed over —
   * rather than a finite value that would read as a real level. Such a track
   * always carries an `exclusionReason` as well.
   */
  integratedLufs?: number;
  /** True peak in dBTP. */
  truePeakDbtp: number;
  /** Crest factor (peak over RMS) in dB. */
  crestFactorDb: number;
  /** Spectral centroid in Hz. */
  spectralCentroidHz: number;
  /** Spectral flatness; near 0 is tonal, near 1 is noise-like. */
  spectralFlatness: number;
  /** Onsets per second. */
  attackPerSecond: number;
  /** Sustain ratio of the track's envelope. */
  sustainRatio: number;
  /** Share of the track's energy in each analysis band. */
  bandOccupancy: MixBandOccupancy;
}

/** One informative band-masking relationship between two tracks. */
export interface MixBandDominance {
  /** Strip id of the masking track. */
  masker: string;
  /** Strip id of the masked track. */
  maskee: string;
  /** Analysis band the pair is measured in. */
  band: MixAnalysisBand;
  /** Masker's mean energy share in `[0, 1]`; 0.5 means equal band energy. */
  ratio: number;
  /** Frames where both tracks exceeded the band energy floor. */
  validFrames: number;
}

/** A time/polarity relationship between two related tracks. */
export interface MixTrackAlignment {
  /** Strip id of the reference track. */
  reference: string;
  /** Strip id of the track measured against the reference. */
  target: string;
  /** Lag in samples that aligns the target onto the reference; positive means the target arrives later. */
  lagSamples: number;
  /** Signed normalized correlation at `lagSamples`, in `[-1, 1]`. */
  correlation: number;
  /** True when the best correlation peak is negative. */
  polarityOpposed: boolean;
}

/** A band several tracks are competing for. */
export interface MixCrowdedBand {
  /** The crowded analysis band. */
  band: MixAnalysisBand;
  /** Crowding in `[0, 1]`: 1 when all of the band's energy sits in one pan position. */
  crowding: number;
}

/** A track whose stereo treatment risks collapsing in mono. */
export interface MixMonoRisk {
  /** Id of the strip at risk. */
  stripId: string;
  /** Inter-channel correlation of the track in `[-1, 1]`. */
  correlation: number;
  /** Stereo width of the track. */
  width: number;
  /** True when the track carries meaningful low-frequency stereo width. */
  wideLowEnd: boolean;
}

/** Cross-track measurements the suggestions were made from. */
export interface MixAssistantMixProfile {
  /** Number of tracks the profile covers. */
  trackCount: number;
  /** Informative masker/maskee band pairs. */
  bandDominance: MixBandDominance[];
  /** Time and polarity relationships between related track pairs. */
  alignment: MixTrackAlignment[];
  /** Bands carrying more simultaneous sources than the image comfortably holds. */
  crowdedBands: MixCrowdedBand[];
  /** Tracks whose stereo image is likely to collapse in mono. */
  monoRisks: MixMonoRisk[];
}

/**
 * What `mixingAssistantSuggest` produces. Nothing has been applied: feeding `scene` to
 * {@link Mixer.fromJson} is the caller's separate step.
 */
export interface MixAssistantResult {
  /** The suggested scene, in the schema {@link Mixer.fromJson} reads. */
  scene: MixSceneDocument;
  /** One entry per input track, in input order. */
  tracks: MixAssistantTrackProfile[];
  /** Cross-track measurements behind the suggestion. */
  mix: MixAssistantMixProfile;
  /**
   * Human-readable reasons in the order the changes were applied; reading it
   * top to bottom retraces how the scene was built. Empty when nothing was
   * suggested (no usable tracks, or every domain switched off).
   */
  explanation: string[];
}
