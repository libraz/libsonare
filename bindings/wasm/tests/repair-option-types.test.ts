/**
 * A wrong-typed mastering-repair option is refused by name, the way the Node addon
 * refuses it; an undefined or null option takes the config default.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masteringRepairDeclick,
  masteringRepairDeclip,
  masteringRepairDecrackle,
  masteringRepairDehum,
  masteringRepairDenoiseClassical,
  masteringRepairDereverbClassical,
  masteringRepairTrimSilence,
} from '../src/index';

const SR = 22050;

function sine(durationSec: number): Float32Array {
  const out = new Float32Array(Math.floor(SR * durationSec));
  for (let i = 0; i < out.length; i++) {
    out[i] = 0.3 * Math.sin((2 * Math.PI * 440 * i) / SR);
  }
  return out;
}

type Run = (options: Record<string, unknown>) => unknown;

const samples = sine(0.3);
const entries: Array<{
  name: string;
  run: Run;
  fields: Array<{ key: string; wrong: unknown; type: 'number' | 'boolean' }>;
}> = [
  {
    name: 'declick',
    run: (o) => masteringRepairDeclick(samples, SR, o as never),
    fields: [
      { key: 'threshold', wrong: '4', type: 'number' },
      { key: 'lpcOrder', wrong: '8', type: 'number' },
    ],
  },
  {
    name: 'declip',
    run: (o) => masteringRepairDeclip(samples, SR, o as never),
    fields: [
      { key: 'clipThreshold', wrong: '0.9', type: 'number' },
      { key: 'iterations', wrong: '3', type: 'number' },
    ],
  },
  {
    name: 'decrackle',
    run: (o) => masteringRepairDecrackle(samples, SR, o as never),
    fields: [{ key: 'threshold', wrong: '4', type: 'number' }],
  },
  {
    name: 'dehum',
    run: (o) => masteringRepairDehum(samples, SR, o as never),
    fields: [
      { key: 'q', wrong: '30', type: 'number' },
      { key: 'adaptive', wrong: 'yes', type: 'boolean' },
    ],
  },
  {
    name: 'denoiseClassical',
    run: (o) => masteringRepairDenoiseClassical(samples, SR, o as never),
    fields: [
      { key: 'reductionDb', wrong: '12', type: 'number' },
      { key: 'nFft', wrong: '1024', type: 'number' },
      { key: 'speechPresenceGain', wrong: 1, type: 'boolean' },
    ],
  },
  {
    name: 'dereverbClassical',
    run: (o) => masteringRepairDereverbClassical(samples, SR, o as never),
    fields: [
      { key: 't60Sec', wrong: '0.5', type: 'number' },
      { key: 'wpeEnabled', wrong: 1, type: 'boolean' },
    ],
  },
  {
    name: 'trimSilence',
    run: (o) => masteringRepairTrimSilence(samples, SR, o as never),
    fields: [{ key: 'paddingSamples', wrong: '64', type: 'number' }],
  },
];

describe('mastering repair option types (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  for (const { name, run, fields } of entries) {
    for (const { key, wrong, type } of fields) {
      it(`${name} refuses a wrong-typed ${key} by name`, () => {
        expect(() => run({ [key]: wrong })).toThrow(new RegExp(`${key} must be a ${type}`));
      });

      it(`${name} reads an undefined or null ${key} as the default`, () => {
        const baseline = run({});
        expect(run({ [key]: undefined })).toEqual(baseline);
        expect(run({ [key]: null })).toEqual(baseline);
      });
    }
  }
});
