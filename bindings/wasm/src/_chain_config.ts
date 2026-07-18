import type { MasteringChainConfig } from './public_types';

type ChainSection = { [key: string]: number | boolean | ChainSection | undefined };

/**
 * Flattens a nested {@link MasteringChainConfig} into the dot-notation
 * `{ "module.processor.param": value }` map the WASM core consumes. Internal
 * helper shared by the mastering-chain / master-audio entry points; mirrors the
 * Node facade so a nested config produces identical overrides on both surfaces.
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
      }
    }
  };
  walk(config as ChainSection, '');
  return out;
}
