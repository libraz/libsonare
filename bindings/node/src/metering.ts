import { refuse, withEntry } from './internal/entry.js';
import {
  assertUnitInterval,
  clippingReportFromNative,
  interleaveMeterChannels,
  refuseZeroSentinel,
  waveformReportFromNative,
} from './internal/metering_native.js';
import { isAbsent } from './internal/value_guards.js';
import { addon } from './native.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertInt32,
  assertNonNegativeSafeInteger,
  assertPositiveFloatValue,
  assertSamplesInWindow,
  requestOnly,
  resolveOptionalNonNegative,
  sampleOneShot,
} from './validation.js';

// The FFT size the library falls back to when `nFft` is omitted. Mirrored
// here so the windowed pre-scan covers exactly the span the call will read; a
// test pins it against the `nFft` the library reports back for an omitted request.
const DEFAULT_SPECTRUM_N_FFT = 2048;

// The linear clip threshold the library applies when none is given, as dBFS (about -0.0087).

/**
 * Check the two spectrum sizes the addon would otherwise truncate: a fractional size
 * must not land on a different one than the caller named.
 */
function assertSpectrumOptions(options: SpectrumOptions): void {
  for (const field of ['nFft', 'octaveFraction'] as const) {
    const value = options[field];
    if (!isAbsent(value)) {
      assertInt32(value, field);
    }
  }
}

// ============================================================================
// Metering — basic / true-peak / clipping / dynamic range
// ============================================================================

/** One contiguous run of clipped samples reported by `meteringDetectClipping`. */
export interface ClippingRegion {
  /** Index of the first clipped sample. */
  startSample: number;
  /** Index one past the last clipped sample. */
  endSample: number;
  /** Run length in samples. */
  length: number;
  /** Largest absolute sample value in the run (linear). */
  peak: number;
}

/**
 * Aggregated clipping report of a whole signal: the region report of the metering family.
 * The repair family's detector record, `ClipDetection`, counts the same two quantities under the same names.
 */
export interface ClippingReport {
  /** Samples at or past the threshold. */
  clippedSamples: number;
  /** `clippedSamples` divided by the input length. */
  clippedRatio: number;
  /** Largest absolute sample value over all reported regions (linear). */
  maxClippedPeak: number;
  /** The clipped runs, in order. */
  regions: ClippingRegion[];
}

/** Sliding-window dynamic range report. */
export interface DynamicRangeReport {
  /** High percentile minus low percentile of the window levels, in dB. */
  dynamicRangeDb: number;
  /** Window RMS level at the low percentile, in dBFS. */
  lowPercentileDb: number;
  /** Window RMS level at the high percentile, in dBFS. */
  highPercentileDb: number;
  /** RMS level of each analysis window, in dBFS. */
  windowRmsDb: Float32Array;
}

/** Options for {@link meteringDetectClipping}. All fields are optional. */
export interface MeteringDetectClippingOptions {
  /** Clip level in dBFS, at most 0 (0 dBFS is a real level). Absent = the library's, about -0.0087 dBFS (0.999 linear); a level too small to reach the detector is a `RangeError`. */
  thresholdDb?: number;
  /** Minimum run length to report. Absent = 1 (the library's); an explicit 0 is a `RangeError`. */
  minRegionSamples?: number;
}

/** Options for {@link meteringDynamicRange}. All fields are optional. */
export interface MeteringDynamicRangeOptions {
  /** Window length in seconds. Absent = 3 s; an explicit 0 is a `RangeError`. */
  windowSec?: number;
  /** Hop length in seconds. Absent = 1 s; an explicit 0 is a `RangeError`. */
  hopSec?: number;
  /** Low percentile rank in [0, 1], at most `highPercentile`. Absent = 0.10. */
  lowPercentile?: number;
  /** High percentile rank in [0, 1]. Absent = 0.95. */
  highPercentile?: number;
}

/** Request form of {@link meteringPeakDb}. */
export interface MeteringPeakDbRequest {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
}

/** Request form of {@link meteringRmsDb}. */
export interface MeteringRmsDbRequest {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
}

/** Request form of {@link meteringCrestFactorDb}. */
export interface MeteringCrestFactorDbRequest {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
}

/** Request form of {@link meteringDcOffset}. */
export interface MeteringDcOffsetRequest {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
}

/** Request form of {@link meteringTruePeakDbtp}. */
export interface MeteringTruePeakDbtpRequest {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
  /** Power of two from 1 to 16. Absent = 4 (the library's); an explicit 0 is a `RangeError`. */
  oversampleFactor?: number;
}

