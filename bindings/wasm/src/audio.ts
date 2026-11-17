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
import { ErrorCode, SonareError } from './errors.js';
import type { NnlsChromaRequest, OnsetStrengthRequest } from './feature_music.js';
import type { PitchPyinRequest, PitchYinRequest } from './feature_pitch.js';
import type {
  RmsEnergyRequest,
  SpectralBandwidthRequest,
  SpectralCentroidRequest,
  SpectralFlatnessRequest,
  SpectralRolloffRequest,
  ZeroCrossingRateRequest,
} from './feature_spectral.js';
import type {
  ChromaRequest,
  MelSpectrogramRequest,
  MfccRequest,
  StftDbRequest,
  StftRequest,
} from './feature_spectrogram.js';
import {
  chroma,
  ebur128LoudnessRange,
  lufs,
  melSpectrogram,
  mfcc,
  momentaryLufs,
  nnlsChroma,
  onsetStrength,
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
  AcousticResult,
  AnalysisResult,
  ChordAnalysisResult,
  ChordDetectionOptions,
  ChromaResult,
  DynamicsResult,
  FunctionalChord,
  HpssResult,
  KeyCandidate,
  KeyDetection,
  KeyDetectionOptions,
  LufsResult,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringOptions,
  MasteringPreset,
  MasteringResult,
  MasteringSoloProcessorParams,
  Matrix2D,
  MelSpectrogramResult,
  MfccResult,
  NoteStretchOptions,
  PitchResult,
  RhythmResult,
  SoloProcessor,
  StftResult,
  TimbreResult,
} from './public_types.js';
import type {
  AnalyzeBpmRequest,
  AnalyzeDynamicsRequest,
  AnalyzeImpulseResponseRequest,
  AnalyzeRequest,
  AnalyzeRhythmRequest,
  AnalyzeTimbreRequest,
  BpmAnalysisResult,
  ChordFunctionalAnalysisRequest,
  DetectOnsetsRequest,
} from './quick_analysis.js';
import {
  analyze,
  analyzeBpm,
  analyzeDynamics,
  analyzeImpulseResponse,
  analyzeRhythm,
  analyzeTimbre,
  chordFunctionalAnalysis,
  detectBeats,
  detectBpm,
  detectChords,
  detectDownbeats,
  detectKey,
  detectKeyCandidates,
  detectOnsets,
} from './quick_analysis.js';
import type { ProgressCallback } from './sonare.js';
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

/** Options for {@link downmix}. */
export interface DownmixOptions {
  /**
   * Scale the fold so the worst case (every input at full scale, in phase)
   * cannot exceed unity. The scale is a peak divisor, not a loudness match, so
   * typical material comes out a few dB quieter. Default `false`.
   */
  normalize?: boolean;
  /** Fold the LFE plane into the front pair at -3 dB instead of dropping it. Default `false`. */
  includeLfe?: boolean;
}

/**
 * Decode audio bytes once and keep every source channel.
 *
 * {@link Audio.fromMemory} folds a multi-channel source to mono as it decodes;
 * this returns the planes instead. Same format set, size ceiling and
 * decoded-buffer contract as `Audio.fromMemory`. A 5.1 source arrives as
 * `L R C LFE Ls Rs`. Folding the planes with {@link downmix} is not normalised
 * and can exceed full scale.
 *
 * @param bytes - Encoded audio bytes such as WAV or MP3.
 * @throws SonareError with `codeName: 'DecodeFailed'` for an empty decode or a
 *   non-finite sample and `'InvalidFormat'` for a declared rate outside the
 *   supported range.
 * @throws RangeError for empty input.
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
 * The fold is not normalised: the coefficients sum, so a source with several
 * full-scale planes can leave the output past full scale (six planes of 1.0
 * fold to 2.414 on a stereo target). Pass `normalize: true` to scale the output
 * so the worst case stays at or below full scale, or leave headroom before
 * calling.
 *
 * @param channels - One `Float32Array` per channel, all of the same length.
 * @param targetLayout - `0` mono, `1` stereo, `2` 5.1, `3` 7.1.
 * @param options - `normalize` scales the fold so the worst case (every input
 *   at full scale, in phase) cannot exceed unity; `includeLfe` folds the LFE
 *   plane in at -3 dB instead of dropping it. Both default to `false`.
 * @throws RangeError for an upmix or an unknown layout.
 * @throws RangeError for no channels, or channels of differing length.
 * @throws TypeError for an `options` that is not an object or a non-boolean option.
 * @returns One `Float32Array` per channel of the target layout.
 */
