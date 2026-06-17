import { addon } from './native.js';
import type {
  EqBandInput,
  EqSpectrumSnapshot,
  HpssResult,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringChainSection,
  MasteringChainStereoResult,
  MasteringOptions,
  MasteringPreset,
  MasteringResult,
  MasteringStereoResult,
  NoteStretchOptions,
  PairAnalysis,
  PairProcessor,
  RealtimeVoiceChangerConfig,
  RealtimeVoiceChangerConfigInput,
  RealtimeVoiceChangerOptions,
  SoloProcessor,
  SpectralEditOptions,
  SpectralRegionOp,
  StereoAnalysis,
  StreamAnalyzerConfig,
  StreamAnalyzerStats,
  StreamFramesI16,
  StreamFramesSoa,
  StreamFramesU8,
  StreamingPlatform,
  StreamQuantizeConfig,
  VoicePresetId,
} from './types.js';
import type { ValidateOptions } from './validation.js';
import { assertSamples } from './validation.js';

export class RealtimeVoiceChanger {
  private native: InstanceType<typeof addon.RealtimeVoiceChanger>;
  private disposed = false;

  constructor(options: RealtimeVoiceChangerOptions) {
    this.native = new addon.RealtimeVoiceChanger(options.preset ?? 'neutral-monitor');
    this.native.prepare(options.sampleRate, options.maxBlockSize ?? 128, options.channels ?? 1);
  }

  reset(): void {
    this.native.reset();
  }

  setConfig(config: RealtimeVoiceChangerConfigInput): void {
    this.native.setConfig(config);
  }

  configJson(): string {
    return this.native.configJson();
  }

  latencySamples(): number {
    return this.native.latencySamples();
  }

  processMono(input: Float32Array): Float32Array {
    return this.native.processMono(input);
  }

  processMonoInto(input: Float32Array, output: Float32Array): void {
    this.native.processMonoInto(input, output);
  }

  processInterleaved(input: Float32Array, channels: 1 | 2): Float32Array {
    return this.native.processInterleaved(input, channels);
  }

  processInterleavedInto(input: Float32Array, channels: 1 | 2, output: Float32Array): void {
    this.native.processInterleavedInto(input, channels, output);
  }

  /**
   * Process a block of planar (non-interleaved) stereo audio in place. The
   * `left` and `right` buffers must have equal length and are mutated with the
   * processed output. Requires the changer to have been prepared with at least
   * 2 channels.
   */
  processPlanarStereo(left: Float32Array, right: Float32Array): void {
    this.native.processPlanarStereo(left, right);
  }

  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    // N-API ObjectWrap instances do not have a `.delete` method, so this guard
    // is purely defensive in case the native binding ever exposes one. The
    // real lifecycle is GC-driven via the C++ destructor.
    if (typeof this.native.delete === 'function') {
      this.native.delete();
    }
  }

  /** Releases native resources; lets `using` (Node 22+) free them automatically. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

// -- Effects --

export function hpss(
  samples: Float32Array,
  sampleRate = 22050,
  kernelHarmonic = 31,
  kernelPercussive = 31,
): HpssResult {
  return addon.hpss(samples, sampleRate, kernelHarmonic, kernelPercussive);
}

export function harmonic(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.harmonic(samples, sampleRate);
}

export function percussive(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.percussive(samples, sampleRate);
}

export function timeStretch(samples: Float32Array, rate: number, sampleRate = 22050): Float32Array {
  if (typeof rate !== 'number' || !Number.isFinite(rate)) {
    throw new TypeError('timeStretch: rate must be a finite number');
  }
  return addon.timeStretch(samples, sampleRate, rate);
}

/**
 * Region-based spectral editing: STFT -> per-op time x frequency bin/frame
 * masking -> iSTFT. A stateless mono transform whose output has the same length
 * and sample rate as the input.
 *
 * Each {@link SpectralRegionOp} in `ops` is a time x frequency rectangle applied
 * in array order (gain / attenuate / mute / heal). Passing an empty `ops` array
 * is the identity transform (the input is returned). Wraps the C
 * `sonare_spectral_edit`.
 *
 * @param samples - Mono input audio.
 * @param sampleRate - Sample rate in Hz.
 * @param ops - Region ops applied in order.
 * @param options - Optional STFT + heal configuration.
 * @returns The edited audio (same length as `samples`).
 */
