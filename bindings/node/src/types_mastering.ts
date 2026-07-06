export type VoicePresetId =
  | 'neutral-monitor'
  | 'bright-idol'
  | 'soft-whisper'
  | 'deep-narrator'
  | 'robot-mascot'
  | 'dark-villain';

export interface RealtimeVoiceChangerPreset {
  schemaVersion: 1;
  id?: string;
  name?: string;
  description?: string;
  macros?: Record<string, number>;
  dsp?: Record<string, unknown>;
}

export type RealtimeVoiceChangerConfigInput = VoicePresetId | RealtimeVoiceChangerPreset;

export interface RealtimeVoiceChangerOptions {
  sampleRate: number;
  maxBlockSize?: number;
  channels?: 1 | 2;
  preset?: RealtimeVoiceChangerConfigInput;
}

/**
 * Flat (normalized) realtime-voice-changer configuration, mirroring the
 * `SonareRealtimeVoiceChangerConfig` POD returned by
 * `realtimeVoiceChangerPresetConfig`.
 */
export interface RealtimeVoiceChangerConfig {
  inputGainDb: number;
  outputGainDb: number;
  wetMix: number;
  retuneSemitones: number;
  retuneMix: number;
  retuneGrainSize: number;
  formantFactor: number;
  formantAmount: number;
  formantBody: number;
  formantBrightness: number;
  formantNasal: number;
  eqHighpassHz: number;
  eqBodyDb: number;
  eqPresenceDb: number;
  eqAirDb: number;
  gateThresholdDb: number;
  gateAttackMs: number;
  gateReleaseMs: number;
  gateRangeDb: number;
  compressorThresholdDb: number;
  compressorRatio: number;
  compressorAttackMs: number;
  compressorReleaseMs: number;
  compressorMakeupGainDb: number;
  deesserFrequencyHz: number;
  deesserThresholdDb: number;
  deesserRatio: number;
  deesserRangeDb: number;
  reverbMix: number;
  reverbTimeMs: number;
  reverbDamping: number;
  reverbSeed: number;
  limiterCeilingDb: number;
  limiterReleaseMs: number;
  /** Whether the inter-sample-peak (true-peak) limiter is enabled (default true). */
  limiterEnableIspLimiter: boolean;
  /** Inter-sample-peak limiter ceiling in dBTP (default -1.0). */
  limiterIspCeilingDbtp: number;
}

/** Options for the high-level {@link mastering} one-shot. All fields are optional. */
export interface MasteringOptions {
  /** Integrated-loudness target in LUFS. Default -14. */
  targetLufs?: number;
  /** True-peak ceiling in dBTP. Default -1. */
  ceilingDb?: number;
  /** True-peak oversampling factor. Default 4. */
  truePeakOversample?: number;
  /** Post true-peak limiter release in ms. Default 0 => library default (50 ms). */
  releaseMs?: number;
  /** Apply the static loudness gain at the input (pre-oversample) rate. Default false. */
  applyGainAtInputRate?: boolean;
}

/** Options for {@link pitchCorrectTimevarying}. All fields are optional. */
export interface PitchCorrectOptions {
  /** `'midi'` retunes toward {@link targetMidi}; `'scale'` snaps to the key. Default `'midi'`. */
  mode?: 'midi' | 'scale';
  /** Fixed target note when `mode` is `'midi'`, in `[0, 127]`. Default 69 (A4). */
  targetMidi?: number;
  /** Scale root pitch class (0=C .. 11=B) when `mode` is `'scale'`. Default 0. */
  scaleRoot?: number;
  /** 12-bit degree mask, bit `i` = semitone `i` above the root enabled. Default C major. */
  scaleModeMask?: number;
  /** Reference MIDI anchoring the scale grid. Default 69 (A4). */
  referenceMidi?: number;
  /** Correction strength in `[0, 1]`; 1 = full snap, 0 = bypass. Default 1. */
  retuneAmount?: number;
  /** Hard clamp on per-frame correction magnitude (semitones). Default 12. */
  maxCorrectionSemitones?: number;
  /** Retune IIR time constant (ms); larger = slower glide. Default 50. */
  retuneSpeedMs?: number;
  /** Corrections below this are bypassed to preserve vibrato (cents). Default 20. */
  vibratoThresholdCents?: number;
  /** Per-frame voiced flags (non-zero = voiced); omit to treat all frames as voiced. */
  voiced?: Int32Array;
  /** Per-frame voicing probability in `[0, 1]`; omit to derive from `voiced`. */
  voicedProb?: Float32Array;
}

