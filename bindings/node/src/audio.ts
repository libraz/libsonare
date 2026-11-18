import type {
  AnalyzeBpmRequest,
  AnalyzeDynamicsRequest,
  AnalyzeImpulseResponseRequest,
  AnalyzeRequest,
  AnalyzeRhythmRequest,
  AnalyzeTimbreRequest,
  ChordFunctionalAnalysisRequest,
  DetectBeatsRequest,
  DetectBpmRequest,
  DetectKeyCandidatesRequest,
  DetectKeyRequest,
  DetectOnsetsRequest,
} from './analysis.js';
import {
  analyzeBpm as analyzeBpmFn,
  analyzeDynamics as analyzeDynamicsFn,
  analyze as analyzeFn,
  analyzeImpulseResponse as analyzeImpulseResponseFn,
  analyzeRhythm as analyzeRhythmFn,
  analyzeTimbre as analyzeTimbreFn,
  chordFunctionalAnalysis as chordFunctionalAnalysisFn,
  detectAcoustic as detectAcousticFn,
  detectChords as detectChordsFn,
  detectKeyCandidates as detectKeyCandidatesFn,
  detectKey as detectKeyFn,
  detectOnsets as detectOnsetsFn,
} from './analysis.js';
import type { VoiceChangeOptions } from './effects_mastering.js';
import {
  harmonic as harmonicFn,
  hpss as hpssFn,
  normalize as normalizeFn,
  noteMove as noteMoveFn,
  noteStretch as noteStretchFn,
  percussive as percussiveFn,
  pitchCorrectToMidi as pitchCorrectToMidiFn,
  pitchShift as pitchShiftFn,
  timeStretch as timeStretchFn,
  voiceChange as voiceChangeFn,
} from './effects_mastering.js';
import type { HarmonicRequest, HpssRequest, PercussiveRequest } from './effects_separation.js';
import type {
  PitchCorrectToMidiRequest,
  PitchShiftRequest,
  TimeStretchRequest,
} from './effects_timepitch.js';
import { ErrorCode, SonareError } from './errors.js';
import type { TrimRequest } from './feature_framing.js';
import type {
  ChromaRequest,
  NnlsChromaRequest,
  PitchPyinRequest,
  PitchYinRequest,
} from './feature_pitch.js';
import { resample as resampleFn } from './feature_resample.js';
import type { OnsetStrengthRequest } from './feature_rhythm.js';
import type {
  MelSpectrogramRequest,
  MfccRequest,
  RmsEnergyRequest,
  SpectralBandwidthRequest,
  SpectralCentroidRequest,
  SpectralFlatnessRequest,
  SpectralRolloffRequest,
  StftDbRequest,
  StftRequest,
  ZeroCrossingRateRequest,
} from './feature_spectral.js';
import {
  chroma as chromaFn,
  lufs as lufsFn,
  melSpectrogram as melSpectrogramFn,
  mfcc as mfccFn,
  momentaryLufs as momentaryLufsFn,
  nnlsChroma as nnlsChromaFn,
  onsetStrength as onsetStrengthFn,
  pitchPyin as pitchPyinFn,
  pitchYin as pitchYinFn,
  rmsEnergy as rmsEnergyFn,
  shortTermLufs as shortTermLufsFn,
  spectralBandwidth as spectralBandwidthFn,
  spectralCentroid as spectralCentroidFn,
  spectralFlatness as spectralFlatnessFn,
  spectralRolloff as spectralRolloffFn,
  stftDb as stftDbFn,
  stft as stftFn,
  trim as trimFn,
  zeroCrossingRate as zeroCrossingRateFn,
} from './features.js';
import {
  clippingReportFromNative,
  clipThresholdLinear,
  refuseZeroSentinel,
} from './internal/metering_native.js';
import { tempoRange } from './internal/tempo_range.js';
import type { MasterAudioRequest } from './mastering_chain.js';
import { masterAudio as masterAudioFn } from './mastering_chain.js';
import type {
  MasteringApplyProcessorRequest,
  MasteringNormalizeLoudnessRequest,
} from './mastering_core.js';
import {
  masteringApplyProcessor as masteringApplyProcessorFn,
  masteringNormalizeLoudness as masteringNormalizeLoudnessFn,
} from './mastering_core.js';
import type { NormalizeRequest } from './mastering_normalize.js';
import type {
  ClippingReport,
  DynamicRangeReport,
  MeteringDetectClippingOptions,
  MeteringDynamicRangeOptions,
  SpectrumOptions,
  SpectrumReport,
} from './metering.js';
import { addon } from './native.js';
import type {
  AcousticOptions,
  AcousticResult,
  AnalysisResult,
  BpmAnalysisResult,
  ChordAnalysisResult,
  ChordDetectionOptions,
  ChromaResult,
  DynamicsResult,
  FunctionalChord,
  HpssResult,
  KeyCandidate,
  KeyDetection,
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
} from './types.js';
import type { OptionsOf, ValidateOptions } from './validation.js';
import { assertPositiveInteger, assertSampleRate } from './validation.js';

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

