import { chainConfigFromDocument, chainConfigFromJson } from './internal/chain_config.js';
import { parseRecordJson } from './internal/record_json.js';
import { audioDefectsFromNative } from './internal/repair_common.js';
import { masteringPlatformNames, masteringPresetNames } from './mastering_catalog.js';
import { addon } from './native.js';
import type {
  MasteringAssistantResult,
  MasteringAudioProfile,
  MasteringChainConfig,
  MasteringPlatform,
  MasteringPreset,
  MasteringStreamingPreviewResult,
  StreamingPlatform,
} from './types.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertClosedToken,
  assertFloatValue,
  assertPositiveInteger,
  requestOnly,
  sampleOneShot,
} from './validation.js';

export type { MasteringAudioProfile, MasteringAudioProfileDefects } from './types.js';

/** Request form of {@link masteringAudioProfile}. */
export interface MasteringAudioProfileRequest {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Analysis window in samples at 48000 Hz, rescaled to the input rate; a positive integer. */
  nFft?: number;
  /** Analysis hop in samples at 48000 Hz, rescaled to the input rate; a positive integer. */
  hopLength?: number;
  /** True-peak oversampling factor; a positive integer. */
  truePeakOversample?: number;
  /** Measure the repair defects. Default true; false leaves `defects` absent. */
  detectDefects?: boolean;
}

/** Stereo counterpart of {@link MasteringAudioProfileRequest}. */
export interface MasteringAudioProfileStereoRequest {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Analysis window in samples at 48000 Hz, rescaled to the input rate; a positive integer. */
  nFft?: number;
  /** Analysis hop in samples at 48000 Hz, rescaled to the input rate; a positive integer. */
  hopLength?: number;
  /** True-peak oversampling factor; a positive integer. */
  truePeakOversample?: number;
  /** Measure the repair defects. Default `true`; `false` leaves `defects` absent. */
  detectDefects?: boolean;
}

/** Fields the assistant reads beside the audio. */
export interface MasteringAssistantSettings {
  /** Integrated loudness target in LUFS. */
  targetLufs?: number;
  /** True-peak ceiling in dBFS. */
  ceilingDbtp?: number;
  /** Let the suggestion enable repair stages. */
  enableRepair?: boolean;
  /** Prefer settings that survive streaming-service normalization. */
  preferStreamingSafe?: boolean;
  /** Blend factor in `[0, 1]` for the speech mono fold. */
  speechMonoAmount?: number;
  /** Delivery target the loudness and ceiling follow. */
  targetPlatform?: MasteringPlatform;
  /** Mastering preset the suggestion starts from; restoration presets are refused. */
  preset?: MasteringPreset;
}

/** Request form of {@link masteringAssistantSuggest} and {@link masteringAssistantSuggestChain}. */
export interface MasteringAssistantSuggestRequest extends MasteringAssistantSettings {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}

/** Request form of {@link masteringAssistantSuggestStereo} and {@link masteringAssistantSuggestChainStereo}. */
export interface MasteringAssistantSuggestStereoRequest extends MasteringAssistantSettings {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}

/** Request form of {@link masteringStreamingPreview}. */
export interface MasteringStreamingPreviewRequest {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Delivery targets to preview; absent: the built-in set. */
  platforms?: StreamingPlatform[];
}

/** Request form of {@link masteringStreamingPreviewStereo}. */
export interface MasteringStreamingPreviewStereoRequest {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Streaming platforms to preview. */
  platforms?: StreamingPlatform[];
}

function assistantResultFromNative(json: string): MasteringAssistantResult {
  const native = parseRecordJson(json) as {
    chainConfig: { params: Record<string, unknown> };
    explanation: string[];
    profile: MasteringAssistantResult['profile'];
  };
  return {
    chainConfig: chainConfigFromDocument(native.chainConfig),
    explanation: native.explanation,
    profile: native.profile,
  };
}

function previewFromNative(json: string): MasteringStreamingPreviewResult {
  return parseRecordJson(json) as MasteringStreamingPreviewResult;
}

// `defects` is absent when the detectors did not run.
function profileFromNative(json: string): MasteringAudioProfile {
  const profile = parseRecordJson(json) as MasteringAudioProfile;
  if (profile.defects !== undefined) {
    profile.defects = audioDefectsFromNative(profile.defects);
  }
  return profile;
}

/** Native params of an audio-profile request; the profile always runs the defect detectors unless told not to. */
function profileParams(
  fnName: string,
  r: Pick<
    MasteringAudioProfileRequest,
    'nFft' | 'hopLength' | 'truePeakOversample' | 'detectDefects'
  >,
): Record<string, number | boolean> {
  const params: Record<string, number | boolean> = {};
  for (const key of ['nFft', 'hopLength', 'truePeakOversample'] as const) {
    const value = r[key];
    if (value !== undefined && value !== null) {
      assertPositiveInteger(fnName, value, key);
      params[key] = value;
    }
  }
  params.detectDefects = r.detectDefects ?? true;
  return params;
}

