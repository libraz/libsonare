import type { MidiFxConfig } from '../types_engine.js';

/** Config key to the snake_case key the native reader takes. */
const NUMBER_KEYS = {
  transposeSemitones: 'transpose_semitones',
  velocityScale: 'velocity_scale',
  velocityOffset: 'velocity_offset',
  velocityGamma: 'velocity_gamma',
  quantizePpq: 'quantize_ppq',
  quantizeStrength: 'quantize_strength',
  arpeggiatorStepPpq: 'arpeggiator_step_ppq',
  arpeggiatorGatePpq: 'arpeggiator_gate_ppq',
  humanizePpq: 'humanize_ppq',
  humanizeVelocity: 'humanize_velocity',
  seed: 'seed',
} as const;

const ARRAY_KEYS = {
  chordIntervals: 'chord_intervals',
  arpeggiatorIntervals: 'arpeggiator_intervals',
} as const;

function finiteNumber(fnName: string, value: unknown, name: string): number {
  if (typeof value !== 'number') {
    throw new TypeError(`${fnName}: ${name} must be a number`);
  }
  if (!Number.isFinite(value)) {
    throw new RangeError(`${fnName}: ${name} must be finite`);
  }
  return value;
}

/**
 * Serialize a {@link MidiFxConfig} into the snake_case JSON the native MIDI-FX
 * reader takes. `undefined` / `null` fields are absent; ranges are checked by
 * the native reader.
 *
 * @throws TypeError when `config` is not an object, carries an unknown key, or a field has the wrong type
 * @throws RangeError when a number is not finite
 */
export function midiFxConfigJson(config: MidiFxConfig, fnName: string): string {
  if (typeof config !== 'object' || config === null || Array.isArray(config)) {
    throw new TypeError(`${fnName}: config must be an object`);
  }
  const record = config as Record<string, unknown>;
  const out: Record<string, number | number[]> = {};
  for (const key of Object.keys(record)) {
    const value = record[key];
    if (Object.hasOwn(NUMBER_KEYS, key)) {
      if (value !== undefined && value !== null) {
        out[NUMBER_KEYS[key as keyof typeof NUMBER_KEYS]] = finiteNumber(fnName, value, key);
      }
    } else if (Object.hasOwn(ARRAY_KEYS, key)) {
      if (value === undefined || value === null) {
        continue;
      }
      if (!Array.isArray(value)) {
        throw new TypeError(`${fnName}: ${key} must be an array of numbers`);
      }
      out[ARRAY_KEYS[key as keyof typeof ARRAY_KEYS]] = value.map((item, i) =>
        finiteNumber(fnName, item, `${key}[${i}]`),
      );
    } else {
      throw new TypeError(`${fnName}: unknown config key '${key}'`);
    }
  }
  return JSON.stringify(out);
}
