import type { VoiceChangeOptions } from './effects_mastering.js';
import {
  harmonic,
  hpss,
  masterAudio,
  mastering,
  masteringChain,
  masteringProcess,
  normalize,
  noteMove,
  noteStretch,
  percussive,
  pitchCorrectToMidi,
  pitchShift,
  timeStretch,
  voiceChange,
} from './effects_mastering.js';
import {
  chroma,
  ebur128LoudnessRange,
  lufs,
  melSpectrogram,
  mfcc,
  momentaryLufs,
  nnlsChroma,
  onsetEnvelope,
  pitchPyin,
  pitchYin,
  resample,
  rmsEnergy,
  shortTermLufs,
  spectralBandwidth,
  spectralCentroid,
  spectralFlatness,
  spectralRolloff,
  stft,
  stftDb,
  trim,
  zeroCrossingRate,
} from './features.js';
import type {
  ClippingReport,
  DynamicRangeReport,
  MeteringDetectClippingOptions,
  MeteringDynamicRangeOptions,
  SpectrumOptions,
  SpectrumReport,
} from './metering.js';
import {
  meteringCrestFactorDb,
  meteringDcOffset,
  meteringDetectClipping,
  meteringDynamicRange,
  meteringPeakDb,
  meteringRmsDb,
  meteringSilenceRatio,
  meteringSpectrum,
  meteringSpectrumFrame,
  meteringTruePeakDb,
} from './metering.js';
import { getSonareModule } from './module_state.js';
import type {
  AnalysisResult,
  ChordAnalysisResult,
  ChordDetectionOptions,
  ChromaResult,
  HpssResult,
  Key,
  KeyCandidate,
  KeyDetectionOptions,
  LufsResult,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringOptions,
  MasteringPreset,
  MasteringProcessorParams,
  MasteringResult,
  MelSpectrogramResult,
  MfccResult,
  Mode,
  NoteStretchOptions,
  PitchClass,
  PitchResult,
  SoloProcessor,
  StftResult,
} from './public_types.js';
import type { MusicAnalyzeOptions } from './quick_analysis.js';
import {
  analyze,
  analyzeWithProgress,
  chordFunctionalAnalysis,
  detectBeats,
  detectBpm,
  detectChords,
  detectDownbeats,
  detectKey,
  detectKeyCandidates,
  detectOnsets,
} from './quick_analysis.js';
import type { ProgressCallback, WasmNnlsChromaResult } from './sonare.js';
import type { ValidateOptions } from './validation.js';
import { validateAudioBuffer } from './validation.js';

// ============================================================================
// Audio Class
// ============================================================================

type BrowserDecodeContext = Pick<BaseAudioContext, 'decodeAudioData' | 'sampleRate'>;

export interface BrowserAudioDecodeOptions {
  /**
   * AudioContext/OfflineAudioContext used for browser codec fallback. Its
   * `sampleRate` becomes the returned Audio sample rate.
   */
  audioContext?: BrowserDecodeContext;
  /**
   * Factory used when `audioContext` is omitted. `targetSampleRate` is passed
   * through so browsers that honor AudioContextOptions decode directly at that
   * rate.
   */
  createAudioContext?: (options?: AudioContextOptions) => BrowserDecodeContext;
  /**
   * Requested fallback decode rate when this helper creates the context. If the
   * browser ignores it or a context is supplied, no extra resampling is applied.
   */
  targetSampleRate?: number;
}

function encodedBytesToArrayBuffer(bytes: Uint8Array): ArrayBuffer {
  const copy = new Uint8Array(bytes.byteLength);
  copy.set(bytes);
  return copy.buffer;
}

function getBrowserAudioContextFactory():
  | ((options?: AudioContextOptions) => BrowserDecodeContext)
  | undefined {
  const root = globalThis as typeof globalThis & {
    AudioContext?: new (options?: AudioContextOptions) => BaseAudioContext;
    webkitAudioContext?: new (options?: AudioContextOptions) => BaseAudioContext;
  };
  const Ctor = root.AudioContext ?? root.webkitAudioContext;
  return Ctor ? (options?: AudioContextOptions) => new Ctor(options) : undefined;
}

