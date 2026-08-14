import { readFileSync } from 'node:fs';

import { beforeAll, describe, expect, it } from 'vitest';
import { meterTapCode, panLawCode, panModeCode, sendTimingCode } from '../src/codes';
import { ErrorCode } from '../src/errors';
import {
  analyzeMelody,
  harmonic,
  hpss,
  init,
  isSonareError,
  mixStereo,
  normalize,
  pitchCorrectTimevarying,
  pitchCorrectToMidi,
  pitchCorrectToMidiTimevarying,
  pitchShift,
  spectralEdit,
  timeStretch,
  voiceChange,
  voiceChangeRealtime,
} from '../src/index';
import { meteringTruePeakDb } from '../src/metering';
import {
  projectLoopModeValue,
  projectTrackKindValue,
  projectWarpModeValue,
} from '../src/project_internal';
import type { PanLawInput } from '../src/public_types';

const sampleRate = 22050;

interface PanLawCorpus {
  accepted: Array<{ value: string; ordinal: number }>;
  normalization: Array<{ value: string; ordinal: number }>;
  numeric: number[];
  rejected: string[];
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
      hpss(samples, sampleRate, 17, 17),
    );
    expect(harmonic({ samples, sampleRate })).toEqual(harmonic(samples, sampleRate));
    expect(timeStretch({ samples, sampleRate, rate: 1.1 })).toEqual(
      timeStretch(samples, sampleRate, 1.1),
    );
    expect(pitchShift({ samples, sampleRate, semitones: 2 })).toEqual(
      pitchShift(samples, sampleRate, 2),
    );
    expect(normalize({ samples, sampleRate, targetDb: -3 })).toEqual(
      normalize(samples, sampleRate, -3),
    );
  });

  it('preserves positional results for pitch and spectral editing requests', () => {
    const samples = sine();
    const f0Hz = new Float32Array(Math.ceil(samples.length / 512)).fill(220);
    expect(pitchCorrectToMidi({ samples, sampleRate, currentMidi: 57, targetMidi: 60 })).toEqual(
      pitchCorrectToMidi(samples, sampleRate, 57, 60),
    );
    expect(
      pitchCorrectToMidiTimevarying({ samples, f0Hz, targetMidi: 60, sampleRate, hopLength: 512 }),
    ).toEqual(pitchCorrectToMidiTimevarying(samples, f0Hz, 60, sampleRate, 512));
    expect(
      pitchCorrectTimevarying({ samples, f0Hz, sampleRate, hopLength: 512, mode: 'midi' }),
    ).toEqual(pitchCorrectTimevarying(samples, f0Hz, sampleRate, 512, { mode: 'midi' }));
    expect(spectralEdit({ samples, sampleRate, ops: [] })).toEqual(
      spectralEdit(samples, sampleRate, []),
    );
  });

  it('preserves positional results for one-shot voice-change requests', () => {
    const samples = sine(4096);
    expect(voiceChange({ samples, sampleRate, pitchSemitones: 2, formantFactor: 1.1 })).toEqual(
      voiceChange(samples, sampleRate, { pitchSemitones: 2, formantFactor: 1.1 }),
    );
    expect(
      voiceChangeRealtime({
        samples,
        sampleRate,
        preset: 'neutral-monitor',
        blockSize: 256,
      }),
    ).toEqual(voiceChangeRealtime(samples, sampleRate, 'neutral-monitor', { blockSize: 256 }));
    expect(voiceChangeRealtime(samples, sampleRate, 'neutral-monitor', { blockSize: 64 })).toEqual(
      voiceChangeRealtime(samples, sampleRate, 'neutral-monitor', { blockSize: 256 }),
    );
  });

  it('preserves positional results for one-shot stereo mix requests', () => {
    const left = sine(1024);
    const right = sine(1024);
    expect(
      mixStereo({
        leftChannels: [left],
        rightChannels: [right],
        sampleRate,
        inputTrimDb: 1.5,
        faderDb: -2,
      }),
    ).toEqual(mixStereo([left], [right], sampleRate, { inputTrimDb: 1.5, faderDb: -2 }));
  });

  // Both call shapes funnel through one private normalizer, so an invalid input
  // must fail identically either way — the positive-path equivalence above does
  // not prove the error path stays in lockstep.
  it('throws identically on invalid input in both call forms', () => {
    const empty = new Float32Array(0);
    const posShift = captureThrow(() => pitchShift(empty, sampleRate, 2));
    const reqShift = captureThrow(() => pitchShift({ samples: empty, sampleRate, semitones: 2 }));
    expect(posShift.threw).toBe(true);
    expect(reqShift.threw).toBe(true);
    expect(reqShift.message).toBe(posShift.message);

    const posNorm = captureThrow(() => normalize(empty, sampleRate, -3));
    const reqNorm = captureThrow(() => normalize({ samples: empty, sampleRate, targetDb: -3 }));
    expect(posNorm.threw).toBe(true);
    expect(reqNorm.threw).toBe(true);
    expect(reqNorm.message).toBe(posNorm.message);
  });
});