export function spectralEdit(
  samples: Float32Array,
  sampleRate: number,
  ops: SpectralRegionOp[] = [],
  options: SpectralEditOptions = {},
): Float32Array {
  return addon.spectralEdit(samples, sampleRate, ops, options);
}

export function pitchShift(
  samples: Float32Array,
  semitones: number,
  sampleRate = 22050,
): Float32Array {
  if (typeof semitones !== 'number' || !Number.isFinite(semitones)) {
    throw new TypeError('pitchShift: semitones must be a finite number');
  }
  return addon.pitchShift(samples, sampleRate, semitones);
}

export function pitchCorrectToMidi(
  samples: Float32Array,
  sampleRate = 22050,
  currentMidi = 69.0,
  targetMidi = 69.0,
): Float32Array {
  return addon.pitchCorrectToMidi(samples, sampleRate, currentMidi, targetMidi);
}

/**
 * Contour-following ("time-varying") pitch correction toward a MIDI target.
 *
 * Unlike {@link pitchCorrectToMidi} (a single constant transpose), this follows
 * the caller-supplied per-frame `f0Hz` contour and retunes every voiced frame
 * toward `targetMidi`, so vibrato/drift in the source is tracked rather than
 * flattened. `voiced` (non-zero = voiced) and `voicedProb` ([0,1]) are optional;
 * omitting them treats every frame as voiced.
 */
export function pitchCorrectToMidiTimevarying(
  samples: Float32Array,
  f0Hz: Float32Array,
  targetMidi: number,
  sampleRate = 22050,
  hopLength = 512,
  voiced?: Int32Array,
  voicedProb?: Float32Array,
): Float32Array {
  return addon.pitchCorrectToMidiTimevarying(
    samples,
    sampleRate,
    f0Hz,
    targetMidi,
    hopLength,
    voiced,
    voicedProb,
  );
}

export function noteStretch(
  samples: Float32Array,
  sampleRate = 22050,
  options: NoteStretchOptions = {},
): Float32Array {
  return addon.noteStretch(
    samples,
    sampleRate,
    options.onsetSample ?? 0,
    options.offsetSample ?? 0,
    options.stretchRatio ?? 1.0,
  );
}

/** Options for {@link voiceChange}. All fields are optional. */
export interface VoiceChangeOptions extends ValidateOptions {
  /** Pitch shift in semitones (negative = down). Default 0. */
  pitchSemitones?: number;
  /** Formant scale factor (>1 brightens, <1 darkens). Default 1. */
  formantFactor?: number;
}

export function voiceChange(
  samples: Float32Array,
  sampleRate = 22050,
  options: VoiceChangeOptions = {},
): Float32Array {
  assertSamples('voiceChange', samples, options.validate !== false);
  return addon.voiceChange(
    samples,
    sampleRate,
    options.pitchSemitones ?? 0.0,
    options.formantFactor ?? 1.0,
  );
}

export interface VoiceChangeRealtimeOptions extends ValidateOptions {
  /** Channel count: 1 = mono, 2 = interleaved stereo (L0,R0,L1,R1,...). */
  channels?: 1 | 2;
}

function latencyCompensatedVoiceChange(
  changer: RealtimeVoiceChanger,
  samples: Float32Array,
  channels: 1 | 2,
  blockFrames: number,
): Float32Array {
  const latencyFrames = Math.max(0, changer.latencySamples());
  if (channels === 1) {
    const total = samples.length + latencyFrames;
    const input = new Float32Array(total);
    input.set(samples);
    const processed = new Float32Array(total);
    for (let pos = 0; pos < total; pos += blockFrames) {
      const inputBlock = input.subarray(pos, Math.min(total, pos + blockFrames));
      const outputBlock = processed.subarray(pos, pos + inputBlock.length);
      changer.processMonoInto(inputBlock, outputBlock);
    }
    return processed.slice(latencyFrames, latencyFrames + samples.length);
  }

  const frames = samples.length / 2;
  const totalFrames = frames + latencyFrames;
  const input = new Float32Array(totalFrames * 2);
  input.set(samples);
  const processed = new Float32Array(totalFrames * 2);
  const frameStride = blockFrames * 2;
  for (let pos = 0; pos < input.length; pos += frameStride) {
    const inputBlock = input.subarray(pos, Math.min(input.length, pos + frameStride));
    const outputBlock = processed.subarray(pos, pos + inputBlock.length);
    changer.processInterleavedInto(inputBlock, 2, outputBlock);
  }
  const offset = latencyFrames * 2;
  return processed.slice(offset, offset + samples.length);
}

