import { refuse, withEntry } from './internal/entry.js';
import { assertMatrix2D, toMatrix2D } from './internal/feature_matrix.js';
import { resolveFftOptions } from './internal/fft_options.js';
import { isAbsent } from './internal/value_guards.js';
import { addon } from './native.js';
import type {
  Matrix2D,
  MelNorm,
  MelSpectrogramResult,
  MfccResult,
  ReassignedSpectrogramResult,
  StftResult,
} from './types.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertAudioSamples,
  assertClosedToken,
  assertFiniteScalar,
  assertFloatFields,
  assertNonNegativeScalar,
  assertPositiveFloatValue,
  assertPositiveInteger,
  assertSampleRate,
  requestOnly,
  sampleOneShot,
} from './validation.js';

/** Request for {@link stft}. */
export interface StftRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Even FFT size in samples. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Also return the magnitude in dB as `db`. Default false. */
  db?: boolean;
  /** Also return phase in radians. Default false. */
  phase?: boolean;
}

/** Request for {@link spectralCentroid}. */
export type SpectralCentroidRequest = StftRequest;

/** Request for {@link spectralFlatness}. */
export type SpectralFlatnessRequest = StftRequest;

/** Request for {@link melFilterbank}. */
export interface MelFilterbankRequest {
  /** Sample rate in Hz the spectrum was taken at. */
  sampleRate: number;
  /** FFT size; the filterbank spans its `nFft / 2 + 1` bins. */
  nFft: number;
  /** Number of bands, in [1, 4096]. Default 128. */
  nMels?: number;
  /** Lower edge of the lowest band in Hz. Default 0. */
  fminHz?: number;
  /** Upper edge of the highest band in Hz. Absent means the Nyquist; when given it must be greater than 0 and at most the Nyquist. */
  fmaxHz?: number;
  /** HTK mel formula instead of Slaney. Default false. */
  htk?: boolean;
  /** Band weighting. Default `'slaney'`. */
  norm?: MelNorm;
}

/** Request for {@link melFrequencies}. */
export interface MelFrequenciesRequest {
  /** Number of points, at least 1. Default 128. */
  nMels?: number;
  /** First point in Hz. Default 0. */
  fminHz?: number;
  /** Last point in Hz; greater than `fminHz`. Default 11025. */
  fmaxHz?: number;
  /** HTK mel formula instead of Slaney. Default false. */
  htk?: boolean;
}

/** Request for {@link melSpectrogram}. */
export interface MelSpectrogramRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Mel bands. Default 128. */
  nMels?: number;
  /** Lower band edge in Hz. Default 0. */
  fminHz?: number;
  /** Upper band edge in Hz. Absent means the Nyquist; an explicit 0 is refused (`RangeError`). */
  fmaxHz?: number;
  /** HTK mel formula instead of Slaney. Default false. */
  htk?: boolean;
  /** Even FFT size in samples. Default `2048`. */
  nFft?: number;
  /** Hop in samples. Default `512`. */
  hopLength?: number;
}

/** Request for {@link mfcc}. */
export interface MfccRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Coefficients kept. Default 20. */
  nMfcc?: number;
  /** Cepstral lifter; 0 means none. Default 0. */
  lifter?: number;
  /** Mel bands. Default `128`. */
  nMels?: number;
  /** Lower Mel band edge in Hz. Default `0`. */
  fminHz?: number;
  /** Upper Mel band edge in Hz. Absent means the Nyquist; an explicit 0 is refused (`RangeError`). */
  fmaxHz?: number;
  /** HTK mel formula instead of Slaney. Default `false`. */
  htk?: boolean;
  /** Even FFT size in samples. Default `2048`. */
  nFft?: number;
  /** Hop in samples. Default `512`. */
  hopLength?: number;
}

/** Request for {@link melDelta}. */
export interface MelDeltaRequest {
  /** Feature matrix: rows = features, cols = frames. */
  features: Matrix2D;
  /** Odd regression width of at least 3. Default 9. */
  width?: number;
}

