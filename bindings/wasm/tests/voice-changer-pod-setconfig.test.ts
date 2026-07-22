import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeVoiceChanger, realtimeVoiceChangerPresetConfig } from '../src/index';

describe('RealtimeVoiceChanger flat POD setConfig', () => {
  beforeAll(async () => {
    await init();
  });

  it('applies every field of a flat preset POD, not just the root fields', () => {
    const pod = realtimeVoiceChangerPresetConfig('bright-idol');
    expect(pod).not.toBeNull();
    if (!pod) {
      return;
    }
    pod.retuneSemitones = 5;
    const vc = new RealtimeVoiceChanger('bright-idol');
    vc.prepare(48000, 128, 1);
    vc.setConfig(pod);
    const applied = JSON.parse(vc.configJson()).dsp;
    // The mutated field and the preset's sections survive instead of reverting
    // to config defaults, which a flat POD through the nested parser would do.
    expect(applied.retune.semitones).toBeCloseTo(5, 4);
    expect(applied.retune.mix).toBeCloseTo(pod.retuneMix, 4);
    expect(applied.formant.factor).toBeCloseTo(pod.formantFactor, 4);
    expect(applied.eq.presenceDb).toBeCloseTo(pod.eqPresenceDb, 4);
    expect(applied.reverb.mix).toBeCloseTo(pod.reverbMix, 4);
    vc.delete();
  });

  it('still applies a nested preset unchanged', () => {
    const vc = new RealtimeVoiceChanger('neutral-monitor');
    vc.prepare(48000, 128, 1);
    vc.setConfig({ schemaVersion: 1, dsp: { retune: { semitones: 3 } } });
    const applied = JSON.parse(vc.configJson()).dsp;
    expect(applied.retune.semitones).toBeCloseTo(3, 4);
    vc.delete();
  });
});
