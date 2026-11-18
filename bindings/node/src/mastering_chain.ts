import { chainConfigJson } from './internal/chain_config.js';
import { addon } from './native.js';
import type {
  MasteringChainConfig,
  MasteringChainResult,
  MasteringChainStereoResult,
  MasteringPreset,
  ProgressCallback,
  StreamingLoudnessGainResult,
} from './types.js';
import type { OptionsOf, ValidateOptions } from './validation.js';
import { assertAudioInput, requestOnly, sampleOneShot } from './validation.js';

type NativeResult = Record<string, unknown>;

// Native still names the loudness range `loudnessRange` / `outputLra`.
function chainResultFromNative<R>(raw: object): R {
  const { outputLra, report, ...rest } = raw as NativeResult & {
    outputLra: number;
    report: { before: NativeResult; after: NativeResult } & NativeResult;
  };
  const summary = (native: NativeResult): NativeResult => {
    const { loudnessRange, ...others } = native as NativeResult & { loudnessRange: number };
    return { ...others, loudnessRangeLu: loudnessRange };
  };
  return {
    ...rest,
    outputLoudnessRangeLu: outputLra,
    report: { ...report, before: summary(report.before), after: summary(report.after) },
  } as R;
}

/** Throws `TypeError` unless `onProgress` and `cancel` are each absent or a function. */
function assertChainCallbacks(fnName: string, request: object): void {
  for (const name of ['onProgress', 'cancel'] as const) {
    const callback = (request as Record<string, unknown>)[name];
    if (callback !== undefined && callback !== null && typeof callback !== 'function') {
      throw new TypeError(`${fnName}: ${name} must be a function`);
    }
  }
}

/** Request for {@link streamingLoudnessGain}. */
export interface StreamingLoudnessGainRequest extends ValidateOptions {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** The chain config the streaming chain will run; read like {@link masterAudio}'s `overrides`. */
  config?: MasteringChainConfig;
}

/** Request for {@link streamingLoudnessGainStereo}. */
export interface StreamingLoudnessGainStereoRequest extends ValidateOptions {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** The chain config the streaming chain will run; read like `masterAudio`'s `overrides`. */
  config?: MasteringChainConfig;
}

// Native still names the input true peak `truePeakDb`.
function streamingGainFromNative(raw: object): StreamingLoudnessGainResult {
  const { truePeakDb, ...rest } = raw as NativeResult & { truePeakDb: number };
  return { ...rest, truePeakDbtp: truePeakDb } as unknown as StreamingLoudnessGainResult;
}

/**
 * Measures the loudness numbers a {@link StreamingMasteringChain} needs.
 *
 * Runs the offline chain described by `config` up to its loudness stage and
 * measures there, so `loudnessStaticGainDb` equals the gain {@link masterAudio}
 * applies (ceiling clamp included) and `truePeakDbtp` is the peak that clamp
 * used. The gain is computed from `config.loudness` whether or not the stage is
 * enabled; a silent or below-gate stage input yields 0 dB.
 *
 * @throws {TypeError} when `sampleRate` is missing
 * @throws {RangeError} on audio out of domain or an out-of-range `sampleRate`
 *
 * @example
 * const { loudnessStaticGainDb, truePeakDbtp } = streamingLoudnessGain(samples, 48000, { config });
 * const chain = new StreamingMasteringChain({
 *   ...config,
 *   sampleRate: 48000,
 *   numChannels: 1,
 *   loudnessStaticGainDb,
 *   loudnessStaticGainPeakDb: truePeakDbtp,
 * });
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
  const r = sampleOneShot<StreamingLoudnessGainRequest>('streamingLoudnessGain', args);
  assertAudioInput('streamingLoudnessGain', r.samples, r.sampleRate, r);
  return streamingGainFromNative(
    addon.masteringStreamingLoudnessGain(
      r.samples,
      r.sampleRate,
      chainConfigJson('streamingLoudnessGain', 'config', r.config),
    ),
  );
}

/** Stereo counterpart of {@link streamingLoudnessGain}, with BS.1770 channel summing. */
export function streamingLoudnessGainStereo(
  request: StreamingLoudnessGainStereoRequest,
): StreamingLoudnessGainResult {
  const r = requestOnly<StreamingLoudnessGainStereoRequest>('streamingLoudnessGainStereo', [
    request,
  ]);
  assertAudioInput('streamingLoudnessGainStereo', r.left, r.sampleRate, r, 'left');
  assertAudioInput('streamingLoudnessGainStereo', r.right, r.sampleRate, r, 'right');
  if (r.left.length !== r.right.length) {
    throw new RangeError('Stereo channel lengths must match.');
  }
  return streamingGainFromNative(
    addon.masteringStreamingLoudnessGainStereo(
      r.left,
      r.right,
      r.sampleRate,
      chainConfigJson('streamingLoudnessGainStereo', 'config', r.config),
    ),
  );
}

