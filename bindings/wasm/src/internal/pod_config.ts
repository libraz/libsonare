import { getSonareModule } from '../module_state.js';
import type {
  RealtimeVoiceChangerConfig,
  RealtimeVoiceChangerConfigInput,
} from '../public_types.js';
import type { WasmRealtimeVoiceChangerPodConfig } from '../sonare.js';
import { voicePresetIdToNative, voicePresetOrdinal } from './voice_preset_ids.js';

// Native still spells these fields the old way; the map runs in both directions.
const NATIVE_KEY: Readonly<Record<string, string>> = {
  voiceEngine: 'voiceEngineEngine',
  reverbVariation: 'reverbVariationVariation',
  eqHighPassHz: 'eqHighpassHz',
  retunePitchSemitones: 'retuneSemitones',
  retuneGrainSizeSamples: 'retuneGrainSize',
};

const PUBLIC_KEY: Readonly<Record<string, string>> = Object.fromEntries(
  Object.entries(NATIVE_KEY).map(([publicKey, nativeKey]) => [nativeKey, publicKey]),
);

// Native-only spellings: a config carrying one is refused, not forwarded.
const RETIRED_KEYS: ReadonlySet<string> = new Set([
  ...Object.values(NATIVE_KEY),
  'harmonyStep1',
  'harmonyStep2',
  'harmonyStep3',
]);

// Native still spells these tokens in snake case; keyed by the public field name.
const NATIVE_TOKENS: Readonly<Record<string, Readonly<Record<string, string>>>> = {
  pitchCorrectionScale: { major: 'major', naturalMinor: 'natural_minor', chromatic: 'chromatic' },
  characterType: {
    none: 'none',
    megaphone: 'megaphone',
    radio: 'radio',
    bbdChorus: 'bbd_chorus',
    strobo: 'strobo',
  },
  reverbVariation: {
    reverb: 'reverb',
    echo: 'echo',
    tempoDelay: 'tempo_delay',
    dubEcho: 'dub_echo',
  },
  reverbVariationDivision: {
    quarter: 'quarter',
    eighth: 'eighth',
    dottedEighth: 'dotted_eighth',
    sixteenth: 'sixteenth',
  },
};

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function hasOwn(table: object, key: string): boolean {
  // biome-ignore lint/suspicious/noPrototypeBuiltins: Object.hasOwn is newer than the ES2020 target.
  return Object.prototype.hasOwnProperty.call(table, key);
}

/** True for the flat config object (as opposed to a nested preset document). */
function isFlatConfig(config: unknown): config is Record<string, unknown> {
  return isRecord(config) && ('retunePitchSemitones' in config || 'retuneSemitones' in config);
}

function mapToken(fnName: string, field: string, value: unknown, native: boolean): string {
  const publicTable = NATIVE_TOKENS[field] as Readonly<Record<string, string>>;
  const table = native
    ? publicTable
    : (Object.fromEntries(Object.entries(publicTable).map(([k, v]) => [v, k])) as Record<
        string,
        string
      >);
  if (typeof value !== 'string') {
    throw new TypeError(`${fnName}: ${field} must be a string`);
  }
  if (!hasOwn(table, value)) {
    throw new RangeError(
      `${fnName}: ${field} must be one of ${Object.keys(publicTable).join(', ')}`,
    );
  }
  return table[value] as string;
}

let flatReference: RealtimeVoiceChangerConfig | undefined;

/** Every flat-config field with a value of its type; the fields do not vary by preset. */
function flatConfigReference(): RealtimeVoiceChangerConfig {
  flatReference ??= podConfigFromNative(
    getSonareModule().realtimeVoiceChangerPresetConfig(
      voicePresetOrdinal('flatConfigReference', 'neutralMonitor'),
    ),
  );
  return flatReference;
}

/**
 * Refuses a flat config missing a field or carrying one of the wrong JS type, naming the public
 * field before native sees it: native reports its own spelling of the key.
 */
function assertFlatConfigFields(fnName: string, config: Record<string, unknown>): void {
  for (const [key, sample] of Object.entries(flatConfigReference())) {
    const value = config[key];
    if (value === undefined) {
      throw new TypeError(`${fnName}: ${key} is required`);
    }
    if (key !== 'harmonySteps' && typeof value !== typeof sample) {
      throw new TypeError(`${fnName}: ${key} must be a ${typeof sample}`);
    }
  }
}

/**
 * The flat config spelt the way native reads it. Every field is required, as in the native
 * parser.
 *
 * @throws TypeError for a missing or wrong-typed field or a
 *   native-only key; RangeError for an unknown token or a `harmonySteps` of another length.
 */
export function podConfigToNative(
  config: RealtimeVoiceChangerConfig,
  fnName = 'RealtimeVoiceChanger.setConfig',
): WasmRealtimeVoiceChangerPodConfig {
  assertFlatConfigFields(fnName, config as unknown as Record<string, unknown>);
  const out: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(config)) {
    if (RETIRED_KEYS.has(key)) {
      throw new TypeError(`${fnName}: unknown config key '${key}'`);
    }
    if (key === 'harmonySteps' || value === undefined) {
      continue;
    }
    out[NATIVE_KEY[key] ?? key] = hasOwn(NATIVE_TOKENS, key)
      ? mapToken(fnName, key, value, true)
      : value;
  }
  const steps = (config as { harmonySteps?: unknown }).harmonySteps;
  if (steps === undefined) {
    throw new TypeError(`${fnName}: harmonySteps is required`);
  }
  if (!Array.isArray(steps)) {
    throw new TypeError(`${fnName}: harmonySteps must be an array of three numbers`);
  }
  if (steps.length !== 3) {
    throw new RangeError(`${fnName}: harmonySteps must hold exactly three numbers`);
  }
  out.harmonyStep1 = steps[0];
  out.harmonyStep2 = steps[1];
  out.harmonyStep3 = steps[2];
  return out as unknown as WasmRealtimeVoiceChangerPodConfig;
}

/** Inverse of {@link podConfigToNative}. */
export function podConfigFromNative(
  config: WasmRealtimeVoiceChangerPodConfig,
): RealtimeVoiceChangerConfig {
  const out: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(config)) {
    if (/^harmonyStep[123]$/.test(key)) {
      continue;
    }
    const field = PUBLIC_KEY[key] ?? key;
    out[field] = hasOwn(NATIVE_TOKENS, field)
      ? mapToken('realtimeVoiceChangerPresetConfig', field, value, false)
      : value;
  }
  out.harmonySteps = [config.harmonyStep1, config.harmonyStep2, config.harmonyStep3];
  return out as unknown as RealtimeVoiceChangerConfig;
}

/**
 * A voice-changer config in the form native reads: a preset id becomes its native id, the flat
 * config its native spelling, and a nested preset document passes through.
 *
 * @throws TypeError for any other type; RangeError for an unknown preset id.
 */
export function nativeVoiceConfig(
  fnName: string,
  config: RealtimeVoiceChangerConfigInput,
): Record<string, unknown> | string {
  if (typeof config === 'string') {
    return voicePresetIdToNative(fnName, config, 'config');
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
