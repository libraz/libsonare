import type { MasteringChainConfig, MasteringChainSection } from './types.js';

/**
 * Top-level keys of the streaming chain's config that the streaming chain reads
 * itself rather than passing to the mastering chain parser.
 *
 * The addon skips exactly these before flattening
 * (`src/addon/sonare_wrap_streaming.cpp`), so the constructor's validation pass
 * must skip them too or it would reject a valid config. This module is internal
 * (never re-exported from the package index), and
 * `tests/chain-config-aliases.test.ts` pins the list to the addon's own literal
 * rather than trusting the hand-copy.
 */
export const STREAMING_ONLY_CONFIG_KEYS = [
  'loudnessStaticGainDb',
  'loudnessStaticGainPeakDb',
] as const;

/**
 * Flattens a nested {@link MasteringChainConfig} into the dot-notation
 * `{ "module.processor.param": value }` map the native core consumes. Internal
 * helper shared by the mastering-chain / master-audio entry points.
 *
 * A key the caller already wrote in dot notation carries through untouched, so
 * both spellings a {@link MasteringChainConfig} accepts reach the core as the
 * same parameter — matching what the Python binding documents. An unknown key
 * in either spelling is rejected by the core, not here.
 *
 * `skipTopLevelKeys` names top-level entries that are not chain parameters at
 * all and must be neither flattened nor rejected. Only the streaming chain has
 * such keys, and it passes them so its leaf typing is checked by exactly this
 * function rather than by a second copy of the rule.
 */
export function flattenChainConfig(
  config: MasteringChainConfig | Record<string, unknown>,
  skipTopLevelKeys: readonly string[] = [],
): Record<string, number | boolean> {
  const out: Record<string, number | boolean> = {};
  const skipped = new Set(skipTopLevelKeys);
  const walk = (node: MasteringChainSection, prefix: string): void => {
    for (const [key, value] of Object.entries(node)) {
      if (!prefix && skipped.has(key)) {
        continue;
      }
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