/**
 * One channel of decoded audio and its sample rate, held in native memory. A multi-channel
 * source is downmixed as it is loaded; {@link Audio.fromFileChannel} loads one channel instead.
 * Release it with `destroy()` or a `using` declaration.
 */
export class Audio {
  private native: InstanceType<typeof addon.Audio>;
  private disposed = false;
  private dataCache?: Float32Array;

  private constructor(native: InstanceType<typeof addon.Audio>) {
    this.native = native;
  }

  /**
   * Decode a file, downmixing a multi-channel source to mono. Reads WAV and MP3, and the
   * FFmpeg formats when `capabilities().features.ffmpeg` is true.
   *
   * @param path - Source file path.
   * @throws SonareError when the file is missing or unreadable, its format is not supported,
   *   or it fails to decode.
   */
  static fromFile(path: string): Audio {
    return new Audio(addon.Audio.fromFile(path));
  }

  /**
   * Load one channel of a file, leaving the others out of it.
   *
   * An {@link Audio} carries a single channel, so {@link fromFile} downmixes a
   * multi-channel source into it. This loads the requested channel instead,
   * which is what rendering a stereo result needs: pair it with
   * {@link fileNumChannels} and load each channel in turn.
   *
   * The file is decoded once per call, so a stereo load costs two decodes. The
   * format set and the decoded-buffer contract are {@link fromFile}'s.
   *
   * @param path - Source file path.
   * @param channelIndex - Zero-based channel to load.
   * @throws RangeError when
   *   `channelIndex` is negative or names no channel the file has, and the same
   *   load errors {@link fromFile} raises.
   *
   * @example
   * ```ts
   * const path = 'take.wav';
   * const planes = Array.from({ length: Audio.fileNumChannels(path) }, (_, i) =>
   *   Audio.fromFileChannel(path, i),
   * );
   * ```
   */
  static fromFileChannel(path: string, channelIndex: number): Audio {
    return new Audio(addon.Audio.fromFileChannel(path, channelIndex));
  }

  /**
   * Channel count of an audio file, without loading it as an {@link Audio}.
   *
   * @param path - Source file path.
   * @throws SonareError on the load errors {@link fromFile} raises.
   */
  static fileNumChannels(path: string): number {
    return addon.Audio.fileChannelCount(path);
  }

  /**
   * Wrap raw mono float samples as an {@link Audio}. `sampleRate` is the rate
   * of the caller's samples and is required.
   */
  static fromBuffer(samples: Float32Array, sampleRate: number): Audio {
    assertSampleRate('Audio.fromBuffer', sampleRate);
    return new Audio(addon.Audio.fromBuffer(samples, sampleRate));
  }

  /**
   * Decode an encoded file held in memory, as {@link fromFile} decodes one on disk.
   *
   * @param data - The file's bytes.
   * @throws SonareError when the format is not supported or the bytes fail to decode.
   */
  static fromMemory(data: Buffer | Uint8Array): Audio {
    return new Audio(addon.Audio.fromMemory(data));
  }

  private requireAlive(): void {
    if (this.disposed) {
      throw new SonareError(ErrorCode.InvalidState, 'Audio has been destroyed');
    }
  }

  /**
   * The decoded samples. The first read copies them out of the native handle
   * once; every later read returns that same array, which is shared with this
   * Audio's own methods and must not be mutated (copy it with `slice()` first).
   *
   * @throws SonareError `InvalidState` after {@link destroy}.
   */
  get data(): Float32Array {
    this.requireAlive();
    const cached = this.dataCache;
    if (cached !== undefined) {
      return cached;
    }
    const data = this.native.getData();
    this.dataCache = data;
    return data;
  }

