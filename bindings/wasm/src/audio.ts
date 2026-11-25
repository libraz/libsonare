import type { VoiceChangeOptions } from './effects_mastering.js';
import {
  harmonic,
  hpss,
  normalize,
  noteMove,
  noteStretch,
  percussive,
  pitchCorrectToMidi,
  pitchShift,
  timeStretch,
  voiceChange,
} from './effects_mastering.js';
import type { HarmonicRequest, HpssRequest, PercussiveRequest } from './effects_separation.js';
import type {
  PitchCorrectToMidiRequest,
  PitchShiftRequest,
  TimeStretchRequest,
} from './effects_timepitch.js';
import { ErrorCode } from './errors.js';
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
  StftRequest,
  TrimRequest,
} from './feature_spectrogram.js';
import {
  chroma,
  ebur128LoudnessRangeLu,
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
  trim,
  zeroCrossingRate,
} from './features.js';
import { CHANNEL_LAYOUT_VALUES, CHANNEL_LAYOUTS } from './internal/channel_layout.js';
import { codedError, entryClass, refuseType, withEntry } from './internal/entry.js';
import { errorText } from './internal/failure_payload.js';
import { releasedHandleError } from './internal/handle_guard.js';
import type { MasterAudioRequest, NormalizeRequest } from './mastering_chain.js';
import { masterAudio } from './mastering_chain.js';
import type {
  MasteringApplyProcessorRequest,
  MasteringNormalizeLoudnessRequest,
} from './mastering_core.js';
import { masteringApplyProcessor, masteringNormalizeLoudness } from './mastering_core.js';
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
  meteringTruePeakDbtp,
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
  MasteringChainResult,
  MasteringNormalizeLoudnessResult,
  MasteringResult,
  Matrix2D,
  MelSpectrogramResult,
  MfccResult,
  NoteMoveOptions,
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
  DetectBeatsRequest,
  DetectBpmRequest,
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
import type { OptionsOf } from './validation.js';
import { assertClosedToken, validateAudioBuffer } from './validation.js';

// ============================================================================
// Audio Class
// ============================================================================

type BrowserDecodeContext = Pick<BaseAudioContext, 'decodeAudioData' | 'sampleRate'>;

/** Options for decoding with the browser codec fallback. */
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
  return downmix(channels, 'mono')[0];
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
 * Speaker bed layout, mirroring the C enum `SonareChannelLayout`: `'mono'`,
 * `'stereo'`, `'5.1'` (L R C LFE Ls Rs) and `'7.1'` (L R C LFE Ls Rs Lss Rss).
 */
export type ChannelLayout = 'mono' | 'stereo' | '5.1' | '7.1';

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
 * @param data - Encoded audio bytes such as WAV or MP3.
 * @throws SonareError with `codeName: 'DecodeFailed'` for an empty decode or a
 *   non-finite sample and `'InvalidFormat'` for a declared rate outside the
 *   supported range.
 * @throws RangeError for empty input.
 *
 * @example
 * ```ts
 * declare const bytes: Uint8Array; // an encoded audio file
 * const { sampleRate, channels } = decodeChannels(bytes);
 * const outputChannels =
 *   channels.length === 6 || channels.length === 8 ? downmix(channels, 'stereo') : channels;
 * ```
 */
export function decodeChannels(data: Uint8Array): DecodedChannels {
  return withEntry('decodeChannels', () => {
    return getSonareModule().decodeChannels(data);
  });
}

