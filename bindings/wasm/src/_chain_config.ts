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

  // Compatibility aliases for the original WASM-only shorthand. Normalize at
  // this boundary so every native entry point receives the core parser's one
  // canonical vocabulary; public types can migrate to the nested spelling
  // without preserving a second C++ parser indefinitely.
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