  /**
   * Number of samples.
   *
   * @throws SonareError `InvalidState` after {@link destroy}.
   */
  get length(): number {
    this.requireAlive();
    return this.native.getLength();
  }

  /**
   * Sample rate in Hz.
   *
   * @throws SonareError `InvalidState` after {@link destroy}.
   */
  get sampleRate(): number {
    this.requireAlive();
    return this.native.getSampleRate();
  }

  /**
   * Duration in seconds.
   *
   * @throws SonareError `InvalidState` after {@link destroy}.
   */
  get durationSec(): number {
    this.requireAlive();
    return this.native.getDuration();
  }

  /** Releases the native handle; later reads throw `SonareError` `InvalidState`. A second call does nothing. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.dataCache = undefined;
    this.native.destroy();
  }

  /**
   * Same as {@link destroy}; lets `using` free the handle (needs TypeScript 5.2+
   * or a runtime with native explicit resource management).
   */
  [Symbol.dispose](): void {
    this.destroy();
  }

  // -- Analysis --

  // The methods below stay on the native handle rather than delegating to their
  // standalone facade counterparts: the handle reads its own buffer in native
  // memory, so routing through `data` would copy it out first. Nothing is given
  // up by not delegating — those facades are plain forwarders with no guard of
  // their own. The metering group below analyze() additionally skips the
  // per-call finiteness scan and defensive copy the standalone metering
  // facades pay, because audio_ passed both once at construction.

  /** Tempo of the held audio in BPM. */
  detectBpm(options?: OptionsOf<DetectBpmRequest>): number {
    this.requireAlive();
    const [bpmMin, bpmMax] = tempoRange('detectBpm', options ?? {});
    return this.native.detectBpm(bpmMin, bpmMax);
  }

  /** Most likely musical key of the held audio. */
  detectKey(options?: OptionsOf<DetectKeyRequest>): KeyDetection {
    return detectKeyFn(audioRequest('detectKey', this.data, this.sampleRate, options));
  }

  /** Ranked key candidates for the held audio. */
  detectKeyCandidates(options?: OptionsOf<DetectKeyCandidatesRequest>): KeyCandidate[] {
    return detectKeyCandidatesFn(
      audioRequest('detectKeyCandidates', this.data, this.sampleRate, options),
    );
  }

  /** Beat times in seconds. */
  detectBeats(options?: OptionsOf<DetectBeatsRequest>): Float32Array {
    this.requireAlive();
    const [bpmMin, bpmMax] = tempoRange('detectBeats', options ?? {});
    return this.native.detectBeats(bpmMin, bpmMax);
  }

  /** Downbeat times in seconds. */
  detectDownbeats(): Float32Array {
    this.requireAlive();
    return this.native.detectDownbeats();
  }

  /** Onset times in seconds. */
  detectOnsets(options?: OptionsOf<DetectOnsetsRequest>): Float32Array {
    return detectOnsetsFn(audioRequest('detectOnsets', this.data, this.sampleRate, options));
  }

  /**
   * Full music analysis of the held buffer. Takes the module {@link analyzeFn}
   * request without the audio; `onProgress` and `cancel` report and stop it.
   */
  analyze(options?: OptionsOf<AnalyzeRequest>): AnalysisResult {
    return analyzeFn(audioRequest('analyze', this.data, this.sampleRate, options));
  }

  // -- Metering --

  /** Sample peak in dBFS. */
  peakDb(): number {
    this.requireAlive();
    return this.native.peakDb();
  }

  /** RMS level in dBFS. */
  rmsDb(): number {
    this.requireAlive();
    return this.native.rmsDb();
  }

  /** Mean sample value (DC offset). */
  dcOffset(): number {
    this.requireAlive();
    return this.native.dcOffset();
  }

  /** Crest factor (peak over RMS) in dB. */
  crestFactorDb(): number {
    this.requireAlive();
    return this.native.crestFactorDb();
  }

