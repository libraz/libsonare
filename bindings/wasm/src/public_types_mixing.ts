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
 * Pan law applied when computing left/right gains from a pan position
 * (see {@link Mixer.setPanLaw}). On mono strips it changes the centre gain;
 * on stereo Balance strips it changes only the far-channel taper, while centre
 * remains unity. Maps to the underlying integer code.
 */
export type PanLaw = 'const3dB' | 'const4.5dB' | 'const6dB' | 'linear0dB';

/** Accepted pan-law name aliases for mixer and realtime-engine inputs. */
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
 * Meter tap point for reading a strip's meter snapshot
 * (see {@link Mixer.meterTap} and {@link Mixer.stripMeter}).
 */
export type MeterTap = 'preFader' | 'postFader';

/** Pre/post-fader send timing (see {@link Mixer.addSend}). */
export type SendTiming = 'preFader' | 'postFader';

/** A single goniometer (left/right) sample returned by {@link Mixer.readGoniometerLatest}. */
export interface GoniometerPoint {
  left: number;
  right: number;
}

/** One analysis band of the mixing assistant's shared 7-band split. */
export type MixAnalysisBand = 'sub' | 'low' | 'lowMid' | 'mid' | 'highMid' | 'high' | 'air';

/** Share of a track's energy in each analysis band; sums to 1, or to 0 when silent. */
export type MixBandOccupancy = Record<MixAnalysisBand, number>;

/**
 * One track handed to {@link suggestMixScene}.
 *
 * Planar and per-track: tracks in one call may differ in length, and each is
 * mono (`right` omitted) or stereo independently of the others. A stereo
 * track's `right` must be the same length as its `left`.
 */
export interface MixAssistantTrack {
  /** Strip id the suggestion is written against. Must be unique and non-empty. */
  id: string;
  /**
   * Optional display name.
   *
   * For a class the classifier can measure, this is only a hint that adjusts
   * its confidence and cannot select the class on its own. For the four it
   * cannot separate by measurement — `keys`, `strings`, `backing` and `fx` —
   * the name is the only thing that can supply the class at all, and it does so
   * only when the measurement produced no answer.
   */
  name?: string;
  /** Left/mono plane. */
  left: Float32Array;
  /** Right plane; omit for a mono track. */
  right?: Float32Array;
}

/**
 * Tunables for {@link suggestMixScene}. Every field is optional and falls back
 * to the core default noted on it; the same field names and defaults are used
 * by the Node and Python surfaces.
 */
export interface MixAssistantOptions {
  /** Absolute integrated-loudness target each track is staged towards, in LUFS. Defaults to -18. */
  targetTrackLufs?: number;
  /**
   * Overall strength of the suggestion in `[0, 1]`, scaling every level-like
   * decision: trims, fader offsets, send levels, EQ cut depths, compression
   * ratios and ranges, and how far a track is spread from the centre. Defaults
   * to 1.
   *
   * `0` is not an empty suggestion. It is every one of those taken and set to
   * zero, plus the decisions that are not levels and so do not scale: the bus
   * topology and routing, and the physical corrections for a measured
   * cancellation (polarity, alignment delay, low-end mono fold). To suggest
   * nothing, switch the domains off instead — that also skips the work.
   */
  suggestionStrength?: number;
  /** Largest cut a single suggested EQ band may apply, in dB. Defaults to 4. */
  eqMaxCutDb?: number;
  /** Headroom the summed mix is left with on the master bus, in dBTP. Defaults to -6. */
  mixBusHeadroomDbtp?: number;
  /** Evaluate the structure domain. Defaults to true. */
  enableStructure?: boolean;
  /** Evaluate the gain-staging domain. Defaults to true. */
  enableGain?: boolean;
  /** Evaluate the balance domain. Defaults to true. */
  enableBalance?: boolean;
  /** Evaluate the EQ domain. Defaults to true. */
  enableEq?: boolean;
  /** Evaluate the dynamics domain. Defaults to true. */
  enableDynamics?: boolean;
  /** Evaluate the stereo-image domain. Defaults to true. */
  enableImage?: boolean;
  /**
   * Suggest a high-pass filter on tracks carrying residue below their register.
   * Defaults to false.
   *
   * Off by default: a survey of mixing best practices found the rule that every
   * track without low-frequency content should be high-passed to be seldom used
   * in studio mixing and unsupported by subjective testing. Switched on, the
   * filter is proposed from the track's measured low-frequency content rather
   * than from its source class, so a part playing below its class's usual
   * register keeps what it plays.
   */
  enableHighPass?: boolean;
  /** Shared STFT size for every track. Defaults to 2048. */
  nFft?: number;
  /** Shared STFT hop for every track. Defaults to 512. */
  hopLength?: number;
}

/** What the assistant measured about one input track. */
export interface MixAssistantTrackProfile {
  stripId: string;
  name: string;
  /** Source class identifier; one of {@link mixSourceClassNames}. */
  source: string;
  /** Classifier confidence in `[0, 1]`. */
  sourceConfidence: number;
  /** False when the track is silent, too short, or has no usable spectral content. */
  usable: boolean;
  /** Why the track was excluded; empty when {@link usable} is true. */
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

/** One informative band-masking relationship between two tracks. */
export interface MixBandDominance {
  /** Strip id of the masking track. */
  masker: string;
  /** Strip id of the masked track. */
  maskee: string;
  band: MixAnalysisBand;
  ratio: number;
  validFrames: number;
}

/** A time/polarity relationship between two related tracks. */
export interface MixTrackAlignment {
  reference: string;
  target: string;
  lagSamples: number;
  correlation: number;
  polarityOpposed: boolean;
}

/** A band several tracks are competing for. */
export interface MixCrowdedBand {
  band: MixAnalysisBand;
  crowding: number;
}

/** A track whose stereo treatment risks collapsing in mono. */
export interface MixMonoRisk {
  stripId: string;
  correlation: number;
  width: number;
  wideLowEnd: boolean;
}

/** Cross-track measurements the suggestions were made from. */
export interface MixAssistantMixProfile {
  trackCount: number;
  bandDominance: MixBandDominance[];
  alignment: MixTrackAlignment[];
  crowdedBands: MixCrowdedBand[];
  monoRisks: MixMonoRisk[];
}

/**
 * What {@link suggestMixScene} produces. Nothing has been applied: feeding
 * `scene` to {@link Mixer.fromSceneJson} is the caller's separate step, and
 * {@link suggestMixSceneJson} returns it already serialized for that.
 */
export interface MixAssistantResult {
  /** The suggested scene, in the schema {@link Mixer.fromSceneJson} reads. */
  scene: Record<string, unknown>;
  /** One entry per input track, in input order. */
  tracks: MixAssistantTrackProfile[];
  mix: MixAssistantMixProfile;
  /**
   * Human-readable reasons in the order the changes were applied; reading it
   * top to bottom retraces how the scene was built. Empty when nothing was
   * suggested (no usable tracks, or every domain switched off).
   */
  explanation: string[];
}