export function voiceChangeRealtime(
  samples: Float32Array,
  sampleRate = 48000,
  preset: RealtimeVoiceChangerConfigInput = 'neutral-monitor',
  options: VoiceChangeRealtimeOptions = {},
): Float32Array {
  const validate = options.validate !== false;
  assertSamples('voiceChangeRealtime', samples, validate);
  const channels = options.channels ?? 1;
  if (channels !== 1 && channels !== 2) {
    throw new Error('voiceChangeRealtime: channels must be 1 or 2.');
  }
  if (channels === 2 && samples.length % 2 !== 0) {
    throw new Error('voiceChangeRealtime: stereo input length must be a multiple of 2.');
  }
  const block = 512;
  const changer = new RealtimeVoiceChanger({
    sampleRate,
    maxBlockSize: block,
    channels,
    preset,
  });
  try {
    return latencyCompensatedVoiceChange(changer, samples, channels, block);
  } finally {
    changer.destroy();
  }
}

export function realtimeVoiceChangerPresetNames(): VoicePresetId[] {
  return addon.realtimeVoiceChangerPresetNames() as VoicePresetId[];
}

export function realtimeVoiceChangerPresetJson(name: VoicePresetId): string {
  return addon.realtimeVoiceChangerPresetJson(name);
}

export function validateRealtimeVoiceChangerPresetJson(json: string): {
  ok: boolean;
  normalizedJson?: string;
  error?: string;
} {
  return addon.validateRealtimeVoiceChangerPresetJson(json);
}

// Ordinals mirror the SonareVoiceCharacterPreset enum (sonare_c_types.h).
const VOICE_PRESET_ORDINALS: Record<VoicePresetId, number> = {
  'neutral-monitor': 0,
  'bright-idol': 1,
  'soft-whisper': 2,
  'deep-narrator': 3,
  'robot-mascot': 4,
  'dark-villain': 5,
};

function resolveVoicePresetOrdinal(preset: VoicePresetId | number): number {
  if (typeof preset === 'number') {
    return preset;
  }
  const ordinal = VOICE_PRESET_ORDINALS[preset];
  if (ordinal === undefined) {
    // Mirror the WASM/Python bindings: an unknown preset name is an error, not
    // a silent `undefined` ordinal that would corrupt the native call.
    throw new Error(`Unknown voice character preset: ${preset}`);
  }
  return ordinal;
}

/**
 * Returns the canonical preset id for a voice-character preset ordinal (or id),
 * or `null` when the ordinal is out of range.
 */
export function voiceCharacterPresetId(preset: VoicePresetId | number): VoicePresetId | null {
  return addon.voiceCharacterPresetId(resolveVoicePresetOrdinal(preset)) as VoicePresetId | null;
}

/**
 * Returns the flat (normalized) realtime-voice-changer config for a built-in
 * preset, skipping the JSON round-trip.
 */
export function realtimeVoiceChangerPresetConfig(
  preset: VoicePresetId | number,
): RealtimeVoiceChangerConfig {
  return addon.realtimeVoiceChangerPresetConfig(
    resolveVoicePresetOrdinal(preset),
  ) as RealtimeVoiceChangerConfig;
}

export function normalize(samples: Float32Array, sampleRate = 22050, targetDb = 0.0): Float32Array {
  return addon.normalize(samples, sampleRate, targetDb);
}

export function mastering(
  samples: Float32Array,
  sampleRate = 22050,
  options: MasteringOptions = {},
): MasteringResult {
  return addon.mastering(
    samples,
    sampleRate,
    options.targetLufs ?? -14.0,
    options.ceilingDb ?? -1.0,
    options.truePeakOversample ?? 4,
  );
}

export function masteringProcess(
  processorName: SoloProcessor,
  samples: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): MasteringResult {
  return addon.masteringProcess(processorName, samples, sampleRate, params);
}

export function masteringProcessStereo(
  processorName: SoloProcessor,
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): MasteringStereoResult {
  return addon.masteringProcessStereo(processorName, left, right, sampleRate, params);
}

