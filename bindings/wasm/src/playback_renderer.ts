import { getSonareModule } from './module_state';
import type {
  PlaybackDiagnostics,
  PlaybackRendererConfig,
  PlaybackRendererOptions,
  RenderPlaybackRequest,
  RenderPlaybackResult,
} from './public_types_playback';
import type { WasmHrtfSet, WasmPlaybackLoudnessMeter, WasmPlaybackRenderer } from './sonare.js';

function configJsonText(config: PlaybackRendererConfig | string): string {
  return typeof config === 'string' ? config : JSON.stringify(config);
}

/**
 * Reads {@link HrtfSet}'s private `native`: TypeScript's `private` is a
 * compile-time rule, so one reviewed read here beats widening the class with a
 * handle accessor no caller wants.
 */
function nativeHrtf(hrtf: HrtfSet | undefined): WasmHrtfSet | null {
  if (hrtf === undefined) {
    return null;
  }
  if (!(hrtf instanceof HrtfSet)) {
    throw new TypeError('hrtf must be an HrtfSet');
  }
  return (hrtf as unknown as { native: WasmHrtfSet }).native;
}

/**
 * An HRTF set (SHRF v1): the direct-sound impulse responses and inter-aural
 * time delays a headphones-target {@link PlaybackRenderer} convolves each
 * virtual speaker's signal with.
 *
 * This build embeds no HRTF data. The package ships the default set as the
 * asset `@libraz/libsonare/hrtf/default.shrf`; fetch or read it and pass the
 * bytes to {@link HrtfSet.fromBytes}. A renderer built from a set keeps its
 * own copy, so deleting the set right after construction is safe.
 *
 * @example
 * ```ts
 * const bytes = new Uint8Array(await (await fetch(hrtfUrl)).arrayBuffer());
 * const hrtf = HrtfSet.fromBytes(bytes);
 * const renderer = new PlaybackRenderer({ config: {}, hrtf, sampleRate: 48000 });
 * hrtf.delete();
 * ```
 */
export class HrtfSet {
  private native: WasmHrtfSet;
  private released = false;

  private constructor(native: WasmHrtfSet) {
    this.native = native;
  }

  /** Builds an HRTF set from SHRF v1 bytes; malformed data throws. */
  static fromBytes(bytes: Uint8Array): HrtfSet {
    return new HrtfSet(getSonareModule().createHrtfSet(bytes));
  }

  /** Releases the native handle. Idempotent, as the Node facade is. */
  delete(): void {
    if (this.released) {
      return;
    }
    this.released = true;
    this.native.delete();
  }

  /** Alias for {@link delete}, provided for cross-binding (Node) compatibility. */
  destroy(): void {
    this.delete();
  }
}

/**
 * Renders decoded movie audio (mono / stereo / 5.1 / 7.1 PCM) to headphones
 * (binaural, head tracking, room model) or to stereo / 5.1 / 7.1 speakers
 * (upmix, loudness alignment, night-mode DRC, dialogue level, speaker
 * calibration, bass management).
 *
 * `config` follows `schemas/playback-renderer-config.schema.json`; see
 * {@link PlaybackRendererConfig}. A headphones target requires `hrtf`.
 *
 * `processPlanar` copies every plane through the embind boundary, so it suits
 * the main thread; like `processInterleaved`, a non-finite sample is replaced
 * with 0 and counted rather than refused. The AudioWorklet path is
 * `SonarePlaybackWorkletProcessor` in the worklet bundle.
 */
export class PlaybackRenderer {
  private native: WasmPlaybackRenderer;
  private released = false;

  constructor(options: PlaybackRendererOptions) {
    this.native = getSonareModule().createPlaybackRenderer(
      configJsonText(options.config),
      nativeHrtf(options.hrtf),
      options.sampleRate ?? 48000,
      options.maxBlockSize ?? 1024,
    );
  }

  /**
   * Renders one planar block; every plane must carry the same frame count, at
   * most `maxBlockSize` (0 is a no-op). With a fixed input layout the plane
   * count must equal {@link inputChannels}; with `input.layout: "auto"` it
   * must be 1, 2, 6 or 8, and a change switches the input layout without
   * changing the latency. Non-finite input samples are replaced with 0 and
   * counted ({@link nonFiniteDiscardCount}).
   */
  processPlanar(planes: Float32Array[]): Float32Array[] {
    return this.native.processPlanar(planes);
  }

