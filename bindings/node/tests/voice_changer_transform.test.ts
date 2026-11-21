import { describe, expect, it } from 'vitest';
import { ErrorCode } from '../src/errors.js';
import {
  RealtimeVoiceChanger,
  type RealtimeVoiceChangerConfig,
  realtimeVoiceChangerPresetConfig,
  realtimeVoiceChangerPresetNames,
  voiceCharacterPresetId,
} from '../src/index.js';
import { expectRefusal } from './_helpers.js';

const SR = 48000;
const NEW_PRESETS = [
  'hardTune',
  'robotMonotone',
  'choirHarmony',
  'vocoderStrings',
  'megaphone',
  'radioVoice',
] as const;

function tone(frames: number, freq = 220): Float32Array {
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; i += 1) {
    out[i] = 0.3 * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

function codeOf(fn: () => unknown): number | undefined {
  try {
    fn();
  } catch (error) {
    return (error as { code?: number }).code;
  }
  return undefined;
}

function withChanger(
  preset: RealtimeVoiceChangerConfig,
  run: (changer: RealtimeVoiceChanger) => void,
  options: { numChannels?: 1 | 2 } = {},
): void {
  const changer = new RealtimeVoiceChanger({
    sampleRate: SR,
    numChannels: 1,
    maxBlockSize: 256,
    config: preset,
    ...options,
  });
  try {
    run(changer);
  } finally {
    changer.destroy();
  }
}

const GRAIN = {
  ...realtimeVoiceChangerPresetConfig('neutralMonitor'),
  voiceEngine: 'grain' as const,
};

describe('voice transformer presets and flat config', () => {
  it('lists every preset and maps its ordinal back to its id', () => {
    const names = realtimeVoiceChangerPresetNames();
    for (const id of NEW_PRESETS) {
      expect(names).toContain(id);
      expect(realtimeVoiceChangerPresetConfig(id).voiceEngine).toBeDefined();
    }
    expect(NEW_PRESETS.map((id) => voiceCharacterPresetId(names.indexOf(id)))).toEqual([
      ...NEW_PRESETS,
    ]);
    expect(voiceCharacterPresetId(11)).toBe('radioVoice');
  });

  it('returns the transformer fields of the default psola engine', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutralMonitor');
    expect(pod.voiceEngine).toBe('psola');
    expect(pod.pitchCorrectionMode).toBe('off');
    expect(pod.vocoderEnabled).toBe(false);
    // 69 fields; the automatic retune grain is the one left out.
    expect(Object.keys(pod)).toHaveLength(68);
    expect('retuneGrainSizeSamples' in pod).toBe(false);
  });

  it('round-trips a changed transformer config through setConfig and configJson', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutralMonitor');
    withChanger(pod, (changer) => {
      changer.setConfig({
        ...pod,
        pitchCorrectionMode: 'auto',
        pitchCorrectionScale: 'naturalMinor',
        pitchCorrectionKeyRoot: 9,
        harmonyVoices: 2,
        harmonySteps: [2, -3, 0],
        characterType: 'bbdChorus',
        reverbVariation: 'tempoDelay',
        reverbVariationDivision: 'dottedEighth',
        midiRelease: 'input',
      });
      const applied = JSON.parse(changer.configJson()).dsp;
      expect(applied.pitchCorrection.mode).toBe('auto');
      expect(applied.pitchCorrection.scale).toBe('naturalMinor');
      expect(applied.pitchCorrection.keyRoot).toBe(9);
      expect(applied.harmony.voices).toBe(2);
      expect(applied.harmony.steps.slice(0, 2)).toEqual([2, -3]);
      expect(applied.character.type).toBe('bbdChorus');
      expect(applied.reverbVariation.variation).toBe('tempoDelay');
      expect(applied.reverbVariation.division).toBe('dottedEighth');
      expect(applied.midi.release).toBe('input');
    });
  });

  it('refuses a flat config that lacks a transformer key', () => {
    const { harmonySteps: _omitted, ...partial } = realtimeVoiceChangerPresetConfig('brightIdol');
    expectRefusal(
      () => new RealtimeVoiceChanger({ sampleRate: SR, numChannels: 1, config: partial as never }),
      TypeError,
    );
  });

  it('refuses out-of-range, fractional and unknown transformer values by name', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutralMonitor');
    withChanger(pod, (changer) => {
      const before = changer.configJson();
      expect(() => changer.setConfig({ ...pod, voiceEngineF0MinHz: Number.NaN })).toThrow(
        RangeError,
      );
      expect(() => changer.setConfig({ ...pod, harmonySteps: [8, 0, 0] })).toThrow(RangeError);
      expect(() => changer.setConfig({ ...pod, vocoderBands: 20.5 })).toThrow(RangeError);
      expect(() => changer.setConfig({ ...pod, characterType: 'sideways' as never })).toThrow(
        RangeError,
      );
      expect(() => changer.setConfig({ ...pod, reverbVariationFeedback: 0.96 })).toThrow(
        RangeError,
      );
      expect(changer.configJson()).toBe(before);
    });
  });

  it('refuses pitch correction on the grain engine and a changed engine on a live changer', () => {
    expect(
      () =>
        new RealtimeVoiceChanger({
          sampleRate: SR,
          numChannels: 1,
          config: { ...GRAIN, pitchCorrectionMode: 'auto' },
        }),
    ).toThrow(RangeError);
    const pod = realtimeVoiceChangerPresetConfig('neutralMonitor');
    withChanger(pod, (changer) => {
      expect(() => changer.setConfig({ ...pod, voiceEngine: 'grain' })).toThrow(RangeError);
    });
  });
});