export function downmix(
  channels: Float32Array[],
  targetLayout: ChannelLayout,
  options: DownmixOptions = {},
): Float32Array[] {
  return getSonareModule().downmix(channels, targetLayout, options);
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
    throw new SonareError(
      ErrorCode.NotSupported,
      `${caller} failed and browser decodeAudioData is unavailable: ${
        nativeError instanceof Error ? nativeError.message : String(nativeError)
      }`,
      { cause: nativeError },
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
    throw new SonareError(
      ErrorCode.DecodeFailed,
      `${caller} failed and browser decodeAudioData fallback failed: ${
        fallbackError instanceof Error ? fallbackError.message : String(fallbackError)
      }`,
      { cause: fallbackError },
    );
  } finally {
    if (createdContext) {
      await closeCreatedContext(context);
    }
  }
}

/**
 * The request an {@link Audio} method hands to its module function: the
 * options bag plus the held samples and rate. `null` options mean absent.
 */
function audioRequest<R>(
  methodName: string,
  samples: Float32Array,
  sampleRate: number,
  options: unknown,
): R {
  const bag = options ?? {};
  if (typeof bag !== 'object' || Array.isArray(bag) || ArrayBuffer.isView(bag)) {
    throw new TypeError(`Audio.${methodName}: options must be an object`);
  }
  for (const key of ['samples', 'sampleRate']) {
    if (key in bag) {
      throw new TypeError(`Audio.${methodName}: options must not carry '${key}'`);
    }
  }
  return { ...bag, samples, sampleRate } as R;
}

type AudioOptions<R> = Omit<R, 'samples' | 'sampleRate'>;