  /** Interleaved variant of {@link processPlanar}. Non-finite input samples are replaced with 0 and counted. */
  processInterleaved(samples: Float32Array, inChannels: number): Float32Array {
    return this.native.processInterleaved(samples, inChannels);
  }

  /** Applies a complete configuration document; a changed prepare key throws. */
  setConfig(config: PlaybackRendererConfig | string): void {
    this.native.setConfig(configJsonText(config));
  }

  /** The current complete configuration document. */
  config(): PlaybackRendererConfig {
    return JSON.parse(this.native.configJson()) as PlaybackRendererConfig;
  }

  /**
   * Publishes the listener head orientation in degrees: right-handed,
   * positive yaw turns the head right, positive pitch looks up, positive roll
   * lowers the right ear. Ignored by a speakers target; a non-finite angle is
   * ignored.
   */
  setHeadOrientation(yawDeg: number, pitchDeg = 0, rollDeg = 0): void {
    this.native.setHeadOrientation(yawDeg, pitchDeg, rollDeg);
  }

  /**
   * Clears DSP state (filters, FIFOs, dynamics, convolution history, pending
   * input-layout drains). Configuration and head pose are kept. Call it after
   * a seek, from the thread that processes.
   */
  reset(): void {
    this.native.reset();
  }

  /**
   * Renderer latency in samples (headphones: near ear). Depends only on the
   * target, the sample rate and distance compensation, never on realtime keys
   * or the input layout.
   */
  latencySamples(): number {
    return this.native.latencySamples();
  }

  /**
   * Channel count of the active input layout. With `input.layout: "auto"`
   * this follows the channel count of the most recent non-empty process call
   * (2 before the first call).
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
   * the layout-switch / truncated-drain counters, as a plain object.
   */
  diagnostics(): PlaybackDiagnostics {
    return JSON.parse(this.native.diagnosticsJson()) as PlaybackDiagnostics;
  }

  /** Non-finite input samples replaced with 0 since construction. */
  nonFiniteDiscardCount(): number {
    return this.native.nonFiniteDiscardCount();
  }

  /** Releases the native handle. Idempotent, as the Node facade is. */
  delete(): void {
    if (this.released) {
      return;
    }
    this.released = true;
    this.native.delete();
  }

  /** Alias for {@link delete}, provided for cross-binding (Node) compatibility. */
  destroy(): void {
    this.delete();
  }
}

/**
 * Integrated-loudness meter for multichannel program material (BS.1770
 * channel weights by channel count: 1, 2, 6 or 8), for measuring
 * `loudness.program_lufs` ahead of a {@link PlaybackRenderer}.
 */
export class PlaybackLoudnessMeter {
  private native: WasmPlaybackLoudnessMeter;
  private released = false;

  constructor(channels: number, sampleRate: number) {
    this.native = getSonareModule().createPlaybackLoudnessMeter(channels, sampleRate);
  }

  /** Feeds interleaved frames of any length. */
  pushInterleaved(samples: Float32Array): void {
    this.native.pushInterleaved(samples);
  }

  /** Integrated loudness of everything pushed so far, in LUFS. */
  integratedLufs(): number {
    return this.native.integratedLufs();
  }

  /** Releases the native handle. Idempotent, as the Node facade is. */
  delete(): void {
    if (this.released) {
      return;
    }
    this.released = true;
    this.native.delete();
  }

  /** Alias for {@link delete}, provided for cross-binding (Node) compatibility. */
  destroy(): void {
    this.delete();
  }
}

/**
 * Offline one-shot playback render: builds a renderer internally, feeds the
 * whole interleaved signal through it, and removes the renderer's own latency
 * so the output aligns with the input frame for frame.
 * An empty or non-finite `samples` is refused with an `InvalidParameter` error.
 */
export function renderPlayback(request: RenderPlaybackRequest): RenderPlaybackResult {
  return getSonareModule().renderPlayback(
    request.samples,
    request.channels,
    request.sampleRate,
    configJsonText(request.config),
    nativeHrtf(request.hrtf),
  );
}
