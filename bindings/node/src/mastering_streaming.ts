import { flattenChainConfig, STREAMING_ONLY_CONFIG_KEYS } from './internal/chain_config.js';
import { addon } from './native.js';
import type {
  EqBandInput,
  EqMatchOptions,
  EqSpectrumSnapshot,
  EqStereoPlacement,
  Key,
  PitchClass,
  StreamBarChord,
  StreamChordChange,
  StreamConfig,
  StreamFrames,
  StreamFramesI16,
  StreamFramesU8,
  StreamProgressiveEstimate,
  StreamQuantizeConfig,
  StreamStats,
  WindowType,
} from './types.js';
import { CHORD_QUALITIES } from './types.js';
import { assertClosedToken, assertFloatValue, assertSampleRate } from './validation.js';

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
   * Precomputed static loudness gain in dB, measured at the loudness stage's
   * input ({@link streamingLoudnessGain} returns it). When `loudness.enabled` is
   * set, the streaming chain cannot measure whole-signal integrated LUFS, so it
   * applies this fixed gain per block before the loudness stage's true-peak
   * limiter. When omitted, an enabled loudness stage throws.
   */
  loudnessStaticGainDb?: number;

  /**
   * True peak (dBFS) of the loudness stage's input the static gain was computed
   * for (`truePeakDb` of {@link streamingLoudnessGain}). When provided, the static gain is clamped to
   * `(loudness.ceilingDb - loudnessStaticGainPeakDb) +
   * max(loudness.maxLimiterGainReductionDb, 0)` so the streaming preview
   * does not drive the loudness limiter harder than the offline chain. When
   * omitted, the static gain is applied verbatim.
   */
  loudnessStaticGainPeakDb?: number;
}

/**
 * Block-by-block streaming variant of {@link masteringChain}.
 *
 * Maintains processor state across {@link processMono}/{@link processStereo}
 * calls. Only ProcessorBase-backed stages are supported: `eq.tilt`,
 * `dynamics.deesser`, `dynamics.transientShaper`, `dynamics.compressor`,
 * `dynamics.multibandComp`, `saturation.tape`, `saturation.exciter`,
 * `spectral.airBand`, `stereo.imager` (stereo only), `stereo.monoMaker`
 * (stereo only), `maximizer.truePeakLimiter`. Constructing with ANY of the five
 * whole-signal repair stages enabled (`repair.declick`, `repair.declip`,
 * `repair.decrackle`, `repair.dehum`, `repair.dereverb`) throws an Error. A
 * `loudness`-enabled config also throws unless
 * {@link StreamingMasteringChainConfig.loudnessStaticGainDb} is supplied.
 *
 * `repair.denoise` runs here, but only with a noise estimator that is recursive
 * in time. A stream defaults it to `'spp'` (`3`, speech-presence probability),
 * so `{ repair: { denoise: { enabled: true } } }` prepares. The offline
 * default, `'quantile'` (`0`), ranks every frame of the whole signal by energy,
 * which a stream never reaches the end of, so it is refused by name rather than
 * substituted; `'mcra'` (`1`) and `'imcra'` (`2`) are accepted too. The two
 * minimum-tracking estimators
 * (`1` and `2`) seed their noise floor from the first frame they see and hold it
 * for the half second their minimum window spans, so a stream opened in the
 * middle of the programme is over-suppressed until it turns over; `3` tracks no
 * minimum and is unaffected. Prefer `3` past that opening too: `1` and `2`
 * over-report the floor for as long as the programme stays intermittent, and on
 * a gated tone they leave the result below the untreated input.
 *
 * @example
 * ```typescript
 * const chain = new StreamingMasteringChain({ eq: { tilt: { tiltDb: 1.0 } } });
 * chain.prepare(44100, 512, 1);
 * const out = chain.processMono(blockSamples);
 * chain.reset();
 * ```
 */
export class StreamingMasteringChain {
  private native: InstanceType<typeof addon.StreamingMasteringChain>;
  private disposed = false;

