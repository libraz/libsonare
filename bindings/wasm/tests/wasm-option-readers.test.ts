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
  mixingScenePresetJson,
  mixStereo,
  RealtimeEngine,
} from '../dist/index.js';
import { assertStripIndex } from './_helpers';

const SR = 48000;
const BLOCK = 512;
const leftOnly = () => new Float32Array(BLOCK).fill(1);
const silence = () => new Float32Array(BLOCK);

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

  describe('mixStereo options', () => {
    it.each([
      ['muted', 'false'],
      ['muted', 0],
      ['pan', '0.3'],
      ['pan', true],
      ['width', '1'],
      ['inputTrimDb', false],
    ])('refuses %s = %j', (key, value) => {
      expectInvalid(
        () => mixStereo([leftOnly()], [silence()], SR, { [key]: value } as never),
        new RegExp(key),
      );
    });

    it('refuses a wrong-typed element of a per-strip array by indexed name', () => {
      expectInvalid(
        () =>
          mixStereo([leftOnly(), leftOnly()], [silence(), silence()], SR, {
            faderDb: [0, '-6'],
          } as never),
        /faderDb\[1\]/,
      );
    });

    it.each([
      ['inputTrimDb', [0, -3]],
      ['faderDb', [0, -6]],
      ['pan', [0, 0.5]],
      ['panMode', ['balance', 'dual-pan']],
      ['width', [1, 0.5]],
      ['muted', [false, true]],
    ])('refuses a %s array longer than the strip list', (key, value) => {
      expectInvalid(
        () => mixStereo([leftOnly()], [silence()], SR, { [key]: value } as never),
        new RegExp(`mixStereo: '${key}' has more entries than strips \\(1\\)`),
      );
    });

    it('defaults the strips a shorter per-strip array leaves out', () => {
      const two = (faderDb: number[]) =>
        mixStereo([leftOnly(), leftOnly()], [silence(), silence()], SR, { faderDb });
      // The control: the one entry given is applied, so the equality is not vacuous.
      expect(Array.from(two([-6]).left)).not.toEqual(Array.from(two([0, 0]).left));
      expect(Array.from(two([-6]).left)).toEqual(Array.from(two([-6, 0]).left));
    });

    it('refuses a non-object options argument', () => {
      expectInvalid(
        () => mixStereo([leftOnly()], [silence()], SR, 'muted' as never),
        /options must be an object/,
      );
    });

    it('treats undefined and null fields as absent', () => {
      const plain = mixStereo([leftOnly()], [silence()], SR);
      const absent = mixStereo([leftOnly()], [silence()], SR, {
        muted: undefined,
        pan: null,
        width: undefined,
      } as never);
      expect(Array.from(absent.left)).toEqual(Array.from(plain.left));
    });
  });

  describe('Mixer.setSurroundPan', () => {
    it.each([
      ['azimuth', '45'],
      ['elevation', true],
      ['divergence', [0.25]],
      ['lfe', 'x'],
      ['distance', {}],
    ])('refuses %s = %j', (key, value) => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), SR, BLOCK);
      try {
        const vocal = mixer.stripById('vocal');
        assertStripIndex(vocal, 'vocal');
        expectInvalid(
          () => mixer.setSurroundPan(vocal, { [key]: value } as never),
          new RegExp(key),
        );
      } finally {
        mixer.delete();
      }
    });

    it('refuses a non-object pan and still accepts absent fields', () => {
      const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), SR, BLOCK);
      try {
        const vocal = mixer.stripById('vocal');
        assertStripIndex(vocal, 'vocal');
        expectInvalid(() => mixer.setSurroundPan(vocal, 5 as never), /pan must be an object/);
        expect(() =>
          mixer.setSurroundPan(vocal, { azimuth: undefined, lfe: null } as never),
        ).not.toThrow();
      } finally {
        mixer.delete();
      }
    });
  });

  describe('flags on a property bag', () => {
    it('refuses a string for a boolean bounce flag', () => {
      const engine = new RealtimeEngine(SR, 128);
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
    expectInvalid(() => mixStereo([leftOnly()], [silence()], rate), /sampleRate/);
    expectInvalid(
      () => mixStereo({ leftChannels: [leftOnly()], rightChannels: [silence()], sampleRate: rate }),
      /sampleRate/,
    );
  });

  it.each([7999, 384001])('Mixer.fromSceneJson refuses %s', (rate) => {
    expectInvalid(
      () => Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), rate, BLOCK),
      /sampleRate/,
    );
  });

  it('accepts the range ends', () => {
    expect(() => mixStereo([leftOnly()], [silence()], 8000)).not.toThrow();
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 384000, BLOCK);
    mixer.delete();
  });
});

describe('mono repair and dynamics default sampleRate', () => {
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
  const same = (a: Float32Array, b: Float32Array) => {
    expect(a.length).toBe(b.length);
    expect(Array.from(a)).toEqual(Array.from(b));
  };

  it('resolves an omitted rate to 22050 on every entry shape', () => {
    const samples = tone();
    same(
      masteringRepairDeclick({ samples }),
      masteringRepairDeclick({ samples, sampleRate: 22050 }),
    );
    same(masteringRepairDeclick(samples), masteringRepairDeclick(samples, 22050));
    same(masteringRepairDeclip({ samples }), masteringRepairDeclip({ samples, sampleRate: 22050 }));
    same(masteringRepairDeclip(samples), masteringRepairDeclip(samples, 22050));
    same(
      masteringRepairDecrackle({ samples }),
      masteringRepairDecrackle({ samples, sampleRate: 22050 }),
    );
    const compressed = masteringDynamicsCompressor({ samples });
    const explicit = masteringDynamicsCompressor({ samples, sampleRate: 22050 });
    same(compressed.samples, explicit.samples);
  });
});

describe('RealtimeEngine.loadSoundFont failures', () => {
  beforeAll(async () => {
    await init();
  });

  it('refuses empty bytes as a RangeError and answers unparsable bytes with InvalidFormat', () => {
    const engine = new RealtimeEngine(SR, 128);
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

describe('mixStereo and Mixer.fromSceneJson share the supported sample-rate range', () => {
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
    expect(() => mixStereo([leftOnly()], [silence()], rate)).toThrow(/sampleRate/);
    expect(() => Mixer.fromSceneJson(plainScene, rate, BLOCK)).toThrow(/sampleRate/);
  });

  it.each([8000, 48000, 384000])('accepts sampleRate %d', (rate) => {
    expect(() => mixStereo([leftOnly()], [silence()], rate)).not.toThrow();
    const mixer = Mixer.fromSceneJson(plainScene, rate, BLOCK);
    mixer.destroy();
  });
});
