import { addon } from './native.js';
import type {
  PlaybackDiagnostics,
  PlaybackRendererConfig,
  PlaybackRendererOptions,
  RenderPlaybackRequest,
  RenderPlaybackResult,
} from './types_playback.js';
import { assertAudioInput, requestObject } from './validation.js';

type NativeHrtfSet = InstanceType<typeof addon.HrtfSet>;
type NativePlaybackRenderer = InstanceType<typeof addon.PlaybackRenderer>;
type NativePlaybackLoudnessMeter = InstanceType<typeof addon.PlaybackLoudnessMeter>;

function configJsonText(config: PlaybackRendererConfig | string): string {
  return typeof config === 'string' ? config : JSON.stringify(config);
}

/**
 * Reads {@link HrtfSet}'s private `native`. Mirrors
 * `normalizeSynthInstrument` in value_coercion.ts: TypeScript's `private` is a
 * compile-time rule, so one reviewed read here beats widening the class with a
 * handle accessor no caller wants.
 */
function nativeHrtf(hrtf: HrtfSet | undefined): NativeHrtfSet | undefined {
  if (hrtf === undefined) {
    return undefined;
  }
  return (hrtf as unknown as { native: NativeHrtfSet }).native;
}

/**
 * An HRTF set (SHRF v1): the direct-sound impulse responses and inter-aural
 * time delays a headphones-target {@link PlaybackRenderer} convolves each
 * virtual speaker's signal with.
 *
 * Built through one of the two static factories, never through `new`
 * directly. A renderer built from one keeps its own copy, so destroying this
 * handle right after construction is safe.
 */
export class HrtfSet {
  private native: NativeHrtfSet;
  private disposed = false;

  private constructor(native: NativeHrtfSet) {
    this.native = native;
  }

  /** The built-in SADIE II-derived HRTF set. Native only; WASM has no built-in set to embed. */
  static default(): HrtfSet {
    return new HrtfSet(addon.HrtfSet.default());
  }

  /** Builds an HRTF set from SHRF v1 bytes; malformed data throws. */
  static fromBytes(bytes: Uint8Array): HrtfSet {
    return new HrtfSet(addon.HrtfSet.fromBytes(bytes));
  }

  /** Releases the native handle now instead of waiting for GC. Idempotent. */
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

/**
 * Renders decoded movie audio (mono / stereo / 5.1 / 7.1 PCM) to headphones
 * (binaural, head tracking, room model) or to stereo / 5.1 / 7.1 speakers
 * (upmix, loudness alignment, night-mode DRC, dialogue level, speaker
 * calibration, bass management).
 *
 * `config` follows `schemas/playback-renderer-config.schema.json`: every key
 * is either fixed at construction ("prepare") or adopted at the next
 * processed block ("realtime") — see {@link PlaybackRendererConfig}. Passing
 * the document as an already-serialized string is also accepted, matching
 * {@link setConfig}.
 */
export class PlaybackRenderer {
  private native: NativePlaybackRenderer;
  private disposed = false;

  constructor(options: PlaybackRendererOptions) {
    this.native = new addon.PlaybackRenderer(
      configJsonText(options.config),
      nativeHrtf(options.hrtf),
      options.sampleRate ?? 48000,
      options.maxBlockSize ?? 1024,
    );
  }

  /** Renders one planar block; every plane must carry the same frame count. */
  processPlanar(planes: Float32Array[]): Float32Array[] {
    return this.native.processPlanar(planes);
  }

  /** Interleaved variant of {@link processPlanar}. */
  processInterleaved(samples: Float32Array, inChannels: number): Float32Array {
    return this.native.processInterleaved(samples, inChannels);
  }

  /** Applies a complete configuration document; a changed prepare key throws. */
  setConfig(config: PlaybackRendererConfig | string): void {
    this.native.setConfig(configJsonText(config));
  }

  /** The current complete configuration document. */
  config(): PlaybackRendererConfig {
    return this.native.config() as PlaybackRendererConfig;
  }

  /**
   * Publishes the listener head orientation in degrees: right-handed,
   * positive yaw turns the head right, positive pitch looks up, positive roll
   * lowers the right ear. Ignored by a speakers target.
   */
  setHeadOrientation(yawDeg: number, pitchDeg = 0, rollDeg = 0): void {
    this.native.setHeadOrientation(yawDeg, pitchDeg, rollDeg);
  }

  /**
   * Clears DSP state (filters, FIFOs, dynamics, convolution history, pending
   * input-layout drains). Configuration and head pose are kept.
   */
  reset(): void {
    this.native.reset();
  }

  /** Renderer latency in samples (headphones: near ear). Fixed at construction. */
  latencySamples(): number {
    return this.native.latencySamples();
  }

  /**
   * Channel count of the active input layout. With `input.layout: "auto"`
   * this follows the channel count of the most recent non-empty
   * {@link processPlanar} / {@link processInterleaved} call (2 before the
   * first call).
   */
  inputChannels(): number {
    return this.native.inputChannels();
  }

  /** Channel count of the output target. */
  outputChannels(): number {
    return this.native.outputChannels();
  }

  /**
   * Inactive stages, per-stage latency, clamps, the active input layout, and
   * the layout-switch / truncated-drain counters.
   */
  diagnostics(): PlaybackDiagnostics {
    return this.native.diagnostics() as PlaybackDiagnostics;
  }

  /** Non-finite input samples replaced with 0 since construction. */
  nonFiniteDiscardCount(): number {
    return this.native.nonFiniteDiscardCount();
  }

  /** Releases the native handle now instead of waiting for GC. Idempotent. */
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

/**
 * Integrated-loudness meter for multichannel program material (BS.1770
 * channel weights by channel count: 1, 2, 6 or 8), for measuring
 * `loudness.program_lufs` ahead of a {@link PlaybackRenderer}.
 */
export class PlaybackLoudnessMeter {
  private native: NativePlaybackLoudnessMeter;
  private disposed = false;

  constructor(channels: number, sampleRate: number) {
    this.native = new addon.PlaybackLoudnessMeter(channels, sampleRate);
  }

  /** Feeds interleaved frames of any length. */
  pushInterleaved(samples: Float32Array): void {
    this.native.pushInterleaved(samples);
  }

  /** Integrated loudness of everything pushed so far, in LUFS. */
  integratedLufs(): number {
    return this.native.integratedLufs();
  }

  /** Releases the native handle now instead of waiting for GC. Idempotent. */
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

/**
 * Offline one-shot playback render: builds a renderer internally, feeds the
 * whole signal through it, and removes the renderer's own latency from the
 * front of the result so the output aligns with the input.
 * An empty or non-finite `samples` is refused with an `InvalidParameter` error.
 */
export function renderPlayback(request: RenderPlaybackRequest): RenderPlaybackResult {
  requestObject('renderPlayback', request, 'request', true);
  assertAudioInput('renderPlayback', request.samples, request.sampleRate, request);
  return addon.renderPlayback(
    request.samples,
    request.channels,
    request.sampleRate,
    configJsonText(request.config),
    nativeHrtf(request.hrtf),
  ) as RenderPlaybackResult;
}
