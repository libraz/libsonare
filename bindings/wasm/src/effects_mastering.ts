import { getSonareModule } from './module_state';
import type {
  HpssResult,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringPreset,
  MasteringProcessorParams,
  MasteringResult,
  MasteringStereoChainResult,
  MasteringStereoResult,
  MixOptions,
  MixResult,
  PairAnalysis,
  PairProcessor,
  RealtimeVoiceChangerConfigInput,
  SoloProcessor,
  StereoAnalysis,
  StreamingPlatform,
} from './public_types';
import type { ProgressCallback } from './sonare.js';
import { RealtimeVoiceChanger } from './streaming_mixing';
import type { ValidateOptions } from './validation';
import { assertSamples } from './validation';

function requireModule() {
  return getSonareModule();
}

// ============================================================================
// Effects
// ============================================================================

/**
 * Perform Harmonic-Percussive Source Separation (HPSS).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param kernelHarmonic - Horizontal median filter size for harmonic (default: 31)
 * @param kernelPercussive - Vertical median filter size for percussive (default: 31)
 * @returns Separated harmonic and percussive components
 */
export function hpss(
  samples: Float32Array,
  sampleRate = 22050,
  kernelHarmonic = 31,
  kernelPercussive = 31,
): HpssResult {
  return requireModule().hpss(samples, sampleRate, kernelHarmonic, kernelPercussive);
}

/**
 * Extract harmonic component from audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Harmonic component
 */
export function harmonic(
  samples: Float32Array,
  sampleRate: number,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('harmonic', samples, options.validate !== false);
  return requireModule().harmonic(samples, sampleRate);
}

/**
 * Extract percussive component from audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Percussive component
 */
export function percussive(
  samples: Float32Array,
  sampleRate: number,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('percussive', samples, options.validate !== false);
  return requireModule().percussive(samples, sampleRate);
}

/**
 * Time-stretch audio without changing pitch.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param rate - Time stretch rate (0.5 = double duration, 2.0 = half duration)
 * @returns Time-stretched audio
 */
export function timeStretch(
  samples: Float32Array,
  sampleRate: number,
  rate: number,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('timeStretch', samples, options.validate !== false);
  return requireModule().timeStretch(samples, sampleRate, rate);
}

/**
 * Pitch-shift audio without changing duration.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param semitones - Pitch shift in semitones (+12 = one octave up, -12 = one octave down)
 * @returns Pitch-shifted audio
 */
export function pitchShift(
  samples: Float32Array,
  sampleRate: number,
  semitones: number,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('pitchShift', samples, options.validate !== false);
  return requireModule().pitchShift(samples, sampleRate, semitones);
}

/**
 * Pitch-correct audio from a current MIDI note to a target MIDI note.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param currentMidi - Detected/current MIDI note number
 * @param targetMidi - Desired MIDI note number
 * @returns Pitch-corrected audio
 */
export function pitchCorrectToMidi(
  samples: Float32Array,
  sampleRate = 22050,
  currentMidi = 69.0,
  targetMidi = 69.0,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('pitchCorrectToMidi', samples, options.validate !== false);
  return requireModule().pitchCorrectToMidi(samples, sampleRate, currentMidi, targetMidi);
}

/**
 * Time-stretch a note region between two sample offsets without changing pitch.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param onsetSample - Note onset position in samples
 * @param offsetSample - Note offset position in samples
 * @param stretchRatio - Stretch ratio (0.5 = double duration, 2.0 = half duration)
 * @returns Audio with the note region stretched
 */
export function noteStretch(
  samples: Float32Array,
  sampleRate = 22050,
  onsetSample = 0,
  offsetSample = 0,
  stretchRatio = 1.0,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('noteStretch', samples, options.validate !== false);
  return requireModule().noteStretch(samples, sampleRate, onsetSample, offsetSample, stretchRatio);
}

/**
 * Apply a voice change by shifting pitch and formants independently.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param pitchSemitones - Pitch shift in semitones
 * @param formantFactor - Formant scaling factor (1.0 = unchanged)
 * @returns Voice-changed audio
 */
