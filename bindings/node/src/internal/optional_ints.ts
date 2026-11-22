import { C_INT_MAX } from '../validation.js';
import { refuse, refuseType } from './entry.js';
import { isAbsent, UINT32_MAX } from './value_guards.js';

/**
 * An optional positive integer whose `0` would read as "not given": absent (`undefined` / `null`)
 * stays `undefined`, a non-number is a `TypeError`, `0` is refused by name (`hint` finishes
 * "omit it ..."), and anything else outside `[1, max]` states that interval.
 */
export function optionalPositiveInt(
  argName: string,
  value: unknown,
  max: number,
  hint = 'for the default',
): number | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  if (typeof value !== 'number') {
    throw refuseType(`${argName} must be an integer`);
  }
  if (value === 0) {
    throw refuse(`${argName} must not be 0; omit it ${hint}`);
  }
  if (!Number.isInteger(value) || value < 1 || value > max) {
    throw refuse(`${argName} must be an integer in [1, ${max}]`);
  }
  return value;
}

/** An optional uint32 id whose `0` would mean "none". */
export function optionalId(
  argName: string,
  value: unknown,
  hint = 'for the default',
): number | undefined {
  return optionalPositiveInt(argName, value, UINT32_MAX, hint);
}

/** An optional count a C `int` carries. */
export function optionalCount(argName: string, value: unknown): number | undefined {
  return optionalPositiveInt(argName, value, C_INT_MAX);
}