  /** Fraction of frames whose level is below `thresholdDb` (dBFS, default `-45`), using frames of `frameLength` samples (default `1024`) every `hopLength` samples (default `256`). */
  silenceRatio(thresholdDb = -45, frameLength = 1024, hopLength = 256): number {
    this.requireAlive();
    // Positivity only, as the free meteringSilenceRatio: the same C entry, which
    // requires both positive and bounds neither.
    assertPositiveInteger('silenceRatio', frameLength, 'frameLength');
    assertPositiveInteger('silenceRatio', hopLength, 'hopLength');
    return this.native.silenceRatio(thresholdDb, frameLength, hopLength);
  }

  /**
   * Inter-sample (true) peak in dBTP. `oversampleFactor` must be a power of two
   * in [1, 16]; omit it for 4.
   */
  truePeakDbtp(oversampleFactor?: number): number {
    this.requireAlive();
    refuseZeroSentinel('Audio.truePeakDbtp', { oversampleFactor }, ['oversampleFactor']);
    return this.native.truePeakDb(oversampleFactor ?? 4);
  }

  /** Clipping report: clipped-sample count and runs at or above the threshold. */
  detectClipping(options: MeteringDetectClippingOptions = {}): ClippingReport {
    this.requireAlive();
    const { thresholdDb, ...rest } = options;
    return clippingReportFromNative(
      this.native.detectClipping({
        ...rest,
        threshold: clipThresholdLinear('detectClipping', thresholdDb),
      }),
    );
  }

  /** Dynamic-range report of the held audio. */
  dynamicRange(options: MeteringDynamicRangeOptions = {}): DynamicRangeReport {
    this.requireAlive();
    return this.native.dynamicRange(options);
  }

  /** Magnitude, power and dB spectrum of the held audio. */
  spectrum(options: SpectrumOptions = {}): SpectrumReport {
    this.requireAlive();
    return this.native.spectrum(options);
  }

  /**
   * True single-frame magnitude / power / dB spectrum starting at sample `startSample`.
   * See {@link meteringSpectrumFrame} for the frame-validation contract.
   */
  spectrumFrame(startSample = 0, options: SpectrumOptions = {}): SpectrumReport {
    this.requireAlive();
    return this.native.spectrumFrame(startSample, options);
  }

  /** EBU R128 loudness range in LU. */
  ebur128LoudnessRangeLu(): number {
    this.requireAlive();
    return this.native.ebur128LoudnessRange();
  }

  /** Tempo analysis: chosen BPM, confidence and ranked candidates. */
  analyzeBpm(options?: OptionsOf<AnalyzeBpmRequest>): BpmAnalysisResult {
    return analyzeBpmFn(audioRequest('analyzeBpm', this.data, this.sampleRate, options));
  }

  /** Room acoustic parameters (RT60, EDT, clarity, definition) of the held audio treated as an impulse response. */
  analyzeImpulseResponse(options?: OptionsOf<AnalyzeImpulseResponseRequest>): AcousticResult {
    return analyzeImpulseResponseFn(
      audioRequest('analyzeImpulseResponse', this.data, this.sampleRate, options),
    );
  }

  /** Room acoustic parameters estimated from the held recording. */
  detectAcoustic(options: AcousticOptions = {}): AcousticResult {
    return detectAcousticFn(this.data, this.sampleRate, options);
  }

  /** Rhythm analysis: tempo, time signature, groove type and syncopation. */
  analyzeRhythm(options?: OptionsOf<AnalyzeRhythmRequest>): RhythmResult {
    return analyzeRhythmFn(audioRequest('analyzeRhythm', this.data, this.sampleRate, options));
  }

  /** Dynamics analysis: dynamic range, peak, RMS, crest factor and loudness curve. */
  analyzeDynamics(options?: OptionsOf<AnalyzeDynamicsRequest>): DynamicsResult {
    return analyzeDynamicsFn(audioRequest('analyzeDynamics', this.data, this.sampleRate, options));
  }

  /** Timbre analysis: brightness, warmth, density, roughness, complexity and per-frame spectral curves. */
  analyzeTimbre(options?: OptionsOf<AnalyzeTimbreRequest>): TimbreResult {
    return analyzeTimbreFn(audioRequest('analyzeTimbre', this.data, this.sampleRate, options));
  }

  /** Chord sequence of the held audio. */
  detectChords(options: ChordDetectionOptions = {}): ChordAnalysisResult {
    return detectChordsFn(this.data, this.sampleRate, options);
  }

