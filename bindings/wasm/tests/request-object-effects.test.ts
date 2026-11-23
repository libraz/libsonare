import { readFileSync } from 'node:fs';

import { beforeAll, describe, expect, it } from 'vitest';
import { meterTapCode, panLawCode, panModeCode, sendTimingCode } from '../src/codes';
import {
  analyzeMelody,
  harmonic,
  hpss,
  init,
  mixStereo,
  normalize,
  pitchCorrectTimevarying,
  pitchCorrectToMidi,
  pitchShift,
  realtimeVoiceChangerRender,
  spectralEdit,
  timeStretch,
  voiceChange,
} from '../src/index';
import { meteringTruePeakDbtp } from '../src/metering';
import {
  projectLoopModeValue,
  projectTrackKindValue,
  projectWarpModeValue,
} from '../src/project_internal';
import type { VoicePresetId } from '../src/public_types';
import { expectRefusalOf } from './_helpers';

const sampleRate = 22050;

interface PanLawCorpus {
  accepted: Array<{ value: string; ordinal: number }>;
  rejected: string[];
  rejected_numeric: number[];
}

const panLawCorpus = JSON.parse(
  readFileSync(new URL('../../../tests/conformance/pan_law_names.json', import.meta.url), 'utf8'),
) as PanLawCorpus;

function sine(length = 8192): Float32Array {
  const samples = new Float32Array(length);
  for (let i = 0; i < samples.length; i++) {
    samples[i] = 0.35 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
  }
  return samples;
}

beforeAll(async () => {
  await init();
});

describe('effects request-object compatibility (WASM)', () => {
  it('preserves positional results for basic transforms', () => {
    const samples = sine();
    expect(hpss({ samples, sampleRate, kernelHarmonic: 17, kernelPercussive: 17 })).toEqual(
      hpss(samples, sampleRate, { kernelHarmonic: 17, kernelPercussive: 17 }),
    );
    expect(harmonic({ samples, sampleRate })).toEqual(harmonic(samples, sampleRate));
    expect(timeStretch({ samples, sampleRate, speedRatio: 1.1 })).toEqual(
      timeStretch(samples, sampleRate, { speedRatio: 1.1 }),
    );
    expect(pitchShift({ samples, sampleRate, pitchSemitones: 2 })).toEqual(
      pitchShift(samples, sampleRate, { pitchSemitones: 2 }),
    );
    expect(normalize({ samples, sampleRate, targetDb: -3 })).toEqual(
      normalize(samples, sampleRate, { targetDb: -3 }),
    );
  });

  it('preserves positional results for pitch and spectral editing requests', () => {
    const samples = sine();
    const f0Hz = new Float32Array(Math.ceil(samples.length / 512)).fill(220);
    expect(pitchCorrectToMidi({ samples, sampleRate, currentMidi: 57, targetMidi: 60 })).toEqual(
      pitchCorrectToMidi(samples, sampleRate, { currentMidi: 57, targetMidi: 60 }),
    );
    expect(
      pitchCorrectTimevarying({
        samples,
        f0Hz,
        sampleRate,
        hopLength: 512,
        targetMode: 'midi',
        targetMidi: 60,
      }),
    ).toEqual(
      pitchCorrectTimevarying(samples, sampleRate, {
        f0Hz,
        hopLength: 512,
        targetMode: 'midi',
        targetMidi: 60,
      }),
    );
    expect(spectralEdit({ samples, sampleRate, ops: [] })).toEqual(
      spectralEdit(samples, sampleRate, { ops: [] }),
    );
  });

  it('preserves positional results for one-shot voice-change requests and refuses a removed field', () => {
    const samples = sine(4096);
    expect(voiceChange({ samples, sampleRate, pitchSemitones: 2, formantFactor: 1.1 })).toEqual(
      voiceChange(samples, sampleRate, { pitchSemitones: 2, formantFactor: 1.1 }),
    );
    const absolute = {
      pitchSemitones: 2,
      formantFactor: 1.1,
      formantMode: 'absolute' as const,
    };
    expect(voiceChange({ samples, sampleRate, ...absolute })).toEqual(
      voiceChange(samples, sampleRate, absolute),
    );
    // The render block is fixed, so a block size is no longer a request field.
    expect(() =>
      realtimeVoiceChangerRender({
        channels: [samples],
        sampleRate,
        config: 'neutralMonitor',
        blockSize: 256,
      } as never),
    ).toThrow(TypeError);
  });

  it('takes one-shot stereo mix as a request over per-strip settings', () => {
    const left = sine(1024);
    const right = sine(1024);
    const trimmed = mixStereo({
      strips: [{ left, right, inputTrimDb: 1.5, faderDb: -2 }],
      sampleRate,
    });
    const plain = mixStereo({ strips: [{ left, right }], sampleRate });
    expect(trimmed.left).toHaveLength(left.length);
    // The control: the settings reach the mix, so the two results differ.
    expect(Array.from(trimmed.left)).not.toEqual(Array.from(plain.left));
    expect(() =>
      (mixStereo as (...args: unknown[]) => unknown)([left], [right], sampleRate),
    ).toThrow(TypeError);
  });

  // Both call shapes funnel through one private normalizer, so an invalid input
  // must fail identically either way — the positive-path equivalence above does
  // not prove the error path stays in lockstep.
  it('throws identically on invalid input in both call forms', () => {
    const empty = new Float32Array(0);
    const posShift = captureThrow(() => pitchShift(empty, sampleRate, { pitchSemitones: 2 }));
    const reqShift = captureThrow(() =>
      pitchShift({ samples: empty, sampleRate, pitchSemitones: 2 }),
    );
    expect(posShift.threw).toBe(true);
    expect(reqShift.threw).toBe(true);
    expect(reqShift.message).toBe(posShift.message);

    const posNorm = captureThrow(() => normalize(empty, sampleRate, { targetDb: -3 }));
    const reqNorm = captureThrow(() => normalize({ samples: empty, sampleRate, targetDb: -3 }));
    expect(posNorm.threw).toBe(true);
    expect(reqNorm.threw).toBe(true);
    expect(reqNorm.message).toBe(posNorm.message);
  });
});