/** Options for {@link noteStretch}. All fields are optional. */
export interface NoteStretchOptions {
  /** First sample of the note to stretch. Default 0. */
  onsetSample?: number;
  /** Last sample of the note to stretch. Default 0. */
  offsetSample?: number;
  /** Stretch ratio (1 = unchanged). Default 1. */
  stretchRatio?: number;
}

export interface LufsResult {
  integratedLufs: number;
  momentaryLufs: number;
  shortTermLufs: number;
  loudnessRange: number;
}

export type MasteringPreset =
  | 'pop'
  | 'edm'
  | 'acoustic'
  | 'hipHop'
  | 'aiMusic'
  | 'speech'
  | 'streaming'
  | 'youtube'
  | 'broadcast'
  | 'podcast'
  | 'audiobook'
  | 'cinema'
  | 'jpop'
  | 'ambient'
  | 'lofi'
  | 'classical'
  | 'drumAndBass'
  | 'techno'
  | 'metal'
  | 'trap'
  | 'rnb'
  | 'jazz'
  | 'kpop'
  | 'trance'
  | 'gameOst';

export interface StreamingPlatform {
  name: string;
  targetLufs: number;
  ceilingDb: number;
}

export type SoloProcessor =
  | 'dynamics.brickwallLimiter'
  | 'dynamics.compressor'
  | 'dynamics.deesser'
  | 'dynamics.expander'
  | 'dynamics.gate'
  | 'dynamics.limiter'
  | 'dynamics.parallelComp'
  | 'dynamics.sidechainRouter'
  | 'dynamics.duckingProcessor'
  | 'dynamics.transientShaper'
  | 'dynamics.upwardCompressor'
  | 'dynamics.upwardExpander'
  | 'dynamics.vocalRider'
  | 'eq.apiStyle'
  | 'eq.bandPass'
  | 'eq.cutFilter'
  | 'eq.dynamic'
  | 'eq.equalizer'
  | 'eq.graphic'
  | 'eq.linearPhase'
  | 'eq.midSide'
  | 'eq.minimumPhase'
  | 'eq.parametric'
  | 'eq.pultec'
  | 'eq.shelving'
  | 'eq.tilt'
  | 'final.bitDepth'
  | 'final.dither'
  | 'final.outputChain'
  | 'maximizer.adaptiveRelease'
  | 'maximizer.loudnessOptimize'
  | 'maximizer.maximizer'
  | 'maximizer.softKneeMax'
  | 'maximizer.truePeakLimiter'
  | 'multiband.compressor'
  | 'multiband.dynamicEq'
  | 'multiband.expander'
  | 'multiband.imager'
  | 'multiband.limiter'
  | 'multiband.saturation'
  | 'repair.declick'
  | 'repair.declip'
  | 'repair.decrackle'
  | 'repair.dehum'
  | 'repair.denoiseClassical'
  | 'repair.dereverbClassical'
  | 'repair.trimSilence'
  | 'saturation.bitcrusher'
  | 'saturation.exciter'
  | 'saturation.hardClipper'
  | 'saturation.multibandExciter'
  | 'saturation.ampSim'
  | 'saturation.softClipper'
  | 'saturation.tape'
  | 'saturation.transformer'
  | 'saturation.tube'
  | 'saturation.waveshaper'
  | 'spectral.airBand'
  | 'spectral.lowEndFocus'
  | 'spectral.presenceEnhancer'
  | 'spectral.spectralShaper'
  | 'stereo.autoPan'
  | 'stereo.haasEnhancer'
  | 'stereo.imager'
  | 'stereo.monoMaker'
  | 'stereo.phaseAlign'
  | 'stereo.stereoBalance';

export type PairProcessor =
  | 'match.applyMatchEq'
  | 'match.alignReferenceToSource'
  | 'match.abSwitch'
  | 'match.abCrossfade';

export type PairAnalysis =
  | 'match.referenceLoudness'
  | 'match.tonalBalance'
  | 'match.tonalBalanceLogBands'
  | 'match.matchEqCurve'
  | 'match.estimateReferenceDelaySamples';

export type StereoAnalysis = 'stereo.monoCompatCheck' | 'stereo.monoCompatCheckLogBands';

export interface MasteringResult {
  samples: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  latencySamples?: number;
}

export interface MasteringStereoResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  latencySamples: number;
}

/**
 * A nested processor / parameter sub-tree of a {@link MasteringChainConfig}.
 * Leaf values are numbers or booleans; nest deeper for processor parameters.
 */