  constructor(config: StreamingMasteringChainConfig = {}) {
    // The addon keeps only the number and boolean leaves of what it is given;
    // anything else is skipped with no error. The canonical flattener runs
    // first so a value that arrived as a string from a JSON preset or a UI
    // field is rejected by dotted path, as on the offline path, and an enum
    // given by name reaches the addon as its number.
    const native: Record<string, unknown> = flattenChainConfig(config, STREAMING_ONLY_CONFIG_KEYS);
    for (const key of STREAMING_ONLY_CONFIG_KEYS) {
      if (config[key] !== undefined) {
        native[key] = config[key];
      }
    }
    this.native = new addon.StreamingMasteringChain(native);
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

  /**
   * Emit delayed audio and finite processor tails after the final mono block.
   * Call until an empty array is returned. The first samples are delayed by
   * {@link latencySamples}; discard that many after concatenation for aligned output.
   * Throws a `NotSupported` error when an enabled stage has an unbounded tail; pass a
   * bounded number of silent blocks to {@link processMono} instead.
   */
  flushMono(): Float32Array {
    return this.native.flushMono();
  }

  /** Stereo counterpart of {@link flushMono}. */
  flushStereo(): { left: Float32Array; right: Float32Array } {
    return this.native.flushStereo();
  }

  /** Reset all processor state without rebuilding. */
  reset(): void {
    this.native.reset();
  }

  /**
   * Set one realtime-safe full-chain parameter at a serialized block boundary.
   * The native chain retains its DSP instances, history and latency. Structural,
   * disabled-stage and unknown parameters are rejected.
   */
  setParameter(key: string, value: number): void {
    this.native.setParameter(key, value);
  }

  /** Total reported latency in samples across all active processors. */
  latencySamples(): number {
    return this.native.latencySamples();
  }

  /** Ordered stage names that will run (e.g. ``"eq.tilt"``). */
  stageNames(): string[] {
    return this.native.stageNames();
  }

  /**
   * Samples a stage replaced with a finite in-domain one, keeping the output
   * finite and in range.
   *
   * A non-finite sample supplied by the caller is rejected before any stage
   * runs, so a replacement is always of a value a stage itself produced.
   *
   * Only the true-peak limiters replace anything, so with the maximizer's
   * limiter and the loudness stage both disabled a zero here means no stage
   * was able to replace anything rather than that nothing needed replacing.
   *
   * Cumulative over every block processed since the last {@link prepare}, and
   * summed across the chain's stages and both channels. {@link prepare}
   * rebuilds the stages and so restarts the count from zero; {@link reset}
   * clears processor state only and leaves the count standing. Reports `0`
   * before the chain has been prepared, and throws after {@link destroy}.
   *
   * @example
   * ```typescript
   * const out = chain.processMono(block);
   * if (chain.nonFiniteSubstitutionCount() > 0) {
   *   // the audio produced so far is not a function of the blocks supplied
   * }
   * ```
   */
  nonFiniteSubstitutionCount(): number {
    return this.native.nonFiniteSubstitutionCount();
  }

  /**
   * Processing calls in which a stage discarded its own recursive state
   * because a non-finite value had reached it.
   *
   * The companion to {@link nonFiniteSubstitutionCount}, and not the same
   * measurement: that one counts samples a stage replaced and so sums
   * across stages, while a discard is a whole stage returning to its
   * post-reset value and is counted once per call however many stages did
   * it. A stage may run more than once per call, which is why the number is
   * a delta over the call and never a sum.
   *
   * Non-finite input is rejected before any stage runs, so what a stage
   * discards is always state it produced itself -- a finite sample large
   * enough to overflow inside a filter, most often. Unlike the substitution
   * count every stage can contribute, so a zero here means no stage
   * discarded rather than that none could.
   *
   * Both {@link processMono}/{@link processStereo} and
   * {@link flushMono}/{@link flushStereo} count, since a flush drives the
   * same stages. Cumulative since the last {@link prepare}, which rebuilds
   * the stages and so clears it -- the same epoch
   * {@link nonFiniteSubstitutionCount} shares, so reading both on one handle
   * gives two numbers measured from the same point. Reports `0` before the
   * chain has been prepared, and throws after {@link destroy}.
   *
   * @example
   * ```typescript
   * const out = chain.processMono(block);
   * if (chain.nonFiniteDiscardCount() > 0) {
   *   // a stage's own state was reset this call; the substitution count
   *   // alone would not have shown that
   * }
   * ```
   */
  nonFiniteDiscardCount(): number {
    return this.native.nonFiniteDiscardCount();
  }

  /**
   * Release the native resources now instead of waiting for garbage collection.
   * Idempotent; any other method called afterwards throws. A long-lived process
   * that creates a StreamingMasteringChain per request must call this, or native memory
   * accumulates for as long as the wrapper stays unreachable-but-uncollected.
   */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.native.destroy();
  }

