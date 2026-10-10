import { addon } from './native.js';

/** A per-processor parameter value: an enum-valued key may be given by its name. */
export type MasteringSoloProcessorParamValue = number | boolean | string;

/** Flat per-processor parameters, keyed as in `masteringInsertParamInfo`. */
export type MasteringSoloProcessorParams = Record<string, MasteringSoloProcessorParamValue>;

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