describe('transform sampleRate is required (WASM parity with Node/Python)', () => {
  it('pitchShift / timeStretch / normalize refuse an omitted sampleRate', () => {
    const samples = sine();
    expect(() => pitchShift({ samples, pitchSemitones: 2 } as never)).toThrow(TypeError);
    expect(() => timeStretch({ samples, speedRatio: 1.1 } as never)).toThrow(TypeError);
    expect(() => normalize({ samples, targetDb: -3 } as never)).toThrow(TypeError);
    // The control: the same requests with a sampleRate render.
    expect(pitchShift({ samples, sampleRate, pitchSemitones: 2 }).length).toBeGreaterThan(0);
  });
});

describe('analyzeMelody reports invalid params as a RangeError (WASM)', () => {
  it('throws a RangeError for a non-positive f0MinHz', () => {
    const samples = sine();
    let caught: unknown;
    try {
      analyzeMelody(samples, sampleRate, { f0MinHz: 0 });
    } catch (error) {
      caught = error;
    }
    expect(caught).toBeInstanceOf(RangeError);
    expect((caught as Error).message).toMatch(
      /analyzeMelody: f0MinHz must be finite and greater than 0$/,
    );
  });

  it('throws a RangeError when f0MinHz is above f0MaxHz', () => {
    expect(() => analyzeMelody(sine(), sampleRate, { f0MinHz: 800, f0MaxHz: 400 })).toThrow(
      /analyzeMelody: f0MaxHz must be finite and greater than f0MinHz$/,
    );
    expect(() => analyzeMelody(sine(), sampleRate, { f0MinHz: 800, f0MaxHz: 400 })).toThrow(
      RangeError,
    );
  });
});