  /**

   * Releases native resources; lets `using` free them automatically (needs TypeScript 5.2+

   * or a runtime with native explicit resource management; Node 22 does not parse `using`).

   */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

const STREAM_WINDOW_TOKENS: readonly WindowType[] = ['hann', 'hamming', 'blackman', 'rectangular'];
const STREAM_REFERENCE_HZ_RANGE = [220, 880] as const;
const STREAM_CONFIG_KEYS: ReadonlySet<string> = new Set([
  'sampleRate',
  'nFft',
  'hopLength',
  'nMels',
  'fminHz',
  'fmaxHz',
  'referenceHz',
  'computeMel',
  'computeChroma',
  'computeOnset',
  'computeSpectral',
  'emitEveryNFrames',
  'magnitudeDownsample',
  'maxPendingFrames',
  'maxProgressionEntries',
  'keyUpdateIntervalSec',
  'bpmUpdateIntervalSec',
  'window',
]);

// Feature bits of the native frame buffers.
const STREAM_FEATURE_MEL = 1;
const STREAM_FEATURE_CHROMA = 2;
const STREAM_FEATURE_ONSET = 4;
const STREAM_FEATURE_SPECTRAL = 8;

type StreamArray = Float32Array | Uint8Array | Int16Array;

interface NativeStreamFrames<T extends StreamArray> {
  nFrames: number;
  nMels: number;
  nChroma: number;
  featureFlags: number;
  timestamps: Float32Array;
  mel: T;
  chroma: T;
  onsetStrength: T;
  rmsEnergy: T;
  spectralCentroid: T;
  spectralFlatness: T;
  chordRoot: Int32Array;
  chordQuality: Int32Array;
  chordConfidence: Float32Array;
}

interface NativeStreamChord {
  root: number;
  quality: number;
  name: string;
  startTime: number;
  confidence: number;
}

interface NativeStreamBarChord extends NativeStreamChord {
  barIndex: number;
}

interface NativeStreamStats {
  totalFrames: number;
  totalSamples: number;
  durationSeconds: number;
  pendingFrames: number;
  droppedOutputFrames: number;
  droppedChordProgressionEntries: number;
  droppedBarProgressionEntries: number;
  nonFiniteDiscardBlocks: number;
  estimate: {
    bpm: number;
    bpmConfidence: number;
    bpmCandidateCount: number;
    key: number;
    keyMinor: boolean;
    keyConfidence: number;
    keyName: string | null;
    keyShortName: string | null;
    chordRoot: number;
    chordQuality: number;
    chordName: string;
    chordConfidence: number;
    chordStartTime: number;
    chordProgression: NativeStreamChord[];
    barChordProgression: NativeStreamBarChord[];
    currentBar: number;
    barDuration: number;
    votedPattern: NativeStreamBarChord[];
    patternLength: number;
    detectedPatternName: string;
    detectedPatternScore: number;
    allPatternScores: Array<{ name: string; score: number }>;
    accumulatedSeconds: number;
    usedFrames: number;
    updated: boolean;
  };
}

/** Native stream buffers hold frame-major data; the public layout is band-major. */
function streamBandMajor<T extends StreamArray>(flat: T, nFrames: number, nBands: number): T {
  const out = new (flat.constructor as new (length: number) => T)(flat.length);
  for (let f = 0; f < nFrames; f++) {
    for (let b = 0; b < nBands; b++) {
      out[b * nFrames + f] = flat[f * nBands + b];
    }
  }
  return out;
}

/** The per-frame arrays every read path shares, absent where the feature was not computed. */
function streamSharedArrays<T extends StreamArray>(native: NativeStreamFrames<T>) {
  const out: { onsetStrength?: T; rmsEnergy: T; spectralCentroid?: T; spectralFlatness?: T } = {
    rmsEnergy: native.rmsEnergy,
  };
  if (native.featureFlags & STREAM_FEATURE_ONSET) out.onsetStrength = native.onsetStrength;
  if (native.featureFlags & STREAM_FEATURE_SPECTRAL) {
    out.spectralCentroid = native.spectralCentroid;
    out.spectralFlatness = native.spectralFlatness;
  }
  return out;
}

function streamQuantizedFrames<T extends Uint8Array | Int16Array>(
  native: NativeStreamFrames<T>,
): StreamFramesU8 | StreamFramesI16 {
  const out: { mel?: T; chroma?: T } = {};
  if (native.featureFlags & STREAM_FEATURE_MEL) {
    out.mel = streamBandMajor(native.mel, native.nFrames, native.nMels);
  }
  if (native.featureFlags & STREAM_FEATURE_CHROMA) {
    out.chroma = streamBandMajor(native.chroma, native.nFrames, native.nChroma);
  }
  return {
    nFrames: native.nFrames,
    nMels: native.nMels,
    nChroma: native.nChroma,
    // Native names the per-frame time `timestamps`.
    timestampsSec: native.timestamps,
    ...out,
    ...streamSharedArrays(native),
  } as StreamFramesU8 | StreamFramesI16;
}

function streamChordChange(c: NativeStreamChord): StreamChordChange {
  return {
    root: c.root as PitchClass,
    quality: CHORD_QUALITIES[c.quality],
    name: c.name,
    // Native names the chord start `startTime`.
    startSec: c.startTime,
    confidence: c.confidence,
  };
}

function streamBarChord(c: NativeStreamBarChord): StreamBarChord {
  return { barIndex: c.barIndex, ...streamChordChange(c) };
}

function streamStats(s: NativeStreamStats): StreamStats {
  const e = s.estimate;
  const estimate: StreamProgressiveEstimate = {
    bpmCandidateCount: e.bpmCandidateCount,
    chordProgression: e.chordProgression.map(streamChordChange),
    barChordProgression: e.barChordProgression.map(streamBarChord),
    votedPattern: (e.votedPattern ?? []).map(streamBarChord),
    patternLength: e.patternLength,
    allPatternScores: (e.allPatternScores ?? []).map((p) => ({ name: p.name, score: p.score })),
    // Native names the accumulated duration `accumulatedSeconds`.
    accumulatedSec: e.accumulatedSeconds,
    usedFrames: e.usedFrames,
    updated: e.updated,
  };
  if (e.bpm > 0) {
    estimate.bpm = e.bpm;
    estimate.bpmConfidence = e.bpmConfidence;
  }
  if (e.keyName !== null && e.keyShortName !== null && e.key >= 0) {
    const key: Key = {
      root: e.key as PitchClass,
      mode: e.keyMinor ? 'minor' : 'major',
      confidence: e.keyConfidence,
      name: e.keyName,
      shortName: e.keyShortName,
    };
    estimate.key = key;
  }
  if (e.chordRoot >= 0) {
    estimate.chord = streamChordChange({
      root: e.chordRoot,
      quality: e.chordQuality,
      name: e.chordName,
      // Native names the chord start `chordStartTime`.
      startTime: e.chordStartTime,
      confidence: e.chordConfidence,
    });
  }
  if (e.currentBar >= 0) {
    estimate.currentBar = e.currentBar;
    // Native names the bar length `barDuration`.
    estimate.barDurationSec = e.barDuration;
  }
  if (e.detectedPatternName) {
    estimate.detectedPatternName = e.detectedPatternName;
    estimate.detectedPatternScore = e.detectedPatternScore;
  }
  return {
    totalFrames: s.totalFrames,
    totalSamples: s.totalSamples,
    // Native names the duration `durationSeconds`.
    durationSec: s.durationSeconds,
    pendingFrames: s.pendingFrames,
    droppedOutputFrames: s.droppedOutputFrames,
    droppedChordProgressionEntries: s.droppedChordProgressionEntries,
    droppedBarProgressionEntries: s.droppedBarProgressionEntries,
    nonFiniteDiscardBlocks: s.nonFiniteDiscardBlocks,
    estimate,
  };
}

/** Refuses an A4 reference outside what native accepts: `TypeError` for a non-number, `RangeError` outside 220..880 Hz. */
function assertStreamReferenceHz(
  fnName: string,
  referenceHz: unknown,
): asserts referenceHz is number {
  if (typeof referenceHz !== 'number') {
    throw new TypeError(`${fnName}: referenceHz must be a number`);
  }
  if (
    !Number.isFinite(referenceHz) ||
    referenceHz < STREAM_REFERENCE_HZ_RANGE[0] ||
    referenceHz > STREAM_REFERENCE_HZ_RANGE[1]
  ) {
    throw new RangeError(
      `${fnName}: referenceHz must be within ${STREAM_REFERENCE_HZ_RANGE[0]}..${STREAM_REFERENCE_HZ_RANGE[1]} Hz`,
    );
  }
}

/** Validates the public config and spells it the way the addon reads it. */
function nativeStreamConfig(config: StreamConfig): Record<string, unknown> {
  if (typeof config !== 'object' || config === null || Array.isArray(config)) {
    throw new TypeError('StreamAnalyzer: config must be an object with a sampleRate');
  }
  for (const key of Object.keys(config)) {
    if (!STREAM_CONFIG_KEYS.has(key)) {
      throw new TypeError(`StreamAnalyzer: unknown config key '${key}'`);
    }
  }
  assertSampleRate('StreamAnalyzer', config.sampleRate);
  const { fminHz, fmaxHz, referenceHz, window, ...rest } = config;
  const native: Record<string, unknown> = { ...rest };
  if (fminHz !== undefined) {
    assertFloatValue('StreamAnalyzer', fminHz, 'fminHz');
    native.fmin = fminHz;
  }
  if (fmaxHz !== undefined) {
    assertFloatValue('StreamAnalyzer', fmaxHz, 'fmaxHz');
    if (!(fmaxHz > 0)) {
      throw new RangeError('StreamAnalyzer: fmaxHz must be positive (omit it for Nyquist)');
    }
    native.fmax = fmaxHz;
  }
  if (referenceHz !== undefined) {
    assertStreamReferenceHz('StreamAnalyzer', referenceHz);
    // Native names the A4 reference `tuningRefHz`.
    native.tuningRefHz = referenceHz;
  }
  if (window !== undefined) {
    assertClosedToken('StreamAnalyzer', window, STREAM_WINDOW_TOKENS, 'window');
    native.window = STREAM_WINDOW_TOKENS.indexOf(window);
  }
  return native;
}

/**
 * Stateful real-time / streaming music analyzer.
 *
 * Feed mono blocks with {@link process}; drain analysis frames with
 * {@link readFrames} (or quantized variants) and query the running musical
 * estimate (BPM/key/chord/pattern) with {@link stats}.
 *
 * @example
 * ```typescript
 * const analyzer = new StreamAnalyzer({ sampleRate: 44100 });
 * analyzer.process(block);
 * const frames = analyzer.readFrames(analyzer.availableFrames);
 * const { estimate } = analyzer.stats();
 * ```
 *
 * The native handle supports one serialized producer (`process`,
 * `processWithOffset`, or `finalize`) concurrently with one serialized consumer
 * (`availableFrames`, a `readFrames*` method, `stats`, `frameCount`, or
 * `currentTimeSec`). Completed values use an allocation-free release/acquire
 * handoff. `reset`, setters, and `destroy` require both roles to be stopped.
 * JavaScript runtimes must still arrange ownership so the same native wrapper
 * is not invoked by multiple threads within either role. A full pending ring
 * drops the newly produced output frame while analysis totals keep advancing.
 */
export class StreamAnalyzer {
  private native: InstanceType<typeof addon.StreamAnalyzer>;
  private disposed = false;

