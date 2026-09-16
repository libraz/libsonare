import { addon } from './native.js';
import type {
  DeclickStereoResult,
  DeclipStereoResult,
  DecrackleStereoResult,
  DehumStereoResult,
  RoomEstimateResult,
} from './types.js';

/** Common input fields for offline repair processors. */
export interface MasteringRepairSamplesRequest {
  samples: Float32Array;
  sampleRate?: number;
}

/** Options for `masteringRepairDeclick`. */
export interface DeclickOptions {
  threshold?: number;
  neighborRatio?: number;
  maxClickSamples?: number;
  lpcOrder?: number;
  residualRatio?: number;
}
export interface MasteringRepairDeclickRequest
  extends MasteringRepairSamplesRequest,
    DeclickOptions {}

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
export interface MasteringRepairDenoiseClassicalRequest
  extends MasteringRepairSamplesRequest,
    DenoiseClassicalOptions {}

/** Offline LPC-based declicker. */
export function masteringRepairDeclick(request: MasteringRepairDeclickRequest): Float32Array;
export function masteringRepairDeclick(
  samples: Float32Array,
  sampleRate?: number,
  options?: DeclickOptions,
): Float32Array;
export function masteringRepairDeclick(
  samples: Float32Array | MasteringRepairDeclickRequest,
  sampleRate = 22050,
  options: DeclickOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.masteringRepairDeclick(request.samples, request.sampleRate ?? 22050, request);
}

/** Request form of `masteringRepairDeclickStereo`. */
export interface MasteringRepairDeclickStereoRequest extends DeclickOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Offline LPC-based declicker for a stereo pair, repairing the union of both
 * channels' detected runs.
 *
 * A run either channel's detector selects is repaired in BOTH channels, so a
 * common-mode click repaired on one side alone never moves the stereo image.
 * Only the selection is shared: each channel's fill is computed from its own
 * samples and its own LPC model, which is why `leftReport` and `rightReport`
 * can genuinely differ. `linkedRuns` on either report counts the runs
 * repaired because of the OTHER channel's own detection -- 0 whenever nothing
 * was borrowed, and always 0 from {@link masteringRepairDeclick}. Merged runs
 * can exceed `maxClickSamples`: that cap governs what may be selected, not
 * how far a selection reaches once both channels agree.
 */
export function masteringRepairDeclickStereo(
  request: MasteringRepairDeclickStereoRequest,
): DeclickStereoResult {
  return addon.masteringRepairDeclickStereo(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request,
  );
}

