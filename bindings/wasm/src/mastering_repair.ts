import { getSonareModule } from './module_state';
import type { RoomEstimateResult } from './public_types_acoustic';
import type {
  MasteringRepairDeclickStereoResult,
  MasteringRepairDeclipStereoResult,
  MasteringRepairDecrackleStereoResult,
  MasteringRepairDehumStereoResult,
} from './public_types_mastering';

function requireModule() {
  return getSonareModule();
}

// ============================================================================
// Mastering repair (declick, denoise_classical, declip, decrackle, dehum,
// dereverb_classical, trim_silence) — hand-written bindings.
// ============================================================================

/** Options for `masteringRepairDeclick`. */
export interface DeclickOptions {
  threshold?: number;
  neighborRatio?: number;
  maxClickSamples?: number;
  lpcOrder?: number;
  residualRatio?: number;
}
export interface MasteringRepairDeclickRequest extends DeclickOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDeclickStereo`. */
export interface MasteringRepairDeclickStereoRequest extends DeclickOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Offline LPC-based declicker for a stereo pair.
 *
 * A run either channel's own detection selects is repaired in BOTH channels —
 * a common-mode click repaired on one side only would move the stereo image.
 * Only the selection is shared: each channel's fill is computed from its own
 * samples and its own AR model, which is why `leftReport` and `rightReport`
 * genuinely differ. Prefer this over calling `masteringRepairDeclick` on each
 * channel separately whenever a click may land in only one channel — a
 * per-channel pass never repairs the other side's image-shifting click.
 */
export function masteringRepairDeclickStereo(
  request: MasteringRepairDeclickStereoRequest,
): MasteringRepairDeclickStereoResult;
export function masteringRepairDeclickStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  config?: DeclickOptions,
): MasteringRepairDeclickStereoResult;
export function masteringRepairDeclickStereo(
  left: Float32Array | MasteringRepairDeclickStereoRequest,
  right?: Float32Array,
  sampleRate?: number,
  config: DeclickOptions = {},
): MasteringRepairDeclickStereoResult {
  const request: MasteringRepairDeclickStereoRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...config }
      : left;
  const { left: leftSamples, right: rightSamples, sampleRate: rate, ...options } = request;
  return requireModule().masteringRepairDeclickStereo(
    leftSamples,
    rightSamples,
    rate ?? 22050,
    options,
  );
}

/** Algorithms accepted by `masteringRepairDenoiseClassical`. */
export type DenoiseClassicalMode = 'logMmse' | 'mmseStsa' | 'spectralSubtraction';

/** Noise PSD estimators accepted by `masteringRepairDenoiseClassical`. */
export type DenoiseClassicalNoiseEstimator = 'quantile' | 'mcra' | 'imcra';

/** Options for `masteringRepairDenoiseClassical`. */
export interface DenoiseClassicalOptions {
  mode?: DenoiseClassicalMode;
  noiseEstimator?: DenoiseClassicalNoiseEstimator;
  nFft?: number;
  hopLength?: number;
  ddAlpha?: number;
  reductionDb?: number;
  overSubtraction?: number;
  spectralFloor?: number;
  noiseEstimationQuantile?: number;
  speechPresenceGain?: boolean;
  gainSmoothing?: boolean;
}
export interface MasteringRepairDenoiseClassicalRequest extends DenoiseClassicalOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Offline LPC-based declicker. */
export function masteringRepairDeclick(request: MasteringRepairDeclickRequest): Float32Array;
export function masteringRepairDeclick(
  samples: Float32Array,
  sampleRate: number,
  options?: DeclickOptions,
): Float32Array;
export function masteringRepairDeclick(
  samples: Float32Array | MasteringRepairDeclickRequest,
  sampleRate?: number,
  options: DeclickOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDeclick(request.samples, request.sampleRate, request);
}

/** Offline STFT-domain classical denoiser (LogMMSE / MMSE-STSA / SpectralSubtraction). */
export function masteringRepairDenoiseClassical(
  request: MasteringRepairDenoiseClassicalRequest,
): Float32Array;
export function masteringRepairDenoiseClassical(
  samples: Float32Array,
  sampleRate: number,
  options?: DenoiseClassicalOptions,
): Float32Array;
export function masteringRepairDenoiseClassical(
  samples: Float32Array | MasteringRepairDenoiseClassicalRequest,
  sampleRate?: number,
  options: DenoiseClassicalOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDenoiseClassical(
    request.samples,
    request.sampleRate,
    request,
  );
}

/** Options for `masteringRepairDeclip`. */
export interface DeclipOptions {
  clipThreshold?: number;
  lpcOrder?: number;
  iterations?: number;
  lpcBlend?: number;
}
export interface MasteringRepairDeclipRequest extends DeclipOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDeclipStereo`. */
export interface MasteringRepairDeclipStereoRequest extends DeclipOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/** Algorithms accepted by `masteringRepairDecrackle`. */
export type DecrackleMode = 'median' | 'waveletShrinkage';