  /**
   * @param config - Configuration; `sampleRate` is required (`TypeError` when
   *   absent, `RangeError` when not a supported integer rate). An unknown key
   *   is a `TypeError` and an out-of-domain value a `RangeError`.
   */
  constructor(config: StreamConfig) {
    this.native = new addon.StreamAnalyzer(nativeStreamConfig(config));
  }

  /** Feed a mono block of samples. */
  process(samples: Float32Array): void {
    this.native.process(samples);
  }

  /**
   * Feed a mono block anchored at a contiguous absolute sample offset. Gaps,
   * seeks, and switching from `process()` require `reset()` first.
   *
   * @param startSample - Cumulative sample count at the start of this block
   */
  processWithOffset(samples: Float32Array, startSample: number): void {
    this.native.processWithOffset(samples, startSample);
  }

  /** Drain any high-rate resampler tail, then zero-pad the final partial frame. */
  finalize(): void {
    this.native.finalize();
  }

  /** Number of analysis frames ready to read. */
  get availableFrames(): number {
    return this.native.availableFrames();
  }

  /**
   * Drain up to `maxFrames` frames. A feature array is absent when its feature
   * is not computed; `mel` and `chroma` are band-major matrices (rows are
   * bands, cols are frames).
   */
  readFrames(maxFrames: number): StreamFrames {
    const native = this.native.readFramesSoa(maxFrames) as NativeStreamFrames<Float32Array>;
    const out: StreamFrames = {
      nFrames: native.nFrames,
      // Native names the per-frame time `timestamps`.
      timestampsSec: native.timestamps,
      ...streamSharedArrays(native),
    };
    if (native.featureFlags & STREAM_FEATURE_MEL) {
      out.mel = {
        data: streamBandMajor(native.mel, native.nFrames, native.nMels),
        rows: native.nMels,
        cols: native.nFrames,
      };
    }
    if (native.featureFlags & STREAM_FEATURE_CHROMA) {
      out.chroma = {
        data: streamBandMajor(native.chroma, native.nFrames, native.nChroma),
        rows: native.nChroma,
        cols: native.nFrames,
      };
      out.chordRoot = native.chordRoot;
      out.chordQuality = native.chordQuality;
      out.chordConfidence = native.chordConfidence;
    }
    return out;
  }

