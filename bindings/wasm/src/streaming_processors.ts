import { flattenChainConfig } from './_chain_config';
import { getSonareModule } from './module_state';
import type {
  EqBand,
  EqMatchOptions,
  EqSpectrumSnapshot,
  EqStereoPlacement,
  StreamingEqualizerConfig,
  StreamingMasteringChainConfig,
  StreamingRetuneConfig,
} from './public_types';

type EqPhaseMode =
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

// ============================================================================
// StreamingMasteringChain Class
// ============================================================================

/**
 * Block-by-block streaming variant of {@link masteringChain}.
 *
 * Maintains processor state across {@link processMono}/{@link processStereo}
 * calls. Only ProcessorBase-backed stages are supported: `eq.tilt`,
 * `dynamics.deesser`, `dynamics.transientShaper`, `dynamics.compressor`,
 * `dynamics.multibandComp`, `saturation.tape`, `saturation.exciter`,
 * `spectral.airBand`, `stereo.imager` (stereo only), `stereo.monoMaker`
 * (stereo only), `maximizer.truePeakLimiter`. Configurations that enable ANY of
 * the six whole-signal repair stages (`repair.declick`, `repair.declip`,
 * `repair.decrackle`, `repair.dehum`, `repair.dereverb`, `repair.denoise`)
 * throw at construction. An enabled `loudness` stage also throws unless
 * {@link StreamingMasteringChainConfig.loudnessStaticGainDb} supplies a
 * precomputed normalization gain.
 *
 * Call {@link delete} (or use a `try/finally`) to release the underlying WASM
 * object — the embind handle is not garbage-collected automatically.
 *
 * Reachable from the AudioWorklet realm through the `sonare/worklet` entry, but
 * the realtime contract is the caller's to keep:
 *
 * - {@link prepare} builds the processors and allocates. Call it once from a
 *   message handler, never from `AudioWorkletProcessor.process()`.
 * - {@link processMono}/{@link processStereo} return fresh arrays. On the render
 *   thread, reuse the returned reference for the block rather than retaining it.
 * - An enabled `loudness` stage needs `loudnessStaticGainDb` measured offline,
 *   because whole-signal integrated LUFS cannot be measured block by block. Pass
 *   `loudnessStaticGainPeakDb` too and the static gain is clamped exactly as the
 *   offline chain clamps it, so the live preview matches the render.
 * - {@link flush} output starts {@link latencySamples} samples early; discard
 *   that many leading samples when time alignment matters.
 *
 * The chain is a host-side stage, not an engine insert: it does not participate
 * in the engine's PDC or bypass, so latency compensation against other engine
 * outputs is also the caller's.
 *
 * @example
 * ```typescript
 * const chain = new StreamingMasteringChain({ eq: { tiltDb: 1.0 } });
 * try {
 *   chain.prepare(44100, 512, 1);
 *   const out = chain.processMono(blockSamples);
 * } finally {
 *   chain.delete();
 * }
 * ```
 */
export class StreamingMasteringChain {
  private chain: import('./sonare.js').WasmStreamingMasteringChain;

  constructor(config: StreamingMasteringChainConfig) {
    const module = getSonareModule();
    const { loudnessStaticGainDb, loudnessStaticGainPeakDb, ...chainConfig } = config;
    this.chain = module.createStreamingMasteringChain({
      __flatParams: flattenChainConfig(chainConfig),
      loudnessStaticGainDb,
      loudnessStaticGainPeakDb,
    });
  }

