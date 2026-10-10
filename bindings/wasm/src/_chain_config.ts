import { getSonareModule } from './module_state.js';
import type { MasteringChainConfig } from './public_types.js';

type ChainSection = { [key: string]: number | boolean | string | ChainSection | undefined };

const emptyList = (path: string): string =>
  `Mastering override '${path}' is an empty list, which has no flat spelling.`;

const notAValue = (path: string): string =>
  `Mastering override '${path}' must be a number or boolean.`;

/**
 * Flattens a nested {@link MasteringChainConfig} into the dot-notation
 * `{ "module.processor.param": value }` map the core consumes. Internal helper
 * shared by the mastering-chain / master-audio entry points. The core owns
 * legacy leaf-name aliases, so this remains a structural flattening step.
 *
 * A key the caller already wrote in dot notation carries through untouched, so
 * both spellings a {@link MasteringChainConfig} accepts reach the core as the
 * same parameter — matching what the Python binding documents. An unknown key
 * in either spelling is rejected by the core, not here.
 *
 * An array recurses as `<path>.<index>` (`crossover.cutoffsHz.0`,
 * `bands.2.thresholdDb`). An empty array is refused by its path, since it has
 * no flat key and dropping it would silently edit nothing.
 *
 * An enum-valued key may be given by its name (`noiseEstimator: 'mcra'`); the
 * core resolves the name to the number every flat parameter list carries and
 * refuses an unknown one with the key and the valid names.
 */
export function flattenChainConfig(config: MasteringChainConfig): Record<string, number | boolean> {
  const out: Record<string, number | boolean> = {};
  const walk = (node: ChainSection, prefix: string): void => {
    for (const [key, value] of Object.entries(node)) {
      const path = prefix ? `${prefix}.${key}` : key;
      if (typeof value === 'number' || typeof value === 'boolean') {
        out[path] = value;
      } else if (typeof value === 'string') {
        const resolved = getSonareModule().masteringEnumValue('', path, value);
        if (resolved === null) {
          throw new TypeError(notAValue(path));
        }
        out[path] = resolved;
      } else if (value !== null && typeof value === 'object') {
        if (Array.isArray(value) && value.length === 0) {
          throw new TypeError(emptyList(path));
        }
        walk(value, path);
      } else if (value !== undefined) {
        throw new TypeError(notAValue(path));
      }
    }
  };
  walk(config as ChainSection, '');

  return out;
}
