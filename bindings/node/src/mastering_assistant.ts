import { chainConfigFromDocument } from './internal/chain_config.js';
import { refuse, refuseType, refuseUnknownKey, withEntry } from './internal/entry.js';
import { requireListBudget } from './internal/list_budget.js';
import { parseRecordJson } from './internal/record_json.js';
import { audioDefectsFromNative } from './internal/repair_common.js';
import { isNonArrayObject } from './internal/value_guards.js';
import { masteringPlatformNames } from './mastering_catalog.js';
import { addon } from './native.js';
import type {
  CapabilityCatalog,
  MasteringAssistantResult,
  MasteringAudioProfile,
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

/** The presets the assistant starts from: every mastering preset, no restoration one. */
function assistantPresetNames(): MasteringPreset[] {
  const catalog = parseRecordJson(addon.capabilityCatalog()) as CapabilityCatalog;
  return catalog.masteringPresets
    .filter((entry) => entry.kind === 'mastering')
    .map((entry) => entry.name as MasteringPreset);
}

/** Request form of {@link masteringAssistantSuggest}. */
export interface MasteringAssistantSuggestRequest extends MasteringAssistantSettings {
  /** Mono input samples. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
}

/** Request form of {@link masteringAssistantSuggestStereo}. */
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

/** The fields of a streaming platform record. */
const PLATFORM_KEYS = ['name', 'targetLufs', 'ceilingDbtp'];

/** The preview targets in the native spelling: absent is the built-in set, an empty list is refused. */
function previewPlatforms(platforms: unknown): StreamingPlatform[] {
  if (platforms === undefined || platforms === null) {
    return [];
  }
  if (!Array.isArray(platforms)) {
    throw refuseType(`platforms must be an array`);
  }
  requireListBudget(platforms, 'platforms');
  if (platforms.length === 0) {
    throw refuse(`platforms must not be empty; omit it for the built-in set`);
  }
  platforms.forEach((platform, i) => {
    if (!isNonArrayObject(platform)) {
      throw refuseType(`platforms[${i}] must be an object`);
    }
    for (const key of Object.keys(platform)) {
      if (!PLATFORM_KEYS.includes(key)) {
        throw refuseUnknownKey(`platforms[${i}].${key}`, PLATFORM_KEYS);
      }
    }
    const { name, targetLufs, ceilingDbtp } = platform as Record<string, unknown>;
    if (typeof name !== 'string') {
      throw refuseType(`platforms[${i}].name must be a string`);
    }
    if (typeof targetLufs !== 'number') {
      throw refuseType(`platforms[${i}].targetLufs must be a number`);
    }
    if (typeof ceilingDbtp !== 'number') {
      throw refuseType(`platforms[${i}].ceilingDbtp must be a number`);
    }
  });
  return platforms as StreamingPlatform[];
}

/** Request form of {@link masteringStreamingPreviewStereo}. */
export interface MasteringStreamingPreviewStereoRequest {
  /** Left channel samples. */
  left: Float32Array;
  /** Right channel samples. */
  right: Float32Array;
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Delivery targets to preview; absent: the built-in set. */
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
  r: Pick<
    MasteringAudioProfileRequest,
    'nFft' | 'hopLength' | 'truePeakOversample' | 'detectDefects'
  >,
): Record<string, number | boolean> {
  const params: Record<string, number | boolean> = {};
  for (const key of ['nFft', 'hopLength', 'truePeakOversample'] as const) {
    const value = r[key];
    if (value !== undefined && value !== null) {
      params[key] = value;
    }
  }
  params.detectDefects = r.detectDefects ?? true;
  return params;
}

/** Native params of an assistant request, with the two name-valued fields checked against the library's tables. */
function assistantParams(r: MasteringAssistantSettings): Record<string, number | boolean | string> {
  const params: Record<string, number | boolean | string> = {};
  for (const key of ['targetLufs', 'ceilingDbtp'] as const) {
    if (r[key] !== undefined && r[key] !== null) {
      assertFloatValue(r[key], key);
      params[key] = r[key];
    }
  }
  if (r.speechMonoAmount !== undefined && r.speechMonoAmount !== null) {
    assertFloatValue(r.speechMonoAmount, 'speechMonoAmount');
    params.speechMonoAmount = r.speechMonoAmount;
  }
  for (const key of ['enableRepair', 'preferStreamingSafe'] as const) {
    if (r[key] !== undefined && r[key] !== null) {
      params[key] = r[key] as boolean;
    }
  }
  if (r.targetPlatform !== undefined && r.targetPlatform !== null) {
    assertClosedToken(r.targetPlatform, masteringPlatformNames(), 'targetPlatform');
    params.targetPlatform = r.targetPlatform;
  }
  if (r.preset !== undefined && r.preset !== null) {
    assertClosedToken(r.preset, assistantPresetNames(), 'preset');
    params.preset = r.preset;
  }
  return params;
}

/**
 * Suggest a mastering chain for mono audio.
 *
 * @param request - Mono `samples`, `sampleRate` in Hz, an integer in `[8000, 384000]`, and the
 *   assistant settings; or pass `(samples, sampleRate, options?)`.
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
  return withEntry('masteringAssistantSuggest', () => {
    const r = sampleOneShot<MasteringAssistantSuggestRequest>('masteringAssistantSuggest', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return assistantResultFromNative(
      addon.masteringAssistantSuggest(
        r.samples,
        r.sampleRate,
        withEntry('masteringAssistantSuggest', () => assistantParams(r)),
      ),
    );
  });
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
  return withEntry('masteringAudioProfile', () => {
    const r = sampleOneShot<MasteringAudioProfileRequest>('masteringAudioProfile', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return profileFromNative(
      addon.masteringAudioProfile(r.samples, r.sampleRate, profileParams(r)),
    );
  });
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
  return withEntry('masteringStreamingPreview', () => {
    const r = sampleOneShot<MasteringStreamingPreviewRequest>('masteringStreamingPreview', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    return previewFromNative(
      addon.masteringStreamingPreview(r.samples, r.sampleRate, previewPlatforms(r.platforms)),
    );
  });
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
  return withEntry('masteringAssistantSuggestStereo', () => {
    const r = requestOnly<MasteringAssistantSuggestStereoRequest>(
      'masteringAssistantSuggestStereo',
      [request],
    );
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    return assistantResultFromNative(
      addon.masteringAssistantSuggestStereo(
        r.left,
        r.right,
        r.sampleRate,
        withEntry('masteringAssistantSuggestStereo', () => assistantParams(r)),
      ),
    );
  });
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
  return withEntry('masteringAudioProfileStereo', () => {
    const r = requestOnly<MasteringAudioProfileStereoRequest>('masteringAudioProfileStereo', [
      request,
    ]);
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    return profileFromNative(
      addon.masteringAudioProfileStereo(r.left, r.right, r.sampleRate, profileParams(r)),
    );
  });
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
  return withEntry('masteringStreamingPreviewStereo', () => {
    const r = requestOnly<MasteringStreamingPreviewStereoRequest>(
      'masteringStreamingPreviewStereo',
      [request],
    );
    assertAudioInput(r.left, r.sampleRate, r, 'left');
    assertAudioInput(r.right, r.sampleRate, r, 'right');
    return previewFromNative(
      addon.masteringStreamingPreviewStereo(
        r.left,
        r.right,
        r.sampleRate,
        previewPlatforms(r.platforms),
      ),
    );
  });
}
