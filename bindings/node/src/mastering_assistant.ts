import { addon } from './native.js';
import type {
  MasteringAssistantResult,
  MasteringStreamingPreviewResult,
  StreamingPlatform,
  TypedJson,
} from './types.js';
import { assertAudioInput, requestObject } from './validation.js';

/**
 * Params accepted by the assistant entry points. Every key is numeric except
 * two NAMES: `targetPlatform`, a delivery target (`'broadcast'`, `'podcast'`,
 * `'club'`, ...), and `preset`, the mastering preset the suggestion starts from
 * (default `'streaming'`; restoration presets are refused). The assistant never
 * picks a preset from the audio. A number is rejected for either: the numeric
 * index the C ABI carries is a transport detail for callers that cannot pass a
 * string, not part of the JavaScript vocabulary.
 */
export type MasteringAssistantParams = Record<string, number | boolean | string>;

export interface MasteringAssistantSuggestRequest {
  samples: Float32Array;
  sampleRate?: number;
  params?: MasteringAssistantParams;
}

/** The repair-defect block of {@link MasteringAudioProfile}, and of each channel of a repair analysis. */
export interface MasteringAudioProfileDefects {
  measured: boolean;
  clickCount: number;
  clickRejected: number;
  clickLongestRunSamples: number;
  clickPerSecond: number;
  crackleSampleCount: number;
  crackleSampleFraction: number;
  cracklePerSecond: number;
  clipSampleCount: number;
  clipRunCount: number;
  clipLongestRunSamples: number;
  clipSampleFraction: number;
  clipFlatRunCount: number;
  clipFlatSampleCount: number;
  clipLongestFlatRunSamples: number;
  /**
   * Level the flat runs (at least -40 dBFS) sit at: the largest run level once the two
   * highest runs are set aside.
   */
  clipFlatLevel: number;
  noiseFloorDbfs: number;
  noiseBandPeakDbfs: number;
  noiseBandPeakIndex: number;
  humFundamentalHz: number;
  humFundamentalProminence: number;
  humHarmonics: number;
  humFundamentalDbfs: number;
  humPeakHarmonicDbfs: number;
  lateDecayRatioDb: number;
}

/**
 * The shape {@link masteringAudioProfile}'s JSON parses to.
 *
 * The profile crosses as a string, so nothing type-checks it on arrival; this
 * declaration is what a conformance check compares against the paths the C++
 * writer publishes, so a field added on one side and not the other fails there
 * rather than reaching a caller as `undefined`.
 */
export interface MasteringAudioProfile {
  durationSec: number;
  bpm: number;
  bpmConfidence: number;
  loudness: {
    integratedLufs: number;
    lraLu: number;
    truePeakDb: number;
    crestFactorDb: number;
  };
  /** Band levels (`*RmsDb`) are dBFS, mean square: a full-scale sine reads -3.01 dBFS in its band. */
  spectral: {
    subRmsDb: number;
    lowRmsDb: number;
    lowMidRmsDb: number;
    midRmsDb: number;
    highMidRmsDb: number;
    highRmsDb: number;
    airRmsDb: number;
    centroidHz: number;
    flatness: number;
    rolloffHz: number;
  };
  dynamics: {
    shortTermLufsStd: number;
    /** Onset peaks per second above a fixed floor of percussive rise; 0 for steady material. */
    attackDensity: number;
    /** Share of frames at or above 0.35 of the RMS reference, which sets aside outlying events. */
    sustainRatio: number;
  };
  /**
   * What the repair detectors measured. `measured` is false when nothing ran —
   * either `detectDefects` was not asked for or the input was too short — and
   * every other field is then at its default rather than a reading.
   */
  defects: MasteringAudioProfileDefects;
}

/**
 * The profile entry points take numeric params only; they have no target platform.
 * `params.nFft` and `params.hopLength` are the window length and hop in samples at 48000 Hz,
 * rescaled to the input sample rate.
 */
export interface MasteringAudioProfileRequest {
  samples: Float32Array;
  sampleRate?: number;
  params?: Record<string, number | boolean>;
}

export interface MasteringStreamingPreviewRequest {
  samples: Float32Array;
  sampleRate?: number;
  platforms?: StreamingPlatform[];
}

/** Request for {@link masteringAssistantSuggestStereo}. */
export interface MasteringAssistantSuggestStereoRequest {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  params?: MasteringAssistantParams;
}

