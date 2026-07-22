import { getSonareModule } from './module_state';
import type {
  RealtimeVoiceChangerConfigInput,
  RealtimeVoiceChangerPodConfig,
  VoicePresetId,
} from './public_types';

/**
 * True when `config` is a flat POD (the shape `realtimeVoiceChangerPresetConfig`
 * returns) rather than a preset name or a nested preset. `retuneSemitones` only
 * exists on the flat POD; the nested form spells it `retune.semitones`.
 */
function isFlatVoiceChangerPod(config: unknown): config is RealtimeVoiceChangerPodConfig {
  return typeof config === 'object' && config !== null && 'retuneSemitones' in config;
}

/**
 * Rewrite the flat 36-field POD into the nested shape the native config parser
 * reads. Passing the flat POD straight to `setConfig` would leave every nested
 * field at its default (only the three root fields are read flat), silently
 * discarding retune/EQ/reverb/etc.
 */
function flatVoiceChangerPodToNested(pod: RealtimeVoiceChangerPodConfig): Record<string, unknown> {
  return {
    inputGainDb: pod.inputGainDb,
    outputGainDb: pod.outputGainDb,
    wetMix: pod.wetMix,
    retune: { semitones: pod.retuneSemitones, mix: pod.retuneMix, grainSize: pod.retuneGrainSize },
    formant: {
      factor: pod.formantFactor,
      amount: pod.formantAmount,
      body: pod.formantBody,
      brightness: pod.formantBrightness,
      nasal: pod.formantNasal,
    },
    eq: {
      highpassHz: pod.eqHighpassHz,
      bodyDb: pod.eqBodyDb,
      presenceDb: pod.eqPresenceDb,
      airDb: pod.eqAirDb,
    },
    gate: {
      thresholdDb: pod.gateThresholdDb,
      attackMs: pod.gateAttackMs,
      releaseMs: pod.gateReleaseMs,
      rangeDb: pod.gateRangeDb,
    },
    compressor: {
      thresholdDb: pod.compressorThresholdDb,
      ratio: pod.compressorRatio,
      attackMs: pod.compressorAttackMs,
      releaseMs: pod.compressorReleaseMs,
      makeupGainDb: pod.compressorMakeupGainDb,
    },
    deesser: {
      frequencyHz: pod.deesserFrequencyHz,
      thresholdDb: pod.deesserThresholdDb,
      ratio: pod.deesserRatio,
      rangeDb: pod.deesserRangeDb,
    },
    reverb: {
      mix: pod.reverbMix,
      timeMs: pod.reverbTimeMs,
      damping: pod.reverbDamping,
      seed: pod.reverbSeed,
    },
    limiter: {
      ceilingDb: pod.limiterCeilingDb,
      releaseMs: pod.limiterReleaseMs,
      enableIspLimiter: pod.limiterEnableIspLimiter,
      ispCeilingDbtp: pod.limiterIspCeilingDbtp,
    },
  };
}

/**
 * Zero-copy realtime buffer pair for {@link RealtimeVoiceChanger} mono
 * processing. The `input` / `output` `Float32Array`s are typed-memory views
 * onto the WASM heap — write samples into `input`, call `process()`, then
 * read from `output`. The views are owned by the {@link RealtimeVoiceChanger}
 * and remain valid until `delete()` is called on it.
 */
export interface RealtimeVoiceChangerMonoBuffer {
  input: Float32Array;
  output: Float32Array;
  process: () => void;
}

/**
 * Zero-copy realtime buffer pair for {@link RealtimeVoiceChanger} interleaved
 * multi-channel processing. Layout is L0,R0,L1,R1,... for stereo. The views
 * are owned by the {@link RealtimeVoiceChanger}.
 */
export interface RealtimeVoiceChangerInterleavedBuffer {
  input: Float32Array;
  output: Float32Array;
  channels: number;
  process: () => void;
}

/**
 * Zero-copy realtime buffer for {@link RealtimeVoiceChanger} planar stereo
 * processing. Each entry in `channels` is a heap-backed `Float32Array` for one
 * channel (matching AudioWorklet's native layout). Process happens in place:
 * write samples into each channel view, call `process()`, then read back from
 * the same views.
 */
export interface RealtimeVoiceChangerPlanarBuffer {
  channels: Float32Array[];
  process: () => void;
}