/**
 * Downmix channels to a narrower layout with the ITU-R BS.775 rule.
 *
 * Center and surround enter the front pair at -3 dB and the LFE plane is
 * dropped; a stereo, 5.1 or 7.1 source folds to mono through the same matrix
 * the decoders use. The source layout is the one the channel count names
 * (1, 2, 6 or 8). Supported targets narrow the bed (7.1 to 5.1, 5.1 or 7.1 to
 * stereo, anything modelled to mono) or copy it. A channel count outside
 * 1/2/6/8 folds to `'mono'` as the unweighted mean of its planes, as the
 * decoders do, and has no other target.
 *
 * The fold is not normalised: the coefficients sum, so a source with several
 * full-scale planes can leave the output past full scale (six planes of 1.0
 * fold to 2.414 on a stereo target). Pass `normalize: true` to scale the output
 * so the worst case stays at or below full scale, or leave headroom before
 * calling.
 *
 * @param channels - One `Float32Array` per channel, all of the same length.
 * @param targetLayout - `'mono'`, `'stereo'`, `'5.1'` or `'7.1'`.
 * @param options - `normalize` scales the fold so the worst case (every input
 *   at full scale, in phase) cannot exceed unity; `includeLfe` folds the LFE
 *   plane in at -3 dB instead of dropping it. Both default to `false`.
 * @throws TypeError for a `targetLayout` that is not a string.
 * @throws SonareError coded `NotSupported` for an upmix.
 * @throws RangeError for an unknown layout name.
 * @throws RangeError for no channels, or channels of differing length.
 * @throws TypeError for an `options` that is not an object or a non-boolean option.
 * @returns One `Float32Array` per channel of the target layout.
 */
export function downmix(
  channels: Float32Array[],
  targetLayout: ChannelLayout,
  options: DownmixOptions = {},
): Float32Array[] {
  return withEntry('downmix', () => {
    assertClosedToken(targetLayout, CHANNEL_LAYOUTS, 'targetLayout');
    return getSonareModule().downmix(channels, CHANNEL_LAYOUT_VALUES[targetLayout], options);
  });
}

/**
 * {@link decodeChannels} with the same browser codec fallback as
 * {@link Audio.fromMemoryWithBrowserFallback}: formats the native decoder does
 * not carry (AAC, OGG, FLAC) go through `decodeAudioData`, and every channel the
 * browser returns is kept.
 */
export async function decodeChannelsWithBrowserFallback(
  data: Uint8Array,
  options: BrowserAudioDecodeOptions = {},
): Promise<DecodedChannels> {
  return withEntry('decodeChannelsWithBrowserFallback', async () => {
    try {
      return decodeChannels(data);
    } catch (nativeError) {
      return decodeWithBrowserCodec(data, options, nativeError, 'decodeChannels');
    }
  });
}

