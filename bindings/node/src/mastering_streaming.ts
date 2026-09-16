import { flattenChainConfig, STREAMING_ONLY_CONFIG_KEYS } from './_chain_config.js';
import { addon } from './native.js';
import type {
  EqBandInput,
  EqSpectrumSnapshot,
  EqStereoPlacement,
  StreamAnalyzerConfig,
  StreamAnalyzerStats,
  StreamFramesI16,
  StreamFramesSoa,
  StreamFramesU8,
  StreamQuantizeConfig,
} from './types.js';

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
 * (stereo only), `maximizer.truePeakLimiter`. Constructing with ANY of the six
 * whole-signal repair stages enabled (`repair.declick`, `repair.declip`,
 * `repair.decrackle`, `repair.dehum`, `repair.dereverb`, `repair.denoise`)
 * throws an Error. A `loudness`-enabled config also throws unless
 * {@link StreamingMasteringChainConfig.loudnessStaticGainDb} is supplied.
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
    // The addon flattens the nested config itself and keeps only the number and
    // boolean leaves; anything else is skipped with no error, so a value that
    // arrived as a string from a JSON preset or a UI field used to build a
    // chain at the stage default and say nothing, while the same object threw
    // a TypeError on the offline path. Run the canonical flattener first purely
    // for its rejection: it names the offending dotted path, so both paths fail
    // the same way on the same input. The result is discarded — the addon still
    // does the flattening that actually reaches the core.
    flattenChainConfig(config, STREAMING_ONLY_CONFIG_KEYS);
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

  /**
   * Emit delayed audio and finite processor tails after the final mono block.
   * Call until an empty array is returned. The first samples are delayed by
   * {@link latencySamples}; discard that many after concatenation for aligned output.
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

  /** Releases native resources; lets `using` (Node 22+) free them automatically. */
  [Symbol.dispose](): void {
    this.destroy();
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
 *
 * The native handle supports one serialized producer (`process`,
 * `processWithOffset`, or `finalize`) concurrently with one serialized consumer
 * (`availableFrames`, a `readFrames*` method, `stats`, `frameCount`, or
 * `currentTime`). Completed values use an allocation-free release/acquire
 * handoff. `reset`, setters, and `destroy` require both roles to be stopped.
 * JavaScript runtimes must still arrange ownership so the same native wrapper
 * is not invoked by multiple threads within either role. A full pending ring
 * drops the newly produced output frame while analysis totals keep advancing.
 */
export class StreamAnalyzer {
  private native: InstanceType<typeof addon.StreamAnalyzer>;
  private disposed = false;

  constructor(config: StreamAnalyzerConfig = {}) {
    if (
      config.outputFormat !== undefined &&
      (typeof config.outputFormat !== 'number' ||
        !Number.isFinite(config.outputFormat) ||
        !Number.isInteger(config.outputFormat) ||
        config.outputFormat !== 0)
    ) {
      throw new TypeError('outputFormat must be the integer 0 (Float32)');
    }
    this.native = new addon.StreamAnalyzer(config);
  }

  /** Feed a mono block of samples. */
  process(samples: Float32Array): void {
    this.native.process(samples);
  }

  /**
   * Feed a mono block anchored at a contiguous absolute sample offset. Gaps,
   * seeks, and switching from `process()` require `reset()` first.
   */
  processWithOffset(samples: Float32Array, sampleOffset: number): void {
    this.native.processWithOffset(samples, sampleOffset);
  }

  /** Drain any high-rate resampler tail, then zero-pad the final partial frame. */
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
   * consistency: the WASM `StreamAnalyzer` class publishes this SOA read as
   * `readFrames` (the embind-level `readFramesSoa` is not re-exported), and
   * Python names it `read_frames`.
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

  /**
   * Hint the expected total duration (seconds) to tune progressive estimates.
   *
   * `seconds` must be finite and non-negative; 0 means "unknown".
   */
  setExpectedDuration(seconds: number): void {
    this.native.setExpectedDuration(seconds);
  }

  /**
   * Set a normalization gain applied to incoming samples.
   *
   * `gain` is a linear factor within 0.01..100, assuming input in the
   * conventional ±1 float domain. A value outside that range throws
   * `InvalidParameter` instead of being clamped, and a non-finite one (or one
   * past the 32-bit float range) a `RangeError` naming the argument; either way
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
   * Set the tuning reference frequency (Hz) for key/chroma analysis.
   *
   * `hz` must be finite and within 220..880, the same range
   * {@link StreamAnalyzerConfig}'s `tuningRefHz` accepts at construction. A
   * value outside it throws rather than being clamped, so the chromagram cannot
   * depend on which entry point supplied the reference.
   */
  setTuningRefHz(hz: number): void {
    this.native.setTuningRefHz(hz);
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

  /** Releases native resources; lets `using` (Node 22+) free them automatically. */
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
 * eq.setBand(0, { type: 'HighShelf', frequencyHz: 8000, gainDb: 6, enabled: true });
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
      throw new Error(`unknown EQ band placement: ${placement}`);
    }
    return this.native.magnitudeResponse(value, frequenciesHz);
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

  /** Releases native resources; lets `using` (Node 22+) free them automatically. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}