describe('transform sampleRate defaults to 22050 (WASM parity with Node/Python)', () => {
  it('pitchShift / timeStretch / normalize accept an omitted sampleRate', () => {
    const samples = sine();
    expect(pitchShift({ samples, semitones: 2 })).toEqual(
      pitchShift({ samples, sampleRate, semitones: 2 }),
    );
    expect(timeStretch({ samples, rate: 1.1 })).toEqual(
      timeStretch({ samples, sampleRate, rate: 1.1 }),
    );
    expect(normalize({ samples, targetDb: -3 })).toEqual(
      normalize({ samples, sampleRate, targetDb: -3 }),
    );
  });
});

describe('analyzeMelody reports invalid params as a branded SonareError (WASM)', () => {
  it('throws a SonareError (not a bare RangeError) for a non-positive fmin', () => {
    const samples = sine();
    let caught: unknown;
    try {
      analyzeMelody(samples, sampleRate, { fmin: 0 });
    } catch (error) {
      caught = error;
    }
    expect(isSonareError(caught)).toBe(true);
  });
});

describe('enum-code helpers reject unknown strings instead of silently defaulting', () => {
  it('throws for an invalid pan law / pan mode / meter tap', () => {
    // biome-ignore lint/suspicious/noExplicitAny: exercising the runtime guard with an invalid enum string
    expect(() => panLawCode('bogus' as any)).toThrow();
    // biome-ignore lint/suspicious/noExplicitAny: same
    expect(() => panModeCode('bogus' as any)).toThrow();
    // biome-ignore lint/suspicious/noExplicitAny: same
    expect(() => meterTapCode('bogus' as any)).toThrow();
  });

  it('still maps valid strings', () => {
    expect(panLawCode('const6dB')).toBe(2);
    expect(panModeCode('balance')).toBe(0);
    expect(panModeCode('pan')).toBe(1);
    expect(panModeCode('stereo-pan')).toBe(1);
    expect(panModeCode('dual-pan')).toBe(2);
    expect(meterTapCode('postFader')).toBe(1);
  });

  it('resolves every shared pan-law spelling and conservative normalization', () => {
    for (const { value, ordinal } of [...panLawCorpus.accepted, ...panLawCorpus.normalization]) {
      expect(panLawCode(value as PanLawInput)).toBe(ordinal);
    }
    for (const value of panLawCorpus.numeric) {
      expect(panLawCode(value)).toBe(value);
    }
    for (const value of panLawCorpus.rejected) {
      expect(() => panLawCode(value as PanLawInput)).toThrow(RangeError);
    }
  });

  it('panModeCode resolves every spelling to the SonarePanMode C ABI ordinal', () => {
    // SonarePanMode (include/sonare/sonare_c_mixing.h): BALANCE=0, STEREO_PAN=1,
    // DUAL_PAN=2. Must match Node's panModeValue and Python's _PAN_MODE_NAMES so
    // the same spelling picks the same panning algorithm on every surface.
    expect(panModeCode('balance')).toBe(0);
    expect(panModeCode('pan')).toBe(1);
    expect(panModeCode('stereopan')).toBe(1);
    expect(panModeCode('stereo-pan')).toBe(1);
    expect(panModeCode('dualpan')).toBe(2);
    expect(panModeCode('dual-pan')).toBe(2);
  });

  it('sendTimingCode rejects an unknown string instead of defaulting to post-fader', () => {
    // biome-ignore lint/suspicious/noExplicitAny: exercising the runtime guard with an invalid enum string
    expect(() => sendTimingCode('bogus' as any)).toThrow();
    expect(sendTimingCode('postFader')).toBe(0);
    expect(sendTimingCode('preFader')).toBe(1);
    expect(() => sendTimingCode(7)).toThrow(/Invalid send timing/);
  });

  it('rejects unknown raw ordinals across code and project resolvers', () => {
    // biome-ignore lint/suspicious/noExplicitAny: direct JS callers bypass TypeScript unions.
    expect(() => panLawCode(99 as any)).toThrow(RangeError);
    // biome-ignore lint/suspicious/noExplicitAny: direct JS callers bypass TypeScript unions.
    expect(() => meterTapCode(-1 as any)).toThrow(RangeError);
    // biome-ignore lint/suspicious/noExplicitAny: direct JS callers bypass TypeScript unions.
    expect(() => projectTrackKindValue('bus' as any)).toThrow(RangeError);
    // biome-ignore lint/suspicious/noExplicitAny: direct JS callers bypass TypeScript unions.
    expect(() => projectWarpModeValue(8 as any)).toThrow(RangeError);
    // biome-ignore lint/suspicious/noExplicitAny: direct JS callers bypass TypeScript unions.
    expect(() => projectLoopModeValue(2 as any)).toThrow(RangeError);
  });
});

describe('metering true peak validation', () => {
  it('rejects an invalid oversample factor as SonareError', () => {
    let caught: unknown;
    try {
      meteringTruePeakDb(new Float32Array([0.5]), 22050, 3);
    } catch (error) {
      caught = error;
    }
    expect(isSonareError(caught)).toBe(true);
    if (!isSonareError(caught)) {
      throw new Error('expected SonareError');
    }
    expect(caught.code).toBe(ErrorCode.InvalidParameter);
    expect(caught.codeName).toBe('InvalidParameter');
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
