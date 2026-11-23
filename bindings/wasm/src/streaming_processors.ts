import { resolveEnumOrdinal } from './codes.js';
import { chainConfigJson } from './internal/chain_config.js';
import {
  assertDeclaredKeys,
  entryClass,
  nameNativeError,
  refuse,
  refuseType,
} from './internal/entry.js';
import { assertHandleLive } from './internal/handle_guard.js';
import { sampleNeed, withHeapNeed } from './internal/memory_budget.js';
import { isAbsent, isNonArrayObject } from './internal/value_guards.js';
import { getSonareModule } from './module_state.js';
import type {
  AutoGainOptions,
  EqBandInput,
  EqMatchOptions,
  EqSpectrumSnapshot,
  EqStereoPlacement,
  StreamingEqualizerConfig,
  StreamingMasteringChainConfig,
  StreamingRetuneAppliedConfig,
  StreamingRetuneConfig,
} from './public_types.js';
import {
  assertClosedToken,
  assertInt32,
  assertIntegerValue,
  assertKnownRequestKeys,
  assertRetuneGrain,
  requireBoolean,
} from './validation.js';

/** Global equalizer phase mode. */
export type EqPhaseMode = 'zero' | 'natural' | 'linear';

// Native reads magnitudeResponse's placement as an ordinal.
const EQ_PLACEMENT_ORDINALS: Record<EqStereoPlacement, number> = {
  stereo: 0,
  left: 1,
  right: 2,
  mid: 3,
  side: 4,
};

/** Absent or null takes `'stereo'`; a non-string is a `TypeError`, an unknown token a `RangeError`. */
function eqPlacementOrdinal(value: unknown): number {
  if (isAbsent(value)) {
    return EQ_PLACEMENT_ORDINALS.stereo;
  }
  return resolveEnumOrdinal(value, EQ_PLACEMENT_ORDINALS, 'placement');
}

/** One block of planar stereo audio: the left and right channels, equal in length. */
export interface StereoBlock {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples, the same length as `left`. */
  right: Float32Array;
}

// Native still reads the phase mode as an ordinal.
const EQ_PHASE_MODE_ORDINALS: Record<EqPhaseMode, number> = {
  zero: 1,
  natural: 2,
  linear: 3,
};
const EQ_PHASE_MODES = Object.keys(EQ_PHASE_MODE_ORDINALS) as EqPhaseMode[];

function assertConfigObject(config: unknown): asserts config is object {
  if (!isNonArrayObject(config)) {
    throw refuseType(`config must be an object`);
  }
}

/** A given block size as a positive integer; absent (`undefined`) leaves the library default. */
function optionalIntegerBlockSize(value: unknown): number | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  assertInt32(value, 'maxBlockSize');
  return value;
}

// ============================================================================
// StreamingMasteringChain Class
// ============================================================================

/**
 * Block-by-block streaming variant of {@link masterAudio}.
 *
 * Maintains processor state across {@link processMono}/{@link processStereo}
 * calls. Only ProcessorBase-backed stages are supported: `eq.tilt`,
 * `dynamics.deesser`, `dynamics.transientShaper`, `dynamics.compressor`,
 * `dynamics.multibandComp`, `saturation.tape`, `saturation.exciter`,
 * `spectral.airBand`, `stereo.imager` (stereo only), `stereo.monoMaker`
 * (stereo only), `maximizer.truePeakLimiter`. Configurations that enable ANY of
 * the five whole-signal repair stages (`repair.declick`, `repair.declip`,
 * `repair.decrackle`, `repair.dehum`, `repair.dereverb`) throw at construction.
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
 * An enabled `loudness` stage applies
 * {@link StreamingMasteringChainConfig.loudnessStaticGainDb} per block, or no
 * gain when it is omitted.
 *
 * Call {@link destroy} (or use a `try/finally`) to release the underlying WASM
 * object — the embind handle is not garbage-collected automatically.
 *
 * Reachable from the AudioWorklet realm through the `sonare/worklet` entry, but
 * the realtime contract is the caller's to keep:
 *
 * - The constructor builds the processors and allocates. Construct once from a
 *   message handler, never from `AudioWorkletProcessor.process()`.
 * - {@link processMono}/{@link processStereo} return fresh arrays. On the render
 *   thread, reuse the returned reference for the block rather than retaining it.
 * - An enabled `loudness` stage takes `loudnessStaticGainDb` measured offline
 *   ({@link streamingLoudnessGain} returns it, or omits it for input with no
 *   measurable loudness), because whole-signal integrated LUFS cannot be
 *   measured block by block. Pass `loudnessStaticGainPeakDbtp` too
 *   and the static gain is clamped exactly as the offline chain clamps it, so
 *   the live preview matches the render.
 * - {@link flush} output starts {@link latencySamples} samples early; discard
 *   that many leading samples when time alignment matters.
 *
 * The chain is a host-side stage, not an engine insert: it does not participate
 * in the engine's PDC or bypass, so latency compensation against other engine
 * outputs is also the caller's.
 *
 * @example
 * ```ts
 * const chain = new StreamingMasteringChain({
 *   sampleRate: 44100,
 *   numChannels: 1,
 *   maxBlockSize: 512,
 *   eq: { tilt: { tiltDb: 1.0 } },
 * });
 * try {
 *   const out = chain.processMono(block);
 * } finally {
 *   chain.destroy();
 * }
 * ```
 */