/**
 * Flattens a nested {@link MasteringChainConfig} into the dot-notation
 * `{ "module.processor.param": value }` map the native core consumes. Internal
 * helper shared by the mastering-chain / master-audio entry points.
 */
function flattenChainConfig(config: MasteringChainConfig): Record<string, number | boolean> {
  const out: Record<string, number | boolean> = {};
  const walk = (node: MasteringChainSection, prefix: string): void => {
    for (const [key, value] of Object.entries(node)) {
      const path = prefix ? `${prefix}.${key}` : key;
      if (typeof value === 'number' || typeof value === 'boolean') {
        out[path] = value;
      } else if (value !== null && typeof value === 'object') {
        walk(value, path);
      }
    }
  };
  walk(config as MasteringChainSection, '');
  return out;
}

export function masteringChain(
  samples: Float32Array,
  sampleRate = 22050,
  config: MasteringChainConfig = {},
  onProgress?: (progress: number, stage: string) => void,
): MasteringChainResult {
  const flat = flattenChainConfig(config);
  if (onProgress) {
    return addon.masteringChainWithProgress(samples, sampleRate, flat, onProgress);
  }
  return addon.masteringChain(samples, sampleRate, flat);
}

export function masteringChainStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  config: MasteringChainConfig = {},
  onProgress?: (progress: number, stage: string) => void,
): MasteringChainStereoResult {
  const flat = flattenChainConfig(config);
  if (onProgress) {
    return addon.masteringChainStereoWithProgress(left, right, sampleRate, flat, onProgress);
  }
  return addon.masteringChainStereo(left, right, sampleRate, flat);
}

/**
 * Configuration accepted by the {@link StreamingMasteringChain} constructor.
 *
 * In addition to the nested/flat mastering chain config keys, the following
 * top-level streaming-only options control the loudness stage in a realtime
 * preview. They are not part of the offline chain config and are read directly
 * by the streaming chain.
 */
export interface StreamingMasteringChainConfig extends Record<string, unknown> {
  /**
   * Precomputed static loudness gain in dB. When `loudness.enabled` is set, the
   * streaming chain cannot measure whole-signal integrated LUFS, so it applies
   * this fixed gain per block before the loudness stage's true-peak limiter
   * (e.g. `target_lufs - measured_integrated_lufs`). When omitted, an enabled
   * loudness stage throws.
   */
  loudnessStaticGainDb?: number;

  /**
   * Offline-measured true-peak (dBFS) of the source the static gain was computed
   * for. When provided, the static gain is clamped to
   * `loudness.ceiling_db - loudnessStaticGainPeakDb` so the streaming preview
   * does not drive the loudness limiter harder than the offline chain. When
   * omitted, the static gain is applied verbatim.
   */
  loudnessStaticGainPeakDb?: number;
}

/**
 * Block-by-block streaming variant of {@link masteringChain}.
 *
 * Maintains processor state across {@link processMono}/{@link processStereo}
 * calls. Only ProcessorBase-backed stages (`eq.tilt`, `dynamics.compressor`,
 * `saturation.tape`, `saturation.exciter`, `spectral.airBand`, `stereo.imager`,
 * `stereo.monoMaker`, `maximizer.truePeakLimiter`) are supported. Constructing
 * with `repair.denoise` enabled throws an Error. A `loudness`-enabled config
 * also throws unless {@link StreamingMasteringChainConfig.loudnessStaticGainDb}
 * is supplied.
 *
 * @example
 * ```typescript
 * const chain = new StreamingMasteringChain({ eq: { tiltDb: 1.0 } });
 * chain.prepare(44100, 512, 1);
 * const out = chain.processMono(blockSamples);
 * chain.reset();
 * ```
 */
export class StreamingMasteringChain {
  private native: InstanceType<typeof addon.StreamingMasteringChain>;

  constructor(config: StreamingMasteringChainConfig = {}) {
    this.native = new addon.StreamingMasteringChain(config);
  }

  /**
   * Initialize processors for the given sample rate and block layout.
   * Stereo-only stages are skipped when ``numChannels`` is 1.
   */
  prepare(sampleRate: number, maxBlockSize: number, numChannels: number): void {
    this.native.prepare(sampleRate, maxBlockSize, numChannels);
  }

