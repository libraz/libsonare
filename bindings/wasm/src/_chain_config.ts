import type { MasteringChainConfig } from './public_types';

type ChainSection = { [key: string]: number | boolean | ChainSection | undefined };

/**
 * Flattens a nested {@link MasteringChainConfig} into the dot-notation
 * `{ "module.processor.param": value }` map the core consumes. Internal helper
 * shared by the mastering-chain / master-audio entry points. The core owns
 * legacy flat-key aliases, so this remains a structural flattening step.
 */
export function flattenChainConfig(config: MasteringChainConfig): Record<string, number | boolean> {
  const out: Record<string, number | boolean> = {};
  const walk = (node: ChainSection, prefix: string): void => {
    for (const [key, value] of Object.entries(node)) {
      const path = prefix ? `${prefix}.${key}` : key;
      if (typeof value === 'number' || typeof value === 'boolean') {
        out[path] = value;
      } else if (value !== null && typeof value === 'object') {
        walk(value, path);
      } else if (value !== undefined) {
        throw new TypeError(`Mastering override '${path}' must be a number or boolean.`);
      }
    }
  };
  walk(config as ChainSection, '');

  return out;
}
