import { addon } from './native.js';
import { assertFiniteScalar } from './validation.js';

/** A per-processor parameter value: an enum-valued key may be given by its name. */
export type MasteringSoloProcessorParamValue = number | boolean | string;

/** Flat per-processor parameters, keyed as in `masteringInsertParamInfo`. */
export type MasteringSoloProcessorParams = Record<string, MasteringSoloProcessorParamValue>;

/** An insert parameter value: a solo value, or a list of numbers for an array-typed key. */
export type MasteringInsertParamValue = MasteringSoloProcessorParamValue | readonly number[];

/** Insert parameters, keyed as in `masteringInsertParamInfo`; the shape an insert is built from. */
export type MasteringInsertParams = Record<string, MasteringInsertParamValue>;

/**
 * Replaces each enum name in `params` with the number the flat parameter lists
 * carry, resolved by the core against `processor`'s declared choices. A string
 * for a key that is not an enum is a wrong type; an unknown name is refused by
 * the core with the key and the valid names. Internal helper shared by the
 * per-processor entry points.
 */
export function resolveProcessorParams(
  processor: string,
  params: MasteringSoloProcessorParams,
): Record<string, number | boolean> {
  let resolved: Record<string, number | boolean> | undefined;
  for (const [key, value] of Object.entries(params)) {
    if (typeof value !== 'string') {
      continue;
    }
    const number = addon.masteringEnumValue(processor, key, value) as number | null;
    if (number === null) {
      throw new TypeError(`Parameter '${key}' must be a number or boolean (got a string).`);
    }
    resolved ??= { ...params } as Record<string, number | boolean>;
    resolved[key] = number;
  }
  return resolved ?? (params as Record<string, number | boolean>);
}

/**
 * Checks each value of an insert's `params` against the `type` its descriptor
 * declares and returns the document the insert is built from. A `string` key
 * takes a string, an `array` key a list of finite numbers; every other key
 * takes a finite number or boolean, or an enum name resolved as
 * {@link resolveProcessorParams} does. A wrong type is a `TypeError`, a
 * non-finite number a `RangeError`. A key the processor does not declare is
 * left to the core, which refuses it by name.
 */
export function resolveInsertParams(
  fnName: string,
  processor: string,
  params: MasteringInsertParams,
): Record<string, number | boolean | string | number[]> {
  const info = JSON.parse(addon.masteringInsertParamInfo(processor) as string) as {
    name: string;
    type: string;
  }[];
  const kinds = new Map(info.map((param) => [param.name, param.type]));
  const scalars: Record<string, MasteringSoloProcessorParamValue> = {};
  const out: Record<string, number | boolean | string | number[]> = {};
  for (const [key, value] of Object.entries(params)) {
    const kind = kinds.get(key);
    if (kind === 'string') {
      if (typeof value !== 'string') {
        throw new TypeError(`${fnName}: params.${key} must be a string`);
      }
      out[key] = value;
    } else if (kind === 'array') {
      if (!Array.isArray(value) || value.some((item) => typeof item !== 'number')) {
        throw new TypeError(`${fnName}: params.${key} must be an array of numbers`);
      }
      for (const item of value as number[]) {
        assertFiniteScalar(fnName, item, `params.${key}[]`);
      }
      out[key] = [...(value as number[])];
    } else if (typeof value === 'string' || typeof value === 'boolean') {
      scalars[key] = value;
    } else if (typeof value === 'number') {
      assertFiniteScalar(fnName, value, `params.${key}`);
      scalars[key] = value;
    } else {
      throw new TypeError(`${fnName}: params.${key} must be a number, boolean or enum name`);
    }
  }
  return { ...out, ...resolveProcessorParams(processor, scalars) };
}