  /** Process one mono block; returns the processed samples (same length). */
  processMono(samples: Float32Array): Float32Array {
    return this.native.processMono(samples);
  }

  /** Process one stereo block; returns the processed channels. */
  processStereo(
    left: Float32Array,
    right: Float32Array,
  ): { left: Float32Array; right: Float32Array } {
    return this.native.processStereo(left, right);
  }

  /** Reset all processor state without rebuilding. */
  reset(): void {
    this.native.reset();
  }

  /** Total reported latency in samples across all active processors. */
  latencySamples(): number {
    return this.native.latencySamples();
  }

  /** Ordered stage names that will run (e.g. ``"eq.tilt"``). */
  stageNames(): string[] {
    return this.native.stageNames();
  }
}

/**
 * Stateful real-time / streaming music analyzer.
 *
 * Feed mono blocks with {@link process}; drain analysis frames with
 * {@link readFramesSoa} (or quantized variants) and query the running musical
 * estimate (BPM/key/chord/pattern) with {@link stats}.
 *
 * @example
 * ```typescript
 * const analyzer = new StreamAnalyzer({ sampleRate: 44100 });
 * analyzer.process(block);
 * const frames = analyzer.readFramesSoa(analyzer.availableFrames());
 * const { estimate } = analyzer.stats();
 * ```
 */
export class StreamAnalyzer {
  private native: InstanceType<typeof addon.StreamAnalyzer>;

  constructor(config: StreamAnalyzerConfig = {}) {
    this.native = new addon.StreamAnalyzer(config);
  }

  /** Feed a mono block of samples. */
  process(samples: Float32Array): void {
    this.native.process(samples);
  }

  /** Feed a mono block anchored at an absolute sample offset. */
  processWithOffset(samples: Float32Array, sampleOffset: number): void {
    this.native.processWithOffset(samples, sampleOffset);
  }

  /** Flush the final partial frame with zero-padding. */
  finalize(): void {
    this.native.finalize();
  }

  /** Number of analysis frames ready to read. */
  availableFrames(): number {
    return this.native.availableFrames();
  }

  /** Drain up to `maxFrames` frames as float32 structure-of-arrays. */
  readFramesSoa(maxFrames: number): StreamFramesSoa {
    return this.native.readFramesSoa(maxFrames);
  }

  /**
   * Alias for {@link readFramesSoa}, provided for cross-binding naming
   * consistency (WASM exposes both `readFrames` and `readFramesSoa`; Python
   * uses `read_frames`).
   */
  readFrames(maxFrames: number): StreamFramesSoa {
    return this.readFramesSoa(maxFrames);
  }

  /**
   * Drain up to `maxFrames` frames as uint8-quantized arrays. Pass
   * `quantizeConfig` to widen the quantization ranges for a stream louder or
   * quieter than the defaults (omitted keeps the defaults).
   */
  readFramesU8(maxFrames: number, quantizeConfig?: StreamQuantizeConfig): StreamFramesU8 {
    return this.native.readFramesU8(maxFrames, quantizeConfig);
  }

  /**
   * Drain up to `maxFrames` frames as int16-quantized arrays. Pass
   * `quantizeConfig` to widen the quantization ranges for a stream louder or
   * quieter than the defaults (omitted keeps the defaults).
   */
  readFramesI16(maxFrames: number, quantizeConfig?: StreamQuantizeConfig): StreamFramesI16 {
    return this.native.readFramesI16(maxFrames, quantizeConfig);
  }

  /** Reset analyzer state; optionally re-anchor to a base sample offset. */
  reset(baseOffset = 0): void {
    this.native.reset(baseOffset);
  }

  /** Current progressive musical estimate and totals. */
  stats(): StreamAnalyzerStats {
    return this.native.stats();
  }

  /** Total frames processed so far. */
  frameCount(): number {
    return this.native.frameCount();
  }

  /** Current analysis time in seconds. */
  currentTime(): number {
    return this.native.currentTime();
  }

  /** Configured sample rate in Hz. */
  sampleRate(): number {
    return this.native.sampleRate();
  }

  /** Hint the expected total duration (seconds) to tune progressive estimates. */
  setExpectedDuration(seconds: number): void {
    this.native.setExpectedDuration(seconds);
  }

