import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  RealtimeVoiceChanger,
  type RealtimeVoiceChangerPodConfig,
  realtimeVoiceChangerPresetConfig,
  realtimeVoiceChangerPresetJson,
  realtimeVoiceChangerPresetNames,
} from '../src/index';

describe('RealtimeVoiceChanger flat POD setConfig', () => {
  beforeAll(async () => {
    await init();
  });

  it('round-trips every field of a flat preset POD', () => {
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
      voiceEngineEngine: 'psola',
      voiceEngineF0MinHz: 90,
      pitchCorrectionMode: 'auto',
      pitchCorrectionAmount: 0.65,
      pitchCorrectionSpeedMs: 35,
      pitchCorrectionKeyRoot: 7,
      pitchCorrectionScale: 'natural_minor',
      pitchCorrectionRobotOctave: -1,
      harmonyVoices: 2,
      harmonyStep1: 3,
      harmonyStep2: -5,
      harmonyStep3: 6,
      harmonyLevel: 0.45,
      harmonyLeadLevel: 0.85,
      harmonyDetuneCents: 9,
      harmonyDelayMs: 17,
      vocoderEnabled: true,
      vocoderBands: 24,
      vocoderAttackMs: 4,
      vocoderReleaseMs: 80,
      vocoderSibilance: 0.55,
      vocoderCarrierSource: 'external',
      vocoderCarrierWave: 'ensemble',
      vocoderEnsembleDetuneCents: 15,
      vocoderLevel: 0.75,
      characterType: 'bbd_chorus',
      characterAmount: 0.35,
      characterTone: -0.4,
      reverbVariationVariation: 'tempo_delay',
      reverbVariationDelayMs: 420,
      reverbVariationFeedback: 0.55,
      reverbVariationTempoBpm: 90,
      reverbVariationDivision: 'dotted_eighth',
      midiRelease: 'input',
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
          mode: pod.formantMode,
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
        voiceEngine: { engine: pod.voiceEngineEngine, f0MinHz: f32(pod.voiceEngineF0MinHz) },
        pitchCorrection: {
          mode: pod.pitchCorrectionMode,
          amount: f32(pod.pitchCorrectionAmount),
          speedMs: f32(pod.pitchCorrectionSpeedMs),
          keyRoot: pod.pitchCorrectionKeyRoot,
          scale: pod.pitchCorrectionScale,
          robotOctave: pod.pitchCorrectionRobotOctave,
        },
        harmony: {
          voices: pod.harmonyVoices,
          steps: [pod.harmonyStep1, pod.harmonyStep2, pod.harmonyStep3],
          level: f32(pod.harmonyLevel),
          leadLevel: f32(pod.harmonyLeadLevel),
          detuneCents: f32(pod.harmonyDetuneCents),
          delayMs: f32(pod.harmonyDelayMs),
        },
        vocoder: {
          enabled: pod.vocoderEnabled,
          bands: pod.vocoderBands,
          attackMs: f32(pod.vocoderAttackMs),
          releaseMs: f32(pod.vocoderReleaseMs),
          sibilance: f32(pod.vocoderSibilance),
          carrierSource: pod.vocoderCarrierSource,
          carrierWave: pod.vocoderCarrierWave,
          ensembleDetuneCents: f32(pod.vocoderEnsembleDetuneCents),
          level: f32(pod.vocoderLevel),
        },
        character: {
          type: pod.characterType,
          amount: f32(pod.characterAmount),
          tone: f32(pod.characterTone),
        },
        reverbVariation: {
          variation: pod.reverbVariationVariation,
          delayMs: f32(pod.reverbVariationDelayMs),
          feedback: f32(pod.reverbVariationFeedback),
          tempoBpm: f32(pod.reverbVariationTempoBpm),
          division: pod.reverbVariationDivision,
        },
        midi: { release: pod.midiRelease },
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

  it('carries the formant mode, fixed once the changer is prepared', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutral-monitor');
    expect(pod?.formantMode).toBe('relative');
    if (!pod) {
      return;
    }
    const grain: RealtimeVoiceChangerPodConfig = { ...pod, voiceEngineEngine: 'grain' };
    const relative = new RealtimeVoiceChanger(grain);
    const absolute = new RealtimeVoiceChanger({
      ...grain,
      formantMode: 'absolute',
      formantFactor: 1.1,
    });
    try {
      relative.prepare(48000, 128, 1);
      absolute.prepare(48000, 128, 1);
      // One analysis frame (1024 samples at 48 kHz) more than the relative chain.
      expect(absolute.latencySamples()).toBe(relative.latencySamples() + 1024);
      absolute.setConfig({ ...grain, formantMode: 'absolute', formantFactor: 1.2 });
      expect(() => absolute.setConfig({ ...grain, formantMode: 'relative' })).toThrow();
      expect(() => relative.setConfig({ ...grain, formantMode: 'absolute' })).toThrow();
    } finally {
      relative.delete();
      absolute.delete();
    }
  });

  it('refuses an unreachable absolute warp and names the formant factor range', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutral-monitor');
    if (!pod) {
      return;
    }
    const unreachable: RealtimeVoiceChangerPodConfig = {
      ...pod,
      voiceEngineEngine: 'grain',
      formantMode: 'absolute',
      retuneSemitones: -9,
    };
    expect(() => new RealtimeVoiceChanger(unreachable)).toThrow(/\[0\.55, 0\.9811\]/);
    expect(() => new RealtimeVoiceChanger(unreachable)).toThrow(
      /limited by formantFactor's \[0\.55, 1\.65\]/,
    );
  });

  it('refuses an out-of-range field instead of clamping it and keeps the previous config', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutral-monitor');
    if (!pod) {
      return;
    }
    const changer = new RealtimeVoiceChanger(pod);
    try {
      const before = changer.configJson();
      expect(() => changer.setConfig({ ...pod, formantFactor: 3.0 })).toThrow(RangeError);
      expect(() => changer.setConfig({ ...pod, formantFactor: 3.0 })).toThrow(
        /formant\.factor out of range \[0\.55, 1\.65\], got 3/,
      );
      expect(() => changer.setPodConfig({ ...pod, limiterCeilingDb: 0.5 })).toThrow(RangeError);
      expect(() => changer.setPodConfig({ ...pod, limiterCeilingDb: 0.5 })).toThrow(
        /limiter\.ceilingDb out of range/,
      );
      expect(changer.configJson()).toBe(before);
    } finally {
      changer.delete();
    }
    expect(() => new RealtimeVoiceChanger({ ...pod, formantFactor: 3.0 })).toThrow(RangeError);
    expect(() => new RealtimeVoiceChanger({ ...pod, formantFactor: 3.0 })).toThrow(
      /formant\.factor out of range/,
    );
  });

  it('requires formantMode on a flat POD in the constructor and in setConfig', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutral-monitor');
    if (!pod) {
      return;
    }
    const { formantMode: _omitted, ...partial } = pod;
    const podWithoutMode = partial as unknown as RealtimeVoiceChangerPodConfig;
    expect(() => new RealtimeVoiceChanger(podWithoutMode)).toThrow(TypeError);
    expect(() => new RealtimeVoiceChanger(podWithoutMode)).toThrow(/formantMode is required/);
    const changer = new RealtimeVoiceChanger(pod);
    try {
      expect(() => changer.setConfig(podWithoutMode)).toThrow(TypeError);
      expect(() =>
        changer.setConfig({ ...pod, formantMode: 3 } as unknown as RealtimeVoiceChangerPodConfig),
      ).toThrow(/formantMode must be a string/);
      // A preset id and a nested preset document keep their own defaults.
      expect(() => changer.setConfig('neutral-monitor')).not.toThrow();
    } finally {
      changer.delete();
    }
  });

  it('refuses a POD without formantMode, as it refuses any partial POD', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutral-monitor');
    if (!pod) {
      return;
    }
    const { formantMode: _omitted, ...partial } = pod;
    const changer = new RealtimeVoiceChanger(pod);
    try {
      expect(() =>
        changer.setPodConfig(partial as unknown as RealtimeVoiceChangerPodConfig),
      ).toThrow(/formantMode/);
    } finally {
      changer.delete();
    }
  });

  it('requires every voice transformer key in both flat POD paths', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutral-monitor');
    const keys = Object.keys(pod);
    expect(keys).toHaveLength(71);
    const changer = new RealtimeVoiceChanger(pod);
    try {
      for (const key of keys) {
        const { [key as keyof RealtimeVoiceChangerPodConfig]: _omitted, ...partial } = pod;
        const incomplete = partial as unknown as RealtimeVoiceChangerPodConfig;
        expect(() => changer.setPodConfig(incomplete), key).toThrow(new RegExp(key));
        // Without retuneSemitones the object is no longer recognised as a flat POD.
        if (key !== 'retuneSemitones') {
          expect(() => new RealtimeVoiceChanger(incomplete), key).toThrow(new RegExp(key));
        }
      }
    } finally {
      changer.delete();
    }
  });

  it('refuses a wrong-typed or out-of-domain voice transformer value by name', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutral-monitor');
    const changer = new RealtimeVoiceChanger(pod);
    const bad = (patch: Record<string, unknown>): RealtimeVoiceChangerPodConfig =>
      ({ ...pod, ...patch }) as unknown as RealtimeVoiceChangerPodConfig;
    try {
      const before = changer.configJson();
      expect(() => changer.setPodConfig(bad({ characterType: 'distortion' }))).toThrow(RangeError);
      expect(() => changer.setPodConfig(bad({ characterType: 2 }))).toThrow(TypeError);
      expect(() => changer.setPodConfig(bad({ vocoderEnabled: 1 }))).toThrow(TypeError);
      expect(() => changer.setPodConfig(bad({ harmonyStep2: 1.5 }))).toThrow(RangeError);
      expect(() => changer.setPodConfig(bad({ harmonyStep1: 8 }))).toThrow(RangeError);
      expect(() => changer.setPodConfig(bad({ characterAmount: Number.NaN }))).toThrow(RangeError);
      expect(() => changer.setPodConfig(bad({ pitchCorrectionAmount: 1.5 }))).toThrow(RangeError);
      expect(changer.configJson()).toBe(before);
    } finally {
      changer.delete();
    }
  });

  it('refuses to change a creation-time field on a live changer', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutral-monitor');
    const changer = new RealtimeVoiceChanger(pod);
    try {
      changer.prepare(48000, 128, 1);
      expect(() => changer.setPodConfig({ ...pod, voiceEngineEngine: 'grain' })).toThrow(
        /fixed at/,
      );
      expect(() => changer.setPodConfig({ ...pod, voiceEngineF0MinHz: 120 })).toThrow(/fixed at/);
      expect(() => changer.setPodConfig({ ...pod, vocoderBands: 24 })).toThrow(/fixed at/);
    } finally {
      changer.delete();
    }
  });

  it('refuses pitch correction on the grain engine', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutral-monitor');
    expect(
      () =>
        new RealtimeVoiceChanger({
          ...pod,
          voiceEngineEngine: 'grain',
          pitchCorrectionMode: 'auto',
        }),
    ).toThrow(/psola/);
  });

  it('describes every preset as a complete flat POD', () => {
    for (const id of realtimeVoiceChangerPresetNames()) {
      const pod = realtimeVoiceChangerPresetConfig(id);
      expect(Object.keys(pod), id).toHaveLength(71);
      const changer = new RealtimeVoiceChanger(pod);
      try {
        changer.setPodConfig(pod);
      } finally {
        changer.delete();
      }
    }
  });
});