@entryClass('StreamingMasteringChain', ['processMono', 'processStereo', 'chain'])
export class StreamingMasteringChain {
  private chainHandle: import('./sonare.js').WasmStreamingMasteringChain;
  private released = false;

  private get chain(): import('./sonare.js').WasmStreamingMasteringChain {
    assertHandleLive(this.released, 'StreamingMasteringChain');
    return this.chainHandle;
  }

  /**
   * Builds and prepares the chain in one step.
   *
   * `sampleRate` (Hz, integer) and `numChannels` are required; an omitted
   * `maxBlockSize` takes the library default. Stereo-only stages are skipped for mono.
   *
   * @throws TypeError for a non-object config, a wrong-typed field or a missing
   *   `sampleRate`; RangeError for an out-of-domain rate, block size or channel count.
   */
  constructor(config: StreamingMasteringChainConfig) {
    assertConfigObject(config);
    const {
      sampleRate,
      maxBlockSize,
      numChannels,
      loudnessStaticGainDb,
      loudnessStaticGainPeakDbtp,
      ...chainConfig
    } = config;
    assertIntegerValue(sampleRate, 'sampleRate');
    const blockSize = optionalIntegerBlockSize(maxBlockSize);
    const options: Record<string, number> = {};
    if (loudnessStaticGainDb !== undefined) {
      options.loudnessStaticGainDb = loudnessStaticGainDb;
    }
    if (loudnessStaticGainPeakDbtp !== undefined) {
      options.loudnessStaticGainPeakDbtp = loudnessStaticGainPeakDbtp;
    }
    const module = getSonareModule();
    this.chainHandle = module.createStreamingMasteringChain(
      chainConfigJson('config', chainConfig),
      options,
    );
    try {
      this.chainHandle.prepare(sampleRate, blockSize, numChannels);
    } catch (error) {
      this.chainHandle.delete();
      throw error;
    }
  }

  /**
   * Process one mono block, returning the processed samples (same length).
   */
  processMono(samples: Float32Array): Float32Array {
    return this.chain.processMono(samples);
  }

  /**
   * Process one stereo block, returning the processed channels.
   */
  processStereo(left: Float32Array, right: Float32Array): StereoBlock {
    try {
      if (left.length !== right.length) {
        throw refuse('left and right must have the same length');
      }
    } catch (error) {
      throw nameNativeError(error, 'StreamingMasteringChain.processStereo');
    }
    return this.chain.processStereo(left, right);
  }

  /**
   * Emit delayed audio and finite processor tails after the final mono block.
   * Call until this returns an empty array. The initial `latencySamples()`
   * samples of the concatenated stream are delayed and should be discarded for
   * time-aligned output. Throws a `NotSupported` error when an enabled stage
   * has an unbounded tail; pass a bounded number of silent blocks to
   * {@link processMono} instead.
   */
  flushMono(): Float32Array {
    return this.chain.flushMono();
  }

  /** Stereo counterpart of {@link flushMono}. */
  flushStereo(): StereoBlock {
    return this.chain.flushStereo();
  }

  /** Reset all processor state without rebuilding. */
  reset(): void {
    this.chain.reset();
  }