function audioBufferChannels(buffer: AudioBuffer): Float32Array[] {
  return Array.from({ length: buffer.numberOfChannels }, (_, channel) =>
    buffer.getChannelData(channel),
  );
}

/** Folds decoded channels to mono through {@link downmix}, the same rule the native decoder applies. */
function foldToMono(channels: Float32Array[], length: number): Float32Array {
  if (channels.length === 0 || length === 0) {
    return new Float32Array(length);
  }
  return downmix(channels, 0)[0];
}

async function closeCreatedContext(context: BrowserDecodeContext): Promise<void> {
  const maybeClosable = context as BrowserDecodeContext & { close?: () => Promise<void> };
  if (maybeClosable.close) {
    await maybeClosable.close();
  }
}

/** Result of {@link decodeChannels}. */
export interface DecodedChannels {
  /** Sample rate of the decoded audio, in Hz. */
  sampleRate: number;
  /** One plane per source channel, in the source's own channel order. All the same length. */
  channels: Float32Array[];
}

/**
 * Speaker bed layout, mirroring the C enum `SonareChannelLayout`: `0` = mono,
 * `1` = stereo, `2` = 5.1 (L R C LFE Ls Rs), `3` = 7.1 (L R C LFE Ls Rs Lss Rss).
 */
export type ChannelLayout = 0 | 1 | 2 | 3;

/**
 * Decode audio bytes once and keep every source channel.
 *
 * {@link Audio.fromMemory} folds a multi-channel source to mono as it decodes;
 * this returns the planes instead. Same format set, size ceiling and
 * decoded-buffer contract as `Audio.fromMemory`. A 5.1 source arrives as
 * `L R C LFE Ls Rs`.
 *
 * @param bytes - Encoded audio bytes such as WAV or MP3.
 * @throws SonareError with `codeName: 'DecodeFailed'` for an empty decode or a
 *   non-finite sample, `'InvalidFormat'` for a declared rate outside the
 *   supported range, and `'InvalidParameter'` for empty input.
 *
 * @example
 * ```typescript
 * const { sampleRate, channels } = decodeChannels(bytes);
 * const stereo = channels.length > 2 ? downmix(channels, 1) : channels;
 * ```
 */
export function decodeChannels(bytes: Uint8Array): DecodedChannels {
  return getSonareModule().decodeChannels(bytes);
}

/**
 * Downmix channels to a narrower layout with the ITU-R BS.775 rule.
 *
 * Center and surround enter the front pair at -3 dB and the LFE plane is
 * dropped; a stereo, 5.1 or 7.1 source folds to mono through the same matrix
 * the decoders use. The source layout is the one the channel count names
 * (1, 2, 6 or 8). Supported targets narrow the bed (7.1 to 5.1, 5.1 or 7.1 to
 * stereo, anything modelled to mono) or copy it. A channel count outside
 * 1/2/6/8 folds to mono (`0`) as the unweighted mean of its planes, as the
 * decoders do, and has no other target.
 *
 * @param channels - One `Float32Array` per channel, all of the same length.
 * @param targetLayout - `0` mono, `1` stereo, `2` 5.1, `3` 7.1.
 * @throws SonareError with `codeName: 'InvalidParameter'` for an upmix, an
 *   unknown layout, no channels, or channels of differing length.
 * @returns One `Float32Array` per channel of the target layout.
 */
export function downmix(channels: Float32Array[], targetLayout: ChannelLayout): Float32Array[] {
  return getSonareModule().downmix(channels, targetLayout);
}