  /**
   * Drain up to `maxFrames` frames as uint8-quantized arrays, band-major. Pass
   * `quantizeConfig` to widen the quantization ranges for a stream louder or
   * quieter than the defaults (omitted keeps the defaults).
   */
  readFramesU8(maxFrames: number, quantizeConfig?: StreamQuantizeConfig): StreamFramesU8 {
    const native = this.native.readFramesU8(
      maxFrames,
      quantizeConfig,
    ) as NativeStreamFrames<Uint8Array>;
    return streamQuantizedFrames(native) as StreamFramesU8;
  }

  /**
   * Drain up to `maxFrames` frames as int16-quantized arrays, band-major. Pass
   * `quantizeConfig` to widen the quantization ranges for a stream louder or
   * quieter than the defaults (omitted keeps the defaults).
   */
  readFramesI16(maxFrames: number, quantizeConfig?: StreamQuantizeConfig): StreamFramesI16 {
    const native = this.native.readFramesI16(
      maxFrames,
      quantizeConfig,
    ) as NativeStreamFrames<Int16Array>;
    return streamQuantizedFrames(native) as StreamFramesI16;
  }

  /**
   * Reset analyzer state.
   *
   * @param startSample - Sample the new stream starts at (default 0)
   */
  reset(startSample = 0): void {
    this.native.reset(startSample);
  }

