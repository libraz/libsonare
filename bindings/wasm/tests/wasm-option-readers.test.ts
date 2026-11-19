/**
 * Option readers refuse a present wrong-typed value by name; only an absent
 * (undefined / null) field takes the default. A string 'false' or a numeric
 * string is a caller error, never a flag or a number nobody wrote.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  init,
  Mixer,
  masteringDynamicsCompressor,
  masteringRepairDeclick,
  masteringRepairDeclip,
  masteringRepairDecrackle,
  mixingScenePreset,
  mixStereo,
  RealtimeEngine,
} from '../dist/index.js';

const SR = 48000;
const BLOCK = 512;
const leftOnly = () => new Float32Array(BLOCK).fill(1);
const silence = () => new Float32Array(BLOCK);
/** A one-strip mix of the left-only source with the given strip settings. */
const oneStrip = (sampleRate: number, strip: Record<string, unknown> = {}) =>
  mixStereo({ strips: [{ left: leftOnly(), right: silence(), ...strip }], sampleRate });

function expectInvalid(fn: () => unknown, name: RegExp): void {
  let caught: unknown;
  try {
    fn();
  } catch (error) {
    caught = error;
  }
  expect(caught).toBeInstanceOf(Error);
  expect((caught as Error).message).toMatch(name);
}

describe('wrong-typed option values are refused by name', () => {
  beforeAll(async () => {
    await init();
  });

  describe('mixStereo strip fields', () => {
    it.each([
      ['muted', 'false'],
      ['muted', 0],
      ['pan', '0.3'],
      ['pan', true],
      ['width', '1'],
      ['inputTrimDb', false],
    ])('refuses %s = %j', (key, value) => {
      expectInvalid(() => oneStrip(SR, { [key]: value }), new RegExp(`strips\\[0\\]\\.${key}`));
    });

    it('names the strip index of a wrong-typed field', () => {
      expectInvalid(
        () =>
          mixStereo({
            strips: [
              { left: leftOnly(), right: silence() },
              { left: leftOnly(), right: silence(), faderDb: '-6' as never },
            ],
            sampleRate: SR,
          }),
        /strips\[1\]\.faderDb/,
      );
    });

    it('refuses the removed per-field options and positional arguments', () => {
      expectInvalid(
        () => mixStereo({ strips: [], sampleRate: SR, faderDb: [0] } as never),
        /unknown request key 'faderDb'/,
      );
      expectInvalid(() => oneStrip(SR, { gain: 1 }), /unknown option 'gain' in 'strips\[0\]'/);
      expectInvalid(
        () => (mixStereo as (...args: unknown[]) => unknown)([leftOnly()], [silence()], SR),
        /request must be an object/,
      );
    });

    it('defaults the strips that leave a field out', () => {
      const two = (faderDb?: number) =>
        mixStereo({
          strips: [
            { left: leftOnly(), right: silence(), faderDb },
            { left: leftOnly(), right: silence() },
          ],
          sampleRate: SR,
        });
      // The control: the one entry given is applied, so the equality is not vacuous.
      expect(Array.from(two(-6).left)).not.toEqual(Array.from(two(0).left));
      expect(Array.from(two(undefined).left)).toEqual(Array.from(two(0).left));
    });

    it('treats undefined and null fields as absent', () => {
      const plain = oneStrip(SR);
      const absent = oneStrip(SR, { muted: undefined, pan: null, width: undefined });
      expect(Array.from(absent.left)).toEqual(Array.from(plain.left));
    });
  });

  describe('Mixer.setSurroundPan', () => {
    it.each([
      ['azimuth', '45'],
      ['divergence', [0.25]],
      ['lfe', 'x'],
    ])('refuses %s = %j', (key, value) => {
      const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
        sampleRate: SR,
        maxBlockSize: BLOCK,
      });
      try {
        const vocal = mixer.stripById('vocal');
        expectInvalid(
          () => mixer.setSurroundPan(vocal, { [key]: value } as never),
          new RegExp(key),
        );
      } finally {
        mixer.destroy();
      }
    });

    it('refuses a non-object pan and still accepts absent fields', () => {
      const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
        sampleRate: SR,
        maxBlockSize: BLOCK,
      });
      try {
        const vocal = mixer.stripById('vocal');
        expectInvalid(
          () => mixer.setSurroundPan(vocal, 5 as never),
          /^Mixer.setSurroundPan: request must be an object$/,
        );
        expect(() =>
          mixer.setSurroundPan(vocal, { azimuth: undefined, lfe: null } as never),
        ).not.toThrow();
      } finally {
        mixer.destroy();
      }
    });
  });

  describe('flags on a property bag', () => {
    it('refuses a string for a boolean bounce flag', () => {
      const engine = new RealtimeEngine({ sampleRate: SR, maxBlockSize: 128 });
      try {
        expectInvalid(
          () =>
            engine.bounceOffline({
              totalFrames: 128,
              blockSize: 128,
              numChannels: 2,
              normalizeLufs: 'false' as never,
            }),
          /normalizeLufs/,
        );
      } finally {
        engine.destroy();
      }
    });
  });
});