async function decodeWithBrowserCodec(
  bytes: Uint8Array,
  options: BrowserAudioDecodeOptions,
  nativeError: unknown,
  nativeDecoder: string,
): Promise<DecodedChannels> {
  const contextFactory = options.createAudioContext ?? getBrowserAudioContextFactory();
  const context =
    options.audioContext ??
    contextFactory?.(
      options.targetSampleRate ? { sampleRate: options.targetSampleRate } : undefined,
    );

  if (!context) {
    throw codedError(
      ErrorCode.NotSupported,
      `${nativeDecoder} failed and browser decodeAudioData is unavailable: ${errorText(
        nativeError,
      )}`,
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
    throw codedError(
      ErrorCode.DecodeFailed,
      `${nativeDecoder} failed and browser decodeAudioData fallback failed: ${errorText(
        fallbackError,
      )}`,
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
    throw refuseType(`Audio.${methodName}: options must be an object`);
  }
  for (const key of ['samples', 'sampleRate']) {
    if (key in bag) {
      throw refuseType(`Audio.${methodName}: options must not carry '${key}'`);
    }
  }
  return { ...bag, samples, sampleRate } as R;
}

/** Module-private key: only the static factories below can construct an {@link Audio}. */
const AUDIO_FACTORY_KEY = Symbol('Audio factory');

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
 * ```ts
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
@entryClass('Audio', ['requireAlive', 'samples'])
export class Audio {
  private _samples: Float32Array | null;
  private readonly _sampleRate: number;

  private constructor(key: symbol, samples: Float32Array, sampleRate: number) {
    if (key !== AUDIO_FACTORY_KEY) {
      throw refuseType(
        'construct it with Audio.fromBuffer, Audio.fromMemory or Audio.fromMemoryWithBrowserFallback',
      );
    }
    this._samples = samples;
    this._sampleRate = sampleRate;
  }

  /** The held samples; refuses once {@link destroy} released them. */
  private requireAlive(): Float32Array {
    if (this._samples === null) {
      throw releasedHandleError('Audio');
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
   * @param sampleRate - Sample rate of the samples in Hz. Required.
   */
  static fromBuffer(samples: Float32Array, sampleRate: number): Audio {
    validateAudioBuffer(samples, sampleRate);
    return new Audio(AUDIO_FACTORY_KEY, samples.slice(), sampleRate);
  }

  /**
   * Create an Audio instance by decoding audio bytes in memory.
   *
   * @param data - Encoded audio bytes such as WAV or MP3.
   */
  static fromMemory(data: Uint8Array): Audio {
    const decoded = getSonareModule().audioFromMemory(data);
    return new Audio(AUDIO_FACTORY_KEY, decoded.samples, decoded.sampleRate);
  }

  /**
   * Decode audio bytes with the native WASM decoder first, then fall back to the
   * browser codec stack (`AudioContext.decodeAudioData`) for formats such as
   * AAC, OGG, and FLAC when available. Browser-decoded multi-channel audio is
   * folded to mono with {@link downmix}, using the native decoder's channel
   * folding coefficients. The browser may resample to the context rate, so the
   * output samples can differ.
   */
  static async fromMemoryWithBrowserFallback(
    data: Uint8Array,
    options: BrowserAudioDecodeOptions = {},
  ): Promise<Audio> {
    try {
      return Audio.fromMemory(data);
    } catch (nativeError) {
      const decoded = await decodeWithBrowserCodec(data, options, nativeError, 'Audio.fromMemory');
      const length = decoded.channels[0]?.length ?? 0;
      return new Audio(AUDIO_FACTORY_KEY, foldToMono(decoded.channels, length), decoded.sampleRate);
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

  /** Tempo of the held audio in BPM; `undefined` when no tempo is found (silence). */
  detectBpm(options?: OptionsOf<DetectBpmRequest>): number | undefined {
    return detectBpm(audioRequest('detectBpm', this.samples, this.sampleRate, options));
  }

  /** Most likely musical key of the held audio. */
  detectKey(options: KeyDetectionOptions = {}): KeyDetection {
    return detectKey(audioRequest('detectKey', this.samples, this.sampleRate, options));
  }

  /** Ranked key candidates for the held audio. */
  detectKeyCandidates(options: KeyDetectionOptions = {}): KeyCandidate[] {
    return detectKeyCandidates(
      audioRequest('detectKeyCandidates', this.samples, this.sampleRate, options),
    );
  }

  /** Onset times in seconds. */
  detectOnsets(options?: OptionsOf<DetectOnsetsRequest>): Float32Array {
    return detectOnsets(audioRequest('detectOnsets', this.samples, this.sampleRate, options));
  }

  /** Beat times in seconds. */
  detectBeats(options?: OptionsOf<DetectBeatsRequest>): Float32Array {
    return detectBeats(audioRequest('detectBeats', this.samples, this.sampleRate, options));
  }

  /** Downbeat times in seconds. */
  detectDownbeats(): Float32Array {
    return detectDownbeats(this.samples, this.sampleRate);
  }

  /** Chord sequence of the held audio. */
  detectChords(options: ChordDetectionOptions = {}): ChordAnalysisResult {
    return detectChords(audioRequest('detectChords', this.samples, this.sampleRate, options));
  }

  /** Chords with their roman numeral and function in the given key. `keyRoot` is required. */
  chordFunctionalAnalysis(options: OptionsOf<ChordFunctionalAnalysisRequest>): FunctionalChord[] {
    return chordFunctionalAnalysis(
      audioRequest('chordFunctionalAnalysis', this.samples, this.sampleRate, options),
    );
  }

  /**
   * Full music analysis of the held buffer. Takes the module {@link analyze}
   * request without the audio; `onProgress` and `cancel` report and stop it.
   */
  analyze(options?: OptionsOf<AnalyzeRequest>): AnalysisResult {
    return analyze(audioRequest('analyze', this.samples, this.sampleRate, options));
  }

  /** Tempo analysis: chosen BPM, confidence and ranked candidates. */
  analyzeBpm(options?: OptionsOf<AnalyzeBpmRequest>): BpmAnalysisResult {
    return analyzeBpm(audioRequest('analyzeBpm', this.samples, this.sampleRate, options));
  }

  /** Rhythm analysis: tempo, time signature, groove type and syncopation. */
  analyzeRhythm(options?: OptionsOf<AnalyzeRhythmRequest>): RhythmResult {
    return analyzeRhythm(audioRequest('analyzeRhythm', this.samples, this.sampleRate, options));
  }

  /** Dynamics analysis: dynamic range, peak, RMS, crest factor and loudness curve. */
  analyzeDynamics(options?: OptionsOf<AnalyzeDynamicsRequest>): DynamicsResult {
    return analyzeDynamics(audioRequest('analyzeDynamics', this.samples, this.sampleRate, options));
  }

  /** Timbre analysis: brightness, warmth, density, roughness, complexity and per-frame spectral curves. */
  analyzeTimbre(options?: OptionsOf<AnalyzeTimbreRequest>): TimbreResult {
    return analyzeTimbre(audioRequest('analyzeTimbre', this.samples, this.sampleRate, options));
  }

  /** Room acoustic parameters (RT60, EDT, clarity, definition) of the held audio treated as an impulse response. */
  analyzeImpulseResponse(options?: OptionsOf<AnalyzeImpulseResponseRequest>): AcousticResult {
    return analyzeImpulseResponse(
      audioRequest('analyzeImpulseResponse', this.samples, this.sampleRate, options),
    );
  }

  // -- Effects --

  /** Harmonic/percussive separation; returns both parts. */
  hpss(options?: OptionsOf<HpssRequest>): HpssResult {
    return hpss(audioRequest('hpss', this.samples, this.sampleRate, options));
  }

  /** Harmonic component of the held audio. */
  harmonic(options?: OptionsOf<HarmonicRequest>): Float32Array {
    return harmonic(audioRequest('harmonic', this.samples, this.sampleRate, options));
  }

  /** Percussive component of the held audio. */
  percussive(options?: OptionsOf<PercussiveRequest>): Float32Array {
    return percussive(audioRequest('percussive', this.samples, this.sampleRate, options));
  }

  /** Time-stretched copy of the held audio. */
  timeStretch(options: OptionsOf<TimeStretchRequest>): Float32Array {
    return timeStretch(audioRequest('timeStretch', this.samples, this.sampleRate, options));
  }

  /** Pitch-shifted copy of the held audio. */
  pitchShift(options: OptionsOf<PitchShiftRequest>): Float32Array {
    return pitchShift(audioRequest('pitchShift', this.samples, this.sampleRate, options));
  }

  /** Copy of the held audio with its pitch corrected to the nearest MIDI notes. */
  pitchCorrectToMidi(options: OptionsOf<PitchCorrectToMidiRequest>): Float32Array {
    return pitchCorrectToMidi(
      audioRequest('pitchCorrectToMidi', this.samples, this.sampleRate, options),
    );
  }

  /** Copy of the held audio with the given notes time-stretched. */
  noteStretch(options: NoteStretchOptions): Float32Array {
    return noteStretch(audioRequest('noteStretch', this.samples, this.sampleRate, options));
  }

  /** Copy of the held audio with the given notes moved in time. */
  noteMove(options: NoteMoveOptions): Float32Array {
    return noteMove(audioRequest('noteMove', this.samples, this.sampleRate, options));
  }

  /** Voice-changed copy of the held audio. */
  voiceChange(options?: VoiceChangeOptions): Float32Array {
    return voiceChange(audioRequest('voiceChange', this.samples, this.sampleRate, options));
  }

  /** Normalized copy of the held audio. */
  normalize(options?: OptionsOf<NormalizeRequest>): Float32Array {
    return normalize(audioRequest('normalize', this.samples, this.sampleRate, options));
  }

  /** Normalizes the held audio to a target loudness and reports the applied gain. */
  masteringNormalizeLoudness(
    options?: OptionsOf<MasteringNormalizeLoudnessRequest>,
  ): MasteringNormalizeLoudnessResult {
    return masteringNormalizeLoudness(
      audioRequest('masteringNormalizeLoudness', this.samples, this.sampleRate, options),
    );
  }

  /** Runs the mastering chain on the held audio; give a `preset`, `overrides` or both. */
  masterAudio(options: OptionsOf<MasterAudioRequest>): MasteringChainResult {
    return masterAudio(audioRequest('masterAudio', this.samples, this.sampleRate, options));
  }

  /** Applies one mastering processor to the held audio. */
  masteringApplyProcessor<P extends SoloProcessor>(
    options: OptionsOf<MasteringApplyProcessorRequest<P>>,
  ): MasteringResult {
    return masteringApplyProcessor(
      audioRequest<MasteringApplyProcessorRequest<P>>(
        'masteringApplyProcessor',
        this.samples,
        this.sampleRate,
        options,
      ),
    );
  }

  /** Copy of the held audio with leading and trailing silence removed. */
  trim(options?: OptionsOf<TrimRequest>): Float32Array {
    return trim(audioRequest('trim', this.samples, this.sampleRate, options));
  }

  // -- Features --

  /** Short-time Fourier transform of the held audio; `db` and `phase` request optional planes. */
  stft(options?: OptionsOf<StftRequest>): StftResult {
    return stft(audioRequest('stft', this.samples, this.sampleRate, options));
  }

  /** Mel spectrogram of the held audio. */
  melSpectrogram(options?: OptionsOf<MelSpectrogramRequest>): MelSpectrogramResult {
    return melSpectrogram(audioRequest('melSpectrogram', this.samples, this.sampleRate, options));
  }

  /** MFCCs of the held audio. */
  mfcc(options?: OptionsOf<MfccRequest>): MfccResult {
    return mfcc(audioRequest('mfcc', this.samples, this.sampleRate, options));
  }

  /** STFT chromagram of the held audio. */
  chroma(options?: OptionsOf<ChromaRequest>): ChromaResult {
    return chroma(audioRequest('chroma', this.samples, this.sampleRate, options));
  }

  /** Rows are the 12 chroma bins, cols are frames. */
  nnlsChroma(options?: OptionsOf<NnlsChromaRequest>): Matrix2D {
    return nnlsChroma(audioRequest('nnlsChroma', this.samples, this.sampleRate, options));
  }

  /** Onset strength envelope, one value per frame. */
  onsetStrength(options?: OptionsOf<OnsetStrengthRequest>): Float32Array {
    return onsetStrength(audioRequest('onsetStrength', this.samples, this.sampleRate, options));
  }

  /** Integrated loudness (LUFS) and loudness range of the held audio. */
  lufs(): LufsResult {
    return lufs(this.samples, this.sampleRate);
  }

  /** Momentary (400 ms) loudness series in LUFS. */
  momentaryLufs(): Float32Array {
    return momentaryLufs(this.samples, this.sampleRate);
  }

  /** Short-term (3 s) loudness series in LUFS. */
  shortTermLufs(): Float32Array {
    return shortTermLufs(this.samples, this.sampleRate);
  }

  /** Spectral centroid per frame, in Hz. */
  spectralCentroid(options?: OptionsOf<SpectralCentroidRequest>): Float32Array {
    return spectralCentroid(
      audioRequest('spectralCentroid', this.samples, this.sampleRate, options),
    );
  }

  /** Spectral bandwidth per frame, in Hz. */
  spectralBandwidth(options?: OptionsOf<SpectralBandwidthRequest>): Float32Array {
    return spectralBandwidth(
      audioRequest('spectralBandwidth', this.samples, this.sampleRate, options),
    );
  }

  /** Spectral rolloff frequency per frame, in Hz. */
  spectralRolloff(options?: OptionsOf<SpectralRolloffRequest>): Float32Array {
    return spectralRolloff(audioRequest('spectralRolloff', this.samples, this.sampleRate, options));
  }

  /** Spectral flatness per frame. */
  spectralFlatness(options?: OptionsOf<SpectralFlatnessRequest>): Float32Array {
    return spectralFlatness(
      audioRequest('spectralFlatness', this.samples, this.sampleRate, options),
    );
  }

  /** Zero-crossing rate per frame. */
  zeroCrossingRate(options?: OptionsOf<ZeroCrossingRateRequest>): Float32Array {
    return zeroCrossingRate(
      audioRequest('zeroCrossingRate', this.samples, this.sampleRate, options),
    );
  }

  /** RMS energy per frame. */
  rmsEnergy(options?: OptionsOf<RmsEnergyRequest>): Float32Array {
    return rmsEnergy(audioRequest('rmsEnergy', this.samples, this.sampleRate, options));
  }

  /** YIN fundamental-frequency track of the held audio. */
  pitchYin(options?: OptionsOf<PitchYinRequest>): PitchResult {
    return pitchYin(audioRequest('pitchYin', this.samples, this.sampleRate, options));
  }

  /** pYIN fundamental-frequency track of the held audio. */
  pitchPyin(options?: OptionsOf<PitchPyinRequest>): PitchResult {
    return pitchPyin(audioRequest('pitchPyin', this.samples, this.sampleRate, options));
  }

  /** Copy of the held audio resampled to `targetSampleRate` (Hz). */
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

  /** Sample peak in dBFS. */
  peakDb(): number {
    return meteringPeakDb(this.samples, this.sampleRate);
  }

  /** RMS level in dBFS. */
  rmsDb(): number {
    return meteringRmsDb(this.samples, this.sampleRate);
  }

  /** Mean sample value (DC offset). */
  dcOffset(): number {
    return meteringDcOffset(this.samples, this.sampleRate);
  }

  /** Crest factor (peak over RMS) in dB; `undefined` for silence. */
  crestFactorDb(): number | undefined {
    return meteringCrestFactorDb(this.samples, this.sampleRate);
  }

  /** Fraction of frames whose level is below `thresholdDb` (dBFS, default `-45`), using frames of `frameLength` samples (default `1024`) every `hopLength` samples (default `256`). */
  silenceRatio(thresholdDb = -45, frameLength = 1024, hopLength = 256): number {
    return meteringSilenceRatio(
      audioRequest('silenceRatio', this.samples, this.sampleRate, {
        thresholdDb,
        frameLength,
        hopLength,
      }),
    );
  }

  /**
   * Inter-sample (true) peak in dBTP. `oversampleFactor` must be a power of two
   * in [1, 16]; omit it for 4.
   */
  truePeakDbtp(oversampleFactor?: number): number {
    return meteringTruePeakDbtp(
      audioRequest('truePeakDbtp', this.samples, this.sampleRate, { oversampleFactor }),
    );
  }

  /** Clipping report: clipped-sample count and runs at or above the threshold. */
  detectClipping(options: MeteringDetectClippingOptions = {}): ClippingReport {
    return meteringDetectClipping(
      audioRequest('detectClipping', this.samples, this.sampleRate, options),
    );
  }

  /** Dynamic-range report of the held audio. */
  dynamicRange(options: MeteringDynamicRangeOptions = {}): DynamicRangeReport {
    return meteringDynamicRange(
      audioRequest('dynamicRange', this.samples, this.sampleRate, options),
    );
  }

  /** Magnitude, power and dB spectrum of the held audio. */
  spectrum(options: SpectrumOptions = {}): SpectrumReport {
    return meteringSpectrum(audioRequest('spectrum', this.samples, this.sampleRate, options));
  }

  /**
   * True single-frame magnitude / power / dB spectrum starting at sample
   * `startSample`. See {@link meteringSpectrumFrame} for the frame-validation contract.
   */
  spectrumFrame(startSample = 0, options: SpectrumOptions = {}): SpectrumReport {
    return meteringSpectrumFrame(
      audioRequest('spectrumFrame', this.samples, this.sampleRate, { ...options, startSample }),
    );
  }

  /** EBU R128 loudness range in LU; `undefined` when shorter than 3 s or silent. */
  ebur128LoudnessRangeLu(): number | undefined {
    return ebur128LoudnessRangeLu(this.samples, this.sampleRate);
  }

  /** Same as {@link destroy}; lets `using` release the samples. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}
