/**
 * `masteringChainConfigFromJson` reads a version 2 `{ version, params }` chain
 * document through the library's parser: every built-in preset survives the trip
 * to a document and back, written either fully dotted or nested, and a document
 * the parser refuses is a coded `InvalidFormat` error naming what it refused.
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
 * Flattens a nested chain config into dotted document paths, with a list index
 * as one path segment. Written here rather than through the facade so the round
 * trip is not checked against the code it exercises.
 */
function documentParams(config: unknown): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  const walk = (node: unknown, path: string): void => {
    if (typeof node !== 'object' || node === null) {
      out[path] = node;
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

function expectSameParams(got: Record<string, unknown>, want: Record<string, unknown>): void {
  expect(Object.keys(got).sort()).toEqual(Object.keys(want).sort());
  for (const [key, value] of Object.entries(got)) {
    if (typeof value === 'number') {
      expect(value, key).toBeCloseTo(want[key] as number, 5);
    } else {
      expect(value, key).toBe(want[key]);
    }
  }
}

describe('masteringChainConfigFromJson', () => {
  it('has presets to round-trip', () => {
    expect(PRESETS.length).toBeGreaterThan(0);
  });

  it.each(PRESETS)('round-trips the %s preset through a dotted chain document', (preset) => {
    const want = documentParams(masteringPresetParams(preset));
    const got = documentParams(
      masteringChainConfigFromJson(JSON.stringify({ version: 2, params: want })),
    );
    expectSameParams(got, want);
  });

  it.each(PRESETS)('round-trips the %s preset through a nested chain document', (preset) => {
    const config = masteringPresetParams(preset);
    const got = documentParams(masteringChainConfigFromJson(JSON.stringify({ params: config })));
    expectSameParams(got, documentParams(config));
  });

  it('reads a partial document into the nested form', () => {
    const parsed = masteringChainConfigFromJson(
      '{"version":2,"params":{"loudness.targetLufs":-9}}',
    );
    expect(parsed.loudness?.targetLufs).toBe(-9);
  });

  it.each([
    ['malformed JSON', '{', 'expected object key string at byte 1'],
    [
      'version 1',
      '{"version":1,"params":{"loudness.targetLufs":-9}}',
      'version must be 2, or left out',
    ],
    [
      'an unsupported version',
      '{"version":3,"params":{"loudness.targetLufs":-9}}',
      'version must be 2, or left out',
    ],
    [
      'an unknown key',
      '{"version":2,"params":{"loudness.bogusKey":1}}',
      'unknown key params.loudness.bogusKey',
    ],
    [
      'a retired key, naming its current spelling',
      '{"version":2,"params":{"eq.tiltDb":1}}',
      'unknown key params.eq.tiltDb (renamed to params.eq.tilt.tiltDb)',
    ],
    [
      'a linear declip threshold, naming its decibel spelling',
      '{"params":{"repair.declip.clipThreshold":0.5}}',
      'unknown key params.repair.declip.clipThreshold (renamed to params.repair.declip.clipThresholdDb)',
    ],
  ])('refuses %s as InvalidFormat', (_label, document, message) => {
    const error = refusal(() => masteringChainConfigFromJson(document));
    expect(isSonareError(error)).toBe(true);
    expect((error as { code: number }).code).toBe(ErrorCode.InvalidFormat);
    expect((error as Error).message).toBe(`masteringChainConfigFromJson: ${message}`);
  });

  it('refuses a document that is not a string with a TypeError', () => {
    // @ts-expect-error the document is a JSON string
    expect(() => masteringChainConfigFromJson({ version: 2, params: {} })).toThrow(TypeError);
  });
});