  /** Set a normalization gain applied to incoming samples. */
  setNormalizationGain(gain: number): void {
    this.native.setNormalizationGain(gain);
  }

  /** Set the tuning reference frequency (Hz) for key/chroma analysis. */
  setTuningRefHz(hz: number): void {
    this.native.setTuningRefHz(hz);
  }
}

const EQ_PHASE_MODES: Record<string, number> = {
  zero: 1,
  'zero-latency': 1,
  zero_latency: 1,
  natural: 2,
  'natural-phase': 2,
  natural_phase: 2,
  linear: 3,
  'linear-phase': 3,
  linear_phase: 3,
};

/**
 * Block-by-block unified equalizer (zero-latency / natural / linear phase).
 *
 * Wraps the native `EqualizerProcessor`; state persists across
 * {@link processMono}/{@link processStereo} calls.
 *
 * @example
 * ```typescript
 * const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
 * eq.setBand(0, { type: 'HighShelf', frequencyHz: 8000, gainDb: 6, enabled: true });
 * const { left, right } = eq.processStereo(blockLeft, blockRight);
 * ```
 */
export class StreamingEqualizer {
  private native: InstanceType<typeof addon.StreamingEqualizer>;

  constructor(config: { sampleRate?: number; maxBlockSize?: number } = {}) {
    this.native = new addon.StreamingEqualizer(config);
  }

  /** Configure one EQ band (0-based index). */
  setBand(index: number, band: EqBandInput): void {
    this.native.setBand(index, band);
  }

  /** Disable all bands. */
  clear(): void {
    this.native.clear();
  }

  /** Set the global phase mode: ``'zero'`` | ``'natural'`` | ``'linear'`` or 1/2/3. */
  setPhaseMode(mode: 'zero' | 'natural' | 'linear' | number): void {
    const value = typeof mode === 'number' ? mode : EQ_PHASE_MODES[mode.toLowerCase()];
    if (value === undefined) {
      throw new Error(`unknown EQ phase mode: ${mode}`);
    }
    this.native.setPhaseMode(value);
  }

  /** Enable or disable output auto-gain compensation. */
  setAutoGain(enabled: boolean): void {
    this.native.setAutoGain(enabled);
  }

  /** Set all-band EQ gain scale as a 0.0..2.0 multiplier. */
  setGainScale(scale: number): void {
    this.native.setGainScale(scale);
  }

  /** Set post-EQ output gain in dB. */
  setOutputGainDb(gainDb: number): void {
    this.native.setOutputGainDb(gainDb);
  }

  /** Set post-EQ stereo balance in -1.0..1.0; mono input ignores pan. */
  setOutputPan(pan: number): void {
    this.native.setOutputPan(pan);
  }

  /** Set a mono external key for dynamic bands with `externalSidechain` enabled. */
  setSidechainMono(samples: Float32Array): void {
    this.native.setSidechainMono(samples);
  }

  /** Set a stereo external key for dynamic bands with `externalSidechain` enabled. */
  setSidechainStereo(left: Float32Array, right: Float32Array): void {
    this.native.setSidechainStereo(left, right);
  }

  /** Clear any pending external key before the next process call. */
  clearSidechain(): void {
    this.native.clearSidechain();
  }

  /** Last applied auto-gain in dB (0 when disabled). */
  lastAutoGainDb(): number {
    return this.native.lastAutoGainDb();
  }

  /** Reported processing latency in samples. */
  latencySamples(): number {
    return this.native.latencySamples();
  }

  /** Process one mono block; returns the processed samples (same length). */
  processMono(samples: Float32Array): Float32Array {
    return this.native.processMono(samples);
  }

  /** Process one stereo block; returns the processed channels. */
  processStereo(
    left: Float32Array,
    right: Float32Array,
  ): { left: Float32Array; right: Float32Array } {
    return this.native.processStereo(left, right);
  }

  /** Latest realtime-safe spectrum snapshot. */
  spectrum(): EqSpectrumSnapshot {
    return this.native.spectrum();
  }

  /** Configure bands to match a reference spectrum (offline analysis). */
  match(
    source: Float32Array,
    reference: Float32Array,
    options: { sampleRate?: number; maxBands?: number } = {},
  ): void {
    this.native.match(source, reference, options);
  }
}

