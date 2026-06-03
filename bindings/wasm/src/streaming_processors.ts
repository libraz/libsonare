import { getSonareModule } from './module_state';
import type {
  EqBand,
  EqMatchOptions,
  EqSpectrumSnapshot,
  StreamingEqualizerConfig,
  StreamingMasteringChainConfig,
  StreamingRetuneConfig,
} from './public_types';

// ============================================================================
// StreamingMasteringChain Class
// ============================================================================

/**
 * Block-by-block streaming variant of {@link masteringChain}.
 *
 * Maintains processor state across {@link processMono}/{@link processStereo}
 * calls. Only ProcessorBase-backed stages are supported. Configurations that
 * enable `repair.denoise` throw at construction. An enabled `loudness` stage
 * also throws unless {@link StreamingMasteringChainConfig.loudnessStaticGainDb}
 * supplies a precomputed normalization gain.
 *
 * Call {@link delete} (or use a `try/finally`) to release the underlying WASM
 * object — the embind handle is not garbage-collected automatically.
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
    this.chain = module.createStreamingMasteringChain(config as Record<string, unknown>);
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

  /** Release the underlying WASM object. Safe to call only once. */
  delete(): void {
    this.chain.delete();
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
   * Set the global phase mode: 1=ZeroLatency, 2=NaturalPhase, 3=LinearPhase.
   */
  setPhaseMode(mode: number): void {
    this.eq.setPhaseMode(mode);
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
   * Update retune settings. Changing `grainSize` takes effect after the next
   * {@link prepare} call.
   */
  setConfig(config: StreamingRetuneConfig): void {
    this.retune.setConfig(config as Record<string, unknown>);
  }

  /** Current native config. */
  config(): Required<StreamingRetuneConfig> {
    return this.retune.config();
  }

  /** Resolved grain size in samples after {@link prepare}. */
  grainSize(): number {
    return this.retune.grainSize();
  }

  /** Process one mono block, returning the shifted samples (same length). */
  processMono(samples: Float32Array): Float32Array {
    return this.retune.processMono(samples);
  }

  /** Release the underlying WASM object. Safe to call only once. */
  delete(): void {
    this.retune.delete();
  }
}
