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
      }
    }
  };
  walk(config as MasteringChainSection, '');

  // Compatibility aliases for the original flat shorthand. Normalize at this
  // boundary so every native entry point receives the core parser's one
  // canonical vocabulary; kept in sync with the WASM facade so a legacy flat
  // override produces the identical override map on both surfaces.
  const aliases: Record<string, string> = {
    'repair.denoise': 'repair.denoise.enabled',
    'repair.nFft': 'repair.denoise.nFft',
    'repair.hopLength': 'repair.denoise.hopLength',
    'repair.ddAlpha': 'repair.denoise.ddAlpha',
    'repair.gainFloor': 'repair.denoise.gainFloor',
    'eq.tiltDb': 'eq.tilt.tiltDb',
    'eq.pivotHz': 'eq.tilt.pivotHz',
  };
  for (const [legacy, canonical] of Object.entries(aliases)) {
    const value = out[legacy];
    if (value !== undefined) {
      out[canonical] = value;
      delete out[legacy];
    }
  }
  return out;
}