  /** Current progressive musical estimate and totals. */
  stats(): StreamStats {
    return streamStats(this.native.stats() as NativeStreamStats);
  }

  /** Total frames processed so far. */
  get frameCount(): number {
    return this.native.frameCount();
  }

  /** Current analysis time in seconds. */
  get currentTimeSec(): number {
    // Native names the position `currentTime`.
    return this.native.currentTime();
  }

  /** Configured sample rate in Hz. */
  get sampleRate(): number {
    return this.native.sampleRate();
  }

  /**
   * Hint the expected total duration to tune progressive estimates.
   *
   * `durationSec` must be finite and non-negative; 0 means "unknown".
   */
  setExpectedDuration(durationSec: number): void {
    this.native.setExpectedDuration(durationSec);
  }

  /**
   * Set a normalization gain applied to incoming samples.
   *
   * `gain` is a linear factor within 0.01..100, assuming input in the
   * conventional ±1 float domain. A value outside that range throws
   * a `RangeError` instead of being clamped, as does a non-finite one (or one
   * past the 32-bit float range); either way
   * the previous gain is kept. The usual recipe
   * (`targetLevel / measuredLevel`) easily lands outside it for a buffer on
   * another scale: an integer-scaled one asks for about 3e-4. Since no getter
   * exposes the effective gain, a clamped request would analyse roughly 30 dB
   * off target with nothing to detect it by. Convert such a buffer to the ±1
   * domain before feeding it instead.
   */
  setNormalizationGain(gain: number): void {
    this.native.setNormalizationGain(gain);
  }