describe('enum-code helpers reject unknown strings instead of silently defaulting', () => {
  it('throws for an invalid pan law / pan mode / meter tap', () => {
    // biome-ignore lint/suspicious/noExplicitAny: exercising the runtime guard with an invalid enum string
    expect(() => panLawCode('bogus' as any, 'panLaw')).toThrow();
    // biome-ignore lint/suspicious/noExplicitAny: same
    expect(() => panModeCode('bogus' as any, 'panMode')).toThrow();
    // biome-ignore lint/suspicious/noExplicitAny: same
    expect(() => meterTapCode('bogus' as any, 'tap')).toThrow();
  });

  it('still maps valid strings', () => {
    expect(panLawCode('const6Db', 'panLaw')).toBe(2);
    expect(panModeCode('balance', 'panMode')).toBe(0);
    expect(panModeCode('stereoPan', 'panMode')).toBe(1);
    expect(panModeCode('dualPan', 'panMode')).toBe(2);
    expect(meterTapCode('postFader', 'tap')).toBe(1);
  });

  it('resolves exactly the shared pan-law spellings and refuses every other', () => {
    for (const { value, ordinal } of panLawCorpus.accepted) {
      expect(panLawCode(value as never, 'panLaw')).toBe(ordinal);
    }
    for (const value of panLawCorpus.rejected) {
      expect(() => panLawCode(value as never, 'panLaw')).toThrow(RangeError);
    }
    for (const value of panLawCorpus.rejected_numeric) {
      expect(() => panLawCode(value as never, 'panLaw')).toThrow(TypeError);
    }
  });

  it('panModeCode resolves every spelling to the SonarePanMode C ABI ordinal', () => {
    // SonarePanMode (include/sonare/sonare_c_mixing.h): BALANCE=0, STEREO_PAN=1,
    // DUAL_PAN=2. Must match Node's panModeValue and Python's _PAN_MODE_NAMES so
    // the same spelling picks the same panning algorithm on every surface.
    expect(panModeCode('balance', 'panMode')).toBe(0);
    // @ts-expect-error panModeCode no longer carries the generic 'pan' spelling
    expect(() => panModeCode('pan')).toThrow(RangeError);
    expect(panModeCode('stereoPan', 'panMode')).toBe(1);
    expect(panModeCode('dualPan', 'panMode')).toBe(2);
  });

  it('sendTimingCode rejects an unknown string instead of defaulting to post-fader', () => {
    // biome-ignore lint/suspicious/noExplicitAny: exercising the runtime guard with an invalid enum string
    expect(() => sendTimingCode('bogus' as any, 'sendTiming')).toThrow();
    expect(sendTimingCode('postFader', 'sendTiming')).toBe(0);
    expect(sendTimingCode('preFader', 'sendTiming')).toBe(1);
    // biome-ignore lint/suspicious/noExplicitAny: direct JS callers bypass TypeScript unions.
    expect(() => sendTimingCode(1 as any, 'sendTiming')).toThrow(TypeError);
  });

  it('refuses raw ordinals across code and project resolvers', () => {
    // biome-ignore lint/suspicious/noExplicitAny: direct JS callers bypass TypeScript unions.
    expect(() => panLawCode(99 as any, 'panLaw')).toThrow(TypeError);
    // biome-ignore lint/suspicious/noExplicitAny: direct JS callers bypass TypeScript unions.
    expect(() => meterTapCode(-1 as any, 'tap')).toThrow(TypeError);
    // biome-ignore lint/suspicious/noExplicitAny: direct JS callers bypass TypeScript unions.
    expect(() => projectTrackKindValue('bus' as any)).toThrow(RangeError);
    // biome-ignore lint/suspicious/noExplicitAny: direct JS callers bypass TypeScript unions.
    expect(() => projectWarpModeValue(8 as any)).toThrow(TypeError);
    // biome-ignore lint/suspicious/noExplicitAny: direct JS callers bypass TypeScript unions.
    expect(() => projectLoopModeValue(2 as any)).toThrow(TypeError);
  });
});

describe('metering true peak validation', () => {
  it('rejects an invalid oversample factor as RangeError', () => {
    const error = expectRefusalOf(() =>
      meteringTruePeakDbtp(new Float32Array([0.5]), 22050, { oversampleFactor: 3 }),
    );
    expect(error.message).toBe(
      'meteringTruePeakDbtp: oversampleFactor must be one of 1, 2, 4, 8, 16',
    );
  });
});

function captureThrow(fn: () => unknown): { threw: boolean; message: string } {
  try {
    fn();
    return { threw: false, message: '' };
  } catch (error) {
    return { threw: true, message: error instanceof Error ? error.message : String(error) };
  }
}

// One named operation, one default, wherever it is exposed. The Python handle
// form defaulted to a character preset while every other entry point defaulted
// to the monitoring preset, so the same buffer rendered audibly differently
// depending on which spelling the caller reached for. The expected value comes
// from the shared corpus the Python suite reads, so a change on one surface
// alone fails on the others.
describe('realtimeVoiceChangerRender default preset', () => {
  it('renders what the shared corpus names as the default', () => {
    const expected = (
      JSON.parse(
        readFileSync(
          new URL('../../../tests/conformance/binding_defaults.json', import.meta.url),
          'utf8',
        ),
      ) as { defaults: Record<string, VoicePresetId> }
    ).defaults['realtime_voice_changer_render.config'];
    // The corpus spells the id as the document does; the request takes the lowerCamel token.
    const token = expected.replace(/-([a-z])/g, (_match, ch: string) => ch.toUpperCase());
    expect(token).toBe('neutralMonitor');

    const sampleRate = 22050;
    const samples = new Float32Array(2048);
    for (let i = 0; i < samples.length; i++) {
      samples[i] = 0.25 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
    }
    // A TypeScript default parameter is not introspectable, so compare renders:
    // omitting the preset must produce exactly what naming the default produces.
    expect(realtimeVoiceChangerRender({ channels: [samples], sampleRate })).toEqual(
      realtimeVoiceChangerRender({
        channels: [samples],
        sampleRate,
        config: token as VoicePresetId,
      }),
    );
  });
});