export function masteringPresetNames(): MasteringPreset[] {
  return addon.masteringPresetNames();
}

export function masterAudio(
  samples: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: MasteringChainConfig = {},
  onProgress?: (progress: number, stage: string) => void,
): MasteringChainResult {
  const flat = flattenChainConfig(overrides);
  if (onProgress) {
    return addon.masterAudioWithProgress(presetName, samples, sampleRate, flat, onProgress);
  }
  return addon.masterAudio(presetName, samples, sampleRate, flat);
}

/**
 * Asynchronous variant of {@link masterAudio}. Runs the full chain on a libuv
 * worker thread; the returned promise resolves with the same shape as the
 * synchronous version. Progress reporting is not available on the async path
 * (use the synchronous `masterAudio` with `onProgress` if you need it, or
 * spin up multiple async calls in parallel).
 */
export function masterAudioAsync(
  samples: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: MasteringChainConfig = {},
): Promise<MasteringChainResult> {
  return addon.masterAudioAsync(presetName, samples, sampleRate, flattenChainConfig(overrides));
}

export function masterAudioStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: MasteringChainConfig = {},
  onProgress?: (progress: number, stage: string) => void,
): MasteringChainStereoResult {
  const flat = flattenChainConfig(overrides);
  if (onProgress) {
    return addon.masterAudioStereoWithProgress(
      presetName,
      left,
      right,
      sampleRate,
      flat,
      onProgress,
    );
  }
  return addon.masterAudioStereo(presetName, left, right, sampleRate, flat);
}

/**
 * Asynchronous variant of {@link masterAudioStereo}.
 */
export function masterAudioStereoAsync(
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  presetName: MasteringPreset = 'pop',
  overrides: MasteringChainConfig = {},
): Promise<MasteringChainStereoResult> {
  return addon.masterAudioStereoAsync(
    presetName,
    left,
    right,
    sampleRate,
    flattenChainConfig(overrides),
  );
}

export function masteringProcessorNames(): SoloProcessor[] {
  return addon.masteringProcessorNames();
}

export function masteringPairProcessorNames(): PairProcessor[] {
  return addon.masteringPairProcessorNames();
}

export function masteringPairAnalysisNames(): PairAnalysis[] {
  return addon.masteringPairAnalysisNames();
}

export function masteringStereoAnalysisNames(): StereoAnalysis[] {
  return addon.masteringStereoAnalysisNames();
}

/**
 * Returns the channel-strip insert / FX processor names that mixing scene
 * inserts can build (includes the creative effects.* reverbs / modulation /
 * delay when FX support is compiled in). Use these to discover valid insert
 * names instead of hardcoding magic strings.
 */
export function masteringInsertNames(): string[] {
  return addon.masteringInsertNames();
}

/**
 * Returns the camelCase parameter names a given insert / FX processor reads, for
 * tooling/validation. Any key NOT in this list is silently ignored by the
 * processor (and would be reported via {@link Mixer.sceneWarnings} when a scene
 * carrying it is loaded). Band/sub-band processors enumerate their indexed
 * `band{i}.<field>` keys. Returns an empty array for an unknown name (or one
 * whose insert needs an unavailable build feature, e.g. FX).
 *
 * @param name - Insert processor name (see {@link masteringInsertNames}).
 */
export function masteringInsertParamNames(name: string): string[] {
  return addon.masteringInsertParamNames(name);
}

/**
 * Apply a two-input `match.*` processor. `source` and `reference` may have
 * independent lengths — the match primitives consume each buffer at its own
 * length.
 */
export function masteringPairProcess(
  processorName: PairProcessor,
  source: Float32Array,
  reference: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): MasteringResult {
  return addon.masteringPairProcess(processorName, source, reference, sampleRate, params);
}

/**
 * Analyze a `source` against a `reference` with a two-input analysis. The two
 * buffers may have independent lengths.
 */
export function masteringPairAnalyze(
  analysisName: PairAnalysis,
  source: Float32Array,
  reference: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): string {
  return addon.masteringPairAnalyze(analysisName, source, reference, sampleRate, params);
}

export function masteringStereoAnalyze(
  analysisName: StereoAnalysis,
  left: Float32Array,
  right: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): string {
  return addon.masteringStereoAnalyze(analysisName, left, right, sampleRate, params);
}