/**
 * Wrapper around audio data that exposes analysis and feature functions as
 * instance methods.
 *
 * Analysis and feature methods take `(options?)`, the module function's request
 * without `samples` and `sampleRate`, and return what that function returns.
 * `detectAcoustic` is reachable as a free function only. The Node facade has
 * the same method set.
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
  private _samples: Float32Array | null;
  private readonly _sampleRate: number;

  private constructor(samples: Float32Array, sampleRate: number) {
    this._samples = samples;
    this._sampleRate = sampleRate;
  }

  /** The held samples; refuses once {@link destroy} released them. */
  private requireAlive(): Float32Array {
    if (this._samples === null) {
      throw new SonareError(ErrorCode.InvalidState, 'Audio has been destroyed');
    }
    return this._samples;
  }

  private get samples(): Float32Array {
    return this.requireAlive();
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
   * The samples. Every read returns the same array, which is shared with this
   * Audio's own methods and must not be mutated or transferred (copy it with
   * `slice()` first).
   *
   * @throws SonareError `InvalidState` after {@link destroy}.
   */
  get data(): Float32Array {
    return this.samples;
  }

  /**
   * Number of samples.
   *
   * @throws SonareError `InvalidState` after {@link destroy}.
   */
  get length(): number {
    return this.samples.length;
  }

  /**
   * Sample rate in Hz.
   *
   * @throws SonareError `InvalidState` after {@link destroy}.
   */
  get sampleRate(): number {
    this.requireAlive();
    return this._sampleRate;
  }

  /**
   * Duration in seconds.
   *
   * @throws SonareError `InvalidState` after {@link destroy}.
   */
  get durationSec(): number {
    return this.samples.length / this._sampleRate;
  }

  /** Releases the samples; later reads throw `SonareError` `InvalidState`. A second call does nothing. */
  destroy(): void {
    this._samples = null;
  }

  // -- Analysis --

  detectBpm(): number {
    return detectBpm(this.samples, this.sampleRate);
  }

  detectKey(options: KeyDetectionOptions = {}): KeyDetection {
    return detectKey(this.samples, this.sampleRate, options);
  }

  detectKeyCandidates(options: KeyDetectionOptions = {}): KeyCandidate[] {
    return detectKeyCandidates(this.samples, this.sampleRate, options);
  }

  detectOnsets(options?: AudioOptions<DetectOnsetsRequest>): Float32Array {
    return detectOnsets(audioRequest('detectOnsets', this.samples, this.sampleRate, options));
  }

  detectBeats(): Float32Array {
    return detectBeats(this.samples, this.sampleRate);
  }

  detectDownbeats(): Float32Array {
    return detectDownbeats(this.samples, this.sampleRate);
  }

  detectChords(options: ChordDetectionOptions = {}): ChordAnalysisResult {
    return detectChords(this.samples, this.sampleRate, options);
  }

  /** Chords with their roman numeral and function in the given key. `keyRoot` is required. */
  chordFunctionalAnalysis(
    options: AudioOptions<ChordFunctionalAnalysisRequest>,
  ): FunctionalChord[] {
    return chordFunctionalAnalysis(
      audioRequest('chordFunctionalAnalysis', this.samples, this.sampleRate, options),
    );
  }

  /**
   * Full music analysis of the held buffer. Takes the module {@link analyze}
   * request without the audio; `onProgress` and `cancel` report and stop it.
   */
  analyze(options?: AudioOptions<AnalyzeRequest>): AnalysisResult {
    return analyze(audioRequest('analyze', this.samples, this.sampleRate, options));
  }

  analyzeBpm(options?: AudioOptions<AnalyzeBpmRequest>): BpmAnalysisResult {
    return analyzeBpm(audioRequest('analyzeBpm', this.samples, this.sampleRate, options));
  }

  analyzeRhythm(options?: AudioOptions<AnalyzeRhythmRequest>): RhythmResult {
    return analyzeRhythm(audioRequest('analyzeRhythm', this.samples, this.sampleRate, options));
  }

  analyzeDynamics(options?: AudioOptions<AnalyzeDynamicsRequest>): DynamicsResult {
    return analyzeDynamics(audioRequest('analyzeDynamics', this.samples, this.sampleRate, options));
  }

  analyzeTimbre(options?: AudioOptions<AnalyzeTimbreRequest>): TimbreResult {
    return analyzeTimbre(audioRequest('analyzeTimbre', this.samples, this.sampleRate, options));
  }

  analyzeImpulseResponse(options?: AudioOptions<AnalyzeImpulseResponseRequest>): AcousticResult {
    return analyzeImpulseResponse(
      audioRequest('analyzeImpulseResponse', this.samples, this.sampleRate, options),
    );
  }

  // -- Effects --

  hpss(kernelHarmonic = 31, kernelPercussive = 31): HpssResult {
    return hpss(this.samples, this.sampleRate, kernelHarmonic, kernelPercussive);
  }

  harmonic(): Float32Array {
    return harmonic(this.samples, this.sampleRate);
  }

  percussive(): Float32Array {
    return percussive(this.samples, this.sampleRate);
  }

  timeStretch(rate: number): Float32Array {
    return timeStretch(this.samples, this.sampleRate, rate);
  }

  pitchShift(semitones: number): Float32Array {
    return pitchShift(this.samples, this.sampleRate, semitones);
  }

  pitchCorrectToMidi(currentMidi = 69.0, targetMidi = 69.0): Float32Array {
    return pitchCorrectToMidi(this.samples, this.sampleRate, currentMidi, targetMidi);
  }

  noteStretch(options: NoteStretchOptions = {}): Float32Array {
    return noteStretch(this.samples, this.sampleRate, options);
  }

  noteMove(options: import('./public_types.js').NoteMoveOptions = {}): Float32Array {
    return noteMove(this.samples, this.sampleRate, options);
  }

  voiceChange(options: VoiceChangeOptions = {}): Float32Array {
    return voiceChange(this.samples, this.sampleRate, options);
  }

  normalize(targetDb = 0.0): Float32Array {
    return normalize(this.samples, this.sampleRate, targetDb);
  }

  mastering(options: MasteringOptions = {}): MasteringResult {
    return mastering(this.samples, this.sampleRate, options);
  }

  masteringChain(
    config: MasteringChainConfig = {},
    onProgress?: ProgressCallback,
  ): MasteringChainResult {
    return masteringChain({
      samples: this.samples,
      sampleRate: this.sampleRate,
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
      samples: this.samples,
      sampleRate: this.sampleRate,
      preset: presetName,
      overrides: overrides ?? {},
      onProgress,
    });
  }

  masteringProcess(
    processorName: SoloProcessor,
    params: MasteringSoloProcessorParams = {},
  ): MasteringResult {
    return masteringProcess(processorName, this.samples, this.sampleRate, params);
  }

  trim(thresholdDb = -60.0): Float32Array {
    return trim(this.samples, this.sampleRate, thresholdDb);
  }

  // -- Features --

  stft(options?: AudioOptions<StftRequest>): StftResult {
    return stft(audioRequest('stft', this.samples, this.sampleRate, options));
  }

  /** Rows are frequency bins, cols are frames. */
  stftDb(options?: AudioOptions<StftDbRequest>): Matrix2D {
    return stftDb(audioRequest('stftDb', this.samples, this.sampleRate, options));
  }

  melSpectrogram(options?: AudioOptions<MelSpectrogramRequest>): MelSpectrogramResult {
    return melSpectrogram(audioRequest('melSpectrogram', this.samples, this.sampleRate, options));
  }

  mfcc(options?: AudioOptions<MfccRequest>): MfccResult {
    return mfcc(audioRequest('mfcc', this.samples, this.sampleRate, options));
  }

  chroma(options?: AudioOptions<ChromaRequest>): ChromaResult {
    return chroma(audioRequest('chroma', this.samples, this.sampleRate, options));
  }

  /** Rows are the 12 chroma bins, cols are frames. */
  nnlsChroma(options?: AudioOptions<NnlsChromaRequest>): Matrix2D {
    return nnlsChroma(audioRequest('nnlsChroma', this.samples, this.sampleRate, options));
  }

  onsetStrength(options?: AudioOptions<OnsetStrengthRequest>): Float32Array {
    return onsetStrength(audioRequest('onsetStrength', this.samples, this.sampleRate, options));
  }

  lufs(): LufsResult {
    return lufs(this.samples, this.sampleRate);
  }

  momentaryLufs(): Float32Array {
    return momentaryLufs(this.samples, this.sampleRate);
  }

  shortTermLufs(): Float32Array {
    return shortTermLufs(this.samples, this.sampleRate);
  }

  spectralCentroid(options?: AudioOptions<SpectralCentroidRequest>): Float32Array {
    return spectralCentroid(
      audioRequest('spectralCentroid', this.samples, this.sampleRate, options),
    );
  }

  spectralBandwidth(options?: AudioOptions<SpectralBandwidthRequest>): Float32Array {
    return spectralBandwidth(
      audioRequest('spectralBandwidth', this.samples, this.sampleRate, options),
    );
  }

  spectralRolloff(options?: AudioOptions<SpectralRolloffRequest>): Float32Array {
    return spectralRolloff(audioRequest('spectralRolloff', this.samples, this.sampleRate, options));
  }

  spectralFlatness(options?: AudioOptions<SpectralFlatnessRequest>): Float32Array {
    return spectralFlatness(
      audioRequest('spectralFlatness', this.samples, this.sampleRate, options),
    );
  }

  zeroCrossingRate(options?: AudioOptions<ZeroCrossingRateRequest>): Float32Array {
    return zeroCrossingRate(
      audioRequest('zeroCrossingRate', this.samples, this.sampleRate, options),
    );
  }

  rmsEnergy(options?: AudioOptions<RmsEnergyRequest>): Float32Array {
    return rmsEnergy(audioRequest('rmsEnergy', this.samples, this.sampleRate, options));
  }

  pitchYin(options?: AudioOptions<PitchYinRequest>): PitchResult {
    return pitchYin(audioRequest('pitchYin', this.samples, this.sampleRate, options));
  }

  pitchPyin(options?: AudioOptions<PitchPyinRequest>): PitchResult {
    return pitchPyin(audioRequest('pitchPyin', this.samples, this.sampleRate, options));
  }

  resample(targetSampleRate: number): Float32Array {
    return resample(this.samples, this.sampleRate, targetSampleRate);
  }

  // -- Metering --
  //
  // These delegate to the module-level buffer-form functions rather than a
  // native handle: WASM's Audio is a plain JS wrapper around a Float32Array,
  // not an embind class, so there is no cheaper path to reach the same
  // measurement. The methods exist for call-shape parity with Node/Python,
  // which do hold a native handle here.

  peakDb(): number {
    return meteringPeakDb(this.samples, this.sampleRate);
  }

  rmsDb(): number {
    return meteringRmsDb(this.samples, this.sampleRate);
  }

  dcOffset(): number {
    return meteringDcOffset(this.samples, this.sampleRate);
  }

  crestFactorDb(): number {
    return meteringCrestFactorDb(this.samples, this.sampleRate);
  }

  silenceRatio(thresholdDb = -45, frameLength = 1024, hopLength = 256): number {
    return meteringSilenceRatio(this.samples, this.sampleRate, thresholdDb, frameLength, hopLength);
  }

  /**
   * Inter-sample (true) peak in dBFS. `oversampleFactor` must be a power of two
   * in [1, 16]; pass 0 to use the library default (4).
   */
  truePeakDb(oversampleFactor = 4): number {
    return meteringTruePeakDb(this.samples, this.sampleRate, oversampleFactor);
  }

  detectClipping(options: MeteringDetectClippingOptions = {}): ClippingReport {
    return meteringDetectClipping(this.samples, this.sampleRate, options);
  }

  dynamicRange(options: MeteringDynamicRangeOptions = {}): DynamicRangeReport {
    return meteringDynamicRange(this.samples, this.sampleRate, options);
  }

  spectrum(options: SpectrumOptions & ValidateOptions = {}): SpectrumReport {
    return meteringSpectrum(this.samples, this.sampleRate, options);
  }

  /**
   * True single-frame magnitude / power / dB spectrum starting at sample
   * `startSample`. See {@link meteringSpectrumFrame} for the frame-validation contract.
   */
  spectrumFrame(startSample = 0, options: SpectrumOptions & ValidateOptions = {}): SpectrumReport {
    return meteringSpectrumFrame(this.samples, this.sampleRate, startSample, options);
  }

  ebur128LoudnessRange(): number {
    return ebur128LoudnessRange(this.samples, this.sampleRate);
  }

  /** Same as {@link destroy}; lets `using` release the samples. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}