  /**
   * Initialize processors for the given sample rate and block layout.
   *
   * @param sampleRate - Sample rate in Hz
   * @param maxBlockSize - Maximum block size per process call
   * @param numChannels - 1 (mono) or 2 (stereo)
   */
  prepare(sampleRate: number, maxBlockSize: number, numChannels: number): void {
    this.chain.prepare(sampleRate, maxBlockSize, numChannels);
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
  processStereo(
    left: Float32Array,
    right: Float32Array,
  ): { left: Float32Array; right: Float32Array } {
    if (left.length !== right.length) {
      throw new Error('Stereo channel lengths must match.');
    }
    return this.chain.processStereo(left, right);
  }

  /**
   * Emit delayed audio and finite processor tails after the final mono block.
   * Call until this returns an empty array. The initial `latencySamples()`
   * samples of the concatenated stream are delayed and should be discarded for
   * time-aligned output.
   */
  flushMono(): Float32Array {
    return this.chain.flushMono();
  }

  /** Stereo counterpart of {@link flushMono}. */
  flushStereo(): { left: Float32Array; right: Float32Array } {
    return this.chain.flushStereo();
  }

  /** Reset all processor state without rebuilding. */
  reset(): void {
    this.chain.reset();
  }

  /** Total reported latency in samples across all active processors. */
  latencySamples(): number {
    return this.chain.latencySamples();
  }

  /** Ordered stage names that will run (e.g. `"eq.tilt"`). */
  stageNames(): string[] {
    return this.chain.stageNames();
  }

  /**
   * Non-finite input samples a stage replaced with a finite in-domain one.
   *
   * Advisory telemetry, and the only thing that separates a degraded stream
   * from a clean one. A stage that meets a non-finite sample substitutes an
   * in-domain value for it — silence or full scale at the inter-sample-peak
   * limiter, an infinity folded onto the ceiling at a sample-domain one — so
   * the block comes back finite, in range and free of any error while carrying
   * values that are not a function of the input. This count is what says so.
   *
   * Cumulative over every block since {@link prepare}, and aggregated over the
   * stages and channels, so it identifies neither which block nor which stage.
   * Read it per block and compare against the previous reading to localize one.
   *
   * {@link prepare} rebuilds the stages and so clears it; {@link reset} does
   * not, because it drops processor state without rebuilding.
   *
   * @example
   * ```typescript
   * chain.processMono(block);
   * if (chain.nonFiniteSubstitutionCount() > previous) {
   *   // the block just produced is not derived from `block` everywhere
   * }
   * ```
   */
  nonFiniteSubstitutionCount(): number {
    return this.chain.nonFiniteSubstitutionCount();
  }

  /** Release the underlying WASM object. Safe to call only once. */
  delete(): void {
    this.chain.delete();
  }

  /** Alias for {@link delete}, provided for cross-binding (Node) compatibility. */
  destroy(): void {
    this.delete();
  }
}

// ============================================================================
// StreamingEqualizer Class
// ============================================================================

/**
 * Block-by-block streaming equalizer wrapping the unified C++
 * `EqualizerProcessor` (up to 24 bands, RBJ/Vicanek biquads, dynamic EQ,
 * linear-phase FIR, mid/side processing, and auto-gain).
 *
 * State is maintained across {@link processMono}/{@link processStereo} calls.
 * Call {@link delete} (or use `try/finally`) to release the underlying WASM
 * object — the embind handle is not garbage-collected automatically.
 *
 * @example
 * ```typescript
 * const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
 * try {
 *   eq.setBand(0, { type: 'HighShelf', frequencyHz: 8000, gainDb: 6, enabled: true });
 *   const out = eq.processStereo(left, right);
 *   const snapshot = eq.spectrum();
 * } finally {
 *   eq.delete();
 * }
 * ```
 */
export class StreamingEqualizer {
  private eq: import('./sonare.js').WasmStreamingEqualizer;

  constructor(config: StreamingEqualizerConfig = {}) {
    const module = getSonareModule();
    this.eq = module.createEqualizer(config as Record<string, unknown>);
  }

  /**
   * Configure the band at `index` (0..23). Omitted fields use C++ defaults.
   */
  setBand(index: number, band: EqBand): void {
    this.eq.setBand(index, band as Record<string, unknown>);
  }

  /** Disable and reset every band. */
  clear(): void {
    this.eq.clear();
  }