/** Request for {@link reassignedSpectrogram}. */
export interface ReassignedSpectrogramRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Power floor under which a bin is not reassigned. Default 1e-6. */
  refPower?: number;
  /** Fill unreassigned bins with NaN instead of the bin centre. Default false. */
  fillNan?: boolean;
  /** Even FFT size in samples. Default `2048`. */
  nFft?: number;
  /** Hop in samples. Default `512`. */
  hopLength?: number;
}

/** Request for {@link spectralContrast}. */
export interface SpectralContrastRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Contrast bands. Default 6. */
  nBands?: number;
  /** Lower edge of the lowest band, in Hz. Default 200. */
  fminHz?: number;
  /** Quantile separating peak from valley. Default 0.02. */
  quantile?: number;
  /** Even FFT size in samples. Default `2048`. */
  nFft?: number;
  /** Hop in samples. Default `512`. */
  hopLength?: number;
}

/** Request for {@link polyFeatures}. */
export interface PolyFeaturesRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Polynomial order. Default 1. */
  order?: number;
  /** Even FFT size in samples. Default `2048`. */
  nFft?: number;
  /** Hop in samples. Default `512`. */
  hopLength?: number;
}

/** Request for {@link zeroCrossings}. */
export interface ZeroCrossingsRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Audio sample rate in Hz, an integer in [8000, 384000]; required for the shared call shape, not read by the computation. */
  sampleRate: number;
  /** Magnitude at or below which a sample counts as zero. Default `1e-10`. */
  threshold?: number;
  /** Scale `threshold` by the maximum absolute sample. Default `false`. */
  refMagnitude?: boolean;
  /** Always report index 0 as a crossing. Default `true`. */
  pad?: boolean;
  /** Treat the sign of zero as positive. Default `true`. */
  zeroPos?: boolean;
}

/** Request for {@link spectralBandwidth}. */
export interface SpectralBandwidthRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Even FFT size in samples. Default `2048`. */
  nFft?: number;
  /** Hop in samples. Default `512`. */
  hopLength?: number;
  /** Norm order of the deviation. Default 2. */
  p?: number;
}

/** Request for {@link spectralRolloff}. */
export interface SpectralRolloffRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Energy fraction below the rolloff frequency. Default 0.85. */
  rollPercent?: number;
  /** Even FFT size in samples. Default `2048`. */
  nFft?: number;
  /** Hop in samples. Default `512`. */
  hopLength?: number;
}

/** Request for {@link spectralFlux}. */
export interface SpectralFluxRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Even FFT size in samples. Default `2048`. */
  nFft?: number;
  /** Hop in samples. Default `512`. */
  hopLength?: number;
  /** Frame lag of the difference. Default 1. */
  lag?: number;
}

/** Request for {@link zeroCrossingRate}. */
export interface ZeroCrossingRateRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Framing window in samples. Default 2048. */
  frameLength?: number;
  /** Hop in samples. Default `512`. */
  hopLength?: number;
}

/** Request for {@link rmsEnergy}. */
export type RmsEnergyRequest = ZeroCrossingRateRequest;

/** Request for {@link pcen}. */
export interface PcenRequest {
  /** Magnitude spectrogram: rows = bins, cols = frames. */
  values: Matrix2D;
  /** Sample rate used to derive the smoothing time constant (default 22050). */
  sampleRate?: number;
  /** Hop length used to derive the smoothing time constant (default 512). */
  hopLength?: number;
  /** Smoothing filter time constant in seconds (default 0.4). */
  timeConstant?: number;
  /** Gain exponent applied to the smoothed energy (default 0.98). */
  gain?: number;
  /** Bias added before the power compression (default 2.0). */
  bias?: number;
  /** Power exponent of the final compression (default 0.5). */
  power?: number;
  /** Numerical floor to avoid division by zero (default 1e-6). */
  eps?: number;
}