/** Request form of {@link masterAudio}. */
export interface MasterAudioRequest extends ValidateOptions {
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
   */
  overrides?: MasteringChainConfig;
  /** Called between chain stages with `progress` in `[0, 1]` and the stage name. */
  onProgress?: ProgressCallback;
  /** Polled between chain stages; return `true` to stop with a `Cancelled` error. */
  cancel?: () => boolean;
}

/** Request form of {@link masterAudioStereo}. */
export interface MasterAudioStereoRequest extends ValidateOptions {
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
   */
  overrides?: MasteringChainConfig;
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
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz, an integer in `[8000, 384000]`
 * @param options - `preset`, `overrides`, `onProgress`, `cancel`, `validate`
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
  const r = sampleOneShot<MasterAudioRequest>('masterAudio', args);
  assertChainCallbacks('masterAudio', r);
  assertAudioInput('masterAudio', r.samples, r.sampleRate, r);
  if (
    (r.preset === undefined || r.preset === null) &&
    (r.overrides === undefined || r.overrides === null)
  ) {
    throw new TypeError('masterAudio: give a preset or overrides');
  }
  const overrides = chainConfigJson('masterAudio', 'overrides', r.overrides);
  const preset = r.preset ?? null;
  const progress = r.onProgress ?? undefined;
  const cancel = r.cancel ?? undefined;
  return chainResultFromNative(
    progress || cancel
      ? addon.masterAudioWithProgress(
          preset,
          r.samples,
          r.sampleRate,
          overrides,
          progress ?? (() => {}),
          cancel ?? (() => false),
        )
      : addon.masterAudio(preset, r.samples, r.sampleRate, overrides),
  );
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
  const r = requestOnly<MasterAudioStereoRequest>('masterAudioStereo', [request]);
  assertChainCallbacks('masterAudioStereo', r);
  assertAudioInput('masterAudioStereo', r.left, r.sampleRate, r, 'left');
  assertAudioInput('masterAudioStereo', r.right, r.sampleRate, r, 'right');
  if (r.left.length !== r.right.length) {
    throw new RangeError('Stereo channel lengths must match.');
  }
  if (
    (r.preset === undefined || r.preset === null) &&
    (r.overrides === undefined || r.overrides === null)
  ) {
    throw new TypeError('masterAudioStereo: give a preset or overrides');
  }
  const overrides = chainConfigJson('masterAudioStereo', 'overrides', r.overrides);
  const preset = r.preset ?? null;
  const progress = r.onProgress ?? undefined;
  const cancel = r.cancel ?? undefined;
  return chainResultFromNative(
    progress || cancel
      ? addon.masterAudioStereoWithProgress(
          preset,
          r.left,
          r.right,
          r.sampleRate,
          overrides,
          progress ?? (() => {}),
          cancel ?? (() => false),
        )
      : addon.masterAudioStereo(preset, r.left, r.right, r.sampleRate, overrides),
  );
}

/** Request form of {@link masterAudioAsync}. */
export interface MasterAudioAsyncRequest extends ValidateOptions {
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
   */
  overrides?: MasteringChainConfig;
}

/** Request form of {@link masterAudioStereoAsync}. */
export interface MasterAudioStereoAsyncRequest extends ValidateOptions {
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
   */
  overrides?: MasteringChainConfig;
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
  try {
    const r = sampleOneShot<MasterAudioAsyncRequest>('masterAudioAsync', args);
    assertAudioInput('masterAudioAsync', r.samples, r.sampleRate, r);
    if (
      (r.preset === undefined || r.preset === null) &&
      (r.overrides === undefined || r.overrides === null)
    ) {
      throw new TypeError('masterAudioAsync: give a preset or overrides');
    }
    return addon
      .masterAudioAsync(
        r.preset ?? null,
        r.samples,
        r.sampleRate,
        chainConfigJson('masterAudioAsync', 'overrides', r.overrides),
      )
      .then((result: object) => chainResultFromNative<MasteringChainResult>(result));
  } catch (error) {
    return Promise.reject(error);
  }
}

/** Asynchronous variant of {@link masterAudioStereo}; takes a request object only. */
export function masterAudioStereoAsync(
  request: MasterAudioStereoAsyncRequest,
): Promise<MasteringChainStereoResult> {
  try {
    const r = requestOnly<MasterAudioStereoAsyncRequest>('masterAudioStereoAsync', [request]);
    assertAudioInput('masterAudioStereoAsync', r.left, r.sampleRate, r, 'left');
    assertAudioInput('masterAudioStereoAsync', r.right, r.sampleRate, r, 'right');
    if (r.left.length !== r.right.length) {
      throw new RangeError('Stereo channel lengths must match.');
    }
    if (
      (r.preset === undefined || r.preset === null) &&
      (r.overrides === undefined || r.overrides === null)
    ) {
      throw new TypeError('masterAudioStereoAsync: give a preset or overrides');
    }
    return addon
      .masterAudioStereoAsync(
        r.preset ?? null,
        r.left,
        r.right,
        r.sampleRate,
        chainConfigJson('masterAudioStereoAsync', 'overrides', r.overrides),
      )
      .then((result: object) => chainResultFromNative<MasteringChainStereoResult>(result));
  } catch (error) {
    return Promise.reject(error);
  }
}