/**
 * {@link decodeChannels} with the same browser codec fallback as
 * {@link Audio.fromMemoryWithBrowserFallback}: formats the native decoder does
 * not carry (AAC, OGG, FLAC) go through `decodeAudioData`, and every channel the
 * browser returns is kept.
 */
export async function decodeChannelsWithBrowserFallback(
  bytes: Uint8Array,
  options: BrowserAudioDecodeOptions = {},
): Promise<DecodedChannels> {
  try {
    return decodeChannels(bytes);
  } catch (nativeError) {
    return decodeWithBrowserCodec(bytes, options, nativeError, 'decodeChannels');
  }
}

async function decodeWithBrowserCodec(
  bytes: Uint8Array,
  options: BrowserAudioDecodeOptions,
  nativeError: unknown,
  caller: string,
): Promise<DecodedChannels> {
  const contextFactory = options.createAudioContext ?? getBrowserAudioContextFactory();
  const context =
    options.audioContext ??
    contextFactory?.(
      options.targetSampleRate ? { sampleRate: options.targetSampleRate } : undefined,
    );

  if (!context) {
    throw new Error(
      `${caller} failed and browser decodeAudioData is unavailable: ${
        nativeError instanceof Error ? nativeError.message : String(nativeError)
      }`,
    );
  }

  const createdContext = !options.audioContext;
  try {
    const decoded = await context.decodeAudioData(encodedBytesToArrayBuffer(bytes));
    return {
      sampleRate: decoded.sampleRate || context.sampleRate,
      channels: audioBufferChannels(decoded),
    };
  } catch (fallbackError) {
    throw new Error(
      `${caller} failed and browser decodeAudioData fallback failed: ${
        fallbackError instanceof Error ? fallbackError.message : String(fallbackError)
      }`,
    );
  } finally {
    if (createdContext) {
      await closeCreatedContext(context);
    }
  }
}

/**
 * Wrapper around audio data that exposes analysis and feature functions as
 * instance methods.
 *
 * Not every module-level function has a method here — `analyzeBpm`,
 * `analyzeRhythm`, `analyzeDynamics`, `analyzeTimbre`, `analyzeImpulseResponse`
 * and `detectAcoustic` are reachable as free functions only, and the Node
 * facade exposes them as methods as well. Where a method does exist on both, it
 * takes the same arguments.
 *
 * @example
 * ```typescript
 * import { init, Audio } from '@libraz/libsonare';
 *
 * await init();
 *
 * const audio = Audio.fromBuffer(samples, 44100);
 * console.log('BPM:', audio.detectBpm());
 * console.log('Key:', audio.detectKey().name);
 *
 * const mel = audio.melSpectrogram();
 * ```
 */
export class Audio {
  private _samples: Float32Array;
  private _sampleRate: number;

  private constructor(samples: Float32Array, sampleRate: number) {
    this._samples = samples;
    this._sampleRate = sampleRate;
  }

  /**
   * Create an Audio instance from raw sample data.
   *
   * @param samples - Mono float samples.
   * @param sampleRate - Sample rate in Hz (default `48000`, matching the
   *   Node/Python surfaces).
   */
  static fromBuffer(samples: Float32Array, sampleRate = 48000): Audio {
    validateAudioBuffer(samples, sampleRate);
    return new Audio(samples.slice(), sampleRate);
  }

  /**
   * Create an Audio instance by decoding audio bytes in memory.
   *
   * @param bytes - Encoded audio bytes such as WAV or MP3.
   */
  static fromMemory(bytes: Uint8Array): Audio {
    const decoded = getSonareModule().audioFromMemory(bytes);
    return new Audio(decoded.samples, decoded.sampleRate);
  }