  /** Chords with their roman numeral and function in the given key. `keyRoot` is required. */
  chordFunctionalAnalysis(options: OptionsOf<ChordFunctionalAnalysisRequest>): FunctionalChord[] {
    return chordFunctionalAnalysisFn(
      audioRequest('chordFunctionalAnalysis', this.data, this.sampleRate, options),
    );
  }

  // -- Effects --

  /** Harmonic/percussive separation; returns both parts. */
  hpss(options?: OptionsOf<HpssRequest>): HpssResult {
    return hpssFn(audioRequest('hpss', this.data, this.sampleRate, options));
  }

  /** Harmonic component of the held audio. */
  harmonic(options?: OptionsOf<HarmonicRequest>): Float32Array {
    return harmonicFn(audioRequest('harmonic', this.data, this.sampleRate, options));
  }

  /** Percussive component of the held audio. */
  percussive(options?: OptionsOf<PercussiveRequest>): Float32Array {
    return percussiveFn(audioRequest('percussive', this.data, this.sampleRate, options));
  }

  /** Time-stretched copy of the held audio. */
  timeStretch(options: OptionsOf<TimeStretchRequest>): Float32Array {
    return timeStretchFn(audioRequest('timeStretch', this.data, this.sampleRate, options));
  }

  /** Pitch-shifted copy of the held audio. */
  pitchShift(options: OptionsOf<PitchShiftRequest>): Float32Array {
    return pitchShiftFn(audioRequest('pitchShift', this.data, this.sampleRate, options));
  }

  /** Copy of the held audio with its pitch corrected to the nearest MIDI notes. */
  pitchCorrectToMidi(options: OptionsOf<PitchCorrectToMidiRequest>): Float32Array {
    return pitchCorrectToMidiFn(
      audioRequest('pitchCorrectToMidi', this.data, this.sampleRate, options),
    );
  }

  /** Copy of the held audio with the given notes time-stretched. */
  noteStretch(options: NoteStretchOptions): Float32Array {
    return noteStretchFn(audioRequest('noteStretch', this.data, this.sampleRate, options));
  }

  /** Copy of the held audio with the given notes moved in time. */
  noteMove(options: NoteMoveOptions): Float32Array {
    return noteMoveFn(audioRequest('noteMove', this.data, this.sampleRate, options));
  }

  /** Voice-changed copy of the held audio. */
  voiceChange(options?: VoiceChangeOptions): Float32Array {
    return voiceChangeFn(audioRequest('voiceChange', this.data, this.sampleRate, options));
  }

  /** Normalized copy of the held audio. */
  normalize(options?: OptionsOf<NormalizeRequest>): Float32Array {
    return normalizeFn(audioRequest('normalize', this.data, this.sampleRate, options));
  }

  /** Normalizes the held audio to a target loudness and reports the applied gain. */
  masteringNormalizeLoudness(
    options?: OptionsOf<MasteringNormalizeLoudnessRequest>,
  ): MasteringNormalizeLoudnessResult {
    return masteringNormalizeLoudnessFn(
      audioRequest('masteringNormalizeLoudness', this.data, this.sampleRate, options),
    );
  }

  /** Runs the mastering chain on the held audio; give a `preset`, `overrides` or both. */
  masterAudio(options: OptionsOf<MasterAudioRequest>): MasteringChainResult {
    return masterAudioFn(audioRequest('masterAudio', this.data, this.sampleRate, options));
  }

  /** Applies one mastering processor to the held audio. */
  masteringApplyProcessor<P extends SoloProcessor>(
    options: OptionsOf<MasteringApplyProcessorRequest<P>>,
  ): MasteringResult {
    return masteringApplyProcessorFn(
      audioRequest<MasteringApplyProcessorRequest<P>>(
        'masteringApplyProcessor',
        this.data,
        this.sampleRate,
        options,
      ),
    );
  }

  /** Copy of the held audio with leading and trailing silence removed. */
  trim(options?: OptionsOf<TrimRequest>): Float32Array {
    return trimFn(audioRequest('trim', this.data, this.sampleRate, options));
  }

  // -- Features --

  /** Short-time Fourier transform of the held audio. */
  stft(options?: OptionsOf<StftRequest>): StftResult {
    return stftFn(audioRequest('stft', this.data, this.sampleRate, options));
  }