export interface MasteringChainSection {
  [key: string]: number | boolean | MasteringChainSection;
}

/**
 * Nested mastering-chain configuration. Top-level keys are the processing
 * modules; nest processor and parameter names beneath them, e.g.
 *
 * ```ts
 * masteringChain(samples, sr, {
 *   dynamics: { compressor: { thresholdDb: -24 } },
 *   loudness: { targetLufs: -14 },
 * });
 * ```
 *
 * A boolean toggles a module/processor's `enabled` flag; setting any field
 * implicitly enables its module unless `enabled: false` is also given. Unknown
 * keys throw at apply time. (`stereo.*` modules apply on the stereo path only.)
 */
export interface MasteringChainConfig {
  repair?: MasteringChainSection;
  eq?: MasteringChainSection;
  dynamics?: MasteringChainSection;
  saturation?: MasteringChainSection;
  spectral?: MasteringChainSection;
  stereo?: MasteringChainSection;
  maximizer?: MasteringChainSection;
  loudness?: MasteringChainSection;
}

export interface MasteringChainResult {
  /** Latency-compensated offline output; no separate latency field is reported. */
  samples: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  stages: string[];
}

export interface MasteringChainStereoResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  inputLufs: number;
  outputLufs: number;
  appliedGainDb: number;
  stages: string[];
}

export type PanMode = 'balance' | 'stereoPan' | 'stereo-pan' | 'dualPan' | 'dual-pan' | number;

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
  truePeakDbL: number;
  truePeakDbR: number;
  maxTruePeakDb: number;
  seq: number;
  /** Number of valid surround planes (5.1/7.1); 0 before the meter sees audio. */
  channelCount: number;
  /** Per-plane peak dB, length channelCount; [0]/[1] mirror peakDbL/peakDbR. */
  peakDb: number[];
  /** Per-plane RMS dB, length channelCount; [0]/[1] mirror rmsDbL/rmsDbR. */
  rmsDb: number[];
  /** Per-plane true-peak dB, length channelCount; [0]/[1] mirror truePeakDbL/R. */
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
 * Pan law applied by a strip's panner. Mapped to the C enum ints
 * `0=const3dB`, `1=const4.5dB`, `2=const6dB`, `3=linear0dB`.
 */
export type PanLaw = 'const3dB' | 'const4.5dB' | 'const6dB' | 'linear0dB';

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

/**
 * How a {@link SpectralRegionOp} modifies the masked STFT bins. Mirrors the C
 * `SonareSpectralEditMode`:
 * - `'gain'`: multiply magnitude by `10^(gainDb/20)` (phase kept).
 * - `'attenuate'`: gain with a (typically negative) `gainDb`.
 * - `'mute'`: hard-zero the masked bins (`gainDb` ignored).
 * - `'heal'`: tonal continuation from neighbouring time frames.
 */
export type SpectralEditMode = 'gain' | 'attenuate' | 'mute' | 'heal';

/**
 * One time x frequency rectangle edit op for {@link spectralEdit}. Ops apply in
 * array order. Mirrors the C `SonareSpectralRegionOp`.
 */
export interface SpectralRegionOp {
  /** Region time start in input samples (clamped to `[0, length]`). Default 0. */
  startSample?: number;
  /** Region time end (exclusive) in input samples. Default = signal length. */
  endSample?: number;
  /** Region frequency low edge in Hz (clamped to `[0, nyquist]`). Default 0. */
  lowHz?: number;
  /** Region frequency high edge in Hz; `<= 0` or `>= nyquist` means nyquist. Default 0. */
  highHz?: number;
  /** Gain in dB for `'gain'` / `'attenuate'`; ignored by `'mute'` / `'heal'`. Default 0. */
  gainDb?: number;
  /** How the op modifies the masked bins. Default `'gain'`. */
  mode?: SpectralEditMode;
}

/**
 * STFT + heal parameters for {@link spectralEdit}. All fields are optional;
 * omitted fields take the documented native defaults. Mirrors the C
 * `SonareSpectralEditConfig`.
 */
export interface SpectralEditOptions {
  /** FFT size; must be a power of two `>= 2`. Default 2048. */
  nFft?: number;
  /** Hop length; must satisfy `0 < hop <= nFft / 2`. Default 512. */
  hopLength?: number;
  /** Analysis/synthesis window. Default `'hann'`. */
  window?: 'hann' | 'hamming' | 'blackman' | 'rectangular';
  /** Neighbour frames each side used by `'heal'`. Default 2. */
  healRadiusFrames?: number;
}