export function voiceChange(
  samples: Float32Array,
  sampleRate = 22050,
  pitchSemitones = 0.0,
  formantFactor = 1.0,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('voiceChange', samples, options.validate !== false);
  return requireModule().voiceChange(samples, sampleRate, pitchSemitones, formantFactor);
}

/** Options for the offline {@link voiceChangeRealtime} convenience wrapper. */
export interface VoiceChangeRealtimeOptions extends ValidateOptions {
  sampleRate?: number;
  /** Voice-changer preset id or full config object. */
  preset?: RealtimeVoiceChangerConfigInput;
  /** Channel count (1 = mono, 2 = interleaved stereo). */
  channels?: 1 | 2;
  /** Block size for the internal render loop (default 512). */
  blockSize?: number;
}

/**
 * Applies the realtime voice-changer chain to a whole buffer in one call.
 *
 * Constructs and prepares a {@link RealtimeVoiceChanger}, runs the block loop
 * for the caller, then disposes it — matching the Python `voice_change_realtime`
 * and Node `voiceChangeRealtime` convenience wrappers. For mono, `samples` is a
 * plain mono buffer; for stereo, `samples` is interleaved (L0,R0,L1,R1,...).
 *
 * @returns The processed buffer (same layout/length as the input).
 */
export function voiceChangeRealtime(
  samples: Float32Array,
  options: VoiceChangeRealtimeOptions = {},
): Float32Array {
  assertSamples('voiceChangeRealtime', samples, options.validate !== false);
  const channels = options.channels ?? 1;
  if (channels !== 1 && channels !== 2) {
    throw new Error('voiceChangeRealtime: channels must be 1 or 2.');
  }
  // 48000 matches the Python voice_change_realtime and Node voiceChangeRealtime
  // convenience wrappers (and the RealtimeVoiceChanger default).
  const sampleRate = options.sampleRate ?? 48000;
  const blockSize = Math.max(1, Math.floor(options.blockSize ?? 512));
  const changer = new RealtimeVoiceChanger(options.preset ?? 'neutral-monitor');
  try {
    changer.prepare(sampleRate, blockSize, channels);
    const out = new Float32Array(samples.length);
    if (channels === 1) {
      for (let offset = 0; offset < samples.length; offset += blockSize) {
        const block = samples.subarray(offset, Math.min(offset + blockSize, samples.length));
        out.set(changer.processMono(block), offset);
      }
    } else {
      const frameStride = blockSize * 2;
      for (let offset = 0; offset < samples.length; offset += frameStride) {
        const block = samples.subarray(offset, Math.min(offset + frameStride, samples.length));
        out.set(changer.processInterleaved(block, 2), offset);
      }
    }
    return out;
  } finally {
    changer.delete();
  }
}

/**
 * Normalize audio to target peak level.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param targetDb - Target peak level in dB (default: 0 dB = full scale)
 * @returns Normalized audio
 */
export function normalize(
  samples: Float32Array,
  sampleRate: number,
  targetDb = 0.0,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('normalize', samples, options.validate !== false);
  return requireModule().normalize(samples, sampleRate, targetDb);
}

/**
 * Apply mastering loudness normalization with a true-peak ceiling.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param targetLufs - Target integrated LUFS (default: -14)
 * @param ceilingDb - True/sample peak ceiling in dBFS (default: -1)
 * @param truePeakOversample - Oversampling factor used for peak estimation
 * @returns Processed audio and loudness metadata
 */
export function mastering(
  samples: Float32Array,
  sampleRate = 22050,
  targetLufs = -14.0,
  ceilingDb = -1.0,
  truePeakOversample = 4,
): MasteringResult {
  return requireModule().mastering(samples, sampleRate, targetLufs, ceilingDb, truePeakOversample);
}

export function masteringProcessorNames(): SoloProcessor[] {
  return requireModule().masteringProcessorNames() as SoloProcessor[];
}

export function masteringPairProcessorNames(): PairProcessor[] {
  return requireModule().masteringPairProcessorNames() as PairProcessor[];
}