/** Request form of {@link meteringSilenceRatio}. */
export interface MeteringSilenceRatioRequest {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
  /** Frames below this level in dBFS count as silent. Default -45. */
  thresholdDb?: number;
  /** Frame length in samples. Default 1024. */
  frameLength?: number;
  /** Hop in samples. Default 256. */
  hopLength?: number;
}

/** Request form of {@link meteringDetectClipping}. */
export interface MeteringDetectClippingRequest extends MeteringDetectClippingOptions {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
}

/** Request form of {@link meteringDynamicRange}. */
export interface MeteringDynamicRangeRequest extends MeteringDynamicRangeOptions {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
}

/** Reads one mono level: the preflight, then the native call over `(samples, sampleRate)`. */
function monoMeter(
  requestName: 'meteringPeakDb' | 'meteringRmsDb' | 'meteringCrestFactorDb' | 'meteringDcOffset',
  args: unknown[],
): number {
  const r = sampleOneShot<MeteringPeakDbRequest>(requestName, args);
  assertAudioInput(r.samples, r.sampleRate, r);
  return addon[requestName](r.samples, r.sampleRate);
}

/** Peak level in dBFS. */
export function meteringPeakDb(request: MeteringPeakDbRequest): number;
export function meteringPeakDb(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MeteringPeakDbRequest>,
): number;
export function meteringPeakDb(...args: unknown[]): number {
  return withEntry('meteringPeakDb', () => {
    return monoMeter('meteringPeakDb', args);
  });
}

/** RMS level in dBFS. */
export function meteringRmsDb(request: MeteringRmsDbRequest): number;
export function meteringRmsDb(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MeteringRmsDbRequest>,
): number;
export function meteringRmsDb(...args: unknown[]): number {
  return withEntry('meteringRmsDb', () => {
    return monoMeter('meteringRmsDb', args);
  });
}

