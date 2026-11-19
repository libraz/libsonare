import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { ErrorCode } from '../src/errors.js';
import {
  RealtimeVoiceChanger,
  type RealtimeVoiceChangerConfig,
  type RealtimeVoiceChangerConfigInput,
  realtimeVoiceChangerPresetConfig,
  realtimeVoiceChangerPresetJson,
  realtimeVoiceChangerPresetNames,
} from '../src/index.js';
import { expectRefusal } from './_helpers.js';

describe('RealtimeVoiceChanger flat POD setConfig', () => {
  it('applies every field of a flat preset POD, not just the root fields', () => {
    const pod = realtimeVoiceChangerPresetConfig('brightIdol');
    pod.retunePitchSemitones = 5;
    const vc = new RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 1 });
    vc.setConfig(pod);
    const applied = JSON.parse(vc.configJson()).dsp;
    // The mutated field and the preset's sections survive instead of reverting
    // to config defaults, which a flat POD through the nested parser would do.
    expect(applied.retune.pitchSemitones).toBeCloseTo(5, 4);
    expect(applied.retune.mix).toBeCloseTo(pod.retuneMix, 4);
    expect(applied.formant.factor).toBeCloseTo(pod.formantFactor, 4);
    expect(applied.eq.presenceDb).toBeCloseTo(pod.eqPresenceDb, 4);
    expect(applied.reverb.mix).toBeCloseTo(pod.reverbMix, 4);
    vc.destroy();
  });

  it('applies a complete nested preset document', () => {
    const vc = new RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 1 });
    const preset = JSON.parse(realtimeVoiceChangerPresetJson('neutralMonitor'));
    preset.dsp.retune.pitchSemitones = 3;
    vc.setConfig(preset);
    const applied = JSON.parse(vc.configJson()).dsp;
    expect(applied.retune.pitchSemitones).toBeCloseTo(3, 4);
    vc.destroy();
  });

  it('rejects a partial nested preset instead of applying defaults', () => {
    const vc = new RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 1 });
    expect(() =>
      // @ts-expect-error deliberately partial nested preset; setConfig must reject it, not fill in defaults.
      vc.setConfig({ schemaVersion: 4, dsp: { retune: { pitchSemitones: 3 } } }),
    ).toThrow(/missing field|field must/i);
    vc.destroy();
  });

  it('reports a refused preset-or-config document as InvalidFormat', () => {
    const partial = { schemaVersion: 4, dsp: { retune: { pitchSemitones: 3 } } } as never;
    expectRefusal(
      () => new RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 1, config: partial }),
      ErrorCode.InvalidFormat,
    );
    const vc = new RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 1 });
    try {
      expectRefusal(() => vc.setConfig(partial), ErrorCode.InvalidFormat);
    } finally {
      vc.destroy();
    }
  });

  it('accepts a flat preset POD in the constructor', () => {
    const pod = realtimeVoiceChangerPresetConfig('brightIdol');
    pod.retunePitchSemitones = -9;
    const vc = new RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 1, config: pod });
    expect(JSON.parse(vc.configJson()).dsp.retune.pitchSemitones).toBeCloseTo(-9, 4);
    vc.destroy();
  });

  it('carries the formant mode, fixed when the changer is created', () => {
    // The grain engine is where the absolute warp adds its analysis frame of latency.
    const pod = {
      ...realtimeVoiceChangerPresetConfig('neutralMonitor'),
      voiceEngine: 'grain' as const,
    };
    expect(pod.formantMode).toBe('relative');
    const relative = new RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 1, config: pod });
    const absolute = new RealtimeVoiceChanger({
      sampleRate: 48000,
      numChannels: 1,
      config: { ...pod, formantMode: 'absolute', formantFactor: 1.1 },
    });
    // One analysis frame (1024 samples at 48 kHz) more than the relative chain.
    expect(absolute.latencySamples).toBe(relative.latencySamples + 1024);
    expect(JSON.parse(absolute.configJson()).dsp.formant.mode).toBe('absolute');

    // A live update keeps the mode; one that changes it is refused.
    absolute.setConfig({ ...pod, formantMode: 'absolute', formantFactor: 1.2 });
    expect(() => absolute.setConfig({ ...pod, formantMode: 'relative' })).toThrow(RangeError);
    expect(() => relative.setConfig({ ...pod, formantMode: 'absolute' })).toThrow(RangeError);
    relative.destroy();
    absolute.destroy();
  });

  it('refuses an out-of-range field instead of clamping it and keeps the previous config', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutralMonitor');
    const vc = new RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 1, config: pod });
    try {
      const before = vc.configJson();
      expect(() => vc.setConfig({ ...pod, formantFactor: 3.0 })).toThrow(RangeError);
      expect(() => vc.setConfig({ ...pod, formantFactor: 3.0 })).toThrow(
        /formant\.factor out of range \[0\.55, 1\.65\], got 3/,
      );
      expect(() => vc.setConfig({ ...pod, limiterCeilingDb: 0.5 })).toThrow(
        /limiter\.ceilingDb out of range/,
      );
      expect(vc.configJson()).toBe(before);
    } finally {
      vc.destroy();
    }
    expect(
      () =>
        new RealtimeVoiceChanger({
          sampleRate: 48000,
          numChannels: 1,
          config: { ...pod, formantFactor: 3.0 },
        }),
    ).toThrow(RangeError);
  });

  it('refuses an unreachable absolute warp and names the formant factor range', () => {
    const pod = {
      ...realtimeVoiceChangerPresetConfig('neutralMonitor'),
      voiceEngine: 'grain' as const,
    };
    const unreachable = { ...pod, formantMode: 'absolute' as const, retunePitchSemitones: -9 };
    expect(
      () => new RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 1, config: unreachable }),
    ).toThrow(/\[0\.55, 0\.9811\]/);
    expect(
      () =>
        new RealtimeVoiceChanger({
          sampleRate: 48000,
          numChannels: 1,
          config: { ...pod, formantMode: 'sideways' as never },
        }),
    ).toThrow();
  });

  it('requires formantMode on a flat POD in the constructor and in setConfig', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutralMonitor');
    const { formantMode: _omitted, ...partial } = pod;
    const podWithoutMode = partial as unknown as typeof pod;
    // @ts-expect-error formantMode is required on the flat POD, as the runtime enforces
    const typed: RealtimeVoiceChangerConfig = partial;
    expect(typed).toBe(partial);
    expect(
      () => new RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 1, config: podWithoutMode }),
    ).toThrow(TypeError);
    expect(
      () => new RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 1, config: podWithoutMode }),
    ).toThrow(/RealtimeVoiceChanger: formantMode must be a string/);
    const vc = new RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 1, config: pod });
    try {
      expect(() => vc.setConfig(podWithoutMode)).toThrow(TypeError);
      expect(() => vc.setConfig({ ...pod, formantMode: 3 } as never)).toThrow(
        /formantMode must be a string/,
      );
      // A preset id keeps its own defaults.
      expect(() => vc.setConfig('neutralMonitor')).not.toThrow();
    } finally {
      vc.destroy();
    }
  });
});

/** A mono changer at 48 kHz. */
function make(config: RealtimeVoiceChangerConfigInput): RealtimeVoiceChanger {
  return new RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 1, config });
}

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
