/**
 * What `overrides` does to a stage it names, as the `overrides` doc states it:
 * a stage carrying any key takes the `enabled` it states and is turned on when it
 * states none, so a full config turns off every preset stage it marks false.
 */

import { describe, expect, it } from 'vitest';
import {
  type MasteringChainConfig,
  masterAudio,
  masteringChainConfigFromJson,
} from '../src/index.js';

const sampleRate = 22_050;
const samples = Float32Array.from(
  { length: 8192 },
  (_, i) => 0.5 * Math.sin((2 * Math.PI * 220 * i) / sampleRate) * (i % 2048 < 1024 ? 1 : 0.2),
);

// Largest sample-wise difference between two renders.
function maxDifference(a: Float32Array, b: Float32Array): number {
  expect(a.length).toBe(b.length);
  let worst = 0;
  for (let i = 0; i < a.length; i += 1) {
    worst = Math.max(worst, Math.abs(a[i] - b[i]));
  }
  return worst;
}

const render = (request: { preset?: 'pop'; overrides?: MasteringChainConfig }) =>
  masterAudio({ samples, sampleRate, ...request }).samples;

// Renders of one effective chain agree to rounding; a stage switched on or off moves
// this signal by far more than this.
const SAME = 1e-5;

describe('masterAudio overrides', () => {
  const compressor = { thresholdDb: -30, ratio: 8 };

  it('turns on a stage that carries a key and states no enabled', () => {
    expect(
      maxDifference(render({ overrides: { dynamics: { compressor } } }), render({ overrides: {} })),
    ).toBeGreaterThan(100 * SAME);
  });

  it('keeps a stage off when it carries a key and states enabled false', () => {
    const off = render({
      overrides: { dynamics: { compressor: { ...compressor, enabled: false } } },
    });
    expect(maxDifference(off, render({ overrides: {} }))).toBeLessThan(SAME);
  });

  it('turns off the preset stages a full config marks false', () => {
    const full = masteringChainConfigFromJson('{"version":2,"params":{}}');
    const disabled = Object.entries(full).flatMap(([module, value]) =>
      'enabled' in value
        ? value.enabled === false
          ? [module]
          : []
        : Object.entries(value as Record<string, { enabled?: boolean }>)
            .filter(([, stage]) => stage.enabled === false)
            .map(([stage]) => `${module}.${stage}`),
    );
    expect(disabled.length).toBeGreaterThan(0);
    const presetAndFull = render({ preset: 'pop', overrides: full });
    expect(maxDifference(presetAndFull, render({ overrides: full }))).toBeLessThan(SAME);
    expect(maxDifference(presetAndFull, render({ preset: 'pop' }))).toBeGreaterThan(100 * SAME);
  });
});