/** Options for `masteringRepairDecrackle`. */
export interface DecrackleOptions {
  threshold?: number;
  mode?: DecrackleMode;
  levels?: number;
}
export interface MasteringRepairDecrackleRequest extends DecrackleOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDecrackleStereo`. */
export interface MasteringRepairDecrackleStereoRequest extends DecrackleOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/** Options for `masteringRepairDehum`. */
export interface DehumOptions {
  fundamentalHz?: number;
  harmonics?: number;
  q?: number;
  adaptive?: boolean;
  searchRangeHz?: number;
  adaptation?: number;
  frameSize?: number;
  pllBandwidth?: number;
}
export interface MasteringRepairDehumRequest extends DehumOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDehumStereo`. */
export interface MasteringRepairDehumStereoRequest extends DehumOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/** Options for `masteringRepairDereverbClassical`. */
export interface DereverbClassicalOptions {
  threshold?: number;
  attenuation?: number;
  nFft?: number;
  hopLength?: number;
  t60Sec?: number;
  lateDelayMs?: number;
  overSubtraction?: number;
  spectralFloor?: number;
  wpeEnabled?: boolean;
  wpeIterations?: number;
  wpeTaps?: number;
  wpeStrength?: number;
}
export interface MasteringRepairDereverbClassicalRequest extends DereverbClassicalOptions {
  samples: Float32Array;
  sampleRate: number;
}

/** Request form of `masteringRepairDereverbConfigForRoom`. */
export interface MasteringRepairDereverbConfigForRoomRequest extends DereverbClassicalOptions {
  /** The measured room, from `estimateRoom`. Only `volume` and `rt60Bands` are read. */
  estimate: RoomEstimateResult;
}

/** Trimming modes accepted by `masteringRepairTrimSilence`. */
export type TrimSilenceMode = 'peak' | 'lufsGated';

/** Options for `masteringRepairTrimSilence`. */
export interface TrimSilenceOptions {
  threshold?: number;
  paddingSamples?: number;
  mode?: TrimSilenceMode;
  gateLufs?: number;
  windowMs?: number;
}
export interface MasteringRepairTrimSilenceRequest extends TrimSilenceOptions {
  samples: Float32Array;
  sampleRate: number;
}

/**
 * Offline LPC-based declipper.
 *
 * Only clipped runs of at most 512 consecutive samples are reconstructed with the LPC solver.
 * The cap is a fixed sample count: it is not derived from `lpcOrder`, from `sampleRate`, or from
 * any other option, so its duration depends on the rate (~10.7 ms at 48 kHz). A longer run is
 * filled with cubic / linear interpolation instead, which keeps the solver's dense matrices
 * bounded by the cap rather than by the input. Exceeding the cap silently changes the
 * reconstruction method rather than throwing: `lpcOrder`, `iterations` and `lpcBlend` have no
 * effect on the interpolated run.
 */
export function masteringRepairDeclip(request: MasteringRepairDeclipRequest): Float32Array;
export function masteringRepairDeclip(
  samples: Float32Array,
  sampleRate: number,
  options?: DeclipOptions,
): Float32Array;
export function masteringRepairDeclip(
  samples: Float32Array | MasteringRepairDeclipRequest,
  sampleRate?: number,
  options: DeclipOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDeclip(request.samples, request.sampleRate, request);
}

/**
 * Offline LPC-based declipper for a stereo pair.
 *
 * Takes the union of both channels' clipped runs. Each channel reconstructs
 * the whole of every union run it has at least one clipped sample in; a
 * channel with none is left untouched there — reconstructing unclipped audio
 * to match the other side would replace real samples with an estimate. A
 * plateau clipped in only one channel therefore produces no linking:
 * `linkedRuns` is non-zero only where both channels are clipped in the same
 * region with different extents.
 */
