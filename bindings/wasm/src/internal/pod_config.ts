import { getSonareModule } from '../module_state.js';
import type {
  RealtimeVoiceChangerConfig,
  RealtimeVoiceChangerConfigInput,
} from '../public_types.js';
import type { WasmRealtimeVoiceChangerPodConfig } from '../sonare.js';
import { assertRetuneGrain } from '../validation.js';
import { refuse, refuseType } from './entry.js';
import { checkedVoicePresetId, voicePresetOrdinal } from './voice_preset_ids.js';

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/** True for the flat config object (as opposed to a nested preset document). */
function isFlatConfig(config: unknown): config is Record<string, unknown> {
  return isRecord(config) && 'retunePitchSemitones' in config;
}

let flatReference: RealtimeVoiceChangerConfig | undefined;

/** Every required flat-config field with a value of its type; they do not vary by preset. */
function flatConfigReference(): RealtimeVoiceChangerConfig {
  flatReference ??= getSonareModule().realtimeVoiceChangerPresetConfig(
    voicePresetOrdinal('neutralMonitor'),
  );
  return flatReference;
}

/**
 * Refuses a flat config missing a field or carrying one of the wrong JS type, naming the public
 * field before native sees it.
 */
function assertFlatConfigFields(config: Record<string, unknown>): void {
  for (const [key, sample] of Object.entries(flatConfigReference())) {
    const value = config[key];
    if (key === 'retuneGrainSizeSamples') {
      continue;
    }
    if (key !== 'harmonySteps' && typeof value !== typeof sample) {
      throw refuseType(`${key} must be a ${typeof sample}`);
    }
  }
}

/**
 * The one optional flat field: an omitted (`undefined` or `null`) retune grain is the
 * automatic grain and leaves the key out; a given one is a multiple of 4 in [4, 8192].
 */
function withRetuneGrain(config: Record<string, unknown>): Record<string, unknown> {
  const grain = config.retuneGrainSizeSamples;
  if (grain === undefined || grain === null) {
    const { retuneGrainSizeSamples: _omitted, ...rest } = config;
    return rest;
  }
  assertRetuneGrain(grain, 'retuneGrainSizeSamples');
  return config;
}

/**
 * The flat config as native reads it. Every field but the retune grain is required, as in the
 * native parser.
 *
 * @throws TypeError for a missing or wrong-typed field; RangeError for a `harmonySteps` of
 *   another length or a retune grain that is not a multiple of 4 in [4, 8192].
 */
export function podConfigToNative(
  input: RealtimeVoiceChangerConfig,
): WasmRealtimeVoiceChangerPodConfig {
  const config = withRetuneGrain(input as unknown as Record<string, unknown>);
  assertFlatConfigFields(config);
  const steps = config.harmonySteps as unknown;
  if (!Array.isArray(steps)) {
    throw refuseType(`harmonySteps must be an array of three numbers`);
  }
  if (steps.length !== 3) {
    throw refuse(`harmonySteps must hold exactly three numbers`);
  }
  return config as unknown as WasmRealtimeVoiceChangerPodConfig;
}

/**
 * A voice-changer config in the form native reads: a preset id is checked, a flat config is
 * validated, and a nested preset document passes through.
 *
 * @throws TypeError for any other type; RangeError for an unknown preset id.
 */
export function nativeVoiceConfig(
  config: RealtimeVoiceChangerConfigInput,
): Record<string, unknown> | string {
  if (typeof config === 'string') {
    return checkedVoicePresetId(config, 'config');
  }
  if (isFlatConfig(config)) {
    return podConfigToNative(config as unknown as RealtimeVoiceChangerConfig) as unknown as Record<
      string,
      unknown
    >;
  }
  if (!isRecord(config)) {
    throw refuseType(`config must be a preset id or an object`);
  }
  return config as unknown as Record<string, unknown>;
}

/** Interleaves equal-length planes (L0,R0,L1,R1,... for two) for the native interleaved calls. */
export function interleavePlanes(planes: readonly Float32Array[]): Float32Array {
  const frames = planes[0]?.length ?? 0;
  const out = new Float32Array(frames * planes.length);
  planes.forEach((plane, ch) => {
    for (let i = 0; i < frames; i++) {
      out[i * planes.length + ch] = plane[i] as number;
    }
  });
  return out;
}

/** Inverse of {@link interleavePlanes}. */
export function deinterleavePlanes(data: Float32Array, numChannels: number): Float32Array[] {
  const frames = data.length / numChannels;
  const planes = Array.from({ length: numChannels }, () => new Float32Array(frames));
  for (let ch = 0; ch < numChannels; ch++) {
    const plane = planes[ch] as Float32Array;
    for (let i = 0; i < frames; i++) {
      plane[i] = data[i * numChannels + ch] as number;
    }
  }
  return planes;
}
