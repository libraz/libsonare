import type { MidiFxConfig } from '../public_types_engine.js';

const NUMBER_FIELDS = [
  ['transposeSemitones', 'transpose_semitones'],
  ['velocityScale', 'velocity_scale'],
  ['velocityOffset', 'velocity_offset'],
  ['velocityGamma', 'velocity_gamma'],
  ['quantizePpq', 'quantize_ppq'],
  ['quantizeStrength', 'quantize_strength'],
  ['arpeggiatorStepPpq', 'arpeggiator_step_ppq'],
  ['arpeggiatorGatePpq', 'arpeggiator_gate_ppq'],
  ['humanizePpq', 'humanize_ppq'],
  ['humanizeVelocity', 'humanize_velocity'],
  ['seed', 'seed'],
] as const;

const ARRAY_FIELDS = [
  ['chordIntervals', 'chord_intervals'],
  ['arpeggiatorIntervals', 'arpeggiator_intervals'],
] as const;

/**
 * Serialize a {@link MidiFxConfig} to the snake_case JSON the native MIDI-FX
 * reader takes. A non-object config is a `TypeError`, as is a wrong-typed
 * field and an unknown key; an absent (`undefined`/`null`) field is omitted.
 */
export function midiFxConfigJson(config: MidiFxConfig, fnName: string): string {
  if (typeof config !== 'object' || config === null || Array.isArray(config)) {
    throw new TypeError(`${fnName}: config must be an object`);
  }
  const source = config as Record<string, unknown>;
  const known = new Set<string>([...NUMBER_FIELDS, ...ARRAY_FIELDS].map(([key]) => key));
  for (const key of Object.keys(source)) {
    if (!known.has(key)) {
      throw new TypeError(`${fnName}: unknown config key '${key}'`);
    }
  }
  const out: Record<string, number | number[]> = {};
  for (const [key, wire] of NUMBER_FIELDS) {
    const value = source[key];
    if (value === undefined || value === null) {
      continue;
    }
    if (typeof value !== 'number') {
      throw new TypeError(`${fnName}: config.${key} must be a number`);
    }
    if (!Number.isFinite(value)) {
      throw new RangeError(`${fnName}: config.${key} must be a finite number`);
    }
    out[wire] = value;
  }
  for (const [key, wire] of ARRAY_FIELDS) {
    const value = source[key];
    if (value === undefined || value === null) {
      continue;
    }
    if (!Array.isArray(value) || value.some((item) => typeof item !== 'number')) {
      throw new TypeError(`${fnName}: config.${key} must be an array of numbers`);
    }
    out[wire] = value as number[];
  }
  return JSON.stringify(out);
}