  /**
   * Decode audio bytes with the native WASM decoder first, then fall back to the
   * browser codec stack (`AudioContext.decodeAudioData`) for formats such as
   * AAC, OGG, and FLAC when available. Browser-decoded multi-channel audio is
   * folded to mono with {@link downmix}, the rule the native decoder applies, so
   * a file folds to the same samples whichever decoder ran.
   */
  static async fromMemoryWithBrowserFallback(
    bytes: Uint8Array,
    options: BrowserAudioDecodeOptions = {},
  ): Promise<Audio> {
    try {
      return Audio.fromMemory(bytes);
    } catch (nativeError) {
      const decoded = await decodeWithBrowserCodec(bytes, options, nativeError, 'Audio.fromMemory');
      const length = decoded.channels[0]?.length ?? 0;
      return new Audio(foldToMono(decoded.channels, length), decoded.sampleRate);
    }
  }

  /**
   * A copy of the raw audio samples. Mirrors Node's `getData()` contract: the
   * returned array is independent of the Audio's internal buffer, so mutating
   * it (or transferring it to a Worker) does not affect subsequent facade
   * calls, which all read the internal snapshot directly.
   */
  get data(): Float32Array {
    return this._samples.slice();
  }

  /** Number of samples. */
  get length(): number {
    return this._samples.length;
  }

  /** Sample rate in Hz. */
  get sampleRate(): number {
    return this._sampleRate;
  }

  /** Duration in seconds. */
  get duration(): number {
    return this._samples.length / this._sampleRate;
  }

  // -- Analysis --

  detectBpm(): number {
    return detectBpm(this._samples, this._sampleRate);
  }

  detectKey(options: KeyDetectionOptions = {}): Key {
    return detectKey(this._samples, this._sampleRate, options);
  }

  detectKeyCandidates(options: KeyDetectionOptions = {}): KeyCandidate[] {
    return detectKeyCandidates(this._samples, this._sampleRate, options);
  }

  detectOnsets(): Float32Array {
    return detectOnsets(this._samples, this._sampleRate);
  }

  detectBeats(): Float32Array {
    return detectBeats(this._samples, this._sampleRate);
  }

  detectDownbeats(): Float32Array {
    return detectDownbeats(this._samples, this._sampleRate);
  }

  detectChords(options: ChordDetectionOptions = {}): ChordAnalysisResult {
    return detectChords(this._samples, this._sampleRate, options);
  }

  chordFunctionalAnalysis(
    keyRoot: PitchClass,
    keyMode: Mode,
    options: ChordDetectionOptions = {},
  ): string[] {
    return chordFunctionalAnalysis(this._samples, keyRoot, keyMode, this._sampleRate, options);
  }

  /**
   * Full music analysis of the held buffer.
   *
   * Takes the same option bag as the module-level {@link analyze} and as the
   * Node facade's `Audio.analyze`; this method used to drop it, so the same
   * call was tunable on one binding and fixed at the defaults on the other.
   */
  analyze(options: MusicAnalyzeOptions = {}): AnalysisResult {
    return analyze(this._samples, this._sampleRate, options);
  }

  analyzeWithProgress(onProgress: ProgressCallback): AnalysisResult {
    return analyzeWithProgress(this._samples, this._sampleRate, onProgress);
  }

  // -- Effects --

  hpss(kernelHarmonic = 31, kernelPercussive = 31): HpssResult {
    return hpss(this._samples, this._sampleRate, kernelHarmonic, kernelPercussive);
  }

  harmonic(): Float32Array {
    return harmonic(this._samples, this._sampleRate);
  }

  percussive(): Float32Array {
    return percussive(this._samples, this._sampleRate);
  }

  timeStretch(rate: number): Float32Array {
    return timeStretch(this._samples, this._sampleRate, rate);
  }

  pitchShift(semitones: number): Float32Array {
    return pitchShift(this._samples, this._sampleRate, semitones);
  }

  pitchCorrectToMidi(currentMidi = 69.0, targetMidi = 69.0): Float32Array {
    return pitchCorrectToMidi(this._samples, this._sampleRate, currentMidi, targetMidi);
  }

  noteStretch(options: NoteStretchOptions = {}): Float32Array {
    return noteStretch(this._samples, this._sampleRate, options);
  }