describe('RealtimeVoiceChanger note API', () => {
  const psola = realtimeVoiceChangerPresetConfig('neutralMonitor');

  it('accepts notes with and without a velocity', () => {
    withChanger(psola, (changer) => {
      changer.noteOn(60);
      changer.noteOn(64, 0.5);
      changer.noteOn(64, 1);
      changer.noteOff(60);
      changer.noteOff(100);
      changer.allNotesOff();
      const out = changer.processMono(tone(128));
      expect(out).toHaveLength(128);
    });
  });

  it('refuses a note or velocity outside its domain with the argument error classes', () => {
    withChanger(psola, (changer) => {
      for (const note of [-1, 128, 60.5, Number.NaN, 2 ** 31]) {
        expect(() => changer.noteOn(note)).toThrow(RangeError);
        expect(() => changer.noteOff(note)).toThrow(RangeError);
      }
      for (const velocity of [0, -0.1, 1.01, Number.NaN, Number.POSITIVE_INFINITY]) {
        expect(() => changer.noteOn(60, velocity)).toThrow(RangeError);
      }
      expect(() => changer.noteOn('60' as never)).toThrow(TypeError);
      expect(() => changer.noteOn(60, true as never)).toThrow(TypeError);
      expect(() => changer.noteOn(60, '1' as never)).toThrow(TypeError);
      expect(() => changer.noteOff(true as never)).toThrow(TypeError);
    });
  });

  it('reports a grain-engine changer and a destroyed one as InvalidState', () => {
    withChanger(GRAIN, (changer) => {
      expect(codeOf(() => changer.noteOn(60))).toBe(ErrorCode.InvalidState);
      expect(codeOf(() => changer.noteOff(60))).toBe(ErrorCode.InvalidState);
      expect(() => changer.allNotesOff()).not.toThrow();
    });
    const destroyed = new RealtimeVoiceChanger({ sampleRate: SR, numChannels: 1, config: psola });
    destroyed.destroy();
    expectRefusal(() => destroyed.noteOn(60), ErrorCode.InvalidState);
    expectRefusal(() => destroyed.noteOff(60), ErrorCode.InvalidState);
    expectRefusal(() => destroyed.allNotesOff(), ErrorCode.InvalidState);
  });
});

describe('RealtimeVoiceChanger external vocoder carrier', () => {
  const external = {
    ...realtimeVoiceChangerPresetConfig('vocoderStrings'),
    vocoderCarrierSource: 'external' as const,
  };

  it('refuses the carrier-less process methods while a carrier is expected', () => {
    withChanger(
      external,
      (changer) => {
        const block = tone(128);
        expect(codeOf(() => changer.processMono(block))).toBe(ErrorCode.InvalidState);
        expect(codeOf(() => changer.processMonoInto(block, new Float32Array(128)))).toBe(
          ErrorCode.InvalidState,
        );
        expect(
          codeOf(() => changer.processInterleavedInto(tone(256), 2, new Float32Array(256))),
        ).toBe(ErrorCode.InvalidState);
        expect(codeOf(() => changer.processPlanar([tone(128), tone(128)]))).toBe(
          ErrorCode.InvalidState,
        );
      },
      { numChannels: 2 },
    );
  });

  it('processes with a carrier on every layout and checks the carrier length', () => {
    withChanger(
      external,
      (changer) => {
        const block = tone(128);
        const carrier = tone(128, 110);
        expect(changer.processMono(block, carrier)).toHaveLength(128);
        // Run past the reported latency so the written block carries signal.
        const into = new Float32Array(128);
        const blocks = Math.ceil(changer.latencySamples / 128) + 1;
        for (let i = 0; i < blocks; i += 1) {
          changer.processMonoInto(block, into, carrier);
        }
        expect(into.some((v) => v !== 0)).toBe(true);
        const interleavedOut = new Float32Array(256);
        changer.processInterleavedInto(tone(256), 2, interleavedOut, carrier);
        const left = tone(128);
        const right = tone(128);
        expect(changer.processPlanar([left, right], carrier)).toHaveLength(2);
        expect(() => changer.processMono(block, new Float32Array(64))).toThrow(RangeError);
        expect(() =>
          changer.processInterleavedInto(tone(256), 2, new Float32Array(256), tone(256)),
        ).toThrow(RangeError);
        expect(() => changer.processPlanar([left, right], tone(64))).toThrow(RangeError);
      },
      { numChannels: 2 },
    );
  });

  it('takes the interleaved carrier last, after the channel count and the output', () => {
    withChanger(
      external,
      (changer) => {
        const carrier = tone(128, 110);
        const out = new Float32Array(256);
        changer.processInterleavedInto(tone(256), 2, out, carrier);
        expectRefusal(
          () => changer.processInterleavedInto(tone(256), 2, carrier as never, out as never),
          RangeError,
        );
      },
      { numChannels: 2 },
    );
  });

  it('ignores the carrier while the carrier source is internal', () => {
    const internal = realtimeVoiceChangerPresetConfig('vocoderStrings');
    expect(internal.vocoderCarrierSource).toBe('internal');
    withChanger(internal, (changer) => {
      const block = tone(128);
      expect(changer.processMono(block, tone(128, 330))).toHaveLength(128);
      expect(changer.processMono(block)).toHaveLength(128);
    });
  });
});
