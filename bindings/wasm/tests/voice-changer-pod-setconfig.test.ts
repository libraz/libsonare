import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  RealtimeVoiceChanger,
  type RealtimeVoiceChangerConfig,
  type RealtimeVoiceChangerConfigInput,
  realtimeVoiceChangerPresetConfig,
  realtimeVoiceChangerPresetJson,
  realtimeVoiceChangerPresetNames,
} from '../src/index';

const SAMPLE_RATE = 48000;

/** A changer prepared for mono blocks of at most 128 samples. */
function make(config: RealtimeVoiceChangerConfigInput): RealtimeVoiceChanger {
  return new RealtimeVoiceChanger({
    sampleRate: SAMPLE_RATE,
    numChannels: 1,
    maxBlockSize: 128,
    config,
  });
}

describe('RealtimeVoiceChanger flat config setConfig', () => {
  beforeAll(async () => {
    await init();
  });

  it('round-trips every field of a flat preset POD', () => {
    const pod = realtimeVoiceChangerPresetConfig('brightIdol');
    expect(pod).not.toBeNull();
    if (!pod) {
      return;
    }
    Object.assign(pod, {
      inputGainDb: 2.5,
      outputGainDb: -4,
      wetMix: 0.37,
      retunePitchSemitones: -7,
      retuneMix: 0.61,
      retuneGrainSizeSamples: 4096,
      formantFactor: 1.23,
      formantAmount: 0.41,
      formantBody: -0.21,
      formantBrightness: 0.31,
      formantNasal: -0.11,
      eqHighPassHz: 123,
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
      voiceEngine: 'psola',
      voiceEngineF0MinHz: 90,
      pitchCorrectionMode: 'auto',
      pitchCorrectionAmount: 0.65,
      pitchCorrectionSpeedMs: 35,
      pitchCorrectionKeyRoot: 7,
      pitchCorrectionScale: 'naturalMinor',
      pitchCorrectionRobotOctave: -1,
      harmonyVoices: 2,
      harmonySteps: [3, -5, 6],
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
      characterType: 'bbdChorus',
      characterAmount: 0.35,
      characterTone: -0.4,
      reverbVariation: 'tempoDelay',
      reverbVariationDelayMs: 420,
      reverbVariationFeedback: 0.55,
      reverbVariationTempoBpm: 90,
      reverbVariationDivision: 'dottedEighth',
      midiRelease: 'input',
    });
    const vc = make(pod);
    try {
      const f32 = Math.fround;
      expect(JSON.parse(vc.configJson()).dsp).toEqual({
        inputGainDb: f32(pod.inputGainDb),
        outputGainDb: f32(pod.outputGainDb),
        wetMix: f32(pod.wetMix),
        retune: {
          pitchSemitones: f32(pod.retunePitchSemitones),
          mix: f32(pod.retuneMix),
          grainSizeSamples: pod.retuneGrainSizeSamples,
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
          highPassHz: f32(pod.eqHighPassHz),
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
        voiceEngine: { engine: pod.voiceEngine, f0MinHz: f32(pod.voiceEngineF0MinHz) },
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
          steps: [...pod.harmonySteps],
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
          variation: pod.reverbVariation,
          delayMs: f32(pod.reverbVariationDelayMs),
          feedback: f32(pod.reverbVariationFeedback),
          tempoBpm: f32(pod.reverbVariationTempoBpm),
          division: pod.reverbVariationDivision,
        },
        midi: { release: pod.midiRelease },
      });
    } finally {
      vc.destroy();
    }
  });

  it('applies a complete nested preset document', () => {
    const vc = make('neutralMonitor');
    const preset = JSON.parse(realtimeVoiceChangerPresetJson('neutralMonitor'));
    preset.dsp.retune.pitchSemitones = 3;
    vc.setConfig(preset);
    const applied = JSON.parse(vc.configJson()).dsp;
    expect(applied.retune.pitchSemitones).toBeCloseTo(3, 4);
    vc.destroy();
  });

  it('rejects a partial nested preset instead of applying defaults', () => {
    const vc = make('neutralMonitor');
    expect(() =>
      // @ts-expect-error preset metadata is deliberately omitted; the runtime rejects it
      vc.setConfig({ schemaVersion: 4, dsp: { retune: { pitchSemitones: 3 } } }),
    ).toThrow(/missing field|field must/i);
    vc.destroy();
  });

  it('accepts a flat preset POD in the constructor', () => {
    const pod = realtimeVoiceChangerPresetConfig('brightIdol');
    expect(pod).not.toBeNull();
    if (!pod) {
      return;
    }
    pod.retunePitchSemitones = -9;
    const vc = make(pod);
    expect(JSON.parse(vc.configJson()).dsp.retune.pitchSemitones).toBeCloseTo(-9, 4);
    vc.destroy();
  });

  it('carries the formant mode, fixed once the changer is prepared', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutralMonitor');
    expect(pod?.formantMode).toBe('relative');
    if (!pod) {
      return;
    }
    const grain: RealtimeVoiceChangerConfig = { ...pod, voiceEngine: 'grain' };
    const relative = make(grain);
    const absolute = make({
      ...grain,
      formantMode: 'absolute',
      formantFactor: 1.1,
    });
    try {
      // One analysis frame (1024 samples at 48 kHz) more than the relative chain.
      expect(absolute.latencySamples).toBe(relative.latencySamples + 1024);
      absolute.setConfig({ ...grain, formantMode: 'absolute', formantFactor: 1.2 });
      expect(() => absolute.setConfig({ ...grain, formantMode: 'relative' })).toThrow();
      expect(() => relative.setConfig({ ...grain, formantMode: 'absolute' })).toThrow();
    } finally {
      relative.destroy();
      absolute.destroy();
    }
  });

  it('refuses an unreachable absolute warp and names the formant factor range', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutralMonitor');
    if (!pod) {
      return;
    }
    const unreachable: RealtimeVoiceChangerConfig = {
      ...pod,
      voiceEngine: 'grain',
      formantMode: 'absolute',
      retunePitchSemitones: -9,
    };
    expect(() => make(unreachable)).toThrow(/\[0\.55, 0\.9811\]/);
    expect(() => make(unreachable)).toThrow(/limited by formantFactor's \[0\.55, 1\.65\]/);
  });

  it('refuses an out-of-range field instead of clamping it and keeps the previous config', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutralMonitor');
    if (!pod) {
      return;
    }
    const changer = make(pod);
    try {
      const before = changer.configJson();
      expect(() => changer.setConfig({ ...pod, formantFactor: 3.0 })).toThrow(RangeError);
      expect(() => changer.setConfig({ ...pod, formantFactor: 3.0 })).toThrow(
        /formant\.factor out of range \[0\.55, 1\.65\], got 3/,
      );
      expect(() => changer.setConfig({ ...pod, limiterCeilingDb: 0.5 })).toThrow(RangeError);
      expect(() => changer.setConfig({ ...pod, limiterCeilingDb: 0.5 })).toThrow(
        /limiter\.ceilingDb out of range/,
      );
      expect(changer.configJson()).toBe(before);
    } finally {
      changer.destroy();
    }
    expect(() => make({ ...pod, formantFactor: 3.0 })).toThrow(RangeError);
    expect(() => make({ ...pod, formantFactor: 3.0 })).toThrow(/formant\.factor out of range/);
  });

  it('requires formantMode on a flat POD in the constructor and in setConfig', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutralMonitor');
    if (!pod) {
      return;
    }
    const { formantMode: _omitted, ...partial } = pod;
    const podWithoutMode = partial as unknown as RealtimeVoiceChangerConfig;
    expect(() => make(podWithoutMode)).toThrow(TypeError);
    expect(() => make(podWithoutMode)).toThrow(
      /RealtimeVoiceChanger: formantMode must be a string/,
    );
    const changer = make(pod);
    try {
      expect(() => changer.setConfig(podWithoutMode)).toThrow(TypeError);
      expect(() =>
        changer.setConfig({ ...pod, formantMode: 3 } as unknown as RealtimeVoiceChangerConfig),
      ).toThrow(/formantMode must be a string/);
      // A preset id and a nested preset document keep their own defaults.
      expect(() => changer.setConfig('neutralMonitor')).not.toThrow();
    } finally {
      changer.destroy();
    }
  });

  it('refuses a POD without formantMode, as it refuses any partial POD', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutralMonitor');
    if (!pod) {
      return;
    }
    const { formantMode: _omitted, ...partial } = pod;
    const changer = make(pod);
    try {
      expect(() => changer.setConfig(partial as unknown as RealtimeVoiceChangerConfig)).toThrow(
        /formantMode/,
      );
    } finally {
      changer.destroy();
    }
  });

  it('requires every voice transformer key in both flat POD paths', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutralMonitor');
    const keys = Object.keys(pod);
    // 69 fields; the automatic retune grain is the one optional key, absent here.
    expect(keys).toHaveLength(68);
    const changer = make(pod);
    try {
      for (const key of keys) {
        const { [key as keyof RealtimeVoiceChangerConfig]: _omitted, ...partial } = pod;
        const incomplete = partial as unknown as RealtimeVoiceChangerConfig;
        // Without retunePitchSemitones the object is no longer recognised as a flat config.
        if (key === 'retunePitchSemitones') {
          expect(() => changer.setConfig(incomplete), key).toThrow();
          expect(() => make(incomplete), key).toThrow();
          continue;
        }
        expect(() => changer.setConfig(incomplete), key).toThrow(new RegExp(key));
        expect(() => make(incomplete), key).toThrow(new RegExp(key));
      }
    } finally {
      changer.destroy();
    }
  });

  it('refuses a wrong-typed or out-of-domain voice transformer value by name', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutralMonitor');
    const changer = make(pod);
    const bad = (patch: Record<string, unknown>): RealtimeVoiceChangerConfig =>
      ({ ...pod, ...patch }) as unknown as RealtimeVoiceChangerConfig;
    try {
      const before = changer.configJson();
      expect(() => changer.setConfig(bad({ characterType: 'distortion' }))).toThrow(RangeError);
      expect(() => changer.setConfig(bad({ characterType: 2 }))).toThrow(TypeError);
      expect(() => changer.setConfig(bad({ vocoderEnabled: 1 }))).toThrow(TypeError);
      expect(() => changer.setConfig(bad({ harmonySteps: [0, 1.5, 0] }))).toThrow(RangeError);
      expect(() => changer.setConfig(bad({ harmonySteps: [8, 0, 0] }))).toThrow(RangeError);
      expect(() => changer.setConfig(bad({ characterAmount: Number.NaN }))).toThrow(RangeError);
      expect(() => changer.setConfig(bad({ pitchCorrectionAmount: 1.5 }))).toThrow(RangeError);
      expect(changer.configJson()).toBe(before);
    } finally {
      changer.destroy();
    }
  });

  it('refuses to change a creation-time field on a live changer', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutralMonitor');
    const changer = make(pod);
    try {
      expect(() => changer.setConfig({ ...pod, voiceEngine: 'grain' })).toThrow(/fixed at/);
      expect(() => changer.setConfig({ ...pod, voiceEngineF0MinHz: 120 })).toThrow(/fixed at/);
      expect(() => changer.setConfig({ ...pod, vocoderBands: 24 })).toThrow(/fixed at/);
    } finally {
      changer.destroy();
    }
  });

  it('refuses pitch correction on the grain engine', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutralMonitor');
    expect(() =>
      make({
        ...pod,
        voiceEngine: 'grain',
        pitchCorrectionMode: 'auto',
      }),
    ).toThrow(/psola/);
  });

  it('describes every preset as a complete flat POD', () => {
    for (const id of realtimeVoiceChangerPresetNames()) {
      const pod = realtimeVoiceChangerPresetConfig(id);
      expect(Object.keys(pod), id).toHaveLength(68);
      const changer = make(pod);
      try {
        changer.setConfig(pod);
      } finally {
        changer.destroy();
      }
    }
  });
});