export function masteringRepairDeclipStereo(
  request: MasteringRepairDeclipStereoRequest,
): MasteringRepairDeclipStereoResult;
export function masteringRepairDeclipStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  config?: DeclipOptions,
): MasteringRepairDeclipStereoResult;
export function masteringRepairDeclipStereo(
  left: Float32Array | MasteringRepairDeclipStereoRequest,
  right?: Float32Array,
  sampleRate?: number,
  config: DeclipOptions = {},
): MasteringRepairDeclipStereoResult {
  const request: MasteringRepairDeclipStereoRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...config }
      : left;
  const { left: leftSamples, right: rightSamples, sampleRate: rate, ...options } = request;
  return requireModule().masteringRepairDeclipStereo(
    leftSamples,
    rightSamples,
    rate ?? 22050,
    options,
  );
}

/** Offline crackle suppressor (median or wavelet-shrinkage). */
export function masteringRepairDecrackle(request: MasteringRepairDecrackleRequest): Float32Array;
export function masteringRepairDecrackle(
  samples: Float32Array,
  sampleRate: number,
  options?: DecrackleOptions,
): Float32Array;
export function masteringRepairDecrackle(
  samples: Float32Array | MasteringRepairDecrackleRequest,
  sampleRate?: number,
  options: DecrackleOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDecrackle(request.samples, request.sampleRate, request);
}

/**
 * Offline crackle suppressor (median or wavelet-shrinkage) for a stereo pair.
 *
 * Crackle is surface damage: the two channels carry different scratches at different instants,
 * so there is no common event for a shared decision to agree about. Each channel is decrackled
 * on its own -- there is no linking, unlike {@link masteringRepairDeclickStereo} and
 * {@link masteringRepairDeclipStereo} -- and this entry point exists to keep the reports and
 * the channel-length contract in one place.
 */
export function masteringRepairDecrackleStereo(
  request: MasteringRepairDecrackleStereoRequest,
): MasteringRepairDecrackleStereoResult;
export function masteringRepairDecrackleStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  config?: DecrackleOptions,
): MasteringRepairDecrackleStereoResult;
export function masteringRepairDecrackleStereo(
  left: Float32Array | MasteringRepairDecrackleStereoRequest,
  right?: Float32Array,
  sampleRate?: number,
  config: DecrackleOptions = {},
): MasteringRepairDecrackleStereoResult {
  const request: MasteringRepairDecrackleStereoRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...config }
      : left;
  const { left: leftSamples, right: rightSamples, sampleRate: rate, ...options } = request;
  return requireModule().masteringRepairDecrackleStereo(
    leftSamples,
    rightSamples,
    rate ?? 22050,
    options,
  );
}

/** Offline mains-hum remover. */
export function masteringRepairDehum(request: MasteringRepairDehumRequest): Float32Array;
export function masteringRepairDehum(
  samples: Float32Array,
  sampleRate: number,
  options?: DehumOptions,
): Float32Array;
export function masteringRepairDehum(
  samples: Float32Array | MasteringRepairDehumRequest,
  sampleRate?: number,
  options: DehumOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDehum(request.samples, request.sampleRate, request);
}

/**
 * Offline mains-hum remover for a stereo pair.
 *
 * With `adaptive` set, mains hum is one physical source, so the tracker reads the channel
 * mean and both cascades follow the one frequency it finds: `appliedFundamentalHz` and
 * `fundamentalDriftHz` come back identical in both reports by construction, while each
 * report's `detected` still measures that channel's own input and each channel keeps its
 * own filter state, so neither channel's transient rings through the other. With `adaptive`
 * clear, which is the default, nothing is shared and the two channels are filtered
 * independently at the configured frequency.
 */
export function masteringRepairDehumStereo(
  request: MasteringRepairDehumStereoRequest,
): MasteringRepairDehumStereoResult;
export function masteringRepairDehumStereo(
  left: Float32Array,
  right: Float32Array,
  sampleRate: number,
  config?: DehumOptions,
): MasteringRepairDehumStereoResult;
export function masteringRepairDehumStereo(
  left: Float32Array | MasteringRepairDehumStereoRequest,
  right?: Float32Array,
  sampleRate?: number,
  config: DehumOptions = {},
): MasteringRepairDehumStereoResult {
  const request: MasteringRepairDehumStereoRequest =
    left instanceof Float32Array
      ? { left, right: right as Float32Array, sampleRate, ...config }
      : left;
  const { left: leftSamples, right: rightSamples, sampleRate: rate, ...options } = request;
  return requireModule().masteringRepairDehumStereo(
    leftSamples,
    rightSamples,
    rate ?? 22050,
    options,
  );
}

