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
  'hard-tune',
  'robot-monotone',
  'choir-harmony',
  'vocoder-strings',
  'megaphone',
  'radio-voice',
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
  options: { channels?: 1 | 2 } = {},
): void {
  const changer = new RealtimeVoiceChanger({
    sampleRate: SR,
    maxBlockSize: 256,
    preset,
    ...options,
  });
  try {
    run(changer);
  } finally {
    changer.destroy();
  }
}

const GRAIN = {
  ...realtimeVoiceChangerPresetConfig('neutral-monitor'),
  voiceEngineEngine: 'grain' as const,
};

describe('voice transformer presets and flat config', () => {
  it('lists every preset and maps its ordinal back to its id', () => {
    const names = realtimeVoiceChangerPresetNames();
    for (const id of NEW_PRESETS) {
      expect(names).toContain(id);
      expect(realtimeVoiceChangerPresetConfig(id).voiceEngineEngine).toBeDefined();
    }
    expect(NEW_PRESETS.map((id) => voiceCharacterPresetId(id))).toEqual([...NEW_PRESETS]);
    expect(voiceCharacterPresetId(11)).toBe('radio-voice');
  });

  it('returns the transformer fields of the default psola engine', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutral-monitor');
    expect(pod.voiceEngineEngine).toBe('psola');
    expect(pod.pitchCorrectionMode).toBe('off');
    expect(pod.vocoderEnabled).toBe(false);
    expect(Object.keys(pod)).toHaveLength(71);
  });

  it('round-trips a changed transformer config through setConfig and configJson', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutral-monitor');
    withChanger(pod, (changer) => {
      changer.setConfig({
        ...pod,
        pitchCorrectionMode: 'auto',
        pitchCorrectionScale: 'natural_minor',
        pitchCorrectionKeyRoot: 9,
        harmonyVoices: 2,
        harmonyStep1: 2,
        harmonyStep2: -3,
        characterType: 'bbd_chorus',
        reverbVariationVariation: 'tempo_delay',
        reverbVariationDivision: 'dotted_eighth',
        midiRelease: 'input',
      });
      const applied = JSON.parse(changer.configJson()).dsp;
      expect(applied.pitchCorrection.mode).toBe('auto');
      expect(applied.pitchCorrection.scale).toBe('natural_minor');
      expect(applied.pitchCorrection.keyRoot).toBe(9);
      expect(applied.harmony.voices).toBe(2);
      expect(applied.harmony.steps.slice(0, 2)).toEqual([2, -3]);
      expect(applied.character.type).toBe('bbd_chorus');
      expect(applied.reverbVariation.variation).toBe('tempo_delay');
      expect(applied.reverbVariation.division).toBe('dotted_eighth');
      expect(applied.midi.release).toBe('input');
    });
  });

  it('refuses a flat config that lacks a transformer key', () => {
    const { harmonyStep3: _omitted, ...partial } = realtimeVoiceChangerPresetConfig('bright-idol');
    expectRefusal(
      () => new RealtimeVoiceChanger({ sampleRate: SR, preset: partial as never }),
      RangeError,
    );
  });

  it('refuses out-of-range, fractional and unknown transformer values by name', () => {
    const pod = realtimeVoiceChangerPresetConfig('neutral-monitor');
    withChanger(pod, (changer) => {
      const before = changer.configJson();
      expect(() => changer.setConfig({ ...pod, voiceEngineF0MinHz: Number.NaN })).toThrow(
        RangeError,
      );
      expect(() => changer.setConfig({ ...pod, harmonyStep1: 8 })).toThrow(RangeError);
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
          preset: { ...GRAIN, pitchCorrectionMode: 'auto' },
        }),
    ).toThrow(RangeError);
    const pod = realtimeVoiceChangerPresetConfig('neutral-monitor');
    withChanger(pod, (changer) => {
      expect(() => changer.setConfig({ ...pod, voiceEngineEngine: 'grain' })).toThrow(RangeError);
    });
  });
});

describe('RealtimeVoiceChanger note API', () => {
  const psola = realtimeVoiceChangerPresetConfig('neutral-monitor');

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
    const destroyed = new RealtimeVoiceChanger({ sampleRate: SR, preset: psola });
    destroyed.destroy();
    expectRefusal(() => destroyed.noteOn(60), ErrorCode.InvalidState);
    expectRefusal(() => destroyed.noteOff(60), ErrorCode.InvalidState);
    expectRefusal(() => destroyed.allNotesOff(), ErrorCode.InvalidState);
  });
});

describe('RealtimeVoiceChanger external vocoder carrier', () => {
  const external = {
    ...realtimeVoiceChangerPresetConfig('vocoder-strings'),
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
        expect(codeOf(() => changer.processInterleaved(tone(256), 2))).toBe(ErrorCode.InvalidState);
        expect(codeOf(() => changer.processPlanarStereo(tone(128), tone(128)))).toBe(
          ErrorCode.InvalidState,
        );
      },
      { channels: 2 },
    );
  });

  it('processes with a carrier on every layout and checks the carrier length', () => {
    withChanger(
      external,
      (changer) => {
        const block = tone(128);
        const carrier = tone(128, 110);
        expect(changer.processMonoWithCarrier(block, carrier)).toHaveLength(128);
        // Run past the reported latency so the written block carries signal.
        const into = new Float32Array(128);
        const blocks = Math.ceil(changer.latencySamples() / 128) + 1;
        for (let i = 0; i < blocks; i += 1) {
          changer.processMonoWithCarrierInto(block, carrier, into);
        }
        expect(into.some((v) => v !== 0)).toBe(true);
        expect(changer.processInterleavedWithCarrier(tone(256), carrier, 2)).toHaveLength(256);
        const interleavedOut = new Float32Array(256);
        changer.processInterleavedWithCarrierInto(tone(256), carrier, 2, interleavedOut);
        const left = tone(128);
        const right = tone(128);
        changer.processPlanarStereoWithCarrier(left, right, carrier);
        expect(() => changer.processMonoWithCarrier(block, new Float32Array(64))).toThrow(
          RangeError,
        );
        expect(() => changer.processInterleavedWithCarrier(tone(256), tone(256), 2)).toThrow(
          RangeError,
        );
        expect(() => changer.processPlanarStereoWithCarrier(left, right, tone(64))).toThrow(
          RangeError,
        );
      },
      { channels: 2 },
    );
  });

  it('takes the interleaved carrier right after the input, then the channel count', () => {
    withChanger(
      external,
      (changer) => {
        const carrier = tone(128, 110);
        expect(changer.processInterleavedWithCarrier(tone(256), carrier, 2)).toHaveLength(256);
        const out = new Float32Array(256);
        changer.processInterleavedWithCarrierInto(tone(256), carrier, 2, out);
        expectRefusal(
          () => changer.processInterleavedWithCarrier(tone(256), 2 as never, carrier as never),
          TypeError,
        );
        expectRefusal(
          () =>
            changer.processInterleavedWithCarrierInto(tone(256), 2 as never, carrier as never, out),
          TypeError,
        );
      },
      { channels: 2 },
    );
  });

  it('ignores the carrier while the carrier source is internal', () => {
    const internal = realtimeVoiceChangerPresetConfig('vocoder-strings');
    expect(internal.vocoderCarrierSource).toBe('internal');
    withChanger(internal, (changer) => {
      const block = tone(128);
      expect(changer.processMonoWithCarrier(block, tone(128, 330))).toHaveLength(128);
      expect(changer.processMono(block)).toHaveLength(128);
    });
  });
});