// The same key list the other package's suite reads: the record config() returns
// is one shape across the two packages, not a table per surface.
const sharedConfigFixture = JSON.parse(
  readFileSync(
    new URL('../../../tests/conformance/realtime_voice_changer_config_keys.json', import.meta.url),
    'utf8',
  ),
) as { keys: string[]; optional_keys: string[] };
const sharedConfigKeys = sharedConfigFixture.keys;
const sharedOptionalKeys = sharedConfigFixture.optional_keys;

describe('RealtimeVoiceChanger config read', () => {
  beforeAll(async () => {
    await init();
  });

  it('reads back each preset as its preset record, the automatic grain absent', () => {
    for (const name of realtimeVoiceChangerPresetNames()) {
      const changer = make(name);
      try {
        // A preset leaves the retune grain automatic, which reads back as the absent key.
        const config = changer.config();
        expect(config).toEqual(realtimeVoiceChangerPresetConfig(name));
        expect('retuneGrainSizeSamples' in config).toBe(false);
      } finally {
        changer.destroy();
      }
    }
  });

  it('reads an omitted or null retune grain as automatic and refuses 0 by name', () => {
    const base = realtimeVoiceChangerPresetConfig('brightIdol');
    for (const grain of [undefined, null]) {
      const changer = make('neutralMonitor');
      try {
        changer.setConfig({ ...base, retuneGrainSizeSamples: grain as never });
        expect('retuneGrainSizeSamples' in changer.config(), String(grain)).toBe(false);
      } finally {
        changer.destroy();
      }
    }
    const changer = make('neutralMonitor');
    try {
      expect(() => changer.setConfig({ ...base, retuneGrainSizeSamples: 0 })).toThrow(
        new RangeError(
          'RealtimeVoiceChanger.setConfig: retuneGrainSizeSamples must not be 0; omit it for the automatic grain',
        ),
      );
      expect(() => changer.setConfig({ ...base, retuneGrainSizeSamples: '512' as never })).toThrow(
        TypeError,
      );
      changer.setConfig({ ...base, retuneGrainSizeSamples: 1024 });
      const explicit = changer.config();
      expect(explicit.retuneGrainSizeSamples).toBe(1024);
      expect(Object.keys(explicit).sort()).toEqual(
        [...sharedConfigKeys, ...sharedOptionalKeys].sort(),
      );
      const copy = make(explicit);
      try {
        expect(copy.config()).toEqual(explicit);
      } finally {
        copy.destroy();
      }
    } finally {
      changer.destroy();
    }
  });

  it('reads back the key set the other package reads back', () => {
    const changer = make('brightIdol');
    try {
      expect(Object.keys(changer.config()).sort()).toEqual(sharedConfigKeys);
    } finally {
      changer.destroy();
    }
  });

  it('builds a changer from the record that reproduces the source', () => {
    const source = make('brightIdol');
    source.setConfig({ ...source.config(), retunePitchSemitones: 5, wetMix: 0.25 });
    const viaConstructor = make(source.config());
    const viaSetter = make('brightIdol');
    try {
      expect(source.config().retunePitchSemitones).toBe(5);
      expect(viaConstructor.configJson()).toBe(source.configJson());
      viaSetter.setConfig(source.config());
      expect(viaSetter.configJson()).toBe(source.configJson());
      expect(viaSetter.config()).toEqual(source.config());
    } finally {
      source.destroy();
      viaConstructor.destroy();
      viaSetter.destroy();
    }
  });
});
