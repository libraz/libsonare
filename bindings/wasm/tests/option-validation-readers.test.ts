/**
 * An argument the facade forwards to embind positionally is read through one typed reader: a
 * boolean is never a truthy coercion, an index array is never an empty cut list, an enum spelling
 * never resolves an inherited property, and a present-but-wrong option is refused rather than
 * replaced by its default.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { resolveEnumOrdinal } from '../src/codes';
import {
  chirp,
  createVocalEditSession,
  detectKey,
  fixFrames,
  hpss,
  init,
  Mixer,
  masteringRepairDenoiseClassical,
  mixingScenePresetJson,
  Project,
  RealtimeEngine,
  remix,
  remixAlignedIntervals,
  spectralEdit,
  zeroCrossings,
} from '../src/index';
import { sine } from './_helpers';

const SR = 22050;
const samples = sine(440, 0.5, { sampleRate: SR });

beforeAll(async () => {
  await init();
});

describe('boolean options', () => {
  it('refuse a non-boolean with TypeError naming the field', () => {
    expect(() => hpss({ samples, sampleRate: SR, hardMask: 'no' as never })).toThrow(
      /hardMask must be a boolean/,
    );
    expect(() => detectKey({ samples, sampleRate: SR, useHpss: 'no' as never })).toThrow(TypeError);
    expect(() => chirp({ linear: 'false' as never })).toThrow(TypeError);
    expect(() => zeroCrossings({ samples, pad: 'false' as never })).toThrow(TypeError);
    expect(() => fixFrames({ frames: new Int32Array([1, 2]), pad: 'no' as never })).toThrow(
      TypeError,
    );
    expect(() =>
      remix({ samples, sampleRate: SR, intervals: [0, 100], alignZeros: 'no' as never }),
    ).toThrow(TypeError);
    expect(() =>
      remixAlignedIntervals({
        samples,
        sampleRate: SR,
        intervals: [0, 100],
        alignZeros: 0 as never,
      }),
    ).toThrow(TypeError);
  });

  it('pass a real boolean and treat undefined as the default', () => {
    expect(() => chirp({ linear: false })).not.toThrow();
    expect(() => chirp({})).not.toThrow();
    expect(() => hpss({ samples, sampleRate: SR, hardMask: true })).not.toThrow();
  });

  it('are refused on handle setters before any native state changes', () => {
    const project = Project.create();
    expect(() => project.setTrackMute(1, 'false' as never)).toThrow(TypeError);
    expect(() => project.setTrackSolo(1, 1 as never)).toThrow(TypeError);
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), SR, 512);
    try {
      expect(() => mixer.setMuted(0, 'false' as never)).toThrow(TypeError);
      expect(() => mixer.setSoloed(0, 'false' as never)).toThrow(TypeError);
      expect(() => mixer.setSoloSafe(0, 'false' as never)).toThrow(TypeError);
      expect(() => mixer.setPolarityInvert(0, true, 'x' as never)).toThrow(TypeError);
      expect(() => mixer.configureMeter('on' as never)).toThrow(TypeError);
    } finally {
      mixer.delete();
    }
    const engine = new RealtimeEngine(48000, 128);
    try {
      expect(() => engine.setLoop(0, 4, 'off' as never)).toThrow(TypeError);
      expect(() => engine.armCapture('yes' as never)).toThrow(TypeError);
      expect(() => engine.setInputMonitor('yes' as never)).toThrow(TypeError);
      expect(() =>
        engine.setCapturePunch({ startSample: 0, endSample: 10, enabled: 'false' as never }),
      ).toThrow(TypeError);
      expect(() =>
        engine.renderOffline({ channels: [new Float32Array(8)], finalize: 1 as never }),
      ).toThrow(TypeError);
    } finally {
      engine.destroy();
    }
  });
});

describe('index arrays', () => {
  it('refuse a non-array-like intervals value instead of cutting nothing', () => {
    expect(() => remix({ samples, sampleRate: SR, intervals: -1 as never })).toThrow(TypeError);
    expect(() => remix({ samples, sampleRate: SR, intervals: null as never })).toThrow(TypeError);
    expect(() =>
      remixAlignedIntervals({ samples, sampleRate: SR, intervals: 1.5 as never }),
    ).toThrow(TypeError);
    expect(() => remix({ samples, sampleRate: SR, intervals: {} as never })).toThrow(TypeError);
  });
});

describe('enum ordinals', () => {
  const values = { linear: 0, hold: 2 } as const;

  it('never resolve an inherited property name', () => {
    expect(() => resolveEnumOrdinal('constructor', values, 'curve')).toThrow(RangeError);
    expect(() => resolveEnumOrdinal('__proto__', values, 'curve')).toThrow(RangeError);
    expect(resolveEnumOrdinal('hold', values, 'curve')).toBe(2);
    expect(resolveEnumOrdinal(0, values, 'curve')).toBe(0);
  });

  it('refuse a wrong type with TypeError and an unknown value with RangeError', () => {
    expect(() => resolveEnumOrdinal(true, values, 'curve')).toThrow(TypeError);
    expect(() => resolveEnumOrdinal(null, values, 'curve')).toThrow(TypeError);
    expect(() => resolveEnumOrdinal('nope', values, 'curve')).toThrow(RangeError);
    expect(() => resolveEnumOrdinal(7, values, 'curve')).toThrow(RangeError);
  });
});

describe('detectKey vocabulary', () => {
  it('accepts the spellings Node and Python accept', () => {
    for (const profile of ['krumhansl-schmuckler', 'Temperley', 'KS']) {
      expect(() => detectKey({ samples, sampleRate: SR, profile: profile as never })).not.toThrow();
    }
    expect(() =>
      detectKey({ samples, sampleRate: SR, modes: ['Major', 'minor'] as never }),
    ).not.toThrow();
    expect(() =>
      detectKey({ samples, sampleRate: SR, modes: 'Major-Minor' as never }),
    ).not.toThrow();
  });

  it('refuses a wrong-typed string option by name', () => {
    expect(() => detectKey({ samples, sampleRate: SR, genreHint: 5 as never })).toThrow(
      /genreHint must be a string/,
    );
    expect(() => detectKey({ samples, sampleRate: SR, profile: 'nope' as never })).toThrow(
      RangeError,
    );
  });
});

describe('masteringRepairDenoiseClassical mode', () => {
  it('refuses a non-string mode as TypeError naming the field', () => {
    expect(() => masteringRepairDenoiseClassical(samples, SR, { mode: 3 as never })).toThrow(
      expect.objectContaining({ name: 'TypeError', message: expect.stringMatching(/mode/) }),
    );
  });
});

describe('spectralEdit ops', () => {
  it('refuses a present ops value that is not an array of objects', () => {
    expect(() => spectralEdit({ samples, sampleRate: SR, ops: 'x' as never })).toThrow(TypeError);
    expect(() => spectralEdit({ samples, sampleRate: SR, ops: ['x'] as never })).toThrow(TypeError);
    expect(() => spectralEdit({ samples, sampleRate: SR, ops: [] })).not.toThrow();
  });
});

describe('createVocalEditSession limits', () => {
  it('refuses a present limits value that is not an object', () => {
    expect(() => createVocalEditSession({ samples, sampleRate: SR, limits: -1 as never })).toThrow(
      /limits must be an object/,
    );
  });
});

describe('surround pan', () => {
  it('refuses an out-of-range field instead of clamping it', () => {
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), SR, 512);
    const engine = new RealtimeEngine(48000, 128);
    try {
      for (const pan of [{ azimuth: 200 }, { divergence: 2 }, { lfe: -0.5 }]) {
        expect(() => mixer.setSurroundPan(0, pan)).toThrow(RangeError);
        expect(() => engine.setTrackStripSurroundPan(1, pan)).toThrow(RangeError);
      }
      expect(() => mixer.setSurroundPan(0, { azimuth: 'x' as never })).toThrow(TypeError);
    } finally {
      mixer.delete();
      engine.destroy();
    }
  });

  it('refuses a prepare block size past the realtime ceiling', () => {
    expect(() => new RealtimeEngine(48000, (1 << 20) + 1)).toThrow(RangeError);
  });
});
