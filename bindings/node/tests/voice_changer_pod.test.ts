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
});