  /**
   * Set one realtime-safe full-chain parameter at a serialized block boundary.
   * Structural, disabled-stage and unknown parameters are rejected.
   */
  setParameter(key: string, value: number): void {
    this.chain.setParameter(key, value);
  }

  /** Total reported latency in samples across all active processors (read-only). */
  get latencySamples(): number {
    return this.chain.latencySamples();
  }

  /** Largest block a process call accepts, as the native side resolved it (the library default when `maxBlockSize` was omitted). */
  get maxBlockSize(): number {
    return this.chain.maxBlockSize();
  }

  /** Ordered stage names that will run (e.g. `"eq.tilt"`); a new array per call. */
  stageNames(): string[] {
    return this.chain.stageNames();
  }

  /**
   * Samples a stage replaced with a finite in-domain one, keeping the output
   * finite and in range.
   *
   * A non-finite sample supplied by the caller is rejected before any stage
   * runs, so a replacement is always of a value a stage itself produced.
   *
   * Only the true-peak limiters replace anything, so with the maximizer's
   * limiter and the loudness stage both disabled a zero here means no stage was
   * able to replace anything rather than that nothing needed replacing.
   *
   * Cumulative over every block since construction, and aggregated over the
   * stages and channels, so it identifies neither which block nor which stage.
   * Read it per block and compare against the previous reading to localize one.
   *
   * {@link reset} does not clear it, because it drops processor state without
   * rebuilding.
   *
   * @example
   * ```ts
   * declare const chain: StreamingMasteringChain;
   * const previous = chain.nonFiniteSubstitutionCount;
   * chain.processMono(block);
   * if (chain.nonFiniteSubstitutionCount > previous) {
   *   // the block just produced is not derived from `block` everywhere
   * }
   * ```
   */
  get nonFiniteSubstitutionCount(): number {
    return this.chain.nonFiniteSubstitutionCount();
  }

  /**
   * Processing calls in which a stage discarded its own recursive state
   * because a non-finite value had reached it.
   *
   * The companion to {@link nonFiniteSubstitutionCount}, and not the same
   * measurement -- a caller who assumes they are will read one and think
   * they have the other. That one counts SAMPLES a stage replaced and so
   * sums across stages; a discard is a whole stage returning to its
   * post-reset value and is counted once per call however many stages did
   * it. A stage may run more than once per call, which is why this is a
   * delta over the call and never a sum.
   *
   * Non-finite input is rejected before any stage runs, so what a stage
   * discards is always state it produced itself -- a finite sample large
   * enough to overflow inside a filter, most often. Unlike the substitution
   * count every stage can contribute, so a zero here means no stage
   * discarded rather than that none could.
   *
   * Both {@link processMono}/{@link processStereo} and
   * {@link flushMono}/{@link flushStereo} count, since a flush drives the
   * same stages. Both counters on one handle share an epoch (construction);
   * {@link reset} clears neither, because it drops processor state without
   * rebuilding.
   */
  get nonFiniteDiscardCount(): number {
    return this.chain.nonFiniteDiscardCount();
  }

  /** Release the underlying WASM object. Idempotent, as the Node facade is. */
  destroy(): void {
    if (this.released) {
      return;
    }
    this.released = true;
    this.chainHandle.delete();
  }