  /**
   * Set the global phase mode: `'zero'` | `'natural'` | `'linear'` or 1/2/3.
   */
  setPhaseMode(mode: EqPhaseMode): void {
    const value = typeof mode === 'number' ? mode : EQ_PHASE_MODES[mode.toLowerCase()];
    if (value === undefined) {
      throw new Error(`unknown EQ phase mode: ${mode}`);
    }
    this.eq.setPhaseMode(value);
  }

  /** Enable or disable output auto-gain compensation. */
  setAutoGain(enabled: boolean): void {
    this.eq.setAutoGain(enabled);
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
    if (left.length !== right.length) {
      throw new Error('Sidechain channel lengths must match.');
    }
    this.eq.setSidechainStereo(left, right);
  }

  /** Release any borrowed external sidechain buffers. */
  clearSidechain(): void {
    this.eq.clearSidechain();
  }

  /** Auto-gain applied on the most recent block, in dB. */
  lastAutoGainDb(): number {
    return this.eq.lastAutoGainDb();
  }

  /** Reported processing latency in samples (non-zero for linear-phase bands). */
  latencySamples(): number {
    return this.eq.latencySamples();
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
  processStereo(
    left: Float32Array,
    right: Float32Array,
  ): { left: Float32Array; right: Float32Array } {
    if (left.length !== right.length) {
      throw new Error('Stereo channel lengths must match.');
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
   * `'Stereo'` is on every path; one placed elsewhere appears only on its own,
   * a mid band having no per-channel magnitude to fold into a left or right
   * curve. Frequencies are clamped to [0 Hz, Nyquist].
   *
   * @example
   * ```ts
   * const freqs = new Float32Array([100, 1000, 10000]);
   * const db = eq.magnitudeResponse(freqs);
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
    return this.eq.magnitudeResponse(value, frequenciesHz);
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
   * @param options - `sampleRate` (default 48000) and `maxBands` (default 8)
   */
  match(source: Float32Array, reference: Float32Array, options: EqMatchOptions = {}): void {
    this.eq.match(source, reference, options as Record<string, unknown>);
  }

  /** Release the underlying WASM object. Safe to call only once. */
  delete(): void {
    this.eq.delete();
  }

  /** Alias for {@link delete}, provided for cross-binding (Node) compatibility. */
  destroy(): void {
    this.delete();
  }
}

// ============================================================================
// StreamingRetune Class
// ============================================================================

/**
 * Block-by-block mono voice retune / pitch shifter.
 *
 * State is maintained across {@link processMono} calls. Call {@link prepare}
 * before processing, and call {@link delete} (or use `try/finally`) to release
 * the underlying WASM object.
 */
export class StreamingRetune {
  private retune: import('./sonare.js').WasmStreamingRetune;

  constructor(config: StreamingRetuneConfig = {}) {
    const module = getSonareModule();
    this.retune = module.createStreamingRetune(config as Record<string, unknown>);
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
   * `grainSize` takes effect after the next {@link prepare} call, and an
   * omitted `grainSize` keeps whatever was last requested — including the `0`
   * sentinel, so a re-{@link prepare} at another sample rate re-derives it.
   */
  setConfig(config: StreamingRetuneConfig): void {
    this.retune.setConfig(config as Record<string, unknown>);
  }

  /** The currently applied controls, with `grainSize` as the effective one. */
  config(): Required<StreamingRetuneConfig> {
    return this.retune.config();
  }

  /** Resolved grain size in samples after {@link prepare}. */
  grainSize(): number {
    return this.retune.grainSize();
  }

  /** Fixed overlap-add latency in samples (one grain); 0 before prepare. */
  latencySamples(): number {
    return this.retune.latencySamples();
  }

  /** Process one mono block, returning the shifted samples (same length). */
  processMono(samples: Float32Array): Float32Array {
    return this.retune.processMono(samples);
  }

  /** Release the underlying WASM object. Safe to call only once. */
  delete(): void {
    this.retune.delete();
  }

  /** Alias for {@link delete}, provided for cross-binding (Node) compatibility. */
  destroy(): void {
    this.delete();
  }
}