/**
 * Request for {@link masteringAudioProfileStereo}. `params.nFft` and `params.hopLength` are in
 * samples at 48000 Hz, rescaled to the input sample rate.
 */
export interface MasteringAudioProfileStereoRequest {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  params?: Record<string, number | boolean>;
}

/** Request for {@link masteringStreamingPreviewStereo}. */
export interface MasteringStreamingPreviewStereoRequest {
  left: Float32Array;
  right: Float32Array;
  sampleRate?: number;
  platforms?: StreamingPlatform[];
}

export function masteringAssistantSuggest(
  request: MasteringAssistantSuggestRequest,
): TypedJson<MasteringAssistantResult>;
export function masteringAssistantSuggest(
  samples: Float32Array,
  sampleRate?: number,
  params?: MasteringAssistantParams,
): TypedJson<MasteringAssistantResult>;
export function masteringAssistantSuggest(
  samples: Float32Array | MasteringAssistantSuggestRequest,
  sampleRate = 22050,
  params: MasteringAssistantParams = {},
): string {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, params }
      : requestObject('masteringAssistantSuggest', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('masteringAssistantSuggest', request.samples, resolvedSampleRate, request);
  return addon.masteringAssistantSuggest(request.samples, resolvedSampleRate, request.params ?? {});
}

/**
 * Suggest a mastering chain and return only its flat `{ "module.param": value }`
 * params -- the same values {@link masteringAssistantSuggest}'s parsed
 * `chainConfig.params` carries, without having to dig them out of the full
 * assistant document (explanation, profile) first. The
 * result can be passed directly as {@link masterAudio}'s `overrides`.
 *
 * @throws Error if a suggested param is not a number or boolean (a v2
 *   structured multiband stage); nothing the assistant produces today does
 *   this, but a caller must never see it silently dropped.
 */
export function masteringAssistantSuggestChain(
  request: MasteringAssistantSuggestRequest,
): Record<string, number | boolean> {
  requestObject('masteringAssistantSuggestChain', request, 'request', true);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('masteringAssistantSuggestChain', request.samples, resolvedSampleRate, request);
  return addon.masteringAssistantSuggestChain(
    request.samples,
    resolvedSampleRate,
    request.params ?? {},
  );
}

export function masteringAudioProfile(
  request: MasteringAudioProfileRequest,
): TypedJson<MasteringAudioProfile>;
export function masteringAudioProfile(
  samples: Float32Array,
  sampleRate?: number,
  params?: Record<string, number | boolean>,
): TypedJson<MasteringAudioProfile>;
export function masteringAudioProfile(
  samples: Float32Array | MasteringAudioProfileRequest,
  sampleRate = 22050,
  params: Record<string, number | boolean> = {},
): string {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, params }
      : requestObject('masteringAudioProfile', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('masteringAudioProfile', request.samples, resolvedSampleRate, request);
  return addon.masteringAudioProfile(request.samples, resolvedSampleRate, request.params ?? {});
}

export function masteringStreamingPreview(
  request: MasteringStreamingPreviewRequest,
): TypedJson<MasteringStreamingPreviewResult>;
export function masteringStreamingPreview(
  samples: Float32Array,
  sampleRate?: number,
  platforms?: StreamingPlatform[],
): TypedJson<MasteringStreamingPreviewResult>;
export function masteringStreamingPreview(
  samples: Float32Array | MasteringStreamingPreviewRequest,
  sampleRate = 22050,
  platforms: StreamingPlatform[] = [],
): string {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, platforms }
      : requestObject('masteringStreamingPreview', samples);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput('masteringStreamingPreview', request.samples, resolvedSampleRate, request);
  return addon.masteringStreamingPreview(
    request.samples,
    resolvedSampleRate,
    request.platforms ?? [],
  );
}

/**
 * The stereo helpers below take a request object only. A `Float32Array` in the
 * first position is the positional spelling of the mono helpers, and would
 * otherwise surface as an unrelated complaint about `left`.
 */
function assertStereoRequest(fnName: string, request: unknown): void {
  if (request === null || typeof request !== 'object' || ArrayBuffer.isView(request)) {
    throw new TypeError(`${fnName} takes a request object { left, right, sampleRate }`);
  }
}