  /** Releases the handle through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

// ============================================================================
// StreamingEqualizer Class
// ============================================================================

/** Every key a streaming EQ band reads; the record type keeps it complete. */
const EQ_BAND_KEYS: Readonly<Record<keyof EqBandInput, true>> = {
  type: true,
  frequencyHz: true,
  gainDb: true,
  q: true,
  enabled: true,
  coeffMode: true,
  slopeDbOct: true,
  placement: true,
  phase: true,
  soloed: true,
  bypassed: true,
  proportionalQ: true,
  proportionalQStrength: true,
  dynamic: true,
  thresholdDb: true,
  autoThreshold: true,
  ratio: true,
  rangeDb: true,
  attackMs: true,
  releaseMs: true,
  detectorDelayMs: true,
  externalSidechain: true,
  sidechainFreqHz: true,
  sidechainQ: true,
};

/** Refuse a band that is not an object or carries a key no band reads. */
function assertEqBand(band: unknown): asserts band is EqBandInput {
  if (!isNonArrayObject(band)) {
    throw refuseType('band must be an object');
  }
  assertDeclaredKeys(band, Object.keys(EQ_BAND_KEYS), 'band');
}

/**
 * Block-by-block streaming equalizer wrapping the unified C++
 * `EqualizerProcessor` (up to 24 bands, RBJ/Vicanek biquads, dynamic EQ,
 * linear-phase FIR, mid/side processing, and auto-gain).
 *
 * State is maintained across {@link processMono}/{@link processStereo} calls.
 * Call {@link destroy} (or use `try/finally`) to release the underlying WASM
 * object — the embind handle is not garbage-collected automatically.
 *
 * @example
 * ```ts
 * const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
 * try {
 *   eq.setBand(0, { type: 'highShelf', frequencyHz: 8000, gainDb: 6, enabled: true });
 *   const out = eq.processStereo(left, right);
 *   const snapshot = eq.spectrum();
 * } finally {
 *   eq.destroy();
 * }
 * ```
 */
@entryClass('StreamingEqualizer', [
  'processMono',
  'processStereo',
  'setSidechainMono',
  'setSidechainStereo',
  'eq',
])
export class StreamingEqualizer {
  private eqHandle: import('./sonare.js').WasmStreamingEqualizer;
  private released = false;

  private get eq(): import('./sonare.js').WasmStreamingEqualizer {
    assertHandleLive(this.released, 'StreamingEqualizer');
    return this.eqHandle;
  }

  /**
   * @param config - `sampleRate` (Hz, required integer) and `maxBlockSize` (omit it for the library default).
   * @throws TypeError for a non-object config, a wrong-typed field or a missing
   *   `sampleRate`; RangeError for an out-of-domain rate or block size.
   */
  constructor(config: StreamingEqualizerConfig) {
    assertConfigObject(config);
    const { sampleRate, maxBlockSize } = config;
    assertIntegerValue(sampleRate, 'sampleRate');
    const blockSize = optionalIntegerBlockSize(maxBlockSize);
    const module = getSonareModule();
    this.eqHandle = module.createEqualizer(
      blockSize === undefined ? { sampleRate } : { sampleRate, maxBlockSize: blockSize },
    );
  }

  /**
   * Configure the band at `index` (0..23). Omitted fields use C++ defaults.
   */
  setBand(index: number, band: EqBandInput): void {
    assertEqBand(band);
    if (band.placement !== undefined) {
      eqPlacementOrdinal(band.placement);
    }
    this.eq.setBand(index, { ...band });
  }

  /** Disable and reset every band. */
  clear(): void {
    this.eq.clear();
  }

  /**
   * Set the global phase mode. `TypeError` for a non-string, `RangeError` for
   * any spelling other than the three tokens (numbers and synonyms included).
   */
  setPhaseMode(mode: EqPhaseMode): void {
    if (isAbsent(mode)) {
      throw refuseType('mode must be a string');
    }
    assertClosedToken(mode, EQ_PHASE_MODES, 'mode');
    this.eq.setPhaseMode(EQ_PHASE_MODE_ORDINALS[mode]);
  }

  /**
   * Enable or disable output auto-gain compensation.
   *
   * @throws TypeError when `options` is not an object or `enabled` is not a boolean
   */
  setAutoGain(options: AutoGainOptions): void {
    if (typeof options !== 'object' || options === null) {
      throw refuseType('options must be an object');
    }
    this.eq.setAutoGain(requireBoolean(options.enabled, 'enabled'));
  }

  /** Set all-band EQ gain scale as a 0.0..2.0 multiplier. */
  setGainScale(scale: number): void {
    this.eq.setGainScale(scale);
  }

  /** Set post-EQ output gain in dB. */
  setOutputGainDb(gainDb: number): void {
    this.eq.setOutputGainDb(gainDb);
  }

  /** Set post-EQ stereo balance in -1.0..1.0; mono input ignores pan. */
  setOutputPan(pan: number): void {
    this.eq.setOutputPan(pan);
  }

  /**
   * Provide a mono external sidechain key for dynamic bands that opt into
   * `external_sidechain`. The samples are copied into an owned buffer.
   */
  setSidechainMono(samples: Float32Array): void {
    this.eq.setSidechainMono(samples);
  }