/** Fraction of frames below `thresholdDb`. */
export function meteringSilenceRatio(request: MeteringSilenceRatioRequest): number;
export function meteringSilenceRatio(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MeteringSilenceRatioRequest>,
): number;
export function meteringSilenceRatio(...args: unknown[]): number {
  return withEntry('meteringSilenceRatio', () => {
    const r = sampleOneShot<MeteringSilenceRatioRequest>('meteringSilenceRatio', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return addon.meteringSilenceRatio(
      r.samples,
      r.sampleRate,
      r.thresholdDb ?? -45,
      r.frameLength ?? 1024,
      r.hopLength ?? 256,
    );
  });
}

/** Crest factor (peak over RMS) in dB; `undefined` for silence, which has neither. */
export function meteringCrestFactorDb(request: MeteringCrestFactorDbRequest): number | undefined;
export function meteringCrestFactorDb(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MeteringCrestFactorDbRequest>,
): number | undefined;
export function meteringCrestFactorDb(...args: unknown[]): number | undefined {
  return withEntry('meteringCrestFactorDb', () => {
    return monoMeter('meteringCrestFactorDb', args);
  });
}

/** Request form of {@link meteringCrestFactorDbStereo}. */
export interface MeteringCrestFactorDbStereoRequest {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate of both channels in Hz. Required. */
  sampleRate: number;
}

/**
 * Crest factor in dB across both channels of a stereo pair.
 *
 * Takes the peak across both channels and the RMS over both together. An
 * out-of-phase pair cancels in the `0.5 * (left + right)` downmix
 * {@link meteringCrestFactorDb} would need, which understates its RMS and so
 * overstates the crest factor. `undefined` when both channels are silent.
 */
export function meteringCrestFactorDbStereo(
  request: MeteringCrestFactorDbStereoRequest,
): number | undefined;
export function meteringCrestFactorDbStereo(...args: unknown[]): number | undefined {
  return withEntry('meteringCrestFactorDbStereo', () => {
    const r = requestOnly<MeteringCrestFactorDbStereoRequest>('meteringCrestFactorDbStereo', args);
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    return addon.meteringCrestFactorDbStereo(r.left, r.right, r.sampleRate);
  });
}

/** DC offset of the signal (its mean). */
export function meteringDcOffset(request: MeteringDcOffsetRequest): number;
export function meteringDcOffset(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MeteringDcOffsetRequest>,
): number;
export function meteringDcOffset(...args: unknown[]): number {
  return withEntry('meteringDcOffset', () => {
    return monoMeter('meteringDcOffset', args);
  });
}

/** Inter-sample (true) peak in dBTP. `oversampleFactor` is a power of two in [1, 16]; omit it for 4. */
export function meteringTruePeakDbtp(request: MeteringTruePeakDbtpRequest): number;
export function meteringTruePeakDbtp(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MeteringTruePeakDbtpRequest>,
): number;
export function meteringTruePeakDbtp(...args: unknown[]): number {
  return withEntry('meteringTruePeakDbtp', () => {
    const r = sampleOneShot<MeteringTruePeakDbtpRequest>('meteringTruePeakDbtp', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    refuseZeroSentinel(r, ['oversampleFactor']);
    // An omitted factor is the library's; the power-of-two rule stays the core's, which names the field.
    if (!isAbsent(r.oversampleFactor)) {
      assertNonNegativeSafeInteger(r.oversampleFactor, 'oversampleFactor');
    }
    return addon.meteringTruePeakDb(r.samples, r.sampleRate, r.oversampleFactor);
  });
}

/**
 * Detect contiguous runs of clipped samples.
 *
 * `thresholdDb` is the clip level in dBFS (at most 0; absent = about -0.0087 dBFS);
 * `minRegionSamples` the shortest run to report (absent = 1).
 */
export function meteringDetectClipping(request: MeteringDetectClippingRequest): ClippingReport;
export function meteringDetectClipping(
  samples: Float32Array,
  sampleRate: number,
  options?: MeteringDetectClippingOptions,
): ClippingReport;
export function meteringDetectClipping(...args: unknown[]): ClippingReport {
  return withEntry('meteringDetectClipping', () => {
    const r = sampleOneShot<MeteringDetectClippingRequest>('meteringDetectClipping', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    refuseZeroSentinel(r, ['minRegionSamples']);
    if (!isAbsent(r.minRegionSamples)) {
      assertNonNegativeSafeInteger(r.minRegionSamples, 'minRegionSamples');
    }
    return clippingReportFromNative(
      addon.meteringDetectClipping(r.samples, r.sampleRate, r.thresholdDb, r.minRegionSamples),
    );
  });
}

/**
 * Sliding-window dynamic range for mono audio. An omitted window / hop takes the
 * library default (3 s / 1 s) and an omitted percentile its default (low = 0.10,
 * high = 0.95); an explicit 0 window or hop is refused.
 */
export function meteringDynamicRange(request: MeteringDynamicRangeRequest): DynamicRangeReport;
export function meteringDynamicRange(
  samples: Float32Array,
  sampleRate: number,
  options?: MeteringDynamicRangeOptions,
): DynamicRangeReport;
export function meteringDynamicRange(...args: unknown[]): DynamicRangeReport {
  return withEntry('meteringDynamicRange', () => {
    const r = sampleOneShot<MeteringDynamicRangeRequest>('meteringDynamicRange', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    refuseZeroSentinel(r, ['windowSec', 'hopSec']);
    for (const field of ['windowSec', 'hopSec'] as const) {
      if (r[field] !== undefined && r[field] !== null) {
        assertPositiveFloatValue(r[field], field);
      }
    }
    for (const field of ['lowPercentile', 'highPercentile'] as const) {
      if (r[field] !== undefined && r[field] !== null) {
        assertUnitInterval(r[field], field);
      }
    }
    if (
      r.lowPercentile !== undefined &&
      r.lowPercentile !== null &&
      r.highPercentile !== undefined &&
      r.highPercentile !== null &&
      r.lowPercentile > r.highPercentile
    ) {
      throw refuse('lowPercentile must not exceed highPercentile');
    }
    return addon.meteringDynamicRange(
      r.samples,
      r.sampleRate,
      r.windowSec ?? 0,
      r.hopSec ?? 0,
      resolveOptionalNonNegative(r.lowPercentile, 'lowPercentile'),
      resolveOptionalNonNegative(r.highPercentile, 'highPercentile'),
    );
  });
}

// ============================================================================
// Metering — stereo / phase-scope / spectrum
// ============================================================================

/** Mid/side vectorscope point series for a (left, right) stereo pair. */
export interface VectorscopeReport {
  /** Mid component of each point. */
  mid: Float32Array;
  /** Side component of each point. */
  side: Float32Array;
}

/** Phase-scope (Lissajous) point series plus summary stats. */
export interface PhaseScopeReport {
  /** Mid component of each point. */
  mid: Float32Array;
  /** Side component of each point. */
  side: Float32Array;
  /** Distance of each point from the origin. */
  radius: Float32Array;
  /** Angle of each point, `atan2(side, mid)`, in radians. */
  angleRad: Float32Array;
  /** Stereo correlation over the full-resolution signal. Absent when either channel has zero energy. */
  correlation?: number;
  /** Mean absolute point angle over the full-resolution signal, in radians. */
  averageAbsAngleRad: number;
  /** Largest point radius over the full-resolution signal. */
  maxRadius: number;
}

/** Options for {@link meteringSpectrum}. All fields are optional; an omitted one takes the library default. */
export interface SpectrumOptions {
  /** FFT size. Absent = 2048; an explicit 0 is a `RangeError`. */
  nFft?: number;
  /** Apply fractional-octave smoothing to magnitude. */
  applyOctaveSmoothing?: boolean;
  /** Smoothing fraction (3 = 1/3-octave). Absent = 3; an explicit 0 is a `RangeError`. */
  octaveFraction?: number;
  /** Linear reference for the dB conversion. Absent = 1.0; an explicit 0 is a `RangeError`. */
  dbRef?: number;
  /** Linear floor used to avoid log(0). Absent = the library epsilon; an explicit 0 is a `RangeError`. */
  dbAmin?: number;
}

/** Single-frame magnitude / power / dB spectrum returned by `meteringSpectrum`. */
export interface SpectrumReport {
  /** Center frequency of each bin in Hz. */
  frequencies: Float32Array;
  /** Linear magnitude of each bin. */
  magnitude: Float32Array;
  /** Power of each bin. */
  power: Float32Array;
  /** Level of each bin in dB. */
  db: Float32Array;
  /** FFT size the spectrum was computed with. */
  nFft: number;
  /** Sample rate of the analyzed audio in Hz. */
  sampleRate: number;
}

/** Request form of {@link meteringStereoCorrelation}. */
export interface MeteringStereoCorrelationRequest {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate of both channels in Hz. Required. */
  sampleRate: number;
}

/** Request form of {@link meteringStereoWidth}. */
export interface MeteringStereoWidthRequest {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate of both channels in Hz. Required. */
  sampleRate: number;
}

/** Request form of {@link meteringVectorscope}. */
export interface MeteringVectorscopeRequest {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate of both channels in Hz. Required. */
  sampleRate: number;
  /**
   * Upper bound on the returned point count, a positive integer. Absent yields one point per
   * input sample; otherwise the point cloud is deterministically decimated to at most
   * `maxPoints` points for display. `0` is refused.
   */
  maxPoints?: number;
}

/** Request form of {@link meteringPhaseScope}. */
export interface MeteringPhaseScopeRequest {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate of both channels in Hz. Required. */
  sampleRate: number;
  /** Upper bound on the returned point count, a positive integer; absent = every point. `0` is refused. */
  maxPoints?: number;
}

/** Request form of {@link meteringSpectrum}. */
export interface MeteringSpectrumRequest extends SpectrumOptions {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. Required. */
  sampleRate: number;
}

/** Request form of {@link meteringSpectrumFrame}. */
export interface MeteringSpectrumFrameRequest extends MeteringSpectrumRequest {
  /** Sample index where the analysis frame begins (0 = first frame, a real position). Default 0. */
  startSample?: number;
}

/** Request form of {@link waveformPeaks}. */
export interface WaveformPeaksRequest {
  /** One `Float32Array` per channel, equal lengths. */
  channels: Float32Array[];
  /** Bucket width in samples. Default 512. */
  samplesPerBucket?: number;
}

/** Request form of {@link waveformPeakPyramid}. */
export interface WaveformPeakPyramidRequest {
  /** One `Float32Array` per channel, equal lengths. */
  channels: Float32Array[];
  /** Bucket widths in samples, one per zoom level. Default [512, 1024, 2048, 4096]. */
  samplesPerBucketLevels?: number[];
}

/** Per-channel min/max waveform buckets: `min[c]` and `max[c]` hold `bucketCount` values for channel `c`. */
export interface WaveformPeaksReport {
  /** Per-channel bucket minima: `min[c]` holds `bucketCount` values. */
  min: Float32Array[];
  /** Per-channel bucket maxima: `max[c]` holds `bucketCount` values. */
  max: Float32Array[];
  /** Number of buckets per channel. */
  bucketCount: number;
  /** Bucket width in samples. */
  samplesPerBucket: number;
}

type StereoMeterFn = 'meteringStereoCorrelation' | 'meteringStereoWidth';

function stereoMeter(requestName: StereoMeterFn, args: unknown[]): number | undefined {
  const r = requestOnly<MeteringStereoCorrelationRequest>(requestName, args);
  assertAudioInput(r.left, r.sampleRate, r, 'left');
  assertAudioInput(r.right, r.sampleRate, r, 'right');
  return addon[requestName](r.left, r.right, r.sampleRate);
}

/**
 * Uncentered correlation (cosine similarity) in [-1, 1] between equal-length channels.
 * `undefined` when either channel has zero energy.
 */
export function meteringStereoCorrelation(
  request: MeteringStereoCorrelationRequest,
): number | undefined;
export function meteringStereoCorrelation(...args: unknown[]): number | undefined {
  return withEntry('meteringStereoCorrelation', () => {
    return stereoMeter('meteringStereoCorrelation', args);
  });
}

/**
 * Stereo width as `sqrt(side_energy / mid_energy)` in `[0, +Infinity)`: the
 * side/mid RMS *amplitude* ratio, not the energy ratio. 0 = pure mono, ~1 =
 * wide stereo, larger = increasingly decorrelated / out-of-phase. The value is
 * unbounded and returns `Infinity` when the mid channel is silent (a
 * mono-collapsed / fully out-of-phase signal). `undefined` when mid and side are
 * both zero.
 *
 * Convert to dB with `20 * Math.log10(value)`; `10 * Math.log10` would
 * understate the true energy ratio by half.
 */
export function meteringStereoWidth(request: MeteringStereoWidthRequest): number | undefined;
export function meteringStereoWidth(...args: unknown[]): number | undefined {
  return withEntry('meteringStereoWidth', () => {
    return stereoMeter('meteringStereoWidth', args);
  });
}

/** A scope request after the shared preflight, with `maxPoints` resolved. */
function scopeRequest(
  requestName: 'meteringVectorscope' | 'meteringPhaseScope',
  args: unknown[],
): MeteringVectorscopeRequest & { maxPoints: number } {
  const r = requestOnly<MeteringVectorscopeRequest>(requestName, args);
  assertAudioInput(r.left, r.sampleRate, r, 'left');
  assertAudioInput(r.right, r.sampleRate, r, 'right');
  refuseZeroSentinel(r, ['maxPoints']);
  const maxPoints = r.maxPoints ?? 0;
  assertNonNegativeSafeInteger(maxPoints, 'maxPoints');
  return { ...r, maxPoints };
}

/**
 * Mid/side vectorscope point series. By default emits one point per input
 * sample; pass `maxPoints` to get a display-sized decimated point set.
 */
export function meteringVectorscope(request: MeteringVectorscopeRequest): VectorscopeReport;
export function meteringVectorscope(...args: unknown[]): VectorscopeReport {
  return withEntry('meteringVectorscope', () => {
    const r = scopeRequest('meteringVectorscope', args);
    return addon.meteringVectorscope(r.left, r.right, r.sampleRate, r.maxPoints);
  });
}

/**
 * Phase-scope point series plus summary stats. By default emits one point per
 * input sample; pass `maxPoints` to decimate the point cloud for display. The
 * summary stats are always computed over the full-resolution signal.
 */
export function meteringPhaseScope(request: MeteringPhaseScopeRequest): PhaseScopeReport;
export function meteringPhaseScope(...args: unknown[]): PhaseScopeReport {
  return withEntry('meteringPhaseScope', () => {
    const r = scopeRequest('meteringPhaseScope', args);
    return addon.meteringPhaseScope(r.left, r.right, r.sampleRate, r.maxPoints);
  });
}

/** The spectrum options the native call reads: only the given ones, `null` read as absent. */
function nativeSpectrumOptions(r: SpectrumOptions): Record<string, unknown> {
  refuseZeroSentinel(r, ['nFft', 'octaveFraction', 'dbRef', 'dbAmin']);
  assertSpectrumOptions(r);
  const native: Record<string, unknown> = {};
  for (const key of [
    'nFft',
    'applyOctaveSmoothing',
    'octaveFraction',
    'dbRef',
    'dbAmin',
  ] as const) {
    if (r[key] !== undefined && r[key] !== null) {
      native[key] = r[key];
    }
  }
  return native;
}

/**
 * Welch-averaged magnitude / power / dB spectrum over the WHOLE mono signal (split
 * into Hann-windowed, 50%-overlapping `nFft`-length frames whose power spectra
 * are averaged). For a true single-frame snapshot, use
 * {@link meteringSpectrumFrame}.
 */
export function meteringSpectrum(request: MeteringSpectrumRequest): SpectrumReport;
export function meteringSpectrum(
  samples: Float32Array,
  sampleRate: number,
  options?: SpectrumOptions,
): SpectrumReport;
export function meteringSpectrum(...args: unknown[]): SpectrumReport {
  return withEntry('meteringSpectrum', () => {
    const r = sampleOneShot<MeteringSpectrumRequest>('meteringSpectrum', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return addon.meteringSpectrum(r.samples, r.sampleRate, nativeSpectrumOptions(r));
  });
}

/**
 * True single-frame magnitude / power / dB spectrum (one Hann-windowed
 * `nFft`-length FFT), for spectrum-analyzer "moment" snapshots that must not be
 * time-averaged like {@link meteringSpectrum}. The analysis frame spans
 * `[startSample, startSample + nFft)`; samples past the end are zero-padded.
 *
 * The frame is also the only span validated: a non-finite sample inside it is
 * rejected, while one outside it neither reaches the FFT nor refuses the call.
 * The emptiness and `sampleRate` checks still cover the whole buffer. Cost per
 * call is therefore set by `nFft` rather than by the length of the buffer, so an
 * analyzer may poll a long recording frame by frame.
 */
export function meteringSpectrumFrame(request: MeteringSpectrumFrameRequest): SpectrumReport;
export function meteringSpectrumFrame(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MeteringSpectrumFrameRequest>,
): SpectrumReport;
export function meteringSpectrumFrame(...args: unknown[]): SpectrumReport {
  return withEntry('meteringSpectrumFrame', () => {
    const r = sampleOneShot<MeteringSpectrumFrameRequest>('meteringSpectrumFrame', args);
    // The frame is the only span scanned for non-finite samples (below).
    assertAudioInput(r.samples, r.sampleRate, { scanFinite: false });
    const native = nativeSpectrumOptions(r);
    const startSample = r.startSample ?? 0;
    assertNonNegativeSafeInteger(startSample, 'startSample');
    const nFft = (native.nFft as number | undefined) ?? DEFAULT_SPECTRUM_N_FFT;
    assertSamplesInWindow(r.samples, true, startSample, nFft);
    // Native takes the start as the positional `frame_offset`.
    return addon.meteringSpectrumFrame(r.samples, r.sampleRate, startSample, native);
  });
}

/**
 * Compute per-channel min/max waveform buckets from planar audio.
 *
 * A non-finite sample is rejected rather than skipped; the JS pre-scan names the offending
 * index. A bucket whose samples are not finite has no min/max to report, and the
 * `0`/`0` it would otherwise carry is what a waveform display draws as silence.
 * Bucket widths are in samples, so the call takes no sample rate.
 */
export function waveformPeaks(request: WaveformPeaksRequest): WaveformPeaksReport;
export function waveformPeaks(...args: unknown[]): WaveformPeaksReport {
  return withEntry('waveformPeaks', () => {
    const r = requestOnly<WaveformPeaksRequest>('waveformPeaks', args);
    // Native reads one interleaved buffer and a channel count.
    const { data, numChannels } = interleaveMeterChannels(r.channels, r);
    const samplesPerBucket = r.samplesPerBucket ?? 512;
    return waveformReportFromNative(addon.waveformPeaks(data, numChannels, samplesPerBucket));
  });
}

/**
 * Compute waveform peak buckets for several zoom levels.
 *
 * Shares {@link waveformPeaks}' bucket kernel, so a non-finite sample is
 * rejected here on the same rule.
 */
export function waveformPeakPyramid(request: WaveformPeakPyramidRequest): WaveformPeaksReport[];
export function waveformPeakPyramid(...args: unknown[]): WaveformPeaksReport[] {
  return withEntry('waveformPeakPyramid', () => {
    const r = requestOnly<WaveformPeakPyramidRequest>('waveformPeakPyramid', args);
    const { data, numChannels } = interleaveMeterChannels(r.channels, r);
    const levels = r.samplesPerBucketLevels ?? [512, 1024, 2048, 4096];
    if (levels.length === 0) {
      throw refuse('samplesPerBucketLevels must not be empty');
    }
    return addon.waveformPeakPyramid(data, numChannels, levels).map(waveformReportFromNative);
  });
}
