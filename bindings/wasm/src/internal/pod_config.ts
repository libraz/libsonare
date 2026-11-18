import { getSonareModule } from '../module_state.js';
import type {
  RealtimeVoiceChangerConfig,
  RealtimeVoiceChangerConfigInput,
} from '../public_types.js';
import type { WasmRealtimeVoiceChangerPodConfig } from '../sonare.js';
import { checkedVoicePresetId, voicePresetOrdinal } from './voice_preset_ids.js';

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/** True for the flat config object (as opposed to a nested preset document). */
function isFlatConfig(config: unknown): config is Record<string, unknown> {
  return isRecord(config) && 'retunePitchSemitones' in config;
}

let flatReference: RealtimeVoiceChangerConfig | undefined;

/** Every flat-config field with a value of its type; the fields do not vary by preset. */
function flatConfigReference(): RealtimeVoiceChangerConfig {
  flatReference ??= getSonareModule().realtimeVoiceChangerPresetConfig(
    voicePresetOrdinal('flatConfigReference', 'neutralMonitor'),
  );
  return flatReference;
}

/**
 * Refuses a flat config missing a field or carrying one of the wrong JS type, naming the public
 * field before native sees it.
 */
function assertFlatConfigFields(fnName: string, config: Record<string, unknown>): void {
  for (const [key, sample] of Object.entries(flatConfigReference())) {
    const value = config[key];
    if (key !== 'harmonySteps' && typeof value !== typeof sample) {
      throw new TypeError(`${fnName}: ${key} must be a ${typeof sample}`);
    }
  }
}

/**
 * The flat config as native reads it. Every field is required, as in the native parser.
 *
 * @throws TypeError for a missing or wrong-typed field; RangeError for a `harmonySteps` of
 *   another length.
 */
export function podConfigToNative(
  config: RealtimeVoiceChangerConfig,
  fnName = 'RealtimeVoiceChanger.setConfig',
): WasmRealtimeVoiceChangerPodConfig {
  assertFlatConfigFields(fnName, config as unknown as Record<string, unknown>);
  const steps = config.harmonySteps as unknown;
  if (!Array.isArray(steps)) {
    throw new TypeError(`${fnName}: harmonySteps must be an array of three numbers`);
  }
  if (steps.length !== 3) {
    throw new RangeError(`${fnName}: harmonySteps must hold exactly three numbers`);
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
  fnName: string,
  config: RealtimeVoiceChangerConfigInput,
): Record<string, unknown> | string {
  if (typeof config === 'string') {
    return checkedVoicePresetId(fnName, config, 'config');
  }
  if (isFlatConfig(config)) {
    return podConfigToNative(
      config as unknown as RealtimeVoiceChangerConfig,
      fnName,
    ) as unknown as Record<string, unknown>;
  }
  if (!isRecord(config)) {
    throw new TypeError(`${fnName}: config must be a preset id or an object`);
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
