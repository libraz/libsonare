import type { MasteringChainConfig, MasteringChainSection } from './types.js';

/**
 * Flattens a nested {@link MasteringChainConfig} into the dot-notation
 * `{ "module.processor.param": value }` map the native core consumes. Internal
 * helper shared by the mastering-chain / master-audio entry points.
 */
export function flattenChainConfig(config: MasteringChainConfig): Record<string, number | boolean> {
  const out: Record<string, number | boolean> = {};
  const walk = (node: MasteringChainSection, prefix: string): void => {
    for (const [key, value] of Object.entries(node)) {
      const path = prefix ? `${prefix}.${key}` : key;
      if (typeof value === 'number' || typeof value === 'boolean') {
        out[path] = value;
      } else if (value !== null && typeof value === 'object') {
        walk(value as MasteringChainSection, path);
      } else if (value !== undefined) {
        throw new TypeError(`Mastering override '${path}' must be a number or boolean.`);
      }
    }
  };
  walk(config as MasteringChainSection, '');

  // The core's canonical_chain_param_key() owns legacy flat-key aliases.
  // Keep this facade as a pure structural flattening step so Node and WASM
  // cannot drift by maintaining a second alias table.
  return out;
}