// ============================================================================
// RealtimeVoiceChanger Class
// ============================================================================

export class RealtimeVoiceChanger {
  private changer: import('./sonare.js').WasmRealtimeVoiceChanger;

  constructor(config: RealtimeVoiceChangerConfigInput = 'neutral-monitor') {
    const module = getSonareModule();
    this.changer = module.createRealtimeVoiceChanger(config as Record<string, unknown> | string);
  }

  prepare(sampleRate: number, maxBlockSize = 128, channels = 1): void {
    this.changer.prepare(sampleRate, maxBlockSize, channels);
  }

  reset(): void {
    this.changer.reset();
  }

  setConfig(config: RealtimeVoiceChangerConfigInput | RealtimeVoiceChangerPodConfig): void {
    // A flat POD (from realtimeVoiceChangerPresetConfig) is rewritten to the
    // nested shape the native parser reads; a preset name or nested preset is
    // passed through unchanged.
    const resolved = isFlatVoiceChangerPod(config) ? flatVoiceChangerPodToNested(config) : config;
    this.changer.setConfig(resolved as Record<string, unknown> | string);
  }

  configJson(): string {
    return this.changer.configJson();
  }

  latencySamples(): number {
    return this.changer.latencySamples();
  }

  processMono(samples: Float32Array): Float32Array {
    return this.changer.processMono(samples);
  }

  processMonoInto(samples: Float32Array, output: Float32Array): void {
    this.changer.processMonoInto(samples, output);
  }

  processInterleaved(samples: Float32Array, channels: number): Float32Array {
    return this.changer.processInterleaved(samples, channels);
  }

  processInterleavedInto(samples: Float32Array, channels: number, output: Float32Array): void {
    this.changer.processInterleavedInto(samples, channels, output);
  }

  /**
   * Acquire a typed-memory view onto the WASM heap for mono input.
   *
   * Write your input samples into the returned `Float32Array` directly (e.g.
   * via `input.set(source)`); no copy crosses the JS↔C++ bridge until
   * {@link processPreparedMono} is called. The view is owned by this
   * RealtimeVoiceChanger and becomes invalid after {@link delete}; it may
   * also be invalidated if you later call this method with a larger
   * `numSamples` value (the underlying buffer may be reallocated).
   */
  getMonoInputBuffer(numSamples: number): Float32Array {
    return this.changer.getMonoInputBuffer(numSamples);
  }

  /** Mono output view counterpart to {@link getMonoInputBuffer}. */
  getMonoOutputBuffer(numSamples: number): Float32Array {
    return this.changer.getMonoOutputBuffer(numSamples);
  }

  /**
   * Process the previously-acquired mono input buffer in place. The output
   * appears in the buffer returned by {@link getMonoOutputBuffer}. No JS↔C++
   * sample-level crossings happen on this call — it just hands control to
   * the underlying DSP on already-on-heap data.
   */
  processPreparedMono(numSamples: number): void {
    this.changer.processPreparedMono(numSamples);
  }

  /** Interleaved input view (layout L0,R0,L1,R1,...). */
  getInterleavedInputBuffer(numFrames: number, numChannels: number): Float32Array {
    return this.changer.getInterleavedInputBuffer(numFrames, numChannels);
  }

  /** Interleaved output view counterpart. */
  getInterleavedOutputBuffer(numFrames: number, numChannels: number): Float32Array {
    return this.changer.getInterleavedOutputBuffer(numFrames, numChannels);
  }

  /**
   * Process the previously-acquired interleaved buffer in place. Output
   * appears in the buffer returned by {@link getInterleavedOutputBuffer}.
   */
  processPreparedInterleaved(numFrames: number, numChannels: number): void {
    this.changer.processPreparedInterleaved(numFrames, numChannels);
  }

  /**
   * Planar-channel input/output view (one Float32Array per channel). Matches
   * AudioWorklet's native layout; processing happens in place.
   */
  getPlanarChannelBuffer(channel: number, numFrames: number): Float32Array {
    return this.changer.getPlanarChannelBuffer(channel, numFrames);
  }

  /**
   * Process the previously-acquired planar channel buffers in place. Each
   * channel must have been obtained from {@link getPlanarChannelBuffer}
   * with the same `numFrames`. Output replaces input in the same buffers.
   */
  processPreparedPlanar(numFrames: number): void {
    this.changer.processPreparedPlanar(numFrames);
  }