  /**
   * Provide a stereo external sidechain key. Both channels must match length.
   */
  setSidechainStereo(left: Float32Array, right: Float32Array): void {
    try {
      if (left.length !== right.length) {
        throw refuse('left and right must have the same length');
      }
    } catch (error) {
      throw nameNativeError(error, 'StreamingEqualizer.setSidechainStereo');
    }
    this.eq.setSidechainStereo(left, right);
  }

  /** Release any borrowed external sidechain buffers. */
  clearSidechain(): void {
    this.eq.clearSidechain();
  }

  /** Auto-gain applied on the most recent block, in dB. */
  get lastAutoGainDb(): number {
    return this.eq.lastAutoGainDb();
  }

  /** Reported processing latency in samples (non-zero for linear-phase bands). */
  get latencySamples(): number {
    return this.eq.latencySamples();
  }

  /** Largest block a process call accepts, as the native side resolved it (the library default when `maxBlockSize` was omitted). */
  get maxBlockSize(): number {
    return this.eq.maxBlockSize();
  }

  /**
   * Number of blocks in which the EQ discarded recursive state because a
   * non-finite value had reached it.
   *
   * Advisory telemetry, and the only thing that separates a degraded EQ from
   * a clean one. A discard returns the affected filter cells to their
   * post-reset value, so the EQ recovers in silence and the output stays
   * finite and in range while carrying samples unrelated to the input;
   * nothing else reports that this happened.
   *
   * The count covers every IIR plane the band layout uses -- stereo, per
   * channel, and mid/side -- together with the automatic output gain and the
   * detector state the dynamic bands drive. Linear-phase bands are not
   * included and have nothing to include: an FIR keeps no recursive state,
   * so a non-finite sample leaves its history on its own.
   *
   * Unlike a mixer strip's meters, nothing here lags: this EQ has no meter of
   * its own, so a discard is always attributed to the block that carried it.
   *
   * Cumulative since this handle was created and never cleared, so two
   * readings bracket a span of audio. The unit is one processed block, never
   * a channel or a plane.
   */
  get nonFiniteDiscardCount(): number {
    return this.eq.nonFiniteDiscardCount();
  }

  /**
   * Process one mono block, returning the equalized samples (same length).
   */
  processMono(samples: Float32Array): Float32Array {
    return this.eq.processMono(samples);
  }