/** Offline STFT-domain classical denoiser (LogMMSE / MMSE-STSA / SpectralSubtraction). */
export function masteringRepairDenoiseClassical(
  request: MasteringRepairDenoiseClassicalRequest,
): Float32Array;
export function masteringRepairDenoiseClassical(
  samples: Float32Array,
  sampleRate?: number,
  options?: DenoiseClassicalOptions,
): Float32Array;
export function masteringRepairDenoiseClassical(
  samples: Float32Array | MasteringRepairDenoiseClassicalRequest,
  sampleRate = 22050,
  options: DenoiseClassicalOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.masteringRepairDenoiseClassical(
    request.samples,
    request.sampleRate ?? 22050,
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
export interface MasteringRepairDeclipRequest
  extends MasteringRepairSamplesRequest,
    DeclipOptions {}

/** Algorithms accepted by `masteringRepairDecrackle`. */
export type DecrackleMode = 'median' | 'waveletShrinkage';

/** Options for `masteringRepairDecrackle`. */
export interface DecrackleOptions {
  threshold?: number;
  mode?: DecrackleMode;
  levels?: number;
}
export interface MasteringRepairDecrackleRequest
  extends MasteringRepairSamplesRequest,
    DecrackleOptions {}

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
export interface MasteringRepairDehumRequest extends MasteringRepairSamplesRequest, DehumOptions {}

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
export interface MasteringRepairDereverbClassicalRequest
  extends MasteringRepairSamplesRequest,
    DereverbClassicalOptions {}

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
export interface MasteringRepairTrimSilenceRequest
  extends MasteringRepairSamplesRequest,
    TrimSilenceOptions {}

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
  sampleRate?: number,
  options?: DeclipOptions,
): Float32Array;
export function masteringRepairDeclip(
  samples: Float32Array | MasteringRepairDeclipRequest,
  sampleRate = 22050,
  options: DeclipOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.masteringRepairDeclip(request.samples, request.sampleRate ?? 22050, request);
}

/** Request form of `masteringRepairDeclipStereo`. */
export interface MasteringRepairDeclipStereoRequest extends DeclipOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Offline LPC-based declipper for a stereo pair, reconstructing the union of
 * both channels' clipped runs.
 *
 * Each channel reconstructs the whole of every union run it has at least one
 * clipped sample in; a channel with no clipped sample in a run is left
 * untouched there, since reconstructing unclipped audio to match the other
 * side would replace real samples with an estimate. `linkedRuns` on either
 * report counts the runs whose reconstruction reached past this channel's own
 * clipped samples because the other channel's run was wider -- a clipped
 * plateau in only one channel produces no linking at all, and it is always 0
 * from {@link masteringRepairDeclip}. See {@link masteringRepairDeclip} for
 * the 512-sample LPC-vs-interpolation cap, which applies per channel here.
 */
export function masteringRepairDeclipStereo(
  request: MasteringRepairDeclipStereoRequest,
): DeclipStereoResult {
  return addon.masteringRepairDeclipStereo(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request,
  );
}

/** Offline crackle suppressor (median or wavelet-shrinkage). */
export function masteringRepairDecrackle(request: MasteringRepairDecrackleRequest): Float32Array;
export function masteringRepairDecrackle(
  samples: Float32Array,
  sampleRate?: number,
  options?: DecrackleOptions,
): Float32Array;
export function masteringRepairDecrackle(
  samples: Float32Array | MasteringRepairDecrackleRequest,
  sampleRate = 22050,
  options: DecrackleOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.masteringRepairDecrackle(request.samples, request.sampleRate ?? 22050, request);
}

/** Request form of `masteringRepairDecrackleStereo`. */
export interface MasteringRepairDecrackleStereoRequest extends DecrackleOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Offline crackle suppressor for a stereo pair (median or wavelet-shrinkage),
 * each channel decrackled on its own.
 *
 * Crackle is surface damage: the two channels carry different scratches at
 * different instants, so there is no common event for a shared decision to
 * agree about and neither mode carries state across channels. Unlike
 * {@link masteringRepairDeclickStereo} and {@link masteringRepairDeclipStereo},
 * no run is ever widened to match the other channel and no report field
 * counts such a widening.
 */
export function masteringRepairDecrackleStereo(
  request: MasteringRepairDecrackleStereoRequest,
): DecrackleStereoResult {
  return addon.masteringRepairDecrackleStereo(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request,
  );
}

/** Offline mains-hum remover. */
export function masteringRepairDehum(request: MasteringRepairDehumRequest): Float32Array;
export function masteringRepairDehum(
  samples: Float32Array,
  sampleRate?: number,
  options?: DehumOptions,
): Float32Array;
export function masteringRepairDehum(
  samples: Float32Array | MasteringRepairDehumRequest,
  sampleRate = 22050,
  options: DehumOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.masteringRepairDehum(request.samples, request.sampleRate ?? 22050, request);
}

/** Request form of `masteringRepairDehumStereo`. */
export interface MasteringRepairDehumStereoRequest extends DehumOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Offline mains-hum remover for a stereo pair.
 *
 * With `adaptive` set, mains hum is one physical source: the tracker reads
 * the channel mean and both cascades follow the one frequency it finds, so
 * `leftReport.appliedFundamentalHz` and `rightReport.appliedFundamentalHz`
 * (and `fundamentalDriftHz`) are identical by construction, even when the two
 * channels carry different hum. Only the frequency is shared -- each channel
 * keeps its own filter state, and each report's `detected` still measures
 * that channel's own input, so `detected.fundamentalHz` can differ between
 * channels while `appliedFundamentalHz` agrees. Without `adaptive`, which is
 * the default, nothing is shared and the two channels are filtered
 * independently at the configured frequency; `fundamentalDriftHz` is then
 * exactly 0, the measurement rather than an unset field.
 */
export function masteringRepairDehumStereo(
  request: MasteringRepairDehumStereoRequest,
): DehumStereoResult {
  return addon.masteringRepairDehumStereo(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request,
  );
}

/** Offline classical dereverberator (spectral subtraction + optional WPE). */
export function masteringRepairDereverbClassical(
  request: MasteringRepairDereverbClassicalRequest,
): Float32Array;
export function masteringRepairDereverbClassical(
  samples: Float32Array,
  sampleRate?: number,
  options?: DereverbClassicalOptions,
): Float32Array;
export function masteringRepairDereverbClassical(
  samples: Float32Array | MasteringRepairDereverbClassicalRequest,
  sampleRate = 22050,
  options: DereverbClassicalOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.masteringRepairDereverbClassical(
    request.samples,
    request.sampleRate ?? 22050,
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
  return addon.masteringRepairDereverbConfigForRoom(request.estimate, request);
}

/** Offline silence trimmer (peak threshold or LUFS-gated). */
export function masteringRepairTrimSilence(
  request: MasteringRepairTrimSilenceRequest,
): Float32Array;
export function masteringRepairTrimSilence(
  samples: Float32Array,
  sampleRate?: number,
  options?: TrimSilenceOptions,
): Float32Array;
export function masteringRepairTrimSilence(
  samples: Float32Array | MasteringRepairTrimSilenceRequest,
  sampleRate = 22050,
  options: TrimSilenceOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.masteringRepairTrimSilence(request.samples, request.sampleRate ?? 22050, request);
}