/** Offline classical dereverberator (spectral subtraction + optional WPE). */
export function masteringRepairDereverbClassical(
  request: MasteringRepairDereverbClassicalRequest,
): Float32Array;
export function masteringRepairDereverbClassical(
  samples: Float32Array,
  sampleRate: number,
  options?: DereverbClassicalOptions,
): Float32Array;
export function masteringRepairDereverbClassical(
  samples: Float32Array | MasteringRepairDereverbClassicalRequest,
  sampleRate?: number,
  options: DereverbClassicalOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairDereverbClassical(
    request.samples,
    request.sampleRate,
    request,
  );
}

/**
 * Point a dereverb config at a measured room.
 *
 * The pair to {@link estimateRoom}, which measures a recording blind. Returns a complete
 * config for {@link masteringRepairDereverbClassical}, so the caller does not have to know
 * which reverberation-time band to use or how the late delay relates to room size.
 *
 * What the room decides is *where* the tail is. Exactly two fields come back changed from
 * what was passed in:
 *
 * - `t60Sec` — the mid-frequency reverberation time, the average of the 500 Hz and 1 kHz
 *   octaves an ISO 3382 room is quoted by.
 * - `lateDelayMs` — Polack's mixing time, sqrt(volume) in milliseconds, past which the
 *   response is a diffuse tail rather than separable early reflections.
 *
 * How *much* to remove is taste rather than measurement, so `attenuation`, `threshold`,
 * `overSubtraction` and `spectralFloor` are never written. A measurement that did not
 * converge leaves its own field alone, so a partial estimate still configures the half it
 * measured; a low-`confidence` estimate is still applied, because whether to trust it is
 * the caller's call.
 *
 * Every field of `config` that is omitted falls back to the library's own dereverb default,
 * matching {@link masteringRepairDereverbClassical}, so calling this with only an estimate
 * returns a config that is ready to run. The C ABI underneath reads and writes the whole
 * config and takes every field literally — it has no "zero means default" rule — which is
 * why an omitted field resolves to its default here rather than to zero.
 *
 * @param estimate - The measured room, from {@link estimateRoom}. Only `volume` and
 *   `rt60Bands` are read. The request form carries it as `estimate` alongside the config
 *   fields.
 * @param config - The config to point at the room; omitted fields take the library default.
 * @returns A complete dereverb config.
 *
 * @example
 * ```ts
 * const estimate = estimateRoom(samples, sampleRate);
 * const config = masteringRepairDereverbConfigForRoom(estimate);
 * const clean = masteringRepairDereverbClassical(samples, sampleRate, config);
 * ```
 *
 * When NEITHER mid band converged, `t60Sec` falls back to the average of whatever
 * bands did, so a low-band-only estimate configures something rather than nothing --
 * that value is no longer a mid-frequency figure. Only `volume`, `rt60Bands` and the
 * band count are read; the rest of the estimate is ignored.
 */
export function masteringRepairDereverbConfigForRoom(
  request: MasteringRepairDereverbConfigForRoomRequest,
): Required<DereverbClassicalOptions>;
export function masteringRepairDereverbConfigForRoom(
  estimate: RoomEstimateResult,
  config?: DereverbClassicalOptions,
): Required<DereverbClassicalOptions>;
export function masteringRepairDereverbConfigForRoom(
  estimate: RoomEstimateResult | MasteringRepairDereverbConfigForRoomRequest,
  config: DereverbClassicalOptions = {},
): Required<DereverbClassicalOptions> {
  const request = 'estimate' in estimate ? estimate : { estimate, ...config };
  return requireModule().masteringRepairDereverbConfigForRoom(request.estimate, request);
}

/** Offline silence trimmer (peak threshold or LUFS-gated). */
export function masteringRepairTrimSilence(
  request: MasteringRepairTrimSilenceRequest,
): Float32Array;
export function masteringRepairTrimSilence(
  samples: Float32Array,
  sampleRate: number,
  options?: TrimSilenceOptions,
): Float32Array;
export function masteringRepairTrimSilence(
  samples: Float32Array | MasteringRepairTrimSilenceRequest,
  sampleRate?: number,
  options: TrimSilenceOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ...options }
      : samples;
  return requireModule().masteringRepairTrimSilence(request.samples, request.sampleRate, request);
}