/**
 * Suggest a mastering chain for a stereo pair, as shared JSON.
 *
 * Profiles through {@link masteringAudioProfileStereo}, so the loudness stage
 * of the suggestion is built on the channel-summed program rather than a
 * downmix that reads roughly 6 dB low.
 */
export function masteringAssistantSuggestStereo(
  request: MasteringAssistantSuggestStereoRequest,
): TypedJson<MasteringAssistantResult> {
  assertStereoRequest('masteringAssistantSuggestStereo', request);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput(
    'masteringAssistantSuggestStereo',
    request.left,
    resolvedSampleRate,
    request,
    'left',
  );
  assertAudioInput(
    'masteringAssistantSuggestStereo',
    request.right,
    resolvedSampleRate,
    request,
    'right',
  );
  return addon.masteringAssistantSuggestStereo(
    request.left,
    request.right,
    resolvedSampleRate,
    request.params ?? {},
  );
}

/**
 * Suggest a mastering chain for a stereo pair and return only its flat
 * `{ "module.param": value }` params, the same values
 * {@link masteringAssistantSuggestStereo}'s parsed `chainConfig.params`
 * carries. The result can be passed directly as {@link masterAudioStereo}'s
 * `overrides`.
 *
 * @throws Error if a suggested param is not a number or boolean (a v2
 *   structured multiband stage); nothing the assistant produces today does
 *   this, but a caller must never see it silently dropped.
 */
export function masteringAssistantSuggestChainStereo(
  request: MasteringAssistantSuggestStereoRequest,
): Record<string, number | boolean> {
  assertStereoRequest('masteringAssistantSuggestChainStereo', request);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput(
    'masteringAssistantSuggestChainStereo',
    request.left,
    resolvedSampleRate,
    request,
    'left',
  );
  assertAudioInput(
    'masteringAssistantSuggestChainStereo',
    request.right,
    resolvedSampleRate,
    request,
    'right',
  );
  return addon.masteringAssistantSuggestChainStereo(
    request.left,
    request.right,
    resolvedSampleRate,
    request.params ?? {},
  );
}

/**
 * Mastering assistant profile of a stereo pair, as shared JSON.
 *
 * The `loudness` block is measured from the two channels: integrated LUFS
 * and LRA come from the channel-summed program and the true peak is the larger
 * of the two. The spectral, dynamics and tempo fields describe shape and timing
 * rather than absolute level and are measured on the downmix, which keeps them
 * comparable with {@link masteringAudioProfile}. Defect detectors run on each
 * channel and their results are aggregated.
 */
export function masteringAudioProfileStereo(
  request: MasteringAudioProfileStereoRequest,
): TypedJson<MasteringAudioProfile> {
  assertStereoRequest('masteringAudioProfileStereo', request);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput(
    'masteringAudioProfileStereo',
    request.left,
    resolvedSampleRate,
    request,
    'left',
  );
  assertAudioInput(
    'masteringAudioProfileStereo',
    request.right,
    resolvedSampleRate,
    request,
    'right',
  );
  return addon.masteringAudioProfileStereo(
    request.left,
    request.right,
    resolvedSampleRate,
    request.params ?? {},
  );
}

/**
 * Preview streaming-platform normalization for a stereo pair, as shared JSON.
 *
 * Measures the integrated loudness with BS.1770 channel summing and reports the
 * larger of the two channel true peaks. Passing a `0.5 * (left + right)` downmix
 * to {@link masteringStreamingPreview} instead reads roughly 6 dB low on
 * decorrelated material, and both the normalization gain and the ceiling-risk
 * flag follow from that measurement.
 */
export function masteringStreamingPreviewStereo(
  request: MasteringStreamingPreviewStereoRequest,
): TypedJson<MasteringStreamingPreviewResult> {
  assertStereoRequest('masteringStreamingPreviewStereo', request);
  const resolvedSampleRate = request.sampleRate ?? 22050;
  assertAudioInput(
    'masteringStreamingPreviewStereo',
    request.left,
    resolvedSampleRate,
    request,
    'left',
  );
  assertAudioInput(
    'masteringStreamingPreviewStereo',
    request.right,
    resolvedSampleRate,
    request,
    'right',
  );
  return addon.masteringStreamingPreviewStereo(
    request.left,
    request.right,
    resolvedSampleRate,
    request.platforms ?? [],
  );
}
