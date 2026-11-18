import type { MasteringChainConfig } from '../public_types.js';

type ChainSection = { [key: string]: unknown };

/**
 * Enum leaves of a chain config, matched on the dotted path with band and list
 * indices as `#`. The position of a token is the ordinal native reads; native
 * still takes ordinals and the facade is the only place that knows the tokens.
 */
const CHAIN_ENUM_TOKENS: ReadonlyArray<readonly [string, readonly string[]]> = [
  ['repair.denoise.mode', ['logMmse', 'mmseStsa', 'spectralSubtraction']],
  ['repair.denoise.noiseEstimator', ['quantile', 'mcra', 'imcra', 'spp']],
  ['repair.decrackle.mode', ['median', 'waveletShrinkage']],
  ['repair.dehum.mode', ['subtract', 'notch']],
  ['dynamics.multibandComp.crossover.slope', ['lr2', 'lr4', 'lr8']],
  [
    'dynamics.multibandComp.crossover.mode',
    ['linkwitzRiley', 'butterworth', 'bessel', 'firLinearPhase'],
  ],
  ['dynamics.multibandComp.bands.#.detector', ['peak', 'rms', 'logRms']],
];

/** Spellings a nested config no longer carries, with the field that replaces each. */
const REMOVED_PATHS = new Map<string, string>(
  Object.entries({
    'repair.nFft': 'repair.denoise.nFft',
    'repair.hopLength': 'repair.denoise.hopLength',
    'repair.ddAlpha': 'repair.denoise.ddAlpha',
    'repair.reductionDb': 'repair.denoise.reductionDb',
    'repair.gainFloor': 'repair.denoise.reductionDb',
    'repair.denoise.gainFloor': 'repair.denoise.reductionDb',
    'eq.tiltDb': 'eq.tilt.tiltDb',
    'eq.pivotHz': 'eq.tilt.pivotHz',
    'repair.declip.clipThreshold': 'repair.declip.clipThresholdDb',
  }),
);

const BAND_PATH = /^dynamics\.multibandComp\.bands\.(\d+)\./;

const emptyList = (path: string): string =>
  `Mastering override '${path}' is an empty list, which has no flat spelling.`;

const notAValue = (path: string): string =>
  `Mastering override '${path}' must be a number or boolean.`;

function enumTokens(path: string): readonly string[] | undefined {
  const pattern = path.replace(/\.\d+\./g, '.#.');
  return CHAIN_ENUM_TOKENS.find(([key]) => key === pattern)?.[1];
}

/**
 * Flattens a nested {@link MasteringChainConfig} into the dot-notation
 * `{ "module.processor.param": value }` map native consumes.
 *
 * An array recurses as `<path>.<index>` (`crossover.cutoffsHz.0`,
 * `bands.2.thresholdDb`); an empty array is refused by its path. An enum leaf is
 * written as its token and sent as its ordinal; a number or an unknown token is a
 * `RangeError`. A dotted key, a removed spelling or a boolean `denoise` is a
 * `TypeError` naming it.
 */
export function flattenChainConfig(config: MasteringChainConfig): Record<string, number | boolean> {
  const out: Record<string, number | boolean> = {};
  const walk = (node: ChainSection, prefix: string): void => {
    for (const [key, value] of Object.entries(node)) {
      const path = prefix ? `${prefix}.${key}` : key;
      if (value === undefined) {
        continue;
      }
      if (key.includes('.')) {
        throw new TypeError(
          `Mastering override '${path}': dotted keys are not accepted; nest the object`,
        );
      }
      const replacement = REMOVED_PATHS.get(path);
      if (replacement !== undefined) {
        throw new TypeError(`Mastering override '${path}' is not accepted; use '${replacement}'`);
      }
      if ((key === 'sidechainHpfEnabled' || key === 'sidechainHpfHz') && BAND_PATH.test(path)) {
        throw new TypeError(
          `Mastering override '${path}' is not accepted; use 'sidechainHighPassHz'`,
        );
      }
      if (path === 'repair.denoise' && (typeof value !== 'object' || value === null)) {
        throw new TypeError(
          `Mastering override '${path}' must be an object; write { enabled: true }`,
        );
      }
      const tokens = enumTokens(path);
      if (tokens !== undefined) {
        if (typeof value !== 'string') {
          throw new TypeError(`Mastering override '${path}' must be a string`);
        }
        const ordinal = tokens.indexOf(value);
        if (ordinal < 0) {
          throw new RangeError(`Mastering override '${path}' must be one of ${tokens.join(', ')}`);
        }
        out[path] = ordinal;
      } else if (path === 'repair.declip.clipThresholdDb') {
        if (typeof value !== 'number') {
          throw new TypeError(notAValue(path));
        }
        if (!Number.isFinite(value) || value > 0) {
          throw new RangeError(
            `Mastering override '${path}' must be a finite level at or below 0 dBFS`,
          );
        }
        out[path] = value;
      } else if (typeof value === 'number' || typeof value === 'boolean') {
        out[path] = value;
      } else if (typeof value === 'object' && value !== null) {
        if (Array.isArray(value) && value.length === 0) {
          throw new TypeError(emptyList(path));
        }
        walk(value as ChainSection, path);
      } else {
        throw new TypeError(notAValue(path));
      }
    }
  };
  walk(config as ChainSection, '');
  return out;
}

/**
 * Nests a flat `{ "module.processor.param": value }` map native returns into a
 * {@link MasteringChainConfig}, the inverse of {@link flattenChainConfig}: indices
 * become arrays and enum ordinals become tokens. A structured value (a version 2
 * document's multiband object) is expanded to its dotted keys first.
 */
export function nestChainConfig(flat: Record<string, unknown>): MasteringChainConfig {
  const root: ChainSection = {};
  const set = (path: string, value: unknown): void => {
    const parts = path.split('.');
    let node: Record<string, unknown> = root;
    for (let i = 0; i < parts.length - 1; i++) {
      const part = parts[i];
      const next = parts[i + 1];
      const isIndex = /^\d+$/.test(next);
      let child = node[part];
      if (child === undefined) {
        child = isIndex ? [] : {};
        node[part] = child;
      }
      node = child as Record<string, unknown>;
    }
    node[parts[parts.length - 1]] = value;
  };
  const expanded: Record<string, unknown> = {};
  const expand = (path: string, value: unknown): void => {
    if (Array.isArray(value)) {
      for (let i = 0; i < value.length; i++) {
        expand(`${path}.${i}`, value[i]);
      }
    } else if (typeof value === 'object' && value !== null) {
      for (const [key, member] of Object.entries(value)) {
        expand(`${path}.${key}`, member);
      }
    } else {
      expanded[path] = value;
    }
  };
  for (const [path, value] of Object.entries(flat)) {
    expand(path, value);
  }
  for (const [path, value] of Object.entries(expanded)) {
    const tokens = enumTokens(path);
    set(path, tokens !== undefined && typeof value === 'number' ? (tokens[value] ?? value) : value);
  }
  return root as MasteringChainConfig;
}