/** Native params of an assistant request, with the two name-valued fields checked against the library's tables. */
function assistantParams(
  fnName: string,
  r: MasteringAssistantSettings,
): Record<string, number | boolean | string> {
  const params: Record<string, number | boolean | string> = {};
  for (const key of ['targetLufs', 'ceilingDbtp'] as const) {
    if (r[key] !== undefined && r[key] !== null) {
      assertFloatValue(fnName, r[key], key);
      params[key] = r[key];
    }
  }
  if (r.speechMonoAmount !== undefined && r.speechMonoAmount !== null) {
    assertFloatValue(fnName, r.speechMonoAmount, 'speechMonoAmount');
    params.speechMonoAmount = r.speechMonoAmount;
  }
  for (const key of ['enableRepair', 'preferStreamingSafe'] as const) {
    if (r[key] !== undefined && r[key] !== null) {
      params[key] = r[key] as boolean;
    }
  }
  if (r.targetPlatform !== undefined && r.targetPlatform !== null) {
    assertClosedToken(fnName, r.targetPlatform, masteringPlatformNames(), 'targetPlatform');
    params.targetPlatform = r.targetPlatform;
  }
  if (r.preset !== undefined && r.preset !== null) {
    assertClosedToken(fnName, r.preset, masteringPresetNames(), 'preset');
    params.preset = r.preset;
  }
  return params;
}

/**
 * Suggest a mastering chain for mono audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz, an integer in `[8000, 384000]`
 * @param options - Assistant settings ({@link MasteringAssistantSuggestRequest})
 * @returns The suggested chain, one explanation line per decision and the measured profile
 * @throws {TypeError} when `sampleRate` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain, or an unknown `targetPlatform` or `preset`
 */
export function masteringAssistantSuggest(
  request: MasteringAssistantSuggestRequest,
): MasteringAssistantResult;
export function masteringAssistantSuggest(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MasteringAssistantSuggestRequest>,
): MasteringAssistantResult;
export function masteringAssistantSuggest(...args: unknown[]): MasteringAssistantResult {
  const r = sampleOneShot<MasteringAssistantSuggestRequest>('masteringAssistantSuggest', args);
  assertAudioInput('masteringAssistantSuggest', r.samples, r.sampleRate, r);
  return assistantResultFromNative(
    addon.masteringAssistantSuggest(
      r.samples,
      r.sampleRate,
      assistantParams('masteringAssistantSuggest', r),
    ),
  );
}

/**
 * Suggest a mastering chain and return it alone, as the nested
 * {@link MasteringChainConfig} {@link masteringAssistantSuggest} carries as
 * `chainConfig`. Pass it as `overrides` to {@link masterAudio} unchanged.
 *
 * @throws {TypeError} when `sampleRate` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain, or an unknown `targetPlatform` or `preset`
 */
export function masteringAssistantSuggestChain(
  request: MasteringAssistantSuggestRequest,
): MasteringChainConfig;
export function masteringAssistantSuggestChain(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MasteringAssistantSuggestRequest>,
): MasteringChainConfig;
export function masteringAssistantSuggestChain(...args: unknown[]): MasteringChainConfig {
  const r = sampleOneShot<MasteringAssistantSuggestRequest>('masteringAssistantSuggestChain', args);
  assertAudioInput('masteringAssistantSuggestChain', r.samples, r.sampleRate, r);
  return chainConfigFromJson(
    addon.masteringAssistantSuggestChain(
      r.samples,
      r.sampleRate,
      assistantParams('masteringAssistantSuggestChain', r),
    ),
  );
}

/**
 * Measure the mastering profile of mono audio: loudness, spectral and dynamics
 * groups, and — unless `detectDefects` is false — the repair detectors' readings
 * under `defects`.
 *
 * @throws {TypeError} when `sampleRate` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain or a non-positive `nFft`, `hopLength` or
 *   `truePeakOversample`
 */
export function masteringAudioProfile(request: MasteringAudioProfileRequest): MasteringAudioProfile;
export function masteringAudioProfile(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MasteringAudioProfileRequest>,
): MasteringAudioProfile;
export function masteringAudioProfile(...args: unknown[]): MasteringAudioProfile {
  const r = sampleOneShot<MasteringAudioProfileRequest>('masteringAudioProfile', args);
  assertAudioInput('masteringAudioProfile', r.samples, r.sampleRate, r);
  return profileFromNative(
    addon.masteringAudioProfile(r.samples, r.sampleRate, profileParams('masteringAudioProfile', r)),
  );
}

/**
 * Preview streaming-platform normalization of mono audio.
 *
 * @throws {TypeError} when `sampleRate` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain
 */
