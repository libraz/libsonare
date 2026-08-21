/**
 * Mixing-assistant types.
 *
 * The assistant suggests; it does not apply. Every entry point returns
 * parameters and never audio, so realising a suggestion is the caller's own
 * second step through {@link Mixer.fromSceneJson}.
 */

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
  /** Display name, used only as a source-classification hint. */
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
  /** Overall strength of the suggestion in `[0, 1]`; 0 suggests nothing. */
  suggestionStrength?: number;
  /** Largest cut a single suggested EQ band may apply, in dB. */
  eqMaxCutDb?: number;
  /** Headroom the summed mix is left with on the master bus, in dBTP. */
  mixBusHeadroomDbtp?: number;
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
  /** Processor parameters, as a nested JSON string. */
  params: string;
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
  /** False when the track was excluded from the decisions; see `exclusionReason`. */
  usable: boolean;
  exclusionReason: string;
  channelCount: number;
  durationSec: number;
  integratedLufs: number;
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