/** STFT magnitude and power, with optional dB and phase planes. */
export function stft(request: StftRequest): StftResult;
export function stft(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<StftRequest>,
): StftResult;
export function stft(...args: unknown[]): StftResult {
  return withEntry('stft', () => {
    const request = sampleOneShot<StftRequest>('stft', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const fft = resolveFftOptions(request.nFft, request.hopLength);
    const r = addon.stft(
      request.samples,
      request.sampleRate,
      fft.nFft,
      fft.hopLength,
      request.db,
      request.phase,
    );
    const out: StftResult = {
      nFft: r.nFft,
      hopLength: r.hopLength,
      sampleRate: r.sampleRate,
      magnitude: toMatrix2D(r.magnitude, r.nBins, r.nFrames),
      power: toMatrix2D(r.power, r.nBins, r.nFrames),
    };
    if (request.db === true) {
      out.db = toMatrix2D(r.db, r.nBins, r.nFrames);
    }
    if (request.phase === true) {
      out.phase = toMatrix2D(r.phase, r.nBins, r.nFrames);
    }
    return out;
  });
}

/** Mel spectrogram. `power` and `db` are rows = mel bands, cols = frames. */
export function melSpectrogram(request: MelSpectrogramRequest): MelSpectrogramResult;
export function melSpectrogram(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MelSpectrogramRequest>,
): MelSpectrogramResult;
export function melSpectrogram(...args: unknown[]): MelSpectrogramResult {
  return withEntry('melSpectrogram', () => {
    const request = sampleOneShot<MelSpectrogramRequest>('melSpectrogram', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const fft = resolveFftOptions(request.nFft, request.hopLength);
    const fminHz = request.fminHz ?? 0;
    const fmaxHz = request.fmaxHz ?? 0;
    assertFloatFields({ fminHz, fmaxHz });
    // An omitted fmaxHz is the core's Nyquist sentinel 0; a given one is a frequency.
    if (request.fmaxHz !== undefined) {
      assertPositiveFloatValue(fmaxHz, 'fmaxHz');
    }
    const r = addon.melSpectrogram(
      request.samples,
      request.sampleRate,
      fft.nFft,
      fft.hopLength,
      request.nMels ?? 128,
      fminHz,
      fmaxHz,
      request.htk ?? false,
    );
    return {
      sampleRate: r.sampleRate,
      hopLength: r.hopLength,
      nFft: r.nFft,
      fminHz: r.fmin,
      fmaxHz: r.fmax,
      htk: r.htk,
      power: toMatrix2D(r.power, r.nMels, r.nFrames),
      db: toMatrix2D(r.db, r.nMels, r.nFrames),
    };
  });
}

/** MFCCs. `coefficients` is rows = coefficients, cols = frames. */
export function mfcc(request: MfccRequest): MfccResult;
export function mfcc(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MfccRequest>,
): MfccResult;
export function mfcc(...args: unknown[]): MfccResult {
  return withEntry('mfcc', () => {
    const request = sampleOneShot<MfccRequest>('mfcc', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const fft = resolveFftOptions(request.nFft, request.hopLength);
    const fminHz = request.fminHz ?? 0;
    const fmaxHz = request.fmaxHz ?? 0;
    assertFloatFields({ fminHz, fmaxHz });
    // An omitted fmaxHz is the core's Nyquist sentinel 0; a given one is a frequency.
    if (request.fmaxHz !== undefined) {
      assertPositiveFloatValue(fmaxHz, 'fmaxHz');
    }
    const r = addon.mfcc(
      request.samples,
      request.sampleRate,
      fft.nFft,
      fft.hopLength,
      request.nMels ?? 128,
      request.nMfcc ?? 20,
      fminHz,
      fmaxHz,
      request.htk ?? false,
      request.lifter ?? 0,
    );
    return {
      sampleRate: r.sampleRate,
      hopLength: r.hopLength,
      nFft: r.nFft,
      nMels: r.nMels,
      fminHz: r.fmin,
      fmaxHz: r.fmax,
      htk: r.htk,
      lifter: r.lifter,
      coefficients: toMatrix2D(r.coefficients, r.nMfcc, r.nFrames),
    };
  });
}

/** First-order regression delta of a feature matrix. Returns rows = features, cols = frames. */
export function melDelta(request: MelDeltaRequest): Matrix2D;
export function melDelta(...args: unknown[]): Matrix2D {
  return withEntry('melDelta', () => {
    const r = requestOnly<MelDeltaRequest>('melDelta', args);
    assertMatrix2D(r.features, 'features');
    const width = r.width ?? 9;
    const out = addon.melDelta(r.features.data, r.features.rows, r.features.cols, width);
    return toMatrix2D(out, r.features.rows, r.features.cols);
  });
}

/** Auger-Flandrin reassigned spectrogram; every matrix is rows = FFT bins, cols = frames. */
export function reassignedSpectrogram(
  request: ReassignedSpectrogramRequest,
): ReassignedSpectrogramResult;
export function reassignedSpectrogram(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<ReassignedSpectrogramRequest>,
): ReassignedSpectrogramResult;
export function reassignedSpectrogram(...args: unknown[]): ReassignedSpectrogramResult {
  return withEntry('reassignedSpectrogram', () => {
    const request = sampleOneShot<ReassignedSpectrogramRequest>('reassignedSpectrogram', args);
    const refPower = request.refPower ?? 1e-6;
    assertNonNegativeScalar(refPower, 'refPower');
    assertAudioInput(request.samples, request.sampleRate, request);
    const fft = resolveFftOptions(request.nFft, request.hopLength);
    const r = addon.reassignedSpectrogram(
      request.samples,
      request.sampleRate,
      fft.nFft,
      fft.hopLength,
      refPower,
      request.fillNan ?? false,
    );
    return {
      magnitude: toMatrix2D(r.magnitude, r.nBins, r.nFrames),
      times: toMatrix2D(r.times, r.nBins, r.nFrames),
      frequencies: toMatrix2D(r.frequencies, r.nBins, r.nFrames),
    };
  });
}

/** Spectral centroid in Hz per frame. */
export function spectralCentroid(request: SpectralCentroidRequest): Float32Array;
export function spectralCentroid(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<SpectralCentroidRequest>,
): Float32Array;
export function spectralCentroid(...args: unknown[]): Float32Array {
  return withEntry('spectralCentroid', () => {
    const request = sampleOneShot<SpectralCentroidRequest>('spectralCentroid', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const fft = resolveFftOptions(request.nFft, request.hopLength);
    return addon.spectralCentroid(request.samples, request.sampleRate, fft.nFft, fft.hopLength);
  });
}

/**
 * Spectral contrast (librosa.feature.spectral_contrast). Returns rows = `nBands + 1` bands, cols = frames.
 *
 * @remarks
 * Band 0 spans `[0, fmin]`, so an `fminHz` below one analysis bin
 * (`sampleRate / nFft`) leaves it empty after the band trim. Row 0 is still
 * finite, but comes from the other bands' extremes rather than from itself, and
 * is neither level-invariant nor confined to the band. Keep `fminHz` at or above
 * one bin width for row 0 to mean anything — at the defaults (22050 Hz, 2048)
 * one bin is 10.8 Hz, so only a small `nFft` or a tiny `fminHz` reaches this.
 */
export function spectralContrast(request: SpectralContrastRequest): Matrix2D;
export function spectralContrast(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<SpectralContrastRequest>,
): Matrix2D;
export function spectralContrast(...args: unknown[]): Matrix2D {
  return withEntry('spectralContrast', () => {
    const request = sampleOneShot<SpectralContrastRequest>('spectralContrast', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const fft = resolveFftOptions(request.nFft, request.hopLength);
    const fminHz = request.fminHz ?? 200;
    assertPositiveFloatValue(fminHz, 'fminHz');
    const r = addon.spectralContrast(
      request.samples,
      request.sampleRate,
      fft.nFft,
      fft.hopLength,
      request.nBands ?? 6,
      fminHz,
      request.quantile ?? 0.02,
    );
    return toMatrix2D(r.data, r.rows, r.cols);
  });
}

/** Per-frame polynomial coefficients (librosa.feature.poly_features). Returns rows = `order + 1`, cols = frames. */
export function polyFeatures(request: PolyFeaturesRequest): Matrix2D;
export function polyFeatures(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<PolyFeaturesRequest>,
): Matrix2D;
export function polyFeatures(...args: unknown[]): Matrix2D {
  return withEntry('polyFeatures', () => {
    const request = sampleOneShot<PolyFeaturesRequest>('polyFeatures', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const fft = resolveFftOptions(request.nFft, request.hopLength);
    const r = addon.polyFeatures(
      request.samples,
      request.sampleRate,
      fft.nFft,
      fft.hopLength,
      request.order ?? 1,
    );
    return toMatrix2D(r.data, r.rows, r.cols);
  });
}

/** Zero-crossing indices of a signal (librosa.zero_crossings). `sampleRate` is validated as an audio sample rate but not read. */
export function zeroCrossings(request: ZeroCrossingsRequest): Int32Array;
export function zeroCrossings(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<ZeroCrossingsRequest>,
): Int32Array;
export function zeroCrossings(...args: unknown[]): Int32Array {
  return withEntry('zeroCrossings', () => {
    const request = sampleOneShot<ZeroCrossingsRequest>('zeroCrossings', args);
    assertAudioSamples(request.samples, request, 'samples', true);
    return addon.zeroCrossings(
      request.samples,
      request.threshold ?? 1e-10,
      request.refMagnitude ?? false,
      request.pad ?? true,
      request.zeroPos ?? true,
    );
  });
}

/** Spectral bandwidth in Hz per frame. */
export function spectralBandwidth(request: SpectralBandwidthRequest): Float32Array;
export function spectralBandwidth(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<SpectralBandwidthRequest>,
): Float32Array;
export function spectralBandwidth(...args: unknown[]): Float32Array {
  return withEntry('spectralBandwidth', () => {
    const request = sampleOneShot<SpectralBandwidthRequest>('spectralBandwidth', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const fft = resolveFftOptions(request.nFft, request.hopLength);
    return addon.spectralBandwidth(
      request.samples,
      request.sampleRate,
      fft.nFft,
      fft.hopLength,
      request.p ?? 2,
    );
  });
}

/** Spectral rolloff frequency in Hz per frame. */
export function spectralRolloff(request: SpectralRolloffRequest): Float32Array;
export function spectralRolloff(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<SpectralRolloffRequest>,
): Float32Array;
export function spectralRolloff(...args: unknown[]): Float32Array {
  return withEntry('spectralRolloff', () => {
    const request = sampleOneShot<SpectralRolloffRequest>('spectralRolloff', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const fft = resolveFftOptions(request.nFft, request.hopLength);
    return addon.spectralRolloff(
      request.samples,
      request.sampleRate,
      fft.nFft,
      fft.hopLength,
      request.rollPercent ?? 0.85,
    );
  });
}

/** Spectral flatness per frame. */
export function spectralFlatness(request: SpectralFlatnessRequest): Float32Array;
export function spectralFlatness(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<SpectralFlatnessRequest>,
): Float32Array;
export function spectralFlatness(...args: unknown[]): Float32Array {
  return withEntry('spectralFlatness', () => {
    const request = sampleOneShot<SpectralFlatnessRequest>('spectralFlatness', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const fft = resolveFftOptions(request.nFft, request.hopLength);
    return addon.spectralFlatness(request.samples, request.sampleRate, fft.nFft, fft.hopLength);
  });
}

/** Spectral flux per frame. */
export function spectralFlux(request: SpectralFluxRequest): Float32Array;
export function spectralFlux(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<SpectralFluxRequest>,
): Float32Array;
export function spectralFlux(...args: unknown[]): Float32Array {
  return withEntry('spectralFlux', () => {
    const request = sampleOneShot<SpectralFluxRequest>('spectralFlux', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const fft = resolveFftOptions(request.nFft, request.hopLength);
    return addon.spectralFlux(
      request.samples,
      request.sampleRate,
      fft.nFft,
      fft.hopLength,
      request.lag ?? 1,
    );
  });
}

/** Zero-crossing rate per frame. */
export function zeroCrossingRate(request: ZeroCrossingRateRequest): Float32Array;
export function zeroCrossingRate(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<ZeroCrossingRateRequest>,
): Float32Array;
export function zeroCrossingRate(...args: unknown[]): Float32Array {
  return withEntry('zeroCrossingRate', () => {
    const request = sampleOneShot<ZeroCrossingRateRequest>('zeroCrossingRate', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    return addon.zeroCrossingRate(
      request.samples,
      request.sampleRate,
      request.frameLength ?? 2048,
      request.hopLength ?? 512,
    );
  });
}

/** RMS energy per frame. */
export function rmsEnergy(request: RmsEnergyRequest): Float32Array;
export function rmsEnergy(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<RmsEnergyRequest>,
): Float32Array;
export function rmsEnergy(...args: unknown[]): Float32Array {
  return withEntry('rmsEnergy', () => {
    const request = sampleOneShot<RmsEnergyRequest>('rmsEnergy', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    return addon.rmsEnergy(
      request.samples,
      request.sampleRate,
      request.frameLength ?? 2048,
      request.hopLength ?? 512,
    );
  });
}

/** Per-channel energy normalization. Returns rows = bins, cols = frames. */
export function pcen(request: PcenRequest): Matrix2D;
export function pcen(...args: unknown[]): Matrix2D {
  return withEntry('pcen', () => {
    const r = requestOnly<PcenRequest>('pcen', args);
    assertMatrix2D(r.values, 'values');
    const { values, ...options } = r;
    // Only what the caller supplied: an omitted field's default belongs to the
    // addon's options reader, so resolving one here would check this file's guess.
    if (options.sampleRate !== undefined) {
      assertSampleRate(options.sampleRate);
    }
    if (options.hopLength !== undefined) {
      assertPositiveInteger(options.hopLength, 'hopLength');
    }
    const out = addon.pcen(values.data, values.rows, values.cols, options);
    return toMatrix2D(out, values.rows, values.cols);
  });
}

const MEL_NORMS: readonly MelNorm[] = ['none', 'slaney'];

/**
 * The mel filterbank that maps an STFT power spectrum onto `nMels` bands, as rows = bands,
 * cols = FFT bins. Band `m` is a triangle over the bins rising from point `m` of
 * `melFrequencies({ nMels: nMels + 2, fminHz, fmaxHz, htk })` to its peak at point `m + 1`
 * and falling to point `m + 2`.
 *
 * A wrong type is a `TypeError`; a value outside its domain, or an upper edge that, resolved
 * to the Nyquist, is not above `fminHz`, is a `RangeError`.
 */
export function melFilterbank(request: MelFilterbankRequest): Matrix2D;
export function melFilterbank(...args: unknown[]): Matrix2D {
  return withEntry('melFilterbank', () => {
    const r = requestOnly<MelFilterbankRequest>('melFilterbank', args);
    // An omitted fmaxHz is the core's Nyquist sentinel 0; a given one is a frequency.
    if (!isAbsent(r.fmaxHz)) {
      assertFiniteScalar(r.fmaxHz, 'fmaxHz');
      if (r.fmaxHz <= 0) {
        throw refuse('fmaxHz must be greater than 0; omit it for the default');
      }
    }
    const norm = r.norm ?? 'slaney';
    assertClosedToken(norm, MEL_NORMS, 'norm');
    const out = addon.melFilterbank(
      r.sampleRate,
      r.nFft,
      r.nMels ?? 128,
      r.fminHz ?? 0,
      r.fmaxHz ?? 0,
      r.htk ?? false,
      MEL_NORMS.indexOf(norm),
    );
    return toMatrix2D(out.data, out.nMels, out.nBins);
  });
}

/**
 * `nMels` frequencies in Hz equally spaced on the mel scale from `fminHz` to `fmaxHz`, both
 * included; one point is `fminHz` alone.
 *
 * A wrong type is a `TypeError`; `nMels` below 1, a negative or non-finite `fminHz`, or an
 * `fmaxHz` that is not finite or not above `fminHz` is a `RangeError`.
 */
export function melFrequencies(request: MelFrequenciesRequest): Float32Array;
export function melFrequencies(...args: unknown[]): Float32Array {
  return withEntry('melFrequencies', () => {
    const r = requestOnly<MelFrequenciesRequest>('melFrequencies', args);
    return addon.melFrequencies(r.nMels ?? 128, r.fminHz ?? 0, r.fmaxHz ?? 11025, r.htk ?? false);
  });
}
