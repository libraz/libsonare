/**
 * Mixing types: the mixer's own pan, metering and automation vocabulary,
 * and the assistant that suggests settings over it.
 *
 * The assistant suggests; it does not apply. Every entry point returns
 * parameters and never audio, so realising a suggestion is the caller's own
 * second step through {@link Mixer.fromSceneJson}.
 */

export type PanMode =
  | 'balance'
  | 'pan'
  | 'stereoPan'
  | 'stereo-pan'
  | 'dualPan'
  | 'dual-pan'
  | number;

/**
 * Surround pan position for a strip feeding a >2-channel bus. Phase 1 honors
 * `azimuth`/`divergence`/`lfe`; `elevation`/`distance` are reserved. All fields
 * are optional and default to a centered point source.
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

/**
 * Per-strip options for {@link mixStereo}. Each field is either one value for
 * every strip or an array with one entry per strip; an array shorter than the
 * strip list leaves the remaining strips at their defaults.
 *
 * Every field applies independently of the others. In particular `pan` and
 * `panMode` are separate requests: `panMode` alone selects the mode at the
 * strip's centre position, and `pan` alone moves the position while keeping the
 * strip's current mode, the same way `Mixer.setPan(strip, pan, panMode?)` does.
 */
export interface MixOptions {
  inputTrimDb?: number | number[];
  faderDb?: number | number[];
  pan?: number | number[];
  panMode?: PanMode | PanMode[];
  width?: number | number[];
  muted?: boolean | boolean[];
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
   * Left-channel inter-sample (true) peak in dB, from the ITU-R BS.1770-4
   * polyphase reconstruction at 4x. A streaming measurement: the centered
   * reconstruction stencil needs a few future samples a realtime path does not
   * have, so each block's last samples read marginally low (about 0.1 dB across
   * 64..8192-sample blocks on a near-Nyquist tone, always under-reading). Use
   * `meteringTruePeakDb` over the whole signal for an exact dBTP number.
   */
  truePeakDbL: number;
  /** Right-channel inter-sample (true) peak in dB. See {@link truePeakDbL}. */
  truePeakDbR: number;
  /** Maximum inter-sample peak across channels in dB. See {@link truePeakDbL}. */
  maxTruePeakDb: number;
  seq: number;
  /** Number of valid surround planes (5.1/7.1); 0 before the meter sees audio. */
  channelCount: number;
  /** Per-plane peak dB, length channelCount; [0]/[1] mirror peakDbL/peakDbR. */
  peakDb: number[];
  /** Per-plane RMS dB, length channelCount; [0]/[1] mirror rmsDbL/rmsDbR. */
  rmsDb: number[];
  /**
   * Per-plane true-peak dB, length channelCount; [0]/[1] mirror
   * {@link truePeakDbL}/R and carry the same streaming caveat.
   */
  truePeakDb: number[];
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
export type AutomationCurve = 'linear' | 'exponential' | 'hold' | 's-curve';

/**
 * Pan law applied by a strip's panner. On mono strips it changes the centre
 * gain; on stereo strips in Balance mode the centre remains unity and it
 * changes only the far-channel taper. Mapped to C enum ints
 * `0=const3dB`, `1=const4.5dB`, `2=const6dB`, `3=linear0dB`.
 */
export type PanLaw = 'const3dB' | 'const4.5dB' | 'const6dB' | 'linear0dB';

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
export type MixBandName = 'sub' | 'low' | 'lowMid' | 'mid' | 'highMid' | 'high' | 'air';

/**
 * Coarse source taxonomy the rule-based classifier resolves to. The runtime
 * list is {@link mixSourceClassNames}; this union mirrors it.
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
  | 'fx';

/** Per-band energy share of one track, keyed by {@link MixBandName}. */
export type MixBandOccupancy = Record<MixBandName, number>;

/** One track handed to {@link suggestMixScene}. */
export interface MixAssistantTrack {
  /** Strip id the suggestion addresses. Must be unique across the request. */
  id: string;
  /**
   * Display name, used as a source-classification hint.
   *
   * For a class the classifier can measure, it only adjusts confidence and
   * cannot select the class on its own. For the four it cannot separate by
   * measurement — `keys`, `strings`, `backing` and `fx` — the name is the only
   * thing that can supply the class at all, and it does so only when the
   * measurement produced no answer.
   */
  name?: string;
  /** Left/mono channel. */
  left: Float32Array;
  /** Right channel; omit for a mono track. Must match `left` in length. */
  right?: Float32Array;
}

/**
 * Assistant tunables. Every field is optional; an omitted field keeps the
 * native default rather than being sent as an explicit value.
 */
