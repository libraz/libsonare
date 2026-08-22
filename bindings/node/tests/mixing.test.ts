import { describe, expect, it } from 'vitest';
import {
  Mixer,
  masteringInsertParamNames,
  mixingScenePresetJson,
  mixingScenePresetNames,
  mixStereo,
} from '../src/index.js';

describe('mixing native binding', () => {
  it('exposes scene presets and renders a stereo mix with input trim', () => {
    expect(mixingScenePresetNames()).toContain('vocalReverbSend');
    expect(mixingScenePresetJson('vocalReverbSend')).toContain('"vocal"');

    const left = new Float32Array([1, 1]);
    const right = new Float32Array([0, 0]);
    const result = mixStereo([left], [right], 48000, { inputTrimDb: 6.0206, faderDb: -6.0206 });
    expect(result.left).toBeInstanceOf(Float32Array);
    expect(result.right).toBeInstanceOf(Float32Array);
    // +6.02 dB trim and -6.02 dB fader cancel to unity. With the Balance pan law
    // no longer attenuating a centered signal by 3 dB, the output passes through
    // at unity instead of sqrt(0.5).
    expect(result.left[0]).toBeCloseTo(1.0, 2);
    expect(result.left[1]).toBeCloseTo(1.0, 2);
    expect(Array.from(result.right)).toEqual([0, 0]);
    expect(result.meters).toHaveLength(1);
    expect(Number.isFinite(result.meters[0].peakDbL)).toBe(true);
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
    const modeOnly = mixStereo([left], [right], 48000, { panMode: 'stereoPan' });
    expect(modeOnly.right[frames - 1]).toBeGreaterThan(0.1);

    const balance = mixStereo([left], [right], 48000, {});
    expect(balance.right[frames - 1]).toBe(0);

    // pan alone must not throw: an absent panMode keeps the strip's current
    // mode rather than being rejected as an unknown one.
    const panOnly = mixStereo([left], [right], 48000, { pan: 0.3 });
    const panWithDefaultMode = mixStereo([left], [right], 48000, {
      pan: 0.3,
      panMode: 'balance',
    });
    expect(Array.from(panOnly.left)).toEqual(Array.from(panWithDefaultMode.left));
    expect(Array.from(panOnly.right)).toEqual(Array.from(panWithDefaultMode.right));

    // A per-strip array shorter than the strip list leaves the remaining strips
    // on their current mode instead of failing on the undefined entry.
    const twoStrips = mixStereo([left, left], [right, right], 48000, {
      panMode: ['stereoPan'],
    });
    expect(twoStrips.right[frames - 1]).toBeGreaterThan(0.1);
  });
});

describe('insert param validation', () => {
  it('enumerates the keys an insert processor reads', () => {
    const comp = masteringInsertParamNames('dynamics.compressor');
    expect(comp).toContain('thresholdDb');
    expect(comp).toContain('ratio');
    // Band-indexed processors expose their band{i}.<field> keys.
    expect(masteringInsertParamNames('eq.parametric')).toContain('band0.frequencyHz');
    // Unknown name -> empty list (no throw).
    expect(masteringInsertParamNames('not.a.real.processor')).toEqual([]);
  });

  it('surfaces silently-ignored insert params as scene warnings', () => {
    // eq.parametric reads only band{i}.* fields, so flat keys take no effect.
    const ignored = JSON.stringify({
      version: 1,
      buses: [{ id: 'master', role: 'master' }],
      strips: [
        {
          id: 'vocal',
          inserts: [
            {
              slot: 'post',
              processor: 'eq.parametric',
              params: JSON.stringify({ highPassHz: 80, presenceDb: 4 }),
            },
          ],
        },
      ],
      connections: [{ source: 'vocal', destination: 'master' }],
    });
    const mixer = Mixer.fromSceneJson(ignored, 48000, 512);
    const warnings = mixer.sceneWarnings();
    expect(warnings.length).toBe(1);
    expect(warnings[0]).toContain('eq.parametric');
    expect(warnings[0]).toContain('highPassHz');
    expect(warnings[0]).toContain('presenceDb');

    // A scene whose params are all consumed reports no warnings.
    const clean = JSON.stringify({
      version: 1,
      buses: [{ id: 'master', role: 'master' }],
      strips: [
        {
          id: 'vocal',
          inserts: [
            {
              slot: 'post',
              processor: 'eq.parametric',
              params: JSON.stringify({ 'band0.frequencyHz': 1000, 'band0.gainDb': 3 }),
            },
          ],
        },
      ],
      connections: [{ source: 'vocal', destination: 'master' }],
    });
    expect(Mixer.fromSceneJson(clean, 48000, 512).sceneWarnings()).toEqual([]);
  });
});