export function masteringAssistantSuggest(
  samples: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): string {
  return addon.masteringAssistantSuggest(samples, sampleRate, params);
}

export function masteringAudioProfile(
  samples: Float32Array,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): string {
  return addon.masteringAudioProfile(samples, sampleRate, params);
}

export function masteringStreamingPreview(
  samples: Float32Array,
  sampleRate = 22050,
  platforms: StreamingPlatform[] = [],
): string {
  return addon.masteringStreamingPreview(samples, sampleRate, platforms);
}

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
  sampleRate = 22050,
  options: DeclickOptions = {},
): Float32Array {
  return addon.masteringRepairDeclick(samples, sampleRate, options);
}

/** Offline STFT-domain classical denoiser (LogMMSE / MMSE-STSA / SpectralSubtraction). */
export function masteringRepairDenoiseClassical(
  samples: Float32Array,
  sampleRate = 22050,
  options: DenoiseClassicalOptions = {},
): Float32Array {
  return addon.masteringRepairDenoiseClassical(samples, sampleRate, options);
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
  sampleRate = 22050,
  options: DeclipOptions = {},
): Float32Array {
  return addon.masteringRepairDeclip(samples, sampleRate, options);
}

/** Offline crackle suppressor (median or wavelet-shrinkage). */
export function masteringRepairDecrackle(
  samples: Float32Array,
  sampleRate = 22050,
  options: DecrackleOptions = {},
): Float32Array {
  return addon.masteringRepairDecrackle(samples, sampleRate, options);
}

/** Offline mains-hum remover. */
export function masteringRepairDehum(
  samples: Float32Array,
  sampleRate = 22050,
  options: DehumOptions = {},
): Float32Array {
  return addon.masteringRepairDehum(samples, sampleRate, options);
}

/** Offline classical dereverberator (spectral subtraction + optional WPE). */
export function masteringRepairDereverbClassical(
  samples: Float32Array,
  sampleRate = 22050,
  options: DereverbClassicalOptions = {},
): Float32Array {
  return addon.masteringRepairDereverbClassical(samples, sampleRate, options);
}

/** Offline silence trimmer (peak threshold or LUFS-gated). */
export function masteringRepairTrimSilence(
  samples: Float32Array,
  sampleRate = 22050,
  options: TrimSilenceOptions = {},
): Float32Array {
  return addon.masteringRepairTrimSilence(samples, sampleRate, options);
}

/** Detector mode for `masteringDynamicsCompressor`. */
export type CompressorDetector = 'peak' | 'rms' | 'log_rms' | 'logRms' | 0 | 1 | 2;

/** Options for `masteringDynamicsCompressor`. */
export interface CompressorOptions extends ValidateOptions {
  thresholdDb?: number;
  ratio?: number;
  attackMs?: number;
  releaseMs?: number;
  kneeDb?: number;
  makeupGainDb?: number;
  autoMakeup?: boolean;
  detector?: CompressorDetector;
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

/** Result of an offline dynamics processor call. */
export interface DynamicsProcessorResult {
  samples: Float32Array;
  latencySamples: number;
}

/** Offline feed-forward compressor (soft-knee, optional makeup, sidechain HPF, PDR). */
export function masteringDynamicsCompressor(
  samples: Float32Array,
  sampleRate = 22050,
  options: CompressorOptions = {},
): DynamicsProcessorResult {
  assertSamples('masteringDynamicsCompressor', samples, options.validate !== false);
  return addon.masteringDynamicsCompressor(samples, sampleRate, options);
}

/** Offline noise gate with hysteresis, hold, and optional key HPF. */
export function masteringDynamicsGate(
  samples: Float32Array,
  sampleRate = 22050,
  options: GateOptions = {},
): DynamicsProcessorResult {
  assertSamples('masteringDynamicsGate', samples, options.validate !== false);
  return addon.masteringDynamicsGate(samples, sampleRate, options);
}

/** Offline transient shaper (envelope-difference attack/sustain control). */
export function masteringDynamicsTransientShaper(
  samples: Float32Array,
  sampleRate = 22050,
  options: TransientShaperOptions = {},
): DynamicsProcessorResult {
  assertSamples('masteringDynamicsTransientShaper', samples, options.validate !== false);
  return addon.masteringDynamicsTransientShaper(samples, sampleRate, options);
}
