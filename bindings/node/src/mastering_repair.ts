import { addon } from './native.js';
import type {
  ClickDetection,
  ClipDetection,
  CrackleDetection,
  DeclickStereoResult,
  DeclipStereoResult,
  DecrackleStereoResult,
  DehumStereoResult,
  DenoiseStereoResult,
  DereverbStereoResult,
  HumDetection,
  NoiseDetection,
  ReverbDetection,
  RoomEstimateResult,
  TrimRange,
  TrimSilenceStereoResult,
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

/** Request form of `masteringRepairDenoiseClassicalStereo`. */
export interface MasteringRepairDenoiseClassicalStereoRequest extends DenoiseClassicalOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Offline STFT-domain classical denoiser for a stereo pair, driven by one
 * channel-linked gain mask.
 *
 * The mask is built from the channel-summed power and applied unchanged to
 * both channels, so the pass cannot move an interchannel level or phase
 * difference. That is also why the result carries a single `report` rather
 * than one per channel: a pair would be two copies of one measurement and
 * would read as though the two could differ.
 *
 * `report.detected` is therefore a *pair-level* measurement. Its levels are
 * absolute dBFS taken on the channel-summed power, so two identical channels
 * read about 3 dB above the same material through
 * {@link masteringRepairDenoiseClassical}. A stereo floor is comparable only
 * against another stereo floor, never against a mono one.
 *
 * Needs at least `nFft` samples and REJECTS a shorter input, which is the
 * opposite of {@link masteringRepairDereverbClassicalStereo} — that one pads.
 *
 * Which options are live depends on `mode`: `overSubtraction` and
 * `spectralFloor` are read only by `spectralSubtraction`, and
 * `speechPresenceGain` and `gainSmoothing` only by the other two, so at the
 * default `logMmse` the first pair does nothing.
 *
 * @example
 * ```ts
 * const { left, right, report } = masteringRepairDenoiseClassicalStereo({
 *   left: leftSamples,
 *   right: rightSamples,
 *   sampleRate: 48000,
 *   reductionDb: 18,
 * });
 * console.log(report.detected.floorDbfs, report.meanReductionDb);
 * ```
 */
export function masteringRepairDenoiseClassicalStereo(
  request: MasteringRepairDenoiseClassicalStereoRequest,
): DenoiseStereoResult {
  return addon.masteringRepairDenoiseClassicalStereo(
    request.left,
    request.right,
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

/** Request form of `masteringRepairDereverbClassicalStereo`. */
export interface MasteringRepairDereverbClassicalStereoRequest extends DereverbClassicalOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Offline classical dereverberator for a stereo pair (spectral subtraction
 * plus an optional WPE pre-stage), driven by one channel-linked mask.
 *
 * The mask is built from the channel-summed power, and the WPE stage
 * accumulates over both channels and applies one predictor set to each, so
 * neither stage can move an interchannel level or phase difference. That is
 * also why the result carries a single `report` rather than one per channel.
 *
 * Every field of that report is a ratio or a fraction, so unlike the denoise
 * pair nothing in it shifts with the channel count: a stereo figure here is
 * comparable against a mono one.
 *
 * An input shorter than `nFft` is PADDED for analysis rather than rejected,
 * the opposite of {@link masteringRepairDenoiseClassicalStereo}.
 *
 * Two report fields are gated on the WPE stage, which is off unless
 * `wpeEnabled` is set: `detected.latePredictability` and
 * `wpePredictorNorm` are then both exactly 0, which is the measurement rather
 * than an unset field. `detected.lateDecayRatioDb` runs the other way from
 * what its name suggests — less negative means the material sustains across
 * the module's late lag, so a reverberant input reads *higher* than the same
 * material dry.
 *
 * @example
 * ```ts
 * const { left, right, report } = masteringRepairDereverbClassicalStereo({
 *   left: leftSamples,
 *   right: rightSamples,
 *   sampleRate: 48000,
 *   wpeEnabled: true,
 * });
 * console.log(report.detected.lateDecayRatioDb, report.wpePredictorNorm);
 * ```
 */
export function masteringRepairDereverbClassicalStereo(
  request: MasteringRepairDereverbClassicalStereoRequest,
): DereverbStereoResult {
  return addon.masteringRepairDereverbClassicalStereo(
    request.left,
    request.right,
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

/** Request form of `masteringRepairTrimSilenceStereo`. */
export interface MasteringRepairTrimSilenceStereoRequest extends TrimSilenceOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Offline silence trimmer for a stereo pair (peak threshold or LUFS-gated),
 * cutting both channels to one shared range.
 *
 * Each channel is scanned on its own and the two ranges are unioned, so the
 * pair keeps whatever *either* channel calls signal and both outputs come back
 * the same length. The scan never reads a downmix: `0.5 * (left + right)`
 * halves material carried by one channel alone, which can drop it under the
 * gate, and cancels an antiphase pair to exactly zero, which would read
 * full-level audio in both channels as silence. Trimming is destructive, so the
 * rule errs toward keeping.
 *
 * The returned channels are SHORTER than the input -- that is the point of the
 * entry, and it is what separates it from every other repair stereo processor,
 * which hand back exactly what they were given. When neither channel carries
 * signal the result is two EMPTY arrays and a success, not an error.
 *
 * `report.range` is the union that was applied. `leftRange` and `rightRange`
 * are the per-channel scans it was formed from, so a caller can see which
 * channel decided each edge. A channel carrying nothing reports an empty range
 * -- `(length, length)` rather than `(0, 0)` -- and contributes nothing to the
 * union.
 *
 * Which option is live depends on `mode`: `threshold` is read only by `'peak'`,
 * and `gateLufs` and `windowMs` only by `'lufsGated'`. That gated mode compares
 * an UNWEIGHTED RMS over a window centred on each sample against `gateLufs`, so
 * the figure it gates on is dBFS rather than a BS.1770 loudness, and `windowMs`
 * sizes that window and does nothing else. The window is clipped at the buffer
 * ends, so a sample near either edge is judged on a shorter one.
 *
 * `paddingSamples` widens the kept range in both directions and is clamped to
 * the buffer, so it can never reach past either end; a pass that kept nothing
 * is not padded. A negative count is refused by name rather than folded into 0.
 *
 * @example
 * ```ts
 * const { left, right, report, leftRange, rightRange } = masteringRepairTrimSilenceStereo({
 *   left: leftSamples,
 *   right: rightSamples,
 *   sampleRate: 48000,
 *   paddingSamples: 256,
 * });
 * console.log(left.length, report.removedHeadSamples, leftRange.first, rightRange.first);
 * ```
 */
export function masteringRepairTrimSilenceStereo(
  request: MasteringRepairTrimSilenceStereoRequest,
): TrimSilenceStereoResult {
  return addon.masteringRepairTrimSilenceStereo(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request,
  );
}

/** Request form of `masteringRepairDetectClicks`. */
export interface MasteringRepairDetectClicksRequest
  extends MasteringRepairSamplesRequest,
    DeclickOptions {}

/**
 * Measure clicks without repairing.
 *
 * Runs the same LPC analysis {@link masteringRepairDeclick} runs, so a run
 * counted here is one the repair would act on -- a cheaper threshold-only scan
 * would report runs it leaves alone. The options are the declicker's own, and
 * they select what is counted: a large `rejected` says the configured run
 * length or neighbour ratio is too tight for this material, not that the
 * material is clean.
 *
 * @example
 * ```ts
 * const detected = masteringRepairDetectClicks({ samples, sampleRate: 48000 });
 * console.log(detected.count, detected.perSecond);
 * ```
 */
export function masteringRepairDetectClicks(
  request: MasteringRepairDetectClicksRequest,
): ClickDetection {
  return addon.masteringRepairDetectClicks(request.samples, request.sampleRate ?? 22050, request);
}

/** Request form of `masteringRepairDetectNoiseFloor`. */
export interface MasteringRepairDetectNoiseFloorRequest
  extends MasteringRepairSamplesRequest,
    DenoiseClassicalOptions {}

/**
 * Measure the noise floor without denoising.
 *
 * Runs the STFT and the configured noise estimator -- the two stages
 * {@link masteringRepairDenoiseClassical} runs -- and stops before the gain
 * mask, which is why the attenuation figures are not here: nothing was
 * attenuated.
 *
 * Needs at least `nFft` samples and REJECTS a shorter buffer, unlike
 * {@link masteringRepairDetectReverb}, which pads one. The levels are absolute
 * dBFS, so they are comparable only against another measurement of the same
 * channel count.
 *
 * @example
 * ```ts
 * const detected = masteringRepairDetectNoiseFloor({ samples, sampleRate: 48000 });
 * console.log(detected.floorDbfs, detected.bandFloorDbfs.length);
 * ```
 */
export function masteringRepairDetectNoiseFloor(
  request: MasteringRepairDetectNoiseFloorRequest,
): NoiseDetection {
  return addon.masteringRepairDetectNoiseFloor(
    request.samples,
    request.sampleRate ?? 22050,
    request,
  );
}

/** Request form of `masteringRepairDetectClipping`. */
export interface MasteringRepairDetectClippingRequest
  extends MasteringRepairSamplesRequest,
    DeclipOptions {}

/**
 * Measure clipping without repairing.
 *
 * Counts samples at or past `clipThreshold`; no other option reaches the
 * result, so `lpcOrder`, `iterations` and `lpcBlend` are accepted and change
 * nothing -- they describe a reconstruction that does not run here.
 * `sampleRate` is validated without being read, since no field of the result
 * is a rate.
 *
 * `longestRunSamples` past 512 is the run {@link masteringRepairDeclip} would
 * fill by interpolation rather than with the LPC solver.
 *
 * @example
 * ```ts
 * const detected = masteringRepairDetectClipping({ samples, sampleRate: 48000 });
 * console.log(detected.sampleCount, detected.runCount);
 * ```
 */
export function masteringRepairDetectClipping(
  request: MasteringRepairDetectClippingRequest,
): ClipDetection {
  return addon.masteringRepairDetectClipping(request.samples, request.sampleRate ?? 22050, request);
}

/** Request form of `masteringRepairDetectCrackle`. */
export interface MasteringRepairDetectCrackleRequest
  extends MasteringRepairSamplesRequest,
    DecrackleOptions {}

/**
 * Measure crackle without repairing.
 *
 * Measured by the median criterion whatever `mode` is set to: wavelet
 * shrinkage removes crackle without ever deciding a sample is crackle, so
 * these counts do not describe what `'waveletShrinkage'` would repair.
 *
 * @example
 * ```ts
 * const detected = masteringRepairDetectCrackle({ samples, sampleRate: 48000 });
 * console.log(detected.sampleCount, detected.perSecond);
 * ```
 */
export function masteringRepairDetectCrackle(
  request: MasteringRepairDetectCrackleRequest,
): CrackleDetection {
  return addon.masteringRepairDetectCrackle(request.samples, request.sampleRate ?? 22050, request);
}

/** Request form of `masteringRepairDetectHum`. */
export interface MasteringRepairDetectHumRequest
  extends MasteringRepairSamplesRequest,
    DehumOptions {}

/**
 * Measure mains hum without filtering.
 *
 * Always measured through the estimation path, whatever `adaptive` is set to:
 * the fixed path notches the configured frequency without ever looking for
 * hum, so a detector following the flag would hand back its own input.
 * `fundamentalProminence` is the winning candidate's energy over the median
 * candidate -- 1.0 means no peak was found at all, and it is not a lock flag.
 *
 * @example
 * ```ts
 * const detected = masteringRepairDetectHum({ samples, sampleRate: 48000, fundamentalHz: 60 });
 * console.log(detected.fundamentalHz, detected.fundamentalProminence);
 * ```
 */
export function masteringRepairDetectHum(request: MasteringRepairDetectHumRequest): HumDetection {
  return addon.masteringRepairDetectHum(request.samples, request.sampleRate ?? 22050, request);
}

/** Request form of `masteringRepairDetectReverb`. */
export interface MasteringRepairDetectReverbRequest
  extends MasteringRepairSamplesRequest,
    DereverbClassicalOptions {}

/**
 * Measure reverberation without dereverberating.
 *
 * Runs the STFT and the module's own late-lag decay statistic. A buffer shorter
 * than `nFft` is PADDED for analysis, as the repair pads it -- the opposite of
 * {@link masteringRepairDetectNoiseFloor}, which refuses one.
 *
 * NOT an ISO 3382 reverberation time; use {@link estimateRoom} for a graded
 * RT60. `latePredictability` comes from the WPE stage, which runs only under
 * `wpeEnabled` and then only its covariance and solve, so it is exactly 0 by
 * default -- the measurement rather than an unset field.
 *
 * @example
 * ```ts
 * const detected = masteringRepairDetectReverb({ samples, sampleRate: 48000 });
 * console.log(detected.lateDecayRatioDb, detected.latePredictability);
 * ```
 */
export function masteringRepairDetectReverb(
  request: MasteringRepairDetectReverbRequest,
): ReverbDetection {
  return addon.masteringRepairDetectReverb(request.samples, request.sampleRate ?? 22050, request);
}

/** Request form of `masteringRepairDetectTrimRange`. */
export interface MasteringRepairDetectTrimRangeRequest
  extends MasteringRepairSamplesRequest,
    TrimSilenceOptions {}

/**
 * Measure the range a trim pass would keep, without trimming.
 *
 * The `paddingSamples` asked for is already INSIDE the returned range, so this
 * is the range {@link masteringRepairTrimSilence} would cut to rather than the
 * detected extent of the signal. A buffer with nothing above the threshold
 * reports `(length, length)` -- an empty range at the far end, not `(0, 0)`.
 *
 * @example
 * ```ts
 * const range = masteringRepairDetectTrimRange({ samples, sampleRate: 48000 });
 * console.log(range.first, range.lastExclusive);
 * ```
 */
export function masteringRepairDetectTrimRange(
  request: MasteringRepairDetectTrimRangeRequest,
): TrimRange {
  return addon.masteringRepairDetectTrimRange(
    request.samples,
    request.sampleRate ?? 22050,
    request,
  );
}

/** Request form of `masteringRepairDetectTrimRangeStereo`. */
export interface MasteringRepairDetectTrimRangeStereoRequest extends TrimSilenceOptions {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
}

/**
 * Measure the one range a stereo trim pass would cut both channels to.
 *
 * Each channel is scanned on its own and the two ranges are unioned, so the
 * pair keeps whatever *either* channel calls signal. A channel with nothing
 * above the threshold contributes NO EDGE at all rather than an edge at the
 * buffer's end: the union of a silent channel and an active one is the active
 * channel's range exactly, so one silent channel does not widen the result.
 *
 * A downmix is not read -- summing to mono halves material carried by one
 * channel alone and cancels an antiphase pair outright, either of which would
 * read full-level audio as silence.
 *
 * @example
 * ```ts
 * const range = masteringRepairDetectTrimRangeStereo({ left, right, sampleRate: 48000 });
 * console.log(range.first, range.lastExclusive);
 * ```
 */
export function masteringRepairDetectTrimRangeStereo(
  request: MasteringRepairDetectTrimRangeStereoRequest,
): TrimRange {
  return addon.masteringRepairDetectTrimRangeStereo(
    request.left,
    request.right,
    request.sampleRate ?? 22050,
    request,
  );
}
