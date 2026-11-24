import { describe, expect, it } from 'vitest';
import {
  ErrorCode,
  Mixer,
  masteringInsertParamNames,
  meteringTruePeakDbtp,
  mixingScenePreset,
  mixingScenePresetNames,
  mixStereo,
} from '../src/index.js';

describe('mixing native binding', () => {
  it('exposes scene presets and renders a stereo mix with input trim', () => {
    expect(mixingScenePresetNames()).toContain('vocalReverbSend');
    expect(JSON.stringify(mixingScenePreset('vocalReverbSend'))).toContain('"vocal"');

    const left = new Float32Array([1, 1]);
    const right = new Float32Array([0, 0]);
    const result = mixStereo({
      strips: [{ left, right, inputTrimDb: 6.0206, faderDb: -6.0206 }],
      sampleRate: 48000,
    });
    expect(result.left).toBeInstanceOf(Float32Array);
    expect(result.right).toBeInstanceOf(Float32Array);
    // +6.02 dB trim and -6.02 dB fader cancel to unity. With the Balance pan law
    // no longer attenuating a centered signal by 3 dB, the output passes through
    // at unity instead of sqrt(0.5).
    expect(result.left[0]).toBeCloseTo(1.0, 2);
    expect(result.left[1]).toBeCloseTo(1.0, 2);
    expect(Array.from(result.right)).toEqual([0, 0]);
    expect(result.meters).toHaveLength(1);
    expect(result.meters[0].numChannels).toBe(2);
    expect(result.meters[0].peakDb).toHaveLength(2);
    expect(Number.isFinite(result.meters[0].peakDb[0])).toBe(true);
    expect(Object.keys(result.meters[0])).not.toContain('peakDbL');
    expect(typeof result.meters[0].likelyMonoCompatible).toBe('boolean');
  });

  it('applies pan and panMode independently of each other', () => {
    // A left-only source: Balance leaves it there, stereoPan collapses the pair
    // to a centered mono sum, so the right output tells the two modes apart.
    const frames = 512;
    const left = new Float32Array(frames).fill(1);
    const right = new Float32Array(frames);

    // panMode alone must reach the strip. It used to be read only when `pan`
    // was also supplied, so a preset that changed the mode silently mixed in
    // the default one.
    const modeOnly = mixStereo({
      strips: [{ left, right, panMode: 'stereoPan' }],
      sampleRate: 48000,
    });
    expect(modeOnly.right[frames - 1]).toBeGreaterThan(0.1);

    const balance = mixStereo({ strips: [{ left, right }], sampleRate: 48000 });
    expect(balance.right[frames - 1]).toBe(0);

    // pan alone must not throw: an absent panMode keeps the strip's current
    // mode rather than being rejected as an unknown one.
    const panOnly = mixStereo({ strips: [{ left, right, pan: 0.3 }], sampleRate: 48000 });
    const panWithDefaultMode = mixStereo({
      strips: [{ left, right, pan: 0.3, panMode: 'balance' }],
      sampleRate: 48000,
    });
    expect(Array.from(panOnly.left)).toEqual(Array.from(panWithDefaultMode.left));
    expect(Array.from(panOnly.right)).toEqual(Array.from(panWithDefaultMode.right));

    // A strip that leaves panMode out stays on its current mode while its neighbour sets one.
    const twoStrips = mixStereo({
      strips: [
        { left, right, panMode: 'stereoPan' },
        { left, right },
      ],
      sampleRate: 48000,
    });
    expect(twoStrips.right[frames - 1]).toBeGreaterThan(0.1);
  });

  it('refuses the removed pan-mode spellings and unknown names with a RangeError, and a number with a TypeError', () => {
    const left = new Float32Array([0.5, 0.5]);
    const right = new Float32Array([0.5, 0.5]);
    for (const panMode of ['pan', 'bogus', 'stereo-pan', 'dual-pan', 'stereopan']) {
      expect(() =>
        mixStereo({
          strips: [{ left, right, pan: 0.5, panMode: panMode as never }],
          sampleRate: 48000,
        }),
      ).toThrow(RangeError);
    }
    for (const panMode of [99, 1, {}]) {
      expect(() =>
        mixStereo({
          strips: [{ left, right, pan: 0.5, panMode: panMode as never }],
          sampleRate: 48000,
        }),
      ).toThrow(TypeError);
    }
  });
});

