import { chainConfigJson } from './internal/chain_config.js';
import { refuse, refuseType, withEntry } from './internal/entry.js';
import { MASTER_AUDIO_SELECTOR_REFUSAL } from './internal/master_audio_selector_refusal.js';
import { isAbsent } from './internal/value_guards.js';
import { addon } from './native.js';
import type {
  MasteringChainConfigInput,
  MasteringChainResult,
  MasteringChainStereoResult,
  MasteringPreset,
  ProgressCallback,
  StreamingLoudnessGainResult,
} from './types.js';
import type { OptionsOf } from './validation.js';
import { assertAudioInput, requestOnly, sampleOneShot } from './validation.js';

type NativeResult = Record<string, unknown>;

/** Throws `TypeError` when neither `preset` nor `overrides` is given. */
function assertChainSelector(request: Pick<MasterAudioRequest, 'preset' | 'overrides'>): void {
  if (isAbsent(request.preset) && isAbsent(request.overrides)) {
    throw refuseType(MASTER_AUDIO_SELECTOR_REFUSAL);
  }
}

/** Throws `TypeError` on a non-function callback or when neither `preset` nor `overrides` is given. */
function assertMasterAudioShape(
  request: Pick<MasterAudioRequest, 'preset' | 'overrides' | 'onProgress' | 'cancel'>,
): void {
  for (const name of ['onProgress', 'cancel'] as const) {
    const callback = request[name];
    if (!isAbsent(callback) && typeof callback !== 'function') {
      throw refuseType(`${name} must be a function`);
    }
  }
  assertChainSelector(request);
}

/** Request for {@link streamingLoudnessGain}. */
export interface StreamingLoudnessGainRequest {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** The chain config the streaming chain will run; read like {@link masterAudio}'s `overrides`. */
  config?: MasteringChainConfigInput;
}

/** Request for {@link streamingLoudnessGainStereo}. */
export interface StreamingLoudnessGainStereoRequest {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** The chain config the streaming chain will run; read like `masterAudio`'s `overrides`. */
  config?: MasteringChainConfigInput;
}

// Native still names the input true peak `truePeakDb`.
function streamingGainFromNative(raw: object): StreamingLoudnessGainResult {
  const { truePeakDb, ...rest } = raw as NativeResult & { truePeakDb?: number };
  // An absent native peak stays absent.
  return {
    ...rest,
    ...(truePeakDb !== undefined ? { truePeakDbtp: truePeakDb } : {}),
  } as unknown as StreamingLoudnessGainResult;
}

/**
 * Measures the loudness numbers a {@link StreamingMasteringChain} needs.
 *
 * Runs the offline chain described by `config` up to its loudness stage and
 * measures there, so `loudnessStaticGainDb` equals the gain {@link masterAudio}
 * applies (ceiling clamp included) and `truePeakDbtp` is the peak that clamp
 * used. The gain is computed from `config.loudness` whether or not the stage is
 * enabled; a stage input with no measurable loudness leaves it absent.
 *
 * @throws {TypeError} when `sampleRate` is missing
 * @throws {RangeError} on audio out of domain or an out-of-range `sampleRate`
 *
 * @example
 * ```ts
 * declare const config: MasteringChainConfig; // the chain the stream will run
 * const { loudnessStaticGainDb, truePeakDbtp } = streamingLoudnessGain(samples, 48000, { config });
 * const chain = new StreamingMasteringChain({
 *   ...config,
 *   sampleRate: 48000,
 *   numChannels: 1,
 *   loudnessStaticGainDb,
 *   loudnessStaticGainPeakDbtp: truePeakDbtp,
 * });
 * ```
 */