export function masteringPairAnalysisNames(): PairAnalysis[] {
  return requireModule().masteringPairAnalysisNames() as PairAnalysis[];
}

export function masteringStereoAnalysisNames(): StereoAnalysis[] {
  return requireModule().masteringStereoAnalysisNames() as StereoAnalysis[];
}

export function masteringProcess(
  processorName: SoloProcessor,
  samples: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): MasteringResult {
  return requireModule().masteringProcess(processorName, samples, sampleRate, params);
}

export function masteringProcessStereo(
  processorName: SoloProcessor,
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): MasteringStereoResult {
  if (left.length !== right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return requireModule().masteringProcessStereo(processorName, left, right, sampleRate, params);
}

export function masteringPairProcess(
  processorName: PairProcessor,
  source: Float32Array,
  reference: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): MasteringResult {
  return requireModule().masteringPairProcess(processorName, source, reference, sampleRate, params);
}

export function masteringPairAnalyze(
  analysisName: PairAnalysis,
  source: Float32Array,
  reference: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): string {
  return requireModule().masteringPairAnalyze(analysisName, source, reference, sampleRate, params);
}

export function masteringStereoAnalyze(
  analysisName: StereoAnalysis,
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): string {
  return requireModule().masteringStereoAnalyze(analysisName, left, right, sampleRate, params);
}

export function masteringAssistantSuggest(
  samples: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): string {
  return requireModule().masteringAssistantSuggest(samples, sampleRate, params);
}

export function masteringAudioProfile(
  samples: Float32Array,
  sampleRate = 22050,
  params: MasteringProcessorParams = {},
): string {
  return requireModule().masteringAudioProfile(samples, sampleRate, params);
}

export function masteringStreamingPreview(
  samples: Float32Array,
  sampleRate = 22050,
  platforms: StreamingPlatform[] = [],
): string {
  return requireModule().masteringStreamingPreview(samples, sampleRate, platforms);
}

// ============================================================================
// Mastering repair (declick, denoise_classical, declip, decrackle, dehum,
// dereverb_classical, trim_silence) — hand-written bindings.
// ============================================================================

/** Options for `masteringRepairDeclick`. */
export interface DeclickOptions {
  threshold?: number;
  neighborRatio?: number;
  maxClickSamples?: number;
  lpcOrder?: number;
  residualRatio?: number;
}

/** Algorithms accepted by `masteringRepairDenoiseClassical`. */
export type DenoiseClassicalMode = 'logMmse' | 'mmseStsa' | 'spectralSubtraction';

/** Noise PSD estimators accepted by `masteringRepairDenoiseClassical`. */
export type DenoiseClassicalNoiseEstimator = 'quantile' | 'mcra' | 'imcra';

/** Options for `masteringRepairDenoiseClassical`. */
export interface DenoiseClassicalOptions {
  mode?: DenoiseClassicalMode;
  noiseEstimator?: DenoiseClassicalNoiseEstimator;
  nFft?: number;
  hopLength?: number;
  ddAlpha?: number;
  gainFloor?: number;
  overSubtraction?: number;
  spectralFloor?: number;
  noiseEstimationQuantile?: number;
  speechPresenceGain?: boolean;
  gainSmoothing?: boolean;
}

/** Offline LPC-based declicker. */
export function masteringRepairDeclick(
  samples: Float32Array,
  sampleRate: number,
  options: DeclickOptions = {},
): Float32Array {
  return requireModule().masteringRepairDeclick(samples, sampleRate, options);
}

/** Offline STFT-domain classical denoiser (LogMMSE / MMSE-STSA / SpectralSubtraction). */
export function masteringRepairDenoiseClassical(
  samples: Float32Array,
  sampleRate: number,
  options: DenoiseClassicalOptions = {},
): Float32Array {
  return requireModule().masteringRepairDenoiseClassical(samples, sampleRate, options);
}

/** Options for `masteringRepairDeclip`. */
export interface DeclipOptions {
  clipThreshold?: number;
  lpcOrder?: number;
  iterations?: number;
  lpcBlend?: number;
}

/** Algorithms accepted by `masteringRepairDecrackle`. */
export type DecrackleMode = 'median' | 'waveletShrinkage';

/** Options for `masteringRepairDecrackle`. */
export interface DecrackleOptions {
  threshold?: number;
  mode?: DecrackleMode;
  levels?: number;
}

/** Options for `masteringRepairDehum`. */
export interface DehumOptions {
  fundamentalHz?: number;
  harmonics?: number;
  q?: number;
  adaptive?: boolean;
  searchRangeHz?: number;
  adaptation?: number;
  frameSize?: number;
  pllBandwidth?: number;
}

/** Options for `masteringRepairDereverbClassical`. */
export interface DereverbClassicalOptions {
  threshold?: number;
  attenuation?: number;
  nFft?: number;
  hopLength?: number;
  t60Sec?: number;
  lateDelayMs?: number;
  overSubtraction?: number;
  spectralFloor?: number;
  wpeEnabled?: boolean;
  wpeIterations?: number;
  wpeTaps?: number;
  wpeStrength?: number;
}

/** Trimming modes accepted by `masteringRepairTrimSilence`. */
export type TrimSilenceMode = 'peak' | 'lufsGated';

/** Options for `masteringRepairTrimSilence`. */
export interface TrimSilenceOptions {
  threshold?: number;
  paddingSamples?: number;
  mode?: TrimSilenceMode;
  gateLufs?: number;
  windowMs?: number;
}

/** Offline LPC-based declipper. */
export function masteringRepairDeclip(
  samples: Float32Array,
  sampleRate: number,
  options: DeclipOptions = {},
): Float32Array {
  return requireModule().masteringRepairDeclip(samples, sampleRate, options);
}

/** Offline crackle suppressor (median or wavelet-shrinkage). */
export function masteringRepairDecrackle(
  samples: Float32Array,
  sampleRate: number,
  options: DecrackleOptions = {},
): Float32Array {
  return requireModule().masteringRepairDecrackle(samples, sampleRate, options);
}

/** Offline mains-hum remover. */
export function masteringRepairDehum(
  samples: Float32Array,
  sampleRate: number,
  options: DehumOptions = {},
): Float32Array {
  return requireModule().masteringRepairDehum(samples, sampleRate, options);
}

/** Offline classical dereverberator (spectral subtraction + optional WPE). */
export function masteringRepairDereverbClassical(
  samples: Float32Array,
  sampleRate: number,
  options: DereverbClassicalOptions = {},
): Float32Array {
  return requireModule().masteringRepairDereverbClassical(samples, sampleRate, options);
}

/** Offline silence trimmer (peak threshold or LUFS-gated). */
export function masteringRepairTrimSilence(
  samples: Float32Array,
  sampleRate: number,
  options: TrimSilenceOptions = {},
): Float32Array {
  return requireModule().masteringRepairTrimSilence(samples, sampleRate, options);
}

// ============================================================================
// Mastering — offline dynamics processors (compressor / gate / transient_shaper)
// ============================================================================

/** Compressor sidechain detector mode. */
export type CompressorDetector = 'peak' | 'rms' | 'log_rms';

/** Options for `masteringDynamicsCompressor`. */
export interface CompressorOptions extends ValidateOptions {
  thresholdDb?: number;
  ratio?: number;
  attackMs?: number;
  releaseMs?: number;
  kneeDb?: number;
  makeupGainDb?: number;
  autoMakeup?: boolean;
  detector?: CompressorDetector | number;
  sidechainHpfEnabled?: boolean;
  sidechainHpfHz?: number;
  pdrTimeMs?: number;
  pdrReleaseScale?: number;
}

/** Options for `masteringDynamicsGate`. */
export interface GateOptions extends ValidateOptions {
  thresholdDb?: number;
  attackMs?: number;
  releaseMs?: number;
  rangeDb?: number;
  holdMs?: number;
  closeThresholdDb?: number;
  keyHpfHz?: number;
}

/** Options for `masteringDynamicsTransientShaper`. */
export interface TransientShaperOptions extends ValidateOptions {
  attackGainDb?: number;
  sustainGainDb?: number;
  fastAttackMs?: number;
  fastReleaseMs?: number;
  slowAttackMs?: number;
  slowReleaseMs?: number;
  sensitivity?: number;
  maxGainDb?: number;
  gainSmoothingMs?: number;
  lookaheadMs?: number;
}

/** Result envelope returned by offline mastering dynamics processors. */
export interface DynamicsResult {
  samples: Float32Array;
  latencySamples: number;
}

const COMPRESSOR_DETECTOR_MAP: Record<CompressorDetector, number> = {
  peak: 0,
  rms: 1,
  log_rms: 2,
};

/** Offline feed-forward compressor (soft knee, optional auto-makeup / sidechain HPF). */
export function masteringDynamicsCompressor(
  samples: Float32Array,
  sampleRate: number,
  options: CompressorOptions = {},
): DynamicsResult {
  assertSamples('masteringDynamicsCompressor', samples, options.validate !== false);
  const detector =
    typeof options.detector === 'string'
      ? COMPRESSOR_DETECTOR_MAP[options.detector]
      : options.detector;
  const opts: Record<string, unknown> = { ...options };
  if (detector !== undefined) {
    opts.detector = detector;
  }
  return requireModule().masteringDynamicsCompressor(samples, sampleRate, opts);
}

/** Offline noise gate (hysteresis, hold, optional key HPF). */
export function masteringDynamicsGate(
  samples: Float32Array,
  sampleRate: number,
  options: GateOptions = {},
): DynamicsResult {
  assertSamples('masteringDynamicsGate', samples, options.validate !== false);
  return requireModule().masteringDynamicsGate(samples, sampleRate, options);
}

/** Offline transient shaper (envelope-difference attack/sustain control). */
export function masteringDynamicsTransientShaper(
  samples: Float32Array,
  sampleRate: number,
  options: TransientShaperOptions = {},
): DynamicsResult {
  assertSamples('masteringDynamicsTransientShaper', samples, options.validate !== false);
  return requireModule().masteringDynamicsTransientShaper(samples, sampleRate, options);
}

/**
 * Apply a configurable mastering chain in WASM.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param config - Chain stage configuration
 * @returns Processed audio, loudness metadata, and applied stage names
 */
export function masteringChain(
  samples: Float32Array,
  sampleRate = 22050,
  config: MasteringChainConfig,
): MasteringChainResult {
  return requireModule().masteringChain(samples, sampleRate, config as Record<string, unknown>);
}

/**
 * Apply a configurable stereo mastering chain in WASM.
 *
 * @param left - Left channel samples
 * @param right - Right channel samples
 * @param sampleRate - Sample rate in Hz
 * @param config - Chain stage configuration
 * @returns Processed stereo audio, loudness metadata, and applied stage names
 */
export function masteringChainStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  config: MasteringChainConfig,
): MasteringStereoChainResult {
  if (left.length !== right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return requireModule().masteringChainStereo(
    left,
    right,
    sampleRate,
    config as Record<string, unknown>,
  );
}

/**
 * Apply a configurable mastering chain in WASM with progress reporting.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param config - Chain stage configuration
 * @param onProgress - Progress callback (progress: 0-1, stage: string)
 * @returns Processed audio, loudness metadata, and applied stage names
 */
export function masteringChainWithProgress(
  samples: Float32Array,
  sampleRate = 22050,
  config: MasteringChainConfig,
  onProgress: ProgressCallback,
): MasteringChainResult {
  return requireModule().masteringChainWithProgress(
    samples,
    sampleRate,
    config as Record<string, unknown>,
    onProgress,
  );
}

/**
 * Apply a configurable stereo mastering chain in WASM with progress reporting.
 *
 * @param left - Left channel samples
 * @param right - Right channel samples
 * @param sampleRate - Sample rate in Hz
 * @param config - Chain stage configuration
 * @param onProgress - Progress callback (progress: 0-1, stage: string)
 * @returns Processed stereo audio, loudness metadata, and applied stage names
 */
export function masteringChainStereoWithProgress(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  config: MasteringChainConfig,
  onProgress: ProgressCallback,
): MasteringStereoChainResult {
  if (left.length !== right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return requireModule().masteringChainStereoWithProgress(
    left,
    right,
    sampleRate,
    config as Record<string, unknown>,
    onProgress,
  );
}

/**
 * List built-in mastering preset identifiers.
 *
 * @returns Preset names in display order (e.g. "pop", "edm", "aiMusic")
 */
export function masteringPresetNames(): MasteringPreset[] {
  return requireModule().masteringPresetNames() as MasteringPreset[];
}

/**
 * Apply a named mastering preset chain to mono audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param presetName - Preset identifier from {@link masteringPresetNames}
 * @param overrides - Optional flat overrides (dot-notation, e.g. `'loudness.targetLufs'`) applied on top of the preset. Pass `null` for preset defaults.
 * @returns Processed audio, loudness metadata, and applied stage names
 */
export function masterAudio(
  samples: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: Record<string, number | boolean> = {},
): MasteringChainResult {
  return requireModule().masterAudio(presetName, samples, sampleRate, overrides);
}

/**
 * Apply a named mastering preset chain to stereo audio.
 *
 * @param left - Left channel samples
 * @param right - Right channel samples
 * @param sampleRate - Sample rate in Hz
 * @param presetName - Preset identifier from {@link masteringPresetNames}
 * @param overrides - Optional flat overrides (dot-notation, e.g. `'loudness.targetLufs'`) applied on top of the preset. Pass `null` for preset defaults.
 * @returns Processed stereo audio, loudness metadata, and applied stage names
 */
export function masterAudioStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: Record<string, number | boolean> = {},
): MasteringStereoChainResult {
  if (left.length !== right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return requireModule().masterAudioStereo(presetName, left, right, sampleRate, overrides);
}

/**
 * Mono `masterAudio` with per-stage progress reporting. `onProgress` is invoked
 * with `(progress, stage)` between each chain stage (progress is in [0,1]).
 */
export function masterAudioWithProgress(
  samples: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset,
  onProgress: ProgressCallback,
  overrides: Record<string, number | boolean> | null = null,
): MasteringChainResult {
  return requireModule().masterAudioWithProgress(
    presetName,
    samples,
    sampleRate,
    overrides,
    onProgress,
  );
}

/**
 * Stereo `masterAudio` with per-stage progress reporting.
 */
export function masterAudioStereoWithProgress(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset,
  onProgress: ProgressCallback,
  overrides: Record<string, number | boolean> | null = null,
): MasteringStereoChainResult {
  if (left.length !== right.length) {
    throw new Error('Stereo channel lengths must match.');
  }
  return requireModule().masterAudioStereoWithProgress(
    presetName,
    left,
    right,
    sampleRate,
    overrides,
    onProgress,
  );
}

export function mixingScenePresetNames(): string[] {
  return requireModule().mixingScenePresetNames();
}

/**
 * Get a built-in mixing scene preset serialized as JSON. This is the canonical
 * name shared with the Node and Python bindings; the returned JSON loads
 * directly into a {@link Mixer} via {@link Mixer.fromSceneJson}.
 *
 * @param presetName - Preset name (see {@link mixingScenePresetNames})
 * @returns Scene JSON string
 */
export function mixingScenePresetJson(presetName: string): string {
  return requireModule().mixingScenePresetJson(presetName);
}

export function mixStereo(
  leftChannels: Float32Array[],
  rightChannels: Float32Array[],
  sampleRate = 48000,
  options: MixOptions = {},
): MixResult {
  if (leftChannels.length === 0 || leftChannels.length !== rightChannels.length) {
    throw new Error('leftChannels and rightChannels must have the same non-zero length.');
  }
  return requireModule().mixStereo(
    leftChannels,
    rightChannels,
    sampleRate,
    options as Record<string, unknown>,
  );
}