  /**
   * Set the A4 reference frequency in Hz for key/chroma analysis.
   *
   * `referenceHz` must be a finite number within 220..880, the same range
   * {@link StreamConfig}'s `referenceHz` accepts at construction. A non-number
   * is a `TypeError`; a value outside the range is a `RangeError` rather than
   * being clamped, so the chromagram cannot depend on which entry point
   * supplied the reference.
   */
  setReferenceHz(referenceHz: number): void {
    assertStreamReferenceHz('StreamAnalyzer.setReferenceHz', referenceHz);
    // Native names the setter `setTuningRefHz`.
    this.native.setTuningRefHz(referenceHz);
  }

  /**
   * Release the native resources now instead of waiting for garbage collection.
   * Idempotent; any other method called afterwards throws. A long-lived process
   * that creates a StreamAnalyzer per request must call this, or native memory
   * accumulates for as long as the wrapper stays unreachable-but-uncollected.
   */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.native.destroy();
  }

  /**

   * Releases native resources; lets `using` free them automatically (needs TypeScript 5.2+

   * or a runtime with native explicit resource management; Node 22 does not parse `using`).

   */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

const EQ_PLACEMENTS: Record<string, number> = {
  stereo: 0,
  left: 1,
  right: 2,
  mid: 3,
  side: 4,
};

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

export type StreamingEqualizerPhaseMode =
  | 'zero'
  | 'zero-latency'
  | 'zero_latency'
  | 'natural'
  | 'natural-phase'
  | 'natural_phase'
  | 'linear'
  | 'linear-phase'
  | 'linear_phase'
  | number;

/**
 * Block-by-block unified equalizer (zero-latency / natural / linear phase).
 *
 * Wraps the native `EqualizerProcessor`; state persists across
 * {@link processMono}/{@link processStereo} calls.
 *
 * @example
 * ```typescript
 * const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
 * eq.setBand(0, { type: 'highShelf', frequencyHz: 8000, gainDb: 6, enabled: true });
 * const { left, right } = eq.processStereo(blockLeft, blockRight);
 * ```
 */
export class StreamingEqualizer {
  private native: InstanceType<typeof addon.StreamingEqualizer>;
  private disposed = false;

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