  noteMove(options: import('./public_types.js').NoteMoveOptions = {}): Float32Array {
    return noteMove(this._samples, this._sampleRate, options);
  }

  voiceChange(options: VoiceChangeOptions = {}): Float32Array {
    return voiceChange(this._samples, this._sampleRate, options);
  }

  normalize(targetDb = 0.0): Float32Array {
    return normalize(this._samples, this._sampleRate, targetDb);
  }

  mastering(options: MasteringOptions = {}): MasteringResult {
    return mastering(this._samples, this._sampleRate, options);
  }

  masteringChain(
    config: MasteringChainConfig = {},
    onProgress?: ProgressCallback,
  ): MasteringChainResult {
    return masteringChain({
      samples: this._samples,
      sampleRate: this._sampleRate,
      config,
      onProgress,
    });
  }

  masterAudio(
    presetName: MasteringPreset = 'pop',
    overrides: MasteringChainConfig | null = null,
    onProgress?: ProgressCallback,
  ): MasteringChainResult {
    return masterAudio({
      samples: this._samples,
      sampleRate: this._sampleRate,
      preset: presetName,
      overrides: overrides ?? {},
      onProgress,
    });
  }

  masteringProcess(
    processorName: SoloProcessor,
    params: MasteringProcessorParams = {},
  ): MasteringResult {
    return masteringProcess(processorName, this._samples, this._sampleRate, params);
  }

  trim(thresholdDb = -60.0): Float32Array {
    return trim(this._samples, this._sampleRate, thresholdDb);
  }

  // -- Features --

  stft(nFft = 2048, hopLength = 512): StftResult {
    return stft(this._samples, this._sampleRate, nFft, hopLength);
  }

  stftDb(nFft = 2048, hopLength = 512): { nBins: number; nFrames: number; db: Float32Array } {
    return stftDb(this._samples, this._sampleRate, nFft, hopLength);
  }

  melSpectrogram(
    nFft = 2048,
    hopLength = 512,
    nMels = 128,
    fmin = 0,
    fmax = 0,
    htk = false,
  ): MelSpectrogramResult {
    return melSpectrogram(this._samples, this._sampleRate, nFft, hopLength, nMels, fmin, fmax, htk);
  }

  mfcc(
    nFft = 2048,
    hopLength = 512,
    nMels = 128,
    nMfcc = 20,
    fmin = 0,
    fmax = 0,
    htk = false,
  ): MfccResult {
    return mfcc(this._samples, this._sampleRate, nFft, hopLength, nMels, nMfcc, fmin, fmax, htk);
  }

  chroma(nFft = 2048, hopLength = 512): ChromaResult {
    return chroma(this._samples, this._sampleRate, nFft, hopLength);
  }

  nnlsChroma(): WasmNnlsChromaResult {
    return nnlsChroma(this._samples, this._sampleRate);
  }

  onsetEnvelope(nFft = 2048, hopLength = 512, nMels = 128): Float32Array {
    return onsetEnvelope(this._samples, this._sampleRate, nFft, hopLength, nMels);
  }

  lufs(): LufsResult {
    return lufs(this._samples, this._sampleRate);
  }

  momentaryLufs(): Float32Array {
    return momentaryLufs(this._samples, this._sampleRate);
  }

  shortTermLufs(): Float32Array {
    return shortTermLufs(this._samples, this._sampleRate);
  }

  spectralCentroid(nFft = 2048, hopLength = 512): Float32Array {
    return spectralCentroid(this._samples, this._sampleRate, nFft, hopLength);
  }

  spectralBandwidth(nFft = 2048, hopLength = 512): Float32Array {
    return spectralBandwidth(this._samples, this._sampleRate, nFft, hopLength);
  }

  spectralRolloff(nFft = 2048, hopLength = 512, rollPercent = 0.85): Float32Array {
    return spectralRolloff(this._samples, this._sampleRate, nFft, hopLength, rollPercent);
  }