describe('sample-rate range', () => {
  beforeAll(async () => {
    await init();
  });

  it.each([7999, 384001, 44100.5])('mixStereo refuses %s', (rate) => {
    expectInvalid(() => oneStrip(rate), /sampleRate/);
  });

  it.each([7999, 384001])('Mixer.fromJson refuses %s', (rate) => {
    expectInvalid(
      () =>
        Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
          sampleRate: rate,
          maxBlockSize: BLOCK,
        }),
      /sampleRate/,
    );
  });

  it('accepts the range ends', () => {
    expect(() => oneStrip(8000)).not.toThrow();
    const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
      sampleRate: 384000,
      maxBlockSize: BLOCK,
    });
    mixer.destroy();
  });
});

describe('mono repair and dynamics sampleRate', () => {
  beforeAll(async () => {
    await init();
  });

  const tone = (): Float32Array => {
    const out = new Float32Array(4096);
    for (let i = 0; i < out.length; i++) {
      out[i] = 0.5 * Math.sin((2 * Math.PI * 220 * i) / 22050);
    }
    out[1000] = 1;
    return out;
  };

  it('refuses an omitted rate on the mono repair entries and on the compressor', () => {
    const samples = tone();
    for (const entry of [
      masteringRepairDeclick,
      masteringRepairDeclip,
      masteringRepairDecrackle,
      masteringDynamicsCompressor,
    ] as unknown as Array<(request: unknown) => unknown>) {
      expect(() => entry({ samples })).toThrow(TypeError);
    }
  });
});

describe('RealtimeEngine.loadSoundFont failures', () => {
  beforeAll(async () => {
    await init();
  });

  it('refuses empty bytes as a RangeError and answers unparsable bytes with InvalidFormat', () => {
    const engine = new RealtimeEngine({ sampleRate: SR, maxBlockSize: 128 });
    try {
      let empty: unknown;
      try {
        engine.loadSoundFont(new Uint8Array(0));
      } catch (error) {
        empty = error;
      }
      expect(empty).toBeInstanceOf(RangeError);
      let junk: unknown;
      try {
        engine.loadSoundFont(new Uint8Array([9, 9, 9]));
      } catch (error) {
        junk = error;
      }
      expect(junk).toMatchObject({ code: ErrorCode.InvalidFormat });
    } finally {
      engine.destroy();
    }
  });
});

describe('mixStereo and Mixer.fromJson share the supported sample-rate range', () => {
  // No inserts, so no band frequency can fall above Nyquist at the lowest rate.
  const plainScene = JSON.stringify({
    version: 1,
    buses: [{ id: 'master', role: 'master' }],
    strips: [{ id: 'vocal' }],
    connections: [{ source: 'vocal', destination: 'master' }],
  });

  beforeAll(async () => {
    await init();
  });

  it.each([7999, 384001, 1e6])('refuses sampleRate %d', (rate) => {
    expect(() => oneStrip(rate)).toThrow(/sampleRate/);
    expect(() => Mixer.fromJson(plainScene, { sampleRate: rate, maxBlockSize: BLOCK })).toThrow(
      /sampleRate/,
    );
  });

  it.each([8000, 48000, 384000])('accepts sampleRate %d', (rate) => {
    expect(() => oneStrip(rate)).not.toThrow();
    const mixer = Mixer.fromJson(plainScene, { sampleRate: rate, maxBlockSize: BLOCK });
    mixer.destroy();
  });
});
