import { describe, expect, it } from 'vitest';
import { RealtimeVoiceChanger, realtimeVoiceChangerPresetConfig } from '../src/index.js';

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

  it('still applies a nested preset unchanged', () => {
    const vc = new RealtimeVoiceChanger({ sampleRate: 48000 });
    vc.setConfig({ schemaVersion: 1, dsp: { retune: { semitones: 3 } } });
    const applied = JSON.parse(vc.configJson()).dsp;
    expect(applied.retune.semitones).toBeCloseTo(3, 4);
    vc.destroy();
  });
});
