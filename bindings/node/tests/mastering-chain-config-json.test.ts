/**
 * `masteringChainConfigFromJson` reads a `{ version, params }` chain document
 * through the library's parser: every built-in preset survives the trip to a
 * document and back, and a document the parser refuses is a coded
 * `InvalidFormat` error.
 */

import { describe, expect, it } from 'vitest';
import { ErrorCode, isSonareError } from '../src/errors.js';
import {
  masteringChainConfigFromJson,
  masteringPresetNames,
  masteringPresetParams,
} from '../src/index.js';

const PRESETS = masteringPresetNames();

/**
 * Flattens a nested chain config into a version 1 document's `params`, written
 * here rather than through the facade so the round trip is not checked against
 * the code it exercises. Enum leaves stay as their names, which the document
 * accepts; the two native spellings the facade hides are written as native
 * reads them.
 */
function documentParams(config: unknown): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  const walk = (node: unknown, path: string): void => {
    if (typeof node !== 'object' || node === null) {
      if (path === 'repair.declip.clipThresholdDb') {
        out['repair.declip.clipThreshold'] = 10 ** ((node as number) / 20);
      } else if (/^dynamics\.multibandComp\.bands\.\d+\.sidechainHighPassHz$/.test(path)) {
        const band = path.slice(0, -'.sidechainHighPassHz'.length);
        out[`${band}.sidechainHpfEnabled`] = true;
        out[`${band}.sidechainHpfHz`] = node;
      } else {
        out[path] = node;
      }
      return;
    }
    for (const [key, value] of Object.entries(node)) {
      walk(value, path ? `${path}.${key}` : key);
    }
  };
  walk(config, '');
  return out;
}

function refusal(call: () => unknown): unknown {
  try {
    call();
  } catch (error) {
    return error;
  }
  throw new Error('expected the call to throw');
}

describe('masteringChainConfigFromJson', () => {
  it('has presets to round-trip', () => {
    expect(PRESETS.length).toBeGreaterThan(0);
  });

  it.each(PRESETS)('round-trips the %s preset through a chain document', (preset) => {
    const want = documentParams(masteringPresetParams(preset));
    const got = documentParams(
      masteringChainConfigFromJson(JSON.stringify({ version: 1, params: want })),
    );
    expect(Object.keys(got).sort()).toEqual(Object.keys(want).sort());
    for (const [key, value] of Object.entries(got)) {
      if (typeof value === 'number') {
        expect(value, key).toBeCloseTo(want[key] as number, 5);
      } else {
        expect(value, key).toBe(want[key]);
      }
    }
  });

  it('reads a partial document into the nested form', () => {
    const parsed = masteringChainConfigFromJson(
      '{"version":1,"params":{"loudness.targetLufs":-9}}',
    );
    expect(parsed.loudness?.targetLufs).toBe(-9);
  });

  it.each([
    ['malformed JSON', '{'],
    ['an unsupported version', '{"version":3,"params":{"loudness.targetLufs":-9}}'],
    ['an unknown key', '{"version":1,"params":{"loudness.bogusKey":1}}'],
  ])('refuses %s as InvalidFormat', (_label, document) => {
    const error = refusal(() => masteringChainConfigFromJson(document));
    expect(isSonareError(error)).toBe(true);
    expect((error as { code: number }).code).toBe(ErrorCode.InvalidFormat);
  });

  it('refuses a retired key and names its current spelling', () => {
    const error = refusal(() =>
      masteringChainConfigFromJson('{"version":1,"params":{"eq.tiltDb":1}}'),
    );
    expect(isSonareError(error)).toBe(true);
    expect((error as { code: number }).code).toBe(ErrorCode.InvalidFormat);
    expect((error as Error).message).toContain('eq.tilt.tiltDb');
  });

  it('refuses a document that is not a string with a TypeError', () => {
    // @ts-expect-error the document is a JSON string
    expect(() => masteringChainConfigFromJson({ version: 1, params: {} })).toThrow(TypeError);
  });
});
