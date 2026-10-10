import { describe, expect, it } from 'vitest';
import {
  RealtimeVoiceChanger,
  realtimeVoiceChangerPresetConfig,
  realtimeVoiceChangerPresetJson,
} from '../src/index.js';

describe('RealtimeVoiceChanger flat POD setConfig', () => {
  it('applies every field of a flat preset POD, not just the root fields', () => {
    const pod = realtimeVoiceChangerPresetConfig('bright-idol');
    pod.retuneSemitones = 5;
    const vc = new RealtimeVoiceChanger({ sampleRate: 48000 });
    vc.setConfig(pod);
    const applied = JSON.parse(vc.configJson()).dsp;
    // The mutated field and the preset's sections survive instead of reverting
    // to config defaults, which a flat POD through the nested parser would do.
    expect(applied.retune.semitones).toBeCloseTo(5, 4);
    expect(applied.retune.mix).toBeCloseTo(pod.retuneMix, 4);
    expect(applied.formant.factor).toBeCloseTo(pod.formantFactor, 4);
    expect(applied.eq.presenceDb).toBeCloseTo(pod.eqPresenceDb, 4);
    expect(applied.reverb.mix).toBeCloseTo(pod.reverbMix, 4);
    vc.destroy();
  });

  it('applies a complete nested preset document', () => {
    const vc = new RealtimeVoiceChanger({ sampleRate: 48000 });
    const preset = JSON.parse(realtimeVoiceChangerPresetJson('neutral-monitor'));
    preset.dsp.retune.semitones = 3;
    vc.setConfig(preset);
    const applied = JSON.parse(vc.configJson()).dsp;
    expect(applied.retune.semitones).toBeCloseTo(3, 4);
    vc.destroy();
  });

  it('rejects a partial nested preset instead of applying defaults', () => {
    const vc = new RealtimeVoiceChanger({ sampleRate: 48000 });
    // @ts-expect-error deliberately partial nested preset; setConfig must reject it, not fill in defaults.
    expect(() => vc.setConfig({ schemaVersion: 1, dsp: { retune: { semitones: 3 } } })).toThrow(
      /missing field|field must/i,
    );
    vc.destroy();
  });

  it('accepts a flat preset POD in the constructor', () => {
    const pod = realtimeVoiceChangerPresetConfig('bright-idol');
    pod.retuneSemitones = -9;
    const vc = new RealtimeVoiceChanger({ sampleRate: 48000, preset: pod });
    expect(JSON.parse(vc.configJson()).dsp.retune.semitones).toBeCloseTo(-9, 4);
    vc.destroy();
  });

  it('carries the formant mode, fixed when the changer is created', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutral-monitor');
    expect(pod.formantMode).toBe('relative');
    const relative = new RealtimeVoiceChanger({ sampleRate: 48000, preset: pod });
    const absolute = new RealtimeVoiceChanger({
      sampleRate: 48000,
      preset: { ...pod, formantMode: 'absolute', formantFactor: 1.1 },
    });
    // One analysis frame (1024 samples at 48 kHz) more than the relative chain.
    expect(absolute.latencySamples()).toBe(relative.latencySamples() + 1024);
    expect(JSON.parse(absolute.configJson()).dsp.formant.mode).toBe('absolute');

    // A live update keeps the mode; one that changes it is refused.
    absolute.setConfig({ ...pod, formantMode: 'absolute', formantFactor: 1.2 });
    expect(() => absolute.setConfig({ ...pod, formantMode: 'relative' })).toThrow(RangeError);
    expect(() => relative.setConfig({ ...pod, formantMode: 'absolute' })).toThrow(RangeError);
    relative.destroy();
    absolute.destroy();
  });

  it('refuses an unreachable absolute warp and names the formant factor range', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutral-monitor');
    const unreachable = { ...pod, formantMode: 'absolute' as const, retuneSemitones: -9 };
    expect(() => new RealtimeVoiceChanger({ sampleRate: 48000, preset: unreachable })).toThrow(
      /\[0\.55, 0\.9811\]/,
    );
    expect(
      () =>
        new RealtimeVoiceChanger({
          sampleRate: 48000,
          preset: { ...pod, formantMode: 'sideways' as never },
        }),
    ).toThrow();
  });

  it('requires formantMode on a flat POD in the constructor and in setConfig', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutral-monitor');
    const { formantMode: _omitted, ...partial } = pod;
    const podWithoutMode = partial as unknown as typeof pod;
    expect(() => new RealtimeVoiceChanger({ sampleRate: 48000, preset: podWithoutMode })).toThrow(
      TypeError,
    );
    expect(() => new RealtimeVoiceChanger({ sampleRate: 48000, preset: podWithoutMode })).toThrow(
      /formantMode is required/,
    );
    const vc = new RealtimeVoiceChanger({ sampleRate: 48000, preset: pod });
    try {
      expect(() => vc.setConfig(podWithoutMode)).toThrow(TypeError);
      expect(() => vc.setConfig({ ...pod, formantMode: 3 } as never)).toThrow(
        /formantMode must be a string/,
      );
      // A preset id keeps its own defaults.
      expect(() => vc.setConfig('neutral-monitor')).not.toThrow();
    } finally {
      vc.destroy();
    }
  });
});
