import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  RealtimeVoiceChanger,
  realtimeVoiceChangerPresetConfig,
  realtimeVoiceChangerPresetJson,
} from '../src/index';

describe('RealtimeVoiceChanger flat POD setConfig', () => {
  beforeAll(async () => {
    await init();
  });

  it('round-trips all 36 fields of a flat preset POD', () => {
    const pod = realtimeVoiceChangerPresetConfig('bright-idol');
    expect(pod).not.toBeNull();
    if (!pod) {
      return;
    }
    Object.assign(pod, {
      inputGainDb: 2.5,
      outputGainDb: -4,
      wetMix: 0.37,
      retuneSemitones: -7,
      retuneMix: 0.61,
      retuneGrainSize: 4096,
      formantFactor: 1.23,
      formantAmount: 0.41,
      formantBody: -0.21,
      formantBrightness: 0.31,
      formantNasal: -0.11,
      eqHighpassHz: 123,
      eqBodyDb: 1.2,
      eqPresenceDb: -2.3,
      eqAirDb: 3.4,
      gateThresholdDb: -41,
      gateAttackMs: 3,
      gateReleaseMs: 456,
      gateRangeDb: 23,
      compressorThresholdDb: -17,
      compressorRatio: 4.5,
      compressorAttackMs: 7,
      compressorReleaseMs: 321,
      compressorMakeupGainDb: 2.5,
      deesserFrequencyHz: 6400,
      deesserThresholdDb: -19,
      deesserRatio: 3.5,
      deesserRangeDb: 4.5,
      reverbMix: 0.27,
      reverbTimeMs: 777,
      reverbDamping: 0.38,
      reverbSeed: 123456,
      limiterCeilingDb: -4.2,
      limiterReleaseMs: 123,
      limiterEnableIspLimiter: false,
      limiterIspCeilingDbtp: -2.7,
    });
    // Construct before prepare so the structural grain size remains the exact
    // requested value. After prepare, grain storage is intentionally fixed for
    // RT safety and configJson reports the active grain size instead (#189).
    const vc = new RealtimeVoiceChanger(pod);
    try {
      const f32 = Math.fround;
      expect(JSON.parse(vc.configJson()).dsp).toEqual({
        inputGainDb: f32(pod.inputGainDb),
        outputGainDb: f32(pod.outputGainDb),
        wetMix: f32(pod.wetMix),
        retune: {
          semitones: f32(pod.retuneSemitones),
          mix: f32(pod.retuneMix),
          grainSize: pod.retuneGrainSize,
        },
        formant: {
          factor: f32(pod.formantFactor),
          amount: f32(pod.formantAmount),
          body: f32(pod.formantBody),
          brightness: f32(pod.formantBrightness),
          nasal: f32(pod.formantNasal),
        },
        eq: {
          highpassHz: f32(pod.eqHighpassHz),
          bodyDb: f32(pod.eqBodyDb),
          presenceDb: f32(pod.eqPresenceDb),
          airDb: f32(pod.eqAirDb),
        },
        gate: {
          thresholdDb: f32(pod.gateThresholdDb),
          attackMs: f32(pod.gateAttackMs),
          releaseMs: f32(pod.gateReleaseMs),
          rangeDb: f32(pod.gateRangeDb),
        },
        compressor: {
          thresholdDb: f32(pod.compressorThresholdDb),
          ratio: f32(pod.compressorRatio),
          attackMs: f32(pod.compressorAttackMs),
          releaseMs: f32(pod.compressorReleaseMs),
          makeupGainDb: f32(pod.compressorMakeupGainDb),
        },
        deesser: {
          frequencyHz: f32(pod.deesserFrequencyHz),
          thresholdDb: f32(pod.deesserThresholdDb),
          ratio: f32(pod.deesserRatio),
          rangeDb: f32(pod.deesserRangeDb),
        },
        reverb: {
          mix: f32(pod.reverbMix),
          timeMs: f32(pod.reverbTimeMs),
          damping: f32(pod.reverbDamping),
          seed: pod.reverbSeed,
        },
        limiter: {
          ceilingDb: f32(pod.limiterCeilingDb),
          releaseMs: f32(pod.limiterReleaseMs),
          enableIspLimiter: pod.limiterEnableIspLimiter,
          ispCeilingDbtp: f32(pod.limiterIspCeilingDbtp),
        },
      });
    } finally {
      vc.delete();
    }
  });

  it('applies a complete nested preset document', () => {
    const vc = new RealtimeVoiceChanger('neutral-monitor');
    vc.prepare(48000, 128, 1);
    const preset = JSON.parse(realtimeVoiceChangerPresetJson('neutral-monitor'));
    preset.dsp.retune.semitones = 3;
    vc.setConfig(preset);
    const applied = JSON.parse(vc.configJson()).dsp;
    expect(applied.retune.semitones).toBeCloseTo(3, 4);
    vc.delete();
  });

  it('rejects a partial nested preset instead of applying defaults', () => {
    const vc = new RealtimeVoiceChanger('neutral-monitor');
    vc.prepare(48000, 128, 1);
    expect(() =>
      // @ts-expect-error preset metadata is deliberately omitted; the runtime rejects it
      vc.setConfig({ schemaVersion: 1, dsp: { retune: { semitones: 3 } } }),
    ).toThrow(/missing field|field must/i);
    vc.delete();
  });

  it('accepts a flat preset POD in the constructor', () => {
    const pod = realtimeVoiceChangerPresetConfig('bright-idol');
    expect(pod).not.toBeNull();
    if (!pod) {
      return;
    }
    pod.retuneSemitones = -9;
    const vc = new RealtimeVoiceChanger(pod);
    expect(JSON.parse(vc.configJson()).dsp.retune.semitones).toBeCloseTo(-9, 4);
    vc.delete();
  });
});