  spectralFlatness(nFft = 2048, hopLength = 512): Float32Array {
    return spectralFlatness(this._samples, this._sampleRate, nFft, hopLength);
  }

  zeroCrossingRate(frameLength = 2048, hopLength = 512): Float32Array {
    return zeroCrossingRate(this._samples, this._sampleRate, frameLength, hopLength);
  }

  rmsEnergy(frameLength = 2048, hopLength = 512): Float32Array {
    return rmsEnergy(this._samples, this._sampleRate, frameLength, hopLength);
  }

  pitchYin(
    frameLength = 2048,
    hopLength = 512,
    fmin = 65.0,
    fmax = 2093.0,
    threshold = 0.1,
    fillNa = false,
  ): PitchResult {
    return pitchYin(
      this._samples,
      this._sampleRate,
      frameLength,
      hopLength,
      fmin,
      fmax,
      threshold,
      fillNa,
    );
  }

  pitchPyin(
    frameLength = 2048,
    hopLength = 512,
    fmin = 65.0,
    fmax = 2093.0,
    threshold = 0.1,
    fillNa = false,
  ): PitchResult {
    return pitchPyin(
      this._samples,
      this._sampleRate,
      frameLength,
      hopLength,
      fmin,
      fmax,
      threshold,
      fillNa,
    );
  }

  resample(targetSr: number): Float32Array {
    return resample(this._samples, this._sampleRate, targetSr);
  }

  // -- Metering --
  //
  // These delegate to the module-level buffer-form functions rather than a
  // native handle: WASM's Audio is a plain JS wrapper around a Float32Array,
  // not an embind class, so there is no cheaper path to reach the same
  // measurement. The methods exist for call-shape parity with Node/Python,
  // which do hold a native handle here.

  peakDb(): number {
    return meteringPeakDb(this._samples, this._sampleRate);
  }

  rmsDb(): number {
    return meteringRmsDb(this._samples, this._sampleRate);
  }

  dcOffset(): number {
    return meteringDcOffset(this._samples, this._sampleRate);
  }

  crestFactorDb(): number {
    return meteringCrestFactorDb(this._samples, this._sampleRate);
  }

  silenceRatio(thresholdDb = -45, frameLength = 1024, hopLength = 256): number {
    return meteringSilenceRatio(
      this._samples,
      this._sampleRate,
      thresholdDb,
      frameLength,
      hopLength,
    );
  }

  /**
   * Inter-sample (true) peak in dBFS. `oversampleFactor` must be a power of two
   * in [1, 16]; pass 0 to use the library default (4).
   */
  truePeakDb(oversampleFactor = 4): number {
    return meteringTruePeakDb(this._samples, this._sampleRate, oversampleFactor);
  }

  detectClipping(options: MeteringDetectClippingOptions = {}): ClippingReport {
    return meteringDetectClipping(this._samples, this._sampleRate, options);
  }

  dynamicRange(options: MeteringDynamicRangeOptions = {}): DynamicRangeReport {
    return meteringDynamicRange(this._samples, this._sampleRate, options);
  }

  spectrum(options: SpectrumOptions & ValidateOptions = {}): SpectrumReport {
    return meteringSpectrum(this._samples, this._sampleRate, options);
  }

  /**
   * True single-frame magnitude / power / dB spectrum starting at `frameOffset`.
   * See {@link meteringSpectrumFrame} for the frame-validation contract.
   */
  spectrumFrame(frameOffset = 0, options: SpectrumOptions & ValidateOptions = {}): SpectrumReport {
    return meteringSpectrumFrame(this._samples, this._sampleRate, frameOffset, options);
  }

  ebur128LoudnessRange(): number {
    return ebur128LoudnessRange(this._samples, this._sampleRate);
  }

  /** No-op: Audio holds no native handle, so `using` works the same as on the other surfaces. */
  [Symbol.dispose](): void {}
}