  /**
   * Convenience factory for the mono zero-copy path: returns the input/output
   * heap views plus a `process()` thunk wired to the same `numSamples`. The
   * views are reused across calls and become invalid after {@link delete}.
   */
  createRealtimeMonoBuffer(numSamples: number): RealtimeVoiceChangerMonoBuffer {
    let input = this.getMonoInputBuffer(numSamples);
    let output = this.getMonoOutputBuffer(numSamples);
    // The cached heap views can detach if WASM linear memory grows (the embind
    // module is built ALLOW_MEMORY_GROWTH). Re-acquire them if detached
    // (byteLength === 0) before use, mirroring the worklet RT path. In the
    // common no-growth case this is a cheap branch with no allocation.
    const reacquireIfDetached = (): void => {
      if (input.byteLength === 0 || output.byteLength === 0) {
        input = this.getMonoInputBuffer(numSamples);
        output = this.getMonoOutputBuffer(numSamples);
      }
    };
    return {
      get input(): Float32Array {
        reacquireIfDetached();
        return input;
      },
      get output(): Float32Array {
        reacquireIfDetached();
        return output;
      },
      process: () => {
        reacquireIfDetached();
        this.processPreparedMono(numSamples);
      },
    };
  }

  /** Same as {@link createRealtimeMonoBuffer} but for interleaved I/O. */
  createRealtimeInterleavedBuffer(
    numFrames: number,
    numChannels: number,
  ): RealtimeVoiceChangerInterleavedBuffer {
    let input = this.getInterleavedInputBuffer(numFrames, numChannels);
    let output = this.getInterleavedOutputBuffer(numFrames, numChannels);
    // Re-acquire detached views after WASM memory growth (see
    // createRealtimeMonoBuffer for rationale).
    const reacquireIfDetached = (): void => {
      if (input.byteLength === 0 || output.byteLength === 0) {
        input = this.getInterleavedInputBuffer(numFrames, numChannels);
        output = this.getInterleavedOutputBuffer(numFrames, numChannels);
      }
    };
    return {
      get input(): Float32Array {
        reacquireIfDetached();
        return input;
      },
      get output(): Float32Array {
        reacquireIfDetached();
        return output;
      },
      channels: numChannels,
      process: () => {
        reacquireIfDetached();
        this.processPreparedInterleaved(numFrames, numChannels);
      },
    };
  }

  /**
   * Convenience factory for the planar zero-copy path. Acquires one
   * heap-backed Float32Array per channel and returns a `process()` thunk
   * wired to the same `numFrames`. Buffers are reused across calls and
   * become invalid after {@link delete}.
   */
  createRealtimePlanarBuffer(
    numFrames: number,
    numChannels: number,
  ): RealtimeVoiceChangerPlanarBuffer {
    let channels: Float32Array[] = [];
    const acquire = (): void => {
      channels = [];
      for (let ch = 0; ch < numChannels; ch++) {
        channels.push(this.getPlanarChannelBuffer(ch, numFrames));
      }
    };
    acquire();
    // Re-acquire detached views after WASM memory growth (see
    // createRealtimeMonoBuffer for rationale).
    const reacquireIfDetached = (): void => {
      if ((channels[0]?.byteLength ?? 0) === 0) {
        acquire();
      }
    };
    return {
      get channels(): Float32Array[] {
        reacquireIfDetached();
        return channels;
      },
      process: () => {
        reacquireIfDetached();
        this.processPreparedPlanar(numFrames);
      },
    };
  }

  delete(): void {
    this.changer.delete();
  }
}

export function realtimeVoiceChangerPresetNames(): VoicePresetId[] {
  // Array.from re-roots embind's vector as a plain, structured-cloneable Array.
  return Array.from(getSonareModule().realtimeVoiceChangerPresetNames()) as VoicePresetId[];
}

export function realtimeVoiceChangerPresetJson(name: VoicePresetId): string {
  return getSonareModule().realtimeVoiceChangerPresetJson(name);
}

export function validateRealtimeVoiceChangerPresetJson(json: string): {
  ok: boolean;
  normalizedJson?: string;
  error?: string;
} {
  return getSonareModule().validateRealtimeVoiceChangerPresetJson(json);
}
