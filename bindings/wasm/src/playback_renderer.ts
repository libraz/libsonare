import { entryClass, refuse, refuseType, withEntry } from './internal/entry.js';
import { assertHandleLive } from './internal/handle_guard.js';
import { stringifyWithinBudget } from './internal/list_budget.js';
import { isNonArrayObject } from './internal/value_guards.js';
import { getSonareModule } from './module_state.js';
import type {
  PlaybackDiagnostics,
  PlaybackLoudnessMeterOptions,
  PlaybackRendererConfig,
  PlaybackRendererOptions,
  RenderPlaybackRequest,
  RenderPlaybackResult,
} from './public_types_playback.js';
import type { WasmHrtfSet, WasmPlaybackLoudnessMeter, WasmPlaybackRenderer } from './sonare.js';
import {
  assertAudioInput,
  assertFiniteScalar,
  assertPositiveInteger,
  assertSampleRate,
  requestObject,
} from './validation.js';

function configJsonText(config: PlaybackRendererConfig): string {
  if (!isNonArrayObject(config)) {
    throw refuseType(`config must be a configuration object`);
  }
  return stringifyWithinBudget(config, 'config');
}

const FLOAT32_MAX = 3.4028234663852886e38;

function assertHeadOrientationAngle(value: unknown, argName: string): asserts value is number {
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be a number`);
  }
  assertFiniteScalar(value, argName);
  if (Math.abs(value) > FLOAT32_MAX) {
    throw refuse(`${argName} must be finite and in [-3.4028235e38, 3.4028235e38]`);
  }
}

function nativeHrtf(hrtf: HrtfSet | undefined): WasmHrtfSet | null {
  if (hrtf === undefined) {
    return null;
  }
  if (!(hrtf instanceof HrtfSet)) {
    throw refuseType('hrtf must be an HrtfSet');
  }
  const handle = hrtfHandles.get(hrtf);
  assertHandleLive(handle === undefined, 'HrtfSet');
  return handle as WasmHrtfSet;
}

/** Live native handles by set; a destroyed set has no entry. */
const hrtfHandles = new WeakMap<HrtfSet, WasmHrtfSet>();

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
 * const hrtfUrl = '/assets/hrtf/default.shrf'; // where the app serves the packaged asset
 * const bytes = new Uint8Array(await (await fetch(hrtfUrl)).arrayBuffer());
 * const hrtf = HrtfSet.fromBytes(bytes);
 * const renderer = new PlaybackRenderer({ config: {}, hrtf, sampleRate: 48000 });
 * hrtf.destroy();
 * ```
 */
@entryClass('HrtfSet')
export class HrtfSet {
  private constructor(native: WasmHrtfSet) {
    hrtfHandles.set(this, native);
  }

  /** Builds an HRTF set from SHRF v1 bytes; malformed data throws. */
  static fromBytes(bytes: Uint8Array): HrtfSet {
    return new HrtfSet(getSonareModule().createHrtfSet(bytes));
  }

  /** Releases the native handle. Idempotent, as the Node facade is. */
  destroy(): void {
    const handle = hrtfHandles.get(this);
    if (handle === undefined) {
      return;
    }
    hrtfHandles.delete(this);
    handle.delete();
  }

  /** Releases the handle through {@link destroy}, so the instance works with `using`. */
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
 * `config` follows `schemas/playback-renderer-config.schema.json`; see
 * {@link PlaybackRendererConfig}. A headphones target requires `hrtf`.
 *
 * `processPlanar` copies every plane through the embind boundary, so it suits
 * the main thread; like `processInterleaved`, a non-finite sample is replaced
 * with 0 and counted rather than refused. The AudioWorklet path is
 * `SonarePlaybackWorkletProcessor` in the worklet bundle.
 */
@entryClass('PlaybackRenderer', ['processPlanar', 'processInterleaved', 'processSilence', 'native'])
export class PlaybackRenderer {
  private nativeHandle: WasmPlaybackRenderer;
  private released = false;

  private get native(): WasmPlaybackRenderer {
    assertHandleLive(this.released, 'PlaybackRenderer');
    return this.nativeHandle;
  }

  constructor(options: PlaybackRendererOptions) {
    const fnName = 'PlaybackRenderer';
    requestObject(fnName, options, 'options', true);
    assertSampleRate(options.sampleRate);
    const maxBlockSize = options.maxBlockSize ?? undefined;
    if (maxBlockSize !== undefined) {
      if (maxBlockSize === 0) {
        throw refuse(`maxBlockSize must not be 0; omit it for the default`);
      }
      assertPositiveInteger(maxBlockSize, 'maxBlockSize');
    }
    this.nativeHandle = getSonareModule().createPlaybackRenderer(
      configJsonText(options.config),
      nativeHrtf(options.hrtf),
      options.sampleRate,
      maxBlockSize,
    );
  }

  /**
   * Renders one planar block; every plane must carry the same frame count, at
   * most `maxBlockSize` (0 is a no-op). With a fixed input layout the plane
   * count must equal {@link numInputChannels}; with `input.layout: "auto"` it
   * must be 1, 2, 6 or 8, and a change switches the input layout without
   * changing the latency. Non-finite input samples are replaced with 0 and
   * counted ({@link nonFiniteDiscardCount}).
   */
  processPlanar(planes: Float32Array[]): Float32Array[] {
    return this.native.processPlanar(planes);
  }

  /**
   * Renders one planar block of silence for a host with no input connected yet. Every stage
   * advances as for a block of zeros through the active input layout, and the layout is neither
   * fixed nor switched, so under `"auto"` the first block carrying input still sets it. `frames`
   * must be an integer in `[0, maxBlockSize]`; 0 is a no-op.
   */
  processSilence(frames: number): Float32Array[] {
    return this.native.processSilence(frames);
  }

  /** Interleaved variant of {@link processPlanar}. Non-finite input samples are replaced with 0 and counted. */
  processInterleaved(samples: Float32Array, inChannels: number): Float32Array {
    return this.native.processInterleaved(samples, inChannels);
  }

  /**
   * Resets the renderer, then renders the whole interleaved buffer in one call
   * and returns the interleaved output with the target's channel count
   * ({@link numOutputChannels}). The result matches `renderPlayback` for the same
   * configuration, and {@link diagnostics} afterwards describes this render.
   * Unlike the block methods, an empty buffer or a non-finite sample throws.
   */
  renderOffline(samples: Float32Array, inChannels: number): Float32Array {
    return this.native.renderOffline(samples, inChannels);
  }

  /** Applies a complete configuration document; a changed prepare key throws. */
  setConfig(config: PlaybackRendererConfig): void {
    this.native.setConfig(configJsonText(config));
  }

  /** The current complete configuration document. */
  config(): PlaybackRendererConfig {
    return JSON.parse(this.native.configJson()) as PlaybackRendererConfig;
  }

  /**
   * Publishes the listener head orientation in degrees: right-handed,
   * positive yaw turns the head right, positive pitch looks up, positive roll
   * lowers the right ear. Ignored by a speakers target. Each argument must be
   * a finite number representable by a 32-bit float.
   */
  setHeadOrientation(yawDeg: number, pitchDeg = 0, rollDeg = 0): void {
    assertHeadOrientationAngle(yawDeg, 'yawDeg');
    assertHeadOrientationAngle(pitchDeg, 'pitchDeg');
    assertHeadOrientationAngle(rollDeg, 'rollDeg');
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
  get latencySamples(): number {
    return this.native.latencySamples();
  }

  /**
   * Channel count of the active input layout. With `input.layout: "auto"`
   * this follows the channel count of the most recent non-empty process call
   * (2 before the first call).
   */
  get numInputChannels(): number {
    return this.native.inputChannels();
  }

  /** Channel count of the output target. */
  get numOutputChannels(): number {
    return this.native.outputChannels();
  }

  /** Largest block a process call accepts, as the native side resolved it (the library default when `maxBlockSize` was omitted). */
  get maxBlockSize(): number {
    return this.native.maxBlockSize();
  }

  /**
   * Inactive stages, per-stage latency, clamps, the active input layout, and
   * the layout-switch / truncated-drain counters, as a plain object.
   */
  diagnostics(): PlaybackDiagnostics {
    return JSON.parse(this.native.diagnosticsJson()) as PlaybackDiagnostics;
  }

  /** Non-finite input samples replaced with 0 since construction. */
  get nonFiniteDiscardCount(): number {
    return this.native.nonFiniteDiscardCount();
  }

  /** Releases the native handle. Idempotent, as the Node facade is. */
  destroy(): void {
    if (this.released) {
      return;
    }
    this.released = true;
    this.nativeHandle.delete();
  }

  /** Releases the handle through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

/**
 * Integrated-loudness meter for multichannel program material (BS.1770
 * channel weights by channel count: 1, 2, 6 or 8), for measuring
 * `loudness.programLufs` ahead of a {@link PlaybackRenderer}.
 */
@entryClass('PlaybackLoudnessMeter', ['pushInterleaved', 'native'])
export class PlaybackLoudnessMeter {
  private nativeHandle: WasmPlaybackLoudnessMeter;
  private released = false;

  private get native(): WasmPlaybackLoudnessMeter {
    assertHandleLive(this.released, 'PlaybackLoudnessMeter');
    return this.nativeHandle;
  }

  constructor(options: PlaybackLoudnessMeterOptions) {
    requestObject('PlaybackLoudnessMeter', options, 'options', true);
    this.nativeHandle = getSonareModule().createPlaybackLoudnessMeter(
      options.numChannels,
      options.sampleRate,
    );
  }

  /** Feeds interleaved frames of any length. */
  pushInterleaved(samples: Float32Array): void {
    this.native.pushInterleaved(samples);
  }

  /** Integrated loudness of everything pushed so far, in LUFS. `undefined` before the first 400 ms block is complete; `-Infinity` while nothing is above the gate. */
  get integratedLufs(): number | undefined {
    const value = this.native.integratedLufs();
    return Number.isNaN(value) ? undefined : value;
  }

  /** Releases the native handle. Idempotent, as the Node facade is. */
  destroy(): void {
    if (this.released) {
      return;
    }
    this.released = true;
    this.nativeHandle.delete();
  }

  /** Releases the handle through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

/**
 * Offline one-shot playback render: builds a renderer internally, feeds the
 * whole interleaved signal through it, and removes the renderer's own latency
 * so the output aligns with the input frame for frame.
 * An empty or non-finite `samples`, or a `sampleRate` outside `[8000, 384000]`, is refused with a
 * `RangeError`.
 */
export function renderPlayback(request: RenderPlaybackRequest): RenderPlaybackResult {
  return withEntry('renderPlayback', () => {
    requestObject('renderPlayback', request, 'request', true);
    assertAudioInput(request.samples, request.sampleRate, request);
    const { samples, channels } = getSonareModule().renderPlayback(
      request.samples,
      request.numChannels,
      request.sampleRate,
      configJsonText(request.config),
      nativeHrtf(request.hrtf),
    );
    return { samples, numChannels: channels };
  });
}