export function masteringStreamingPreview(
  request: MasteringStreamingPreviewRequest,
): MasteringStreamingPreviewResult;
export function masteringStreamingPreview(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<MasteringStreamingPreviewRequest>,
): MasteringStreamingPreviewResult;
export function masteringStreamingPreview(...args: unknown[]): MasteringStreamingPreviewResult {
  const r = sampleOneShot<MasteringStreamingPreviewRequest>('masteringStreamingPreview', args);
  assertAudioInput('masteringStreamingPreview', r.samples, r.sampleRate, r);
  return previewFromNative(
    addon.masteringStreamingPreview(r.samples, r.sampleRate, r.platforms ?? []),
  );
}

/**
 * Suggest a mastering chain for a stereo pair. Takes a request object only.
 *
 * Profiles through {@link masteringAudioProfileStereo}, so the loudness stage
 * of the suggestion is built on the channel-summed program rather than a
 * downmix that reads roughly 6 dB low.
 *
 * @throws {TypeError} when `sampleRate` is missing or a field has the wrong type
 * @throws {RangeError} on audio out of domain, or an unknown `targetPlatform` or `preset`
 */
export function masteringAssistantSuggestStereo(
  request: MasteringAssistantSuggestStereoRequest,
): MasteringAssistantResult {
  const r = requestOnly<MasteringAssistantSuggestStereoRequest>('masteringAssistantSuggestStereo', [
    request,
  ]);
  assertAudioInput('masteringAssistantSuggestStereo', r.left, r.sampleRate, r, 'left');
  assertAudioInput('masteringAssistantSuggestStereo', r.right, r.sampleRate, r, 'right');
  return assistantResultFromNative(
    addon.masteringAssistantSuggestStereo(
      r.left,
      r.right,
      r.sampleRate,
      assistantParams('masteringAssistantSuggestStereo', r),
    ),
  );
}

/**
 * Stereo counterpart of {@link masteringAssistantSuggestChain}: the nested
 * {@link MasteringChainConfig} alone, ready to pass as `overrides` to
 * {@link masterAudioStereo}.
 */
export function masteringAssistantSuggestChainStereo(
  request: MasteringAssistantSuggestStereoRequest,
): MasteringChainConfig {
  const r = requestOnly<MasteringAssistantSuggestStereoRequest>(
    'masteringAssistantSuggestChainStereo',
    [request],
  );
  assertAudioInput('masteringAssistantSuggestChainStereo', r.left, r.sampleRate, r, 'left');
  assertAudioInput('masteringAssistantSuggestChainStereo', r.right, r.sampleRate, r, 'right');
  return chainConfigFromJson(
    addon.masteringAssistantSuggestChainStereo(
      r.left,
      r.right,
      r.sampleRate,
      assistantParams('masteringAssistantSuggestChainStereo', r),
    ),
  );
}

/**
 * Mastering profile of a stereo pair.
 *
 * The `loudness` group is measured from the two channels: integrated LUFS and
 * loudness range come from the channel-summed program and the true peak is the
 * larger of the two. The spectral, dynamics and tempo fields describe shape and
 * timing rather than absolute level and are measured on the downmix, which keeps
 * them comparable with {@link masteringAudioProfile}. Defect detectors run on
 * each channel and their results are aggregated.
 */
export function masteringAudioProfileStereo(
  request: MasteringAudioProfileStereoRequest,
): MasteringAudioProfile {
  const r = requestOnly<MasteringAudioProfileStereoRequest>('masteringAudioProfileStereo', [
    request,
  ]);
  assertAudioInput('masteringAudioProfileStereo', r.left, r.sampleRate, r, 'left');
  assertAudioInput('masteringAudioProfileStereo', r.right, r.sampleRate, r, 'right');
  return profileFromNative(
    addon.masteringAudioProfileStereo(
      r.left,
      r.right,
      r.sampleRate,
      profileParams('masteringAudioProfileStereo', r),
    ),
  );
}

/**
 * Preview streaming-platform normalization for a stereo pair.
 *
 * Measures the integrated loudness with BS.1770 channel summing and reports the
 * larger of the two channel true peaks. Passing a `0.5 * (left + right)` downmix
 * to {@link masteringStreamingPreview} instead reads roughly 6 dB low on
 * decorrelated material, and both the normalization gain and the ceiling-risk
 * flag follow from that measurement.
 */
export function masteringStreamingPreviewStereo(
  request: MasteringStreamingPreviewStereoRequest,
): MasteringStreamingPreviewResult {
  const r = requestOnly<MasteringStreamingPreviewStereoRequest>('masteringStreamingPreviewStereo', [
    request,
  ]);
  assertAudioInput('masteringStreamingPreviewStereo', r.left, r.sampleRate, r, 'left');
  assertAudioInput('masteringStreamingPreviewStereo', r.right, r.sampleRate, r, 'right');
  return previewFromNative(
    addon.masteringStreamingPreviewStereo(r.left, r.right, r.sampleRate, r.platforms ?? []),
  );
}