  /**
   * Process one stereo block, returning the equalized channels.
   */
  processStereo(left: Float32Array, right: Float32Array): StereoBlock {
    try {
      if (left.length !== right.length) {
        throw refuse('left and right must have the same length');
      }
    } catch (error) {
      throw nameNativeError(error, 'StreamingEqualizer.processStereo');
    }
    return this.eq.processStereo(left, right);
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
   * `'stereo'` is on every path; one placed elsewhere appears only on its own,
   * a mid band having no per-channel magnitude to fold into a left or right
   * curve. Frequencies are clamped to [0 Hz, Nyquist].
   *
   * @example
   * ```ts
   * declare const eq: StreamingEqualizer;
   * const freqs = new Float32Array([100, 1000, 10000]);
   * const db = eq.magnitudeResponse(freqs);
   * ```
   */
  magnitudeResponse(
    frequenciesHz: Float32Array,
    placement: EqStereoPlacement = 'stereo',
  ): Float32Array {
    const ordinal = eqPlacementOrdinal(placement);
    return this.eq.magnitudeResponse(ordinal, frequenciesHz);
  }

  /**
   * Read the latest pre/post spectrum snapshot for metering. `seq` increments
   * each time a new snapshot is published.
   */
  spectrum(): EqSpectrumSnapshot {
    return this.eq.spectrum();
  }

  /**
   * Configure bands so the source spectrum matches the reference spectrum.
   *
   * @param source - Source audio (mono samples)
   * @param reference - Reference audio (mono samples)
   * @param options - `sampleRate` (default 48000), `referenceSampleRate` (default
   *   `sampleRate`; a reference at another rate is resampled onto the source's)
   *   and `maxBands` (default 8)
   */
  match(source: Float32Array, reference: Float32Array, options: EqMatchOptions = {}): void {
    const call = (): void => this.eq.match(source, reference, options as Record<string, unknown>);
    // The native reader refuses any other input; only a typed array is sized here.
    if (!(source instanceof Float32Array) || !(reference instanceof Float32Array)) {
      call();
      return;
    }
    withHeapNeed(
      sampleNeed('source', source.length, undefined, (n) => 4 * (n + reference.length)),
      call,
    );
  }

  /** Release the underlying WASM object. Idempotent, as the Node facade is. */
  destroy(): void {
    if (this.released) {
      return;
    }
    this.released = true;
    this.eqHandle.delete();
  }

  /** Releases the handle through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

// ============================================================================
// StreamingRetune Class
// ============================================================================

// The grain is checked here because native rounds a fraction and reads a negative as automatic;
// native refuses wrong-typed controls.
function assertRetuneConfig(config: StreamingRetuneConfig): Record<string, unknown> {
  if (!isNonArrayObject(config)) {
    throw refuseType(`config must be an object`);
  }
  assertKnownRequestKeys('StreamingRetune', config);
  const { grainSizeSamples, ...rest } = config as Record<string, unknown>;
  if (isAbsent(grainSizeSamples)) {
    return rest;
  }
  assertRetuneGrain(grainSizeSamples, 'grainSizeSamples');
  return config as Record<string, unknown>;
}

/**
 * Block-by-block mono voice retune / pitch shifter.
 * For a pitch shift with no other processing use this rather than the voice changer, which
 * always applies its input high-pass (20 Hz at the lowest setting).
 *
 * State is maintained across {@link processMono} calls. Call {@link prepare}
 * before processing, and call {@link destroy} (or use `try/finally`) to release
 * the underlying WASM object.
 */
@entryClass('StreamingRetune', ['processMono', 'retune'])
export class StreamingRetune {
  private retuneHandle: import('./sonare.js').WasmStreamingRetune;
  private released = false;

  private get retune(): import('./sonare.js').WasmStreamingRetune {
    assertHandleLive(this.released, 'StreamingRetune');
    return this.retuneHandle;
  }

  constructor(config: StreamingRetuneConfig = {}) {
    const module = getSonareModule();
    this.retuneHandle = module.createStreamingRetune(assertRetuneConfig(config));
  }

  /**
   * Allocate and initialize native state for the given sample rate and maximum
   * process block size.
   */
  prepare(sampleRate: number, maxBlockSize: number): void {
    this.retune.prepare(sampleRate, maxBlockSize);
  }

  /** Reset delay, grain, and overlap-add state without changing config. */
  reset(): void {
    this.retune.reset();
  }

  /**
   * Update the live controls; omitted keys keep their current value. Changing
   * `grainSizeSamples` takes effect after the next {@link prepare} call, and an
   * omitted `grainSizeSamples` keeps the configured one, automatic included, so a
   * re-{@link prepare} at another sample rate re-derives an automatic grain.
   *
   * @throws TypeError for a wrong-typed or unknown key; RangeError for a `grainSizeSamples`
   *   of 0 or not a multiple of 4 in [4, 8192], or a control outside its domain.
   */
  setConfig(config: StreamingRetuneConfig): void {
    this.retune.setConfig(assertRetuneConfig(config));
  }

  /**
   * The configured controls, in the form {@link setConfig} accepts, so
   * `other.setConfig(retune.config())` reproduces them. An automatic grain reads back as an
   * absent `grainSizeSamples`; the grain in use is {@link grainSizeSamples}.
   */
  config(): StreamingRetuneAppliedConfig {
    const native = this.retune.config();
    const controls = { pitchSemitones: native.pitchSemitones, mix: native.mix };
    return native.grainSizeSamples === 0
      ? controls
      : { ...controls, grainSizeSamples: native.grainSizeSamples };
  }

  /** Grain size in use, in samples, after {@link prepare}; 0 before. */
  get grainSizeSamples(): number {
    return this.retune.grainSizeSamples();
  }

  /** Fixed overlap-add latency in samples (one grain); 0 before prepare. */
  get latencySamples(): number {
    return this.retune.latencySamples();
  }

  /** Process one mono block, returning the shifted samples (same length). */
  processMono(samples: Float32Array): Float32Array {
    return this.retune.processMono(samples);
  }

  /** Release the underlying WASM object. Idempotent, as the Node facade is. */
  destroy(): void {
    if (this.released) {
      return;
    }
    this.released = true;
    this.retuneHandle.delete();
  }

  /** Releases the handle through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}