export function streamingLoudnessGain(
  request: StreamingLoudnessGainRequest,
): StreamingLoudnessGainResult;
export function streamingLoudnessGain(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<StreamingLoudnessGainRequest>,
): StreamingLoudnessGainResult;
export function streamingLoudnessGain(...args: unknown[]): StreamingLoudnessGainResult {
  return withEntry('streamingLoudnessGain', () => {
    const r = sampleOneShot<StreamingLoudnessGainRequest>('streamingLoudnessGain', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return streamingGainFromNative(
      addon.masteringStreamingLoudnessGain(
        r.samples,
        r.sampleRate,
        chainConfigJson('config', r.config),
      ),
    );
  });
}

/** Stereo counterpart of {@link streamingLoudnessGain}, with BS.1770 channel summing. */
export function streamingLoudnessGainStereo(
  request: StreamingLoudnessGainStereoRequest,
): StreamingLoudnessGainResult {
  return withEntry('streamingLoudnessGainStereo', () => {
    const r = requestOnly<StreamingLoudnessGainStereoRequest>('streamingLoudnessGainStereo', [
      request,
    ]);
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    if (r.left.length !== r.right.length) {
      throw refuse('left and right must have the same length');
    }
    return streamingGainFromNative(
      addon.masteringStreamingLoudnessGainStereo(
        r.left,
        r.right,
        r.sampleRate,
        chainConfigJson('config', r.config),
      ),
    );
  });
}

/** Request form of {@link masterAudio}. */
export interface MasterAudioRequest {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Built-in preset to start from. Absent: the chain runs `overrides` alone. Giving
   * neither `preset` nor `overrides` is refused with a `TypeError`.
   */
  preset?: MasteringPreset;
  /**
   * Chain config applied over the preset, or run alone when `preset` is absent. An
   * explicit `{}` without a preset runs an empty chain and returns the input unchanged.
   * A stage that carries any key takes the `enabled` it states, and is turned on when
   * it states none; so a full config, such as {@link masteringChainConfigFromJson}
   * returns, turns off every preset stage it marks `enabled: false`.
   */
  overrides?: MasteringChainConfigInput;
  /** Called between chain stages with `progress` in `[0, 1]` and the stage name. */
  onProgress?: ProgressCallback;
  /** Polled between chain stages; return `true` to stop with a `Cancelled` error. */
  cancel?: () => boolean;
}

/** Request form of {@link masterAudioStereo}. */
export interface MasterAudioStereoRequest {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Built-in preset to start from. Absent: the chain runs `overrides` alone. Giving
   * neither `preset` nor `overrides` is refused with a `TypeError`.
   */
  preset?: MasteringPreset;
  /**
   * Chain config applied over the preset, or run alone when `preset` is absent. An
   * explicit `{}` without a preset runs an empty chain and returns the input unchanged.
   * A stage that carries any key takes the `enabled` it states, and is turned on when
   * it states none; so a full config, such as {@link masteringChainConfigFromJson}
   * returns, turns off every preset stage it marks `enabled: false`.
   */
  overrides?: MasteringChainConfigInput;
  /** Called between chain stages with `progress` in `[0, 1]` and the stage name. */
  onProgress?: ProgressCallback;
  /** Polled between chain stages; return `true` to stop with a `Cancelled` error. */
  cancel?: () => boolean;
}

/**
 * Master mono audio. With a `preset` the chain starts from that preset and
 * `overrides` is applied on top; without one the chain runs `overrides` alone.
 * Giving neither is refused, since that call would return the input unchanged.
 *
 * @param request - Mono `samples`, `sampleRate` in Hz, an integer in `[8000, 384000]`, `preset`,
 *   `overrides`, `onProgress` and `cancel`; or pass `(samples, sampleRate, options?)`.
 * @returns Processed audio, loudness metadata, and applied stage names
 * @throws {TypeError} when `sampleRate` is missing, neither `preset` nor `overrides` is given,
 *   or a request field has the wrong type
 * @throws {RangeError} on audio out of domain, an unknown `preset` or an invalid override
 */
export function masterAudio(request: MasterAudioRequest): MasteringChainResult;
export function masterAudio(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MasterAudioRequest>,
): MasteringChainResult;
export function masterAudio(...args: unknown[]): MasteringChainResult {
  return withEntry('masterAudio', () => {
    const r = sampleOneShot<MasterAudioRequest>('masterAudio', args, assertMasterAudioShape);
    assertAudioInput(r.samples, r.sampleRate, r);
    const overrides = chainConfigJson('overrides', r.overrides);
    const preset = r.preset ?? null;
    const progress = r.onProgress ?? undefined;
    const cancel = r.cancel ?? undefined;
    return (
      progress || cancel
        ? addon.masterAudio(preset, r.samples, r.sampleRate, overrides, progress, cancel)
        : addon.masterAudio(preset, r.samples, r.sampleRate, overrides)
    ) as MasteringChainResult;
  });
}

/**
 * Stereo counterpart of {@link masterAudio}; takes a request object only.
 *
 * @returns Processed stereo audio, loudness metadata, and applied stage names
 * @throws {TypeError} when `sampleRate` is missing, neither `preset` nor `overrides` is given,
 *   or a request field has the wrong type
 * @throws {RangeError} on audio out of domain, unequal channel lengths, an unknown `preset`
 *   or an invalid override
 */
export function masterAudioStereo(request: MasterAudioStereoRequest): MasteringChainStereoResult {
  return withEntry('masterAudioStereo', () => {
    const r = requestOnly<MasterAudioStereoRequest>('masterAudioStereo', [request]);
    assertMasterAudioShape(r);
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    if (r.left.length !== r.right.length) {
      throw refuse('left and right must have the same length');
    }
    const overrides = chainConfigJson('overrides', r.overrides);
    const preset = r.preset ?? null;
    const progress = r.onProgress ?? undefined;
    const cancel = r.cancel ?? undefined;
    return (
      progress || cancel
        ? addon.masterAudioStereo(
            preset,
            r.left,
            r.right,
            r.sampleRate,
            overrides,
            progress,
            cancel,
          )
        : addon.masterAudioStereo(preset, r.left, r.right, r.sampleRate, overrides)
    ) as MasteringChainStereoResult;
  });
}

/** Request form of {@link masterAudioAsync}. */
export interface MasterAudioAsyncRequest {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Built-in preset to start from. Absent: the chain runs `overrides` alone. Giving
   * neither `preset` nor `overrides` is refused with a `TypeError`.
   */
  preset?: MasteringPreset;
  /**
   * Chain config applied over the preset, or run alone when `preset` is absent. An
   * explicit `{}` without a preset runs an empty chain and returns the input unchanged.
   * A stage that carries any key takes the `enabled` it states, and is turned on when
   * it states none; so a full config, such as {@link masteringChainConfigFromJson}
   * returns, turns off every preset stage it marks `enabled: false`.
   */
  overrides?: MasteringChainConfigInput;
}

/** Request form of {@link masterAudioStereoAsync}. */
export interface MasterAudioStereoAsyncRequest {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /**
   * Built-in preset to start from. Absent: the chain runs `overrides` alone. Giving
   * neither `preset` nor `overrides` is refused with a `TypeError`.
   */
  preset?: MasteringPreset;
  /**
   * Chain config applied over the preset, or run alone when `preset` is absent. An
   * explicit `{}` without a preset runs an empty chain and returns the input unchanged.
   * A stage that carries any key takes the `enabled` it states, and is turned on when
   * it states none; so a full config, such as {@link masteringChainConfigFromJson}
   * returns, turns off every preset stage it marks `enabled: false`.
   */
  overrides?: MasteringChainConfigInput;
}

/**
 * Asynchronous variant of {@link masterAudio}. Runs the full chain on a libuv
 * worker thread and resolves with the same shape as the synchronous call; there
 * is no progress or cancellation, so use the synchronous `masterAudio` for those.
 * Every validation failure is a rejected promise, never a synchronous throw.
 *
 * @throws {TypeError} (rejection) when `sampleRate` is missing, neither `preset` nor `overrides`
 *   is given, or a field has the wrong type
 * @throws {RangeError} (rejection) on audio out of domain, an unknown `preset` or an invalid override
 */
export function masterAudioAsync(request: MasterAudioAsyncRequest): Promise<MasteringChainResult>;
export function masterAudioAsync(
  samples: Float32Array,
  sampleRate: number,
  options: OptionsOf<MasterAudioAsyncRequest>,
): Promise<MasteringChainResult>;
export function masterAudioAsync(...args: unknown[]): Promise<MasteringChainResult> {
  return withEntry('masterAudioAsync', () => {
    try {
      const r = sampleOneShot<MasterAudioAsyncRequest>(
        'masterAudioAsync',
        args,
        assertChainSelector,
      );
      assertAudioInput(r.samples, r.sampleRate, r);
      return addon
        .masterAudioAsync(
          r.preset ?? null,
          r.samples,
          r.sampleRate,
          chainConfigJson('overrides', r.overrides),
        )
        .then((result: object) => result as MasteringChainResult);
    } catch (error) {
      return Promise.reject(error);
    }
  });
}

/** Asynchronous variant of {@link masterAudioStereo}; takes a request object only. */
export function masterAudioStereoAsync(
  request: MasterAudioStereoAsyncRequest,
): Promise<MasteringChainStereoResult> {
  return withEntry('masterAudioStereoAsync', () => {
    try {
      const r = requestOnly<MasterAudioStereoAsyncRequest>('masterAudioStereoAsync', [request]);
      assertChainSelector(r);
      assertAudioInput(r.left, r.sampleRate, r, 'left');
      assertAudioInput(r.right, r.sampleRate, r, 'right');
      if (r.left.length !== r.right.length) {
        throw refuse('left and right must have the same length');
      }
      return addon
        .masterAudioStereoAsync(
          r.preset ?? null,
          r.left,
          r.right,
          r.sampleRate,
          chainConfigJson('overrides', r.overrides),
        )
        .then((result: object) => result as MasteringChainStereoResult);
    } catch (error) {
      return Promise.reject(error);
    }
  });
}