describe('insert param validation', () => {
  it('enumerates the keys an insert processor reads', () => {
    const comp = masteringInsertParamNames('dynamics.compressor');
    expect(comp).toContain('thresholdDb');
    expect(comp).toContain('ratio');
    // Band-indexed processors expose their band{i}.<field> keys.
    expect(masteringInsertParamNames('eq.parametric')).toContain('band0.frequencyHz');
    // An unknown name is refused rather than answered with an empty list.
    expect(() => masteringInsertParamNames('not.a.real.processor' as never)).toThrow(RangeError);
  });

  it('refuses an unknown insert param at its path in a scene document', () => {
    const unknown = JSON.stringify({
      version: 2,
      buses: [{ id: 'master', role: 'master' }],
      strips: [
        {
          id: 'vocal',
          inserts: [
            {
              slot: 'post',
              processor: 'eq.parametric',
              params: { highPassHz: 80 },
            },
          ],
        },
      ],
      connections: [{ source: 'vocal', destination: 'master' }],
    });
    expect(() => Mixer.fromJson(unknown, { sampleRate: 48000, maxBlockSize: 512 })).toThrow(
      expect.objectContaining({
        code: ErrorCode.InvalidFormat,
        message:
          'Mixer.fromJson: unknown key strips[0].inserts[0].params.highPassHz (expected band{i}.attackMs, band{i}.autoThreshold, band{i}.bypassed, band{i}.coeffMode, band{i}.detectorDelayMs, band{i}.dynamic, band{i}.enabled, band{i}.frequencyHz, band{i}.gainDb, band{i}.phase, band{i}.placement, band{i}.proportionalQ, band{i}.proportionalQStrength, band{i}.q, band{i}.rangeDb, band{i}.ratio, band{i}.releaseMs, band{i}.sidechainFreqHz, band{i}.sidechainQ, band{i}.slopeDbOct, band{i}.soloed, band{i}.thresholdDb, band{i}.type)',
      }),
    );

    // A scene whose params are all consumed reports no warnings.
    const clean = JSON.stringify({
      version: 2,
      buses: [{ id: 'master', role: 'master' }],
      strips: [
        {
          id: 'vocal',
          inserts: [
            {
              slot: 'post',
              processor: 'eq.parametric',
              params: { 'band0.frequencyHz': 1000, 'band0.gainDb': 3 },
            },
          ],
        },
      ],
      connections: [{ source: 'vocal', destination: 'master' }],
    });
    expect(Mixer.fromJson(clean, { sampleRate: 48000, maxBlockSize: 512 }).sceneWarnings()).toEqual(
      [],
    );
  });

  it('refuses an unknown scene key by path and exempts $ and x- keys', () => {
    const scene = (extra: Record<string, unknown>, stripExtra: Record<string, unknown>) =>
      JSON.stringify({
        version: 2,
        ...extra,
        buses: [{ id: 'master', role: 'master' }],
        strips: [{ id: 'vocal', ...stripExtra }],
        connections: [{ source: 'vocal', destination: 'master' }],
      });
    expect(() =>
      Mixer.fromJson(scene({}, { faderDB: -3 }), { sampleRate: 48000, maxBlockSize: 512 }),
    ).toThrow(
      expect.objectContaining({
        code: ErrorCode.InvalidFormat,
        message: expect.stringContaining('unknown key strips[0].faderDB'),
      }),
    );
    const annotated = scene({ $schema: 'x', 'x-note': 1 }, { 'x-color': 'red' });
    expect(
      Mixer.fromJson(annotated, { sampleRate: 48000, maxBlockSize: 512 }).sceneWarnings(),
    ).toEqual([]);
  });

  it('reports the one-shot true peak of a signal whose peak is in its last samples', () => {
    const left = new Float32Array(4096);
    left.set([0.9, -0.9, 0.9, -0.9], left.length - 4);
    const right = new Float32Array(left);
    const result = mixStereo({ strips: [{ left, right }], sampleRate: 48000 });
    expect(result.meters[0].truePeakDbtp[0]).toBeCloseTo(meteringTruePeakDbtp(left, 48000), 4);
    expect(result.meters[0].truePeakDbtp[0]).toBeGreaterThan(-1);
  });

  it("flushes a streaming mixer's meters on request", () => {
    const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
      sampleRate: 48000,
      maxBlockSize: 256,
    });
    try {
      expect(() => mixer.flushMeters()).not.toThrow();
    } finally {
      mixer.destroy();
    }
  });
});
