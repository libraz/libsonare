import type { ProjectMidiEvent } from './types.js';

/**
 * Per-call validation options accepted by guarded wrappers. Empty-buffer
 * checks are always performed; pass `{ validate: false }` to opt out of the
 * O(n) NaN/Inf scan on hot paths where the caller already controls the data.
 */
export interface ValidateOptions {
  validate?: boolean;
}

export function assertNonEmptySamples(
  fnName: string,
  samples: ArrayLike<number>,
  argName = 'samples',
): void {
  if (samples.length === 0) {
    throw new RangeError(`${fnName}: ${argName} must not be empty`);
  }
}

export function assertFiniteSamples(
  fnName: string,
  samples: ArrayLike<number>,
  validate: boolean,
  argName = 'samples',
): void {
  if (!validate) {
    return;
  }
  for (let i = 0; i < samples.length; i++) {
    const v = samples[i] as number;
    if (!Number.isFinite(v)) {
      throw new RangeError(`${fnName}: ${argName} contains NaN or Inf at index ${i}`);
    }
  }
}

export function assertSamples(
  fnName: string,
  samples: ArrayLike<number>,
  validate: boolean,
  argName = 'samples',
): void {
  assertNonEmptySamples(fnName, samples, argName);
  assertFiniteSamples(fnName, samples, validate, argName);
}

export function assertFiniteScalar(fnName: string, value: number, argName: string): void {
  if (!Number.isFinite(value)) {
    throw new RangeError(`${fnName}: ${argName} must be a finite number`);
  }
}

export function assertU7(fnName: string, value: number, argName: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 127) {
    throw new RangeError(`${fnName}: ${argName} must be an integer in [0, 127]`);
  }
  return value;
}

export function assertNibble(fnName: string, value: number, argName: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 15) {
    throw new RangeError(`${fnName}: ${argName} must be an integer in [0, 15]`);
  }
  return value;
}

export function midi1Event(
  fnName: string,
  ppq: number,
  group: number,
  status: number,
  channel: number,
  data1: number,
  data2 = 0,
): ProjectMidiEvent {
  assertFiniteScalar(fnName, ppq, 'ppq');
  if (ppq < 0) {
    throw new RangeError(`${fnName}: ppq must be non-negative`);
  }
  const g = assertNibble(fnName, group, 'group');
  const ch = assertNibble(fnName, channel, 'channel');
  const d1 = assertU7(fnName, data1, 'data1');
  const d2 = assertU7(fnName, data2, 'data2');
  const word = ((0x2 << 28) | (g << 24) | (status << 20) | (ch << 16) | (d1 << 8) | d2) >>> 0;
  return { ppq, data0: word, data1: 0 };
}

export function assertU32(fnName: string, value: number, argName: string): void {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) {
    throw new RangeError(`${fnName}: ${argName} must be an integer in [0, 4294967295]`);
  }
}

export function assertProjectMidiEvents(
  fnName: string,
  events: ReadonlyArray<ProjectMidiEvent | readonly [number, number, number]>,
): void {
  if (!Array.isArray(events)) {
    throw new TypeError(`${fnName}: events must be an array`);
  }
  events.forEach((event, index) => {
    const prefix = `events[${index}]`;
    if (Array.isArray(event)) {
      if (event.length < 3) {
        throw new TypeError(`${fnName}: ${prefix} must contain [ppq, data0, data1]`);
      }
      assertFiniteScalar(fnName, event[0], `${prefix}.ppq`);
      if (event[0] < 0) {
        throw new RangeError(`${fnName}: ${prefix}.ppq must be non-negative`);
      }
      assertU32(fnName, event[1], `${prefix}.data0`);
      assertU32(fnName, event[2], `${prefix}.data1`);
      return;
    }
    if (event === null || typeof event !== 'object') {
      throw new TypeError(`${fnName}: ${prefix} must be a MIDI event object or tuple`);
    }
    assertFiniteScalar(fnName, event.ppq, `${prefix}.ppq`);
    if (event.ppq < 0) {
      throw new RangeError(`${fnName}: ${prefix}.ppq must be non-negative`);
    }
    assertU32(fnName, event.data0, `${prefix}.data0`);
    if (event.data1 !== undefined) {
      assertU32(fnName, event.data1, `${prefix}.data1`);
    }
  });
}

/**
 * Audio object wrapping decoded audio samples.
 */