  /** Set the global phase mode, accepting the documented phase-name aliases or 1/2/3. */
  setPhaseMode(mode: StreamingEqualizerPhaseMode): void {
    const value = typeof mode === 'number' ? mode : EQ_PHASE_MODES[mode.toLowerCase()];
    if (value === undefined) {
      throw new RangeError(`unknown EQ phase mode: ${mode}`);
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

  /**
   * Set a mono external key for dynamic bands with `externalSidechain` enabled.
   *
   * The samples are copied into an owned buffer, so the source ArrayBuffer may
   * be mutated, transferred or detached after this call without affecting the
   * key. Every sample must be finite; an empty array clears the key.
   */
  setSidechainMono(samples: Float32Array): void {
    this.native.setSidechainMono(samples);
  }

  /**
   * Set a stereo external key for dynamic bands with `externalSidechain` enabled.
   *
   * Both channels are copied, with the same detach safety and finiteness
   * requirement as {@link setSidechainMono}.
   */
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

  /**
   * Number of blocks in which the equalizer discarded recursive state
   * because a non-finite value had reached it.
   *
   * Advisory telemetry, and the only thing that separates a degraded EQ
   * from a clean one. A discard returns the affected filter cells to their
   * post-reset value, so the EQ recovers in silence and the output stays
   * finite and in range while carrying samples unrelated to the input;
   * nothing else reports that this happened.
   *
   * The count covers every IIR plane the band layout uses -- stereo, per
   * channel, and mid/side -- together with the automatic output gain and
   * the detector state the dynamic bands drive. Linear-phase bands are not
   * included and have nothing to include: an FIR keeps no recursive state,
   * so a non-finite sample leaves its history on its own.
   *
   * Monotonic for the lifetime of the instance. The unit is one processed
   * block, never a channel or a plane, so a stereo block that discards on
   * both channels adds one and the number does not depend on a dimension
   * you did not choose.
   *
   * @example
   * ```ts
   * eq.processStereo(left, right);
   * if (eq.nonFiniteDiscardCount() > 0) {
   *   // the audio just produced is not a function of `left`/`right`
   * }
   * ```
   */
  nonFiniteDiscardCount(): number {
    return this.native.nonFiniteDiscardCount();
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

  /**
   * The composite magnitude of the bands, in dB, at each requested frequency —
   * the curve to draw over {@link spectrum}.
   *
   * Built from the same coefficient design, tilt expansion and cut-slope
   * cascade the audio path uses, so it states what the equalizer does rather
   * than what its settings look like, and it carries the output gain, the gain
   * scale and whatever each dynamic band is applying at the moment of the call.
   * Disabled, bypassed and — when anything is soloed — unsoloed bands drop out,
   * and a soloed band is drawn as the band pass it is heard as.
   *
   * `placement` selects which signal path the curve is for. A band placed on
   * `'Stereo'` is on every path; one placed elsewhere appears only on its own,
   * a mid band having no per-channel magnitude to fold into a left or right
   * curve. Frequencies are clamped to [0 Hz, Nyquist].
   *
   * @example
   * ```ts
   * const db = eq.magnitudeResponse(new Float32Array([100, 1000, 10000]));
   * ```
   */
  magnitudeResponse(
    frequenciesHz: Float32Array,
    placement: EqStereoPlacement = 'Stereo',
  ): Float32Array {
    const value = EQ_PLACEMENTS[placement.toLowerCase()];
    if (value === undefined) {
      throw new RangeError(`unknown EQ band placement: ${placement}`);
    }
    return this.native.magnitudeResponse(value, frequenciesHz);
  }

  /** Latest realtime-safe spectrum snapshot. */
  spectrum(): EqSpectrumSnapshot {
    return this.native.spectrum();
  }

  /**
   * Configure bands to match a reference spectrum (offline analysis). The two
   * buffers may differ in length and rate; see {@link EqMatchOptions}.
   */
  match(source: Float32Array, reference: Float32Array, options: EqMatchOptions = {}): void {
    this.native.match(source, reference, options);
  }
  /**
   * Release the native resources now instead of waiting for garbage collection.
   * Idempotent; any other method called afterwards throws. A long-lived process
   * that creates a StreamingEqualizer per request must call this, or native memory
   * accumulates for as long as the wrapper stays unreachable-but-uncollected.
   */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.native.destroy();
  }

  /**

   * Releases native resources; lets `using` free them automatically (needs TypeScript 5.2+

   * or a runtime with native explicit resource management; Node 22 does not parse `using`).

   */
  [Symbol.dispose](): void {
    this.destroy();
  }
}