export interface MixAssistantOptions {
  /** Absolute integrated-loudness target each track is staged towards, in LUFS. */
  targetTrackLufs?: number;
  /**
   * Overall strength of the suggestion in `[0, 1]`, scaling every level-like
   * decision: trims, fader offsets, send levels, EQ cut depths, compression
   * ratios and ranges, and how far a track is spread from the centre.
   *
   * `0` is not an empty suggestion. It is every one of those taken and set to
   * zero, plus the decisions that are not levels and so do not scale: the bus
   * topology and routing, and the physical corrections for a measured
   * cancellation (polarity, alignment delay, low-end mono fold). To suggest
   * nothing, switch the domains off instead — that also skips the work.
   */
  suggestionStrength?: number;
  /** Largest cut a single suggested EQ band may apply, in dB. */
  eqMaxCutDb?: number;
  /** Headroom the summed mix is left with on the master bus, in dBTP. */
  mixBusHeadroomDbtp?: number;
  /**
   * Tempo the suggested delay times are voiced against, in BPM.
   *
   * Defaults to 0, which selects the transport's own fallback tempo: the
   * assistant is handed bare stems and cannot measure a tempo the set as a
   * whole agrees on. Pass the song's tempo and the delay lands on the beat.
   *
   * A positive value outside 20–400 BPM is rejected rather than clamped.
   */
  tempoBpm?: number;
  /** Evaluate the bus/structure domain. */
  enableStructure?: boolean;
  /** Evaluate the gain-staging domain. */
  enableGain?: boolean;
  /** Evaluate the level-balance domain. */
  enableBalance?: boolean;
  /** Evaluate the EQ domain. */
  enableEq?: boolean;
  /** Evaluate the dynamics domain. */
  enableDynamics?: boolean;
  /** Evaluate the stereo-image domain. */
  enableImage?: boolean;
  /**
   * Suggest a high-pass filter on tracks carrying residue below their register.
   *
   * Off by default: a survey of mixing best practices found the rule that every
   * track without low-frequency content should be high-passed to be seldom used
   * in studio mixing and unsupported by subjective testing. Switched on, the
   * filter is proposed from the track's measured low-frequency content rather
   * than from its source class, so a part playing below its class's usual
   * register keeps what it plays.
   */
  enableHighPass?: boolean;
  /** Shared STFT size for every track. */
  nFft?: number;
  /** Shared STFT hop length for every track. */
  hopLength?: number;
}

/** Inputs for {@link suggestMixScene} and {@link suggestMixSceneJson}. */
export interface SuggestMixSceneRequest {
  /** Tracks to mix. An empty array yields an empty suggestion. */
  tracks: MixAssistantTrack[];
  /** Shared sample rate for every track. */
  sampleRate: number;
  /** Assistant tunables; omitted fields keep the native defaults. */
  options?: MixAssistantOptions;
}

/** One processor slot on a scene strip or bus. */
export interface MixSceneInsert {
  slot: string;
  processor: string;
  /**
   * Processor parameters, keyed by the parameter name the processor's catalog
   * entry declares. Numbers and booleans throughout, except for the two keys a
   * processor reads itself: a string for a named rig or an embedded impulse
   * response, and a per-band array for the acoustic room morph.
   */
  params: Record<string, number | boolean | string | number[]>;
  /** Present only when the insert is keyed off another strip. */
  sidechainKey?: string;
}

/** A strip send to a destination bus. */
export interface MixSceneSend {
  id: string;
  destinationBusId: string;
  sendDb: number;
  timing: string;
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
  panMode: number;
  dualPanLeft: number;
  dualPanRight: number;
  polarityInvertLeft: boolean;
  polarityInvertRight: boolean;
  panLaw: number;
  channelDelaySamples: number;
  /** Present only for a non-stereo source. */
  sourceLayout?: string;
  /** Present only when the surround pan has moved off its centered default. */
  surroundPan?: {
    azimuth: number;
    elevation: number;
    divergence: number;
    lfe: number;
    distance: number;
  };
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
  metering?: {
    enabled: boolean;
    lufs: boolean;
    truePeak: boolean;
    /** Requested factor; the meter resolves it to the nearest of 2x / 4x / 8x. */
    truePeakOversample: number;
  };
  inserts: MixSceneInsert[];
  sends: MixSceneSend[];
}

/** A bus in a mixer scene. Defaulted fields are omitted from the document. */
export interface MixSceneBus {
  id: string;
  role: string;
  layout?: string;
  inputTrimDb?: number;
  width?: number;
  polarityInvertLeft?: boolean;
  polarityInvertRight?: boolean;
  inserts: MixSceneInsert[];
}

/** A VCA group in a mixer scene. */
export interface MixSceneVcaGroup {
  id: string;
  gainDb: number;
  members: string[];
}

/** A routing edge in a mixer scene. */
export interface MixSceneConnection {
  source: string;
  destination: string;
}

/**
 * A mixer scene document, in the schema {@link Mixer.fromSceneJson} reads.
 * {@link suggestMixSceneJson} returns the same document as its JSON text.
 */
export interface MixSceneDocument {
  version: number;
  strips: MixSceneStrip[];
  buses: MixSceneBus[];
  vcaGroups: MixSceneVcaGroup[];
  connections: MixSceneConnection[];
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
  channelCount: number;
  durationSec: number;
  /**
   * BS.1770 integrated loudness in LUFS, or `null` for a track with no gated
   * block to measure — a silent stem, or one muted before it was handed over.
   * The measurement is `-Infinity` there, which JSON has no number for, so the
   * document carries `null` rather than a finite value that would read as a
   * real level. Such a track always carries an `exclusionReason` as well.
   */
  integratedLufs: number | null;
  truePeakDb: number;
  crestFactorDb: number;
  spectralCentroidHz: number;
  spectralFlatness: number;
  attackDensity: number;
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
  band: MixBandName;
  ratio: number;
  validFrames: number;
}

/** A related track pair and the time/polarity relationship measured between them. */
export interface MixAlignmentPair {
  reference: string;
  target: string;
  lagSamples: number;
  correlation: number;
  polarityOpposed: boolean;
}

/** A band carrying more simultaneous sources than the image comfortably holds. */
export interface MixCrowdedBand {
  band: MixBandName;
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
  alignment: MixAlignmentPair[];
  crowdedBands: MixCrowdedBand[];
  monoRisks: MixMonoRisk[];
}

/** What {@link suggestMixScene} produces. */
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