  /** Rows are frequency bins, cols are frames. */
  stftDb(options?: OptionsOf<StftDbRequest>): Matrix2D {
    return stftDbFn(audioRequest('stftDb', this.data, this.sampleRate, options));
  }

  /** Mel spectrogram of the held audio. */
  melSpectrogram(options?: OptionsOf<MelSpectrogramRequest>): MelSpectrogramResult {
    return melSpectrogramFn(audioRequest('melSpectrogram', this.data, this.sampleRate, options));
  }

  /** MFCCs of the held audio. */
  mfcc(options?: OptionsOf<MfccRequest>): MfccResult {
    return mfccFn(audioRequest('mfcc', this.data, this.sampleRate, options));
  }

  /** STFT chromagram of the held audio. */
  chroma(options?: OptionsOf<ChromaRequest>): ChromaResult {
    return chromaFn(audioRequest('chroma', this.data, this.sampleRate, options));
  }

  /** Spectral centroid per frame, in Hz. */
  spectralCentroid(options?: OptionsOf<SpectralCentroidRequest>): Float32Array {
    return spectralCentroidFn(
      audioRequest('spectralCentroid', this.data, this.sampleRate, options),
    );
  }

  /** Spectral bandwidth per frame, in Hz. */
  spectralBandwidth(options?: OptionsOf<SpectralBandwidthRequest>): Float32Array {
    return spectralBandwidthFn(
      audioRequest('spectralBandwidth', this.data, this.sampleRate, options),
    );
  }

  /** Spectral rolloff frequency per frame, in Hz. */
  spectralRolloff(options?: OptionsOf<SpectralRolloffRequest>): Float32Array {
    return spectralRolloffFn(audioRequest('spectralRolloff', this.data, this.sampleRate, options));
  }

  /** Spectral flatness per frame. */
  spectralFlatness(options?: OptionsOf<SpectralFlatnessRequest>): Float32Array {
    return spectralFlatnessFn(
      audioRequest('spectralFlatness', this.data, this.sampleRate, options),
    );
  }

  /** Zero-crossing rate per frame. */
  zeroCrossingRate(options?: OptionsOf<ZeroCrossingRateRequest>): Float32Array {
    return zeroCrossingRateFn(
      audioRequest('zeroCrossingRate', this.data, this.sampleRate, options),
    );
  }

  /** RMS energy per frame. */
  rmsEnergy(options?: OptionsOf<RmsEnergyRequest>): Float32Array {
    return rmsEnergyFn(audioRequest('rmsEnergy', this.data, this.sampleRate, options));
  }

  /** YIN fundamental-frequency track of the held audio. */
  pitchYin(options?: OptionsOf<PitchYinRequest>): PitchResult {
    return pitchYinFn(audioRequest('pitchYin', this.data, this.sampleRate, options));
  }

  /** pYIN fundamental-frequency track of the held audio. */
  pitchPyin(options?: OptionsOf<PitchPyinRequest>): PitchResult {
    return pitchPyinFn(audioRequest('pitchPyin', this.data, this.sampleRate, options));
  }

  /** Copy of the held audio resampled to `targetSampleRate` (Hz). */
  resample(targetSampleRate: number): Float32Array {
    return resampleFn(this.data, this.sampleRate, targetSampleRate);
  }

  /** Onset strength envelope, one value per frame. */
  onsetStrength(options?: OptionsOf<OnsetStrengthRequest>): Float32Array {
    return onsetStrengthFn(audioRequest('onsetStrength', this.data, this.sampleRate, options));
  }

  /** Rows are the 12 chroma bins, cols are frames. */
  nnlsChroma(options?: OptionsOf<NnlsChromaRequest>): Matrix2D {
    return nnlsChromaFn(audioRequest('nnlsChroma', this.data, this.sampleRate, options));
  }

  /** Integrated loudness (LUFS) and loudness range of the held audio. */
  lufs(options: ValidateOptions = {}): LufsResult {
    return lufsFn(this.data, this.sampleRate, options);
  }

  /** Momentary (400 ms) loudness series in LUFS. */
  momentaryLufs(options: ValidateOptions = {}): Float32Array {
    return momentaryLufsFn(this.data, this.sampleRate, options);
  }

  /** Short-term (3 s) loudness series in LUFS. */
  shortTermLufs(options: ValidateOptions = {}): Float32Array {
    return shortTermLufsFn(this.data, this.sampleRate, options);
  }
}
