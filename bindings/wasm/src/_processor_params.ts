import { getSonareModule } from './module_state.js';
import type { MasteringSoloProcessorParams } from './public_types.js';

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
    const number = getSonareModule().masteringEnumValue(processor, key, value);
    if (number === null) {
      throw new TypeError(`Parameter '${key}' must be a number or boolean (got a string).`);
    }
    resolved ??= { ...params } as Record<string, number | boolean>;
    resolved[key] = number;
  }
  return resolved ?? (params as Record<string, number | boolean>);
}
