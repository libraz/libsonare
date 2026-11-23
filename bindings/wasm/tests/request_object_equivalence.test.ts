/**
 * The request-object equivalence inventory: one row per exported request-object overload, across
 * the per-domain files that run them, and the one overload compared through its handle.
 */

import { describe, expect, it } from 'vitest';
import * as sonare from '../src/index';
import { unknownKeyError } from './_helpers';
import { ALT_SR, overloadedFunctions, SIG2 } from './request_equivalence/harness';
import { ROWS as ANALYSIS } from './request_equivalence/rows_analysis';
import { ROWS as EFFECTS } from './request_equivalence/rows_effects';
import { ROWS as FEATURES } from './request_equivalence/rows_features';
import { ROWS as MASTERING } from './request_equivalence/rows_mastering';
import { ROWS as SPECTRAL } from './request_equivalence/rows_spectral';

const ROWS = [...ANALYSIS, ...FEATURES, ...SPECTRAL, ...EFFECTS, ...MASTERING];

type DynamicCall = (...args: unknown[]) => unknown;

interface SampleOneShotEntry {
  name: string;
  prefix: string;
  call: DynamicCall;
}

function dynamicCall(fn: unknown): DynamicCall {
  return fn as DynamicCall;
}

const SAMPLE_ONE_SHOT_ENTRIES: SampleOneShotEntry[] = [
  { name: 'stft', prefix: 'stft', call: dynamicCall(sonare.stft) },
  { name: 'detectBpm', prefix: 'detectBpm', call: dynamicCall(sonare.detectBpm) },
  { name: 'cqt', prefix: 'cqt', call: dynamicCall(sonare.cqt) },
  { name: 'meteringRmsDb', prefix: 'meteringRmsDb', call: dynamicCall(sonare.meteringRmsDb) },
  {
    name: 'extractPercussiveEvents',
    prefix: 'extractPercussiveEvents',
    call: dynamicCall(sonare.extractPercussiveEvents),
  },
  {
    name: 'analyzePolyphonic',
    prefix: 'analyzePolyphonic',
    call: dynamicCall(sonare.analyzePolyphonic),
  },
  {
    name: 'Project.analyzeTempo',
    prefix: 'Project.analyzeTempo',
    call: (...args: unknown[]) => {
      const project = new sonare.Project();
      try {
        return dynamicCall(project.analyzeTempo).apply(project, args);
      } finally {
        project.destroy();
      }
    },
  },
];

const SAMPLE_ONE_SHOT_ORDER_PROBES = [
  {
    name: 'unknown option before out-of-range sampleRate',
    request: [{ samples: SIG2, sampleRate: 0, bogus: true }],
    positional: [SIG2, 0, { bogus: true }],
    expected: 'unknown-key',
  },
  {
    name: 'wrong samples type before out-of-range sampleRate',
    request: [{ samples: 'not samples', sampleRate: 0 }],
    positional: ['not samples', 0],
    expected: 'samples-type',
  },
  {
    name: 'wrong samples type before wrong sampleRate type',
    request: [{ samples: 'not samples', sampleRate: 'not a rate' }],
    positional: ['not samples', 'not a rate'],
    expected: 'samples-type',
  },
] as const;

const SAMPLE_ONE_SHOT_ORDER_CASES = SAMPLE_ONE_SHOT_ENTRIES.flatMap((entry) =>
  SAMPLE_ONE_SHOT_ORDER_PROBES.map((probe) => ({ entry, probe })),
);

async function captureRefusal(call: () => unknown): Promise<Error> {
  try {
    await call();
  } catch (error) {
    if (error instanceof Error) {
      return error;
    }
    throw error;
  }
  throw new Error('expected the call to refuse');
}

function expectSampleOneShotOrderError(
  error: Error,
  prefix: string,
  kind: (typeof SAMPLE_ONE_SHOT_ORDER_PROBES)[number]['expected'],
): void {
  expect(error).toBeInstanceOf(TypeError);
  if (kind === 'unknown-key') {
    expect(error).toEqual(unknownKeyError(`${prefix}: unknown key bogus`));
    return;
  }
  expect(error.message).toBe(`${prefix}: samples must be a Float32Array`);
}

describe('request-object equivalence', () => {
  it('has one row per exported request-object overload', () => {
    const overloaded = ROWS.filter((row) => row.form !== 'requestOnly').map((row) => row.name);
    expect(overloaded.sort()).toEqual(overloadedFunctions());
  });

  it('compares a polyphonic analysis through its notes, not its handle', () => {
    const options = { nFft: 2048, hopLength: 512 };
    const request = sonare.analyzePolyphonic({ samples: SIG2, sampleRate: ALT_SR, ...options });
    const positional = sonare.analyzePolyphonic(SIG2, ALT_SR, options);
    try {
      expect(request.noteCount).toBe(positional.noteCount);
      expect(request.frameCount).toBe(positional.frameCount);
      expect(request.notes).toEqual(positional.notes);
    } finally {
      request.destroy();
      positional.destroy();
    }
  });

  it.each([
    { name: 'default', options: undefined },
    { name: 'explicit', options: { nFft: 2048, hopLength: 512 } },
  ])('compares polyphonic note data with $name options', ({ options }) => {
    using request = sonare.analyzePolyphonic({ samples: SIG2, sampleRate: ALT_SR, ...options });
    using positional = sonare.analyzePolyphonic(SIG2, ALT_SR, options);
    expect(request.noteCount).toBe(positional.noteCount);
    expect(request.frameCount).toBeGreaterThan(0);
    expect(request.frameCount).toBe(positional.frameCount);
    expect(request.notes()).toEqual(positional.notes());
  });

  it('shares the same polyphonic sample-rate refusal', () => {
    const thrownBy = (run: () => unknown): Error => {
      try {
        run();
      } catch (error) {
        return error as Error;
      }
      throw new Error('expected the call to throw');
    };
    const requestError = thrownBy(() => sonare.analyzePolyphonic({ samples: SIG2, sampleRate: 0 }));
    const positionalError = thrownBy(() => sonare.analyzePolyphonic(SIG2, 0));
    expect(requestError).toBeInstanceOf(RangeError);
    expect(requestError.message).toContain('sampleRate');
    expect(positionalError).toBeInstanceOf(RangeError);
    expect(positionalError.message).toBe(requestError.message);
  });

  it('forwards the phase option identically through both STFT forms', () => {
    const omittedRequest = sonare.stft({ samples: SIG2, sampleRate: ALT_SR });
    const omittedPositional = sonare.stft(SIG2, ALT_SR);
    expect(omittedRequest.phase).toBeUndefined();
    expect(omittedRequest).toEqual(omittedPositional);

    const request = sonare.stft({ samples: SIG2, sampleRate: ALT_SR, phase: true });
    const positional = sonare.stft(SIG2, ALT_SR, { phase: true });
    expect(request.phase?.data).toHaveLength(request.magnitude.data.length);
    expect(request).toEqual(positional);
  });

  it.each(SAMPLE_ONE_SHOT_ORDER_CASES)(
    '$entry.name: $probe.name wins in request and positional forms',
    async ({ entry, probe }) => {
      const requestError = await captureRefusal(() => entry.call(...probe.request));
      const positionalError = await captureRefusal(() => entry.call(...probe.positional));
      expectSampleOneShotOrderError(requestError, entry.prefix, probe.expected);
      expectSampleOneShotOrderError(positionalError, entry.prefix, probe.expected);
    },
  );

  it('checks positional options shape before the sample-rate domain', async () => {
    const error = await captureRefusal(() => dynamicCall(sonare.stft)(SIG2, 0, 42));
    expect(error).toBeInstanceOf(TypeError);
    expect(error.message).toBe('stft: options must be an object');
  });

  it('checks surplus request arguments before the sample-rate domain', async () => {
    const error = await captureRefusal(() => dynamicCall(sonare.stft)({ samples: SIG2 }, 0));
    expect(error).toBeInstanceOf(TypeError);
    expect(error.message).toBe('stft: unexpected argument after the request object');
  });

  it('refuses a missing mastering selector before a fractional rate in mono and stereo', async () => {
    const sampleRate = 22050.5;
    const stereo = { left: SIG2, right: SIG2, sampleRate };
    for (const [name, call] of [
      ['masterAudio', () => sonare.masterAudio({ samples: SIG2, sampleRate })],
      ['masterAudio', () => sonare.masterAudio(SIG2, sampleRate)],
      ['masterAudioStereo', () => sonare.masterAudioStereo(stereo)],
    ] as const) {
      const error = await captureRefusal(call);
      expect(error).toBeInstanceOf(TypeError);
      expect(error.message).toBe(`${name}: give preset or overrides`);
    }
  });

  const empty = new Float32Array(0);
  const booleanOptions = [
    ['stft', sonare.stft, { db: 'yes' }, 'db'],
    ['stft', sonare.stft, { phase: 'yes' }, 'phase'],
    ['nnlsChroma', sonare.nnlsChroma, { enableStftBlend: 'yes' }, 'enableStftBlend'],
    ['analyzeMelody', sonare.analyzeMelody, { usePyin: 'yes' }, 'usePyin'],
    ['analyzeMelody', sonare.analyzeMelody, { center: 'yes' }, 'center'],
    [
      'masteringAudioProfile',
      sonare.masteringAudioProfile,
      { detectDefects: 'yes' },
      'detectDefects',
    ],
    ['remix', sonare.remix, { intervals: [0, 1], alignZeros: 'yes' }, 'alignZeros'],
    [
      'remixAlignedIntervals',
      sonare.remixAlignedIntervals,
      { intervals: [0, 1], alignZeros: 'yes' },
      'alignZeros',
    ],
  ] as const;

  it.each([
    {
      name: 'mixStereo strips shape',
      call: dynamicCall(sonare.mixStereo),
      request: { strips: 'not strips', sampleRate: 0 },
      message: 'mixStereo: strips must be an array',
    },
    {
      name: 'mixStereo muted type',
      call: dynamicCall(sonare.mixStereo),
      request: { strips: [{ left: empty, right: SIG2, muted: 'yes' }], sampleRate: ALT_SR },
      message: 'mixStereo: strips[0].muted must be a boolean',
    },
    {
      name: 'mixStereo faderDb type',
      call: dynamicCall(sonare.mixStereo),
      request: { strips: [{ left: empty, right: SIG2, faderDb: 'bad' }], sampleRate: ALT_SR },
      message: 'mixStereo: strips[0].faderDb must be a number',
    },
    {
      name: 'biquadFilter zeroPhase type',
      call: dynamicCall(sonare.biquadFilter),
      request: {
        samples: SIG2,
        sampleRate: 0,
        type: 'lowPass',
        cutoffHz: 1000,
        zeroPhase: 'yes',
      },
      message: 'biquadFilter: zeroPhase must be a boolean',
    },
    {
      name: 'masterAudioStereo required selector',
      call: dynamicCall(sonare.masterAudioStereo),
      request: { left: SIG2, right: SIG2, sampleRate: 0 },
      message: 'masterAudioStereo: give preset or overrides',
    },
    {
      name: 'masteringRepairApply stages shape',
      call: dynamicCall(sonare.masteringRepairApply),
      request: { channels: [SIG2], sampleRate: 0, stages: 'not stages' },
      message: 'masteringRepairApply: stages must be an array',
    },
    {
      name: 'roomMorphStereo receiverSpacingM type',
      call: dynamicCall(sonare.roomMorphStereo),
      request: { left: SIG2, right: SIG2, sampleRate: 0, receiverSpacingM: 'not a number' },
      message: 'roomMorphStereo: receiverSpacingM must be a number',
    },
    {
      name: 'fourierTempogram center type',
      call: dynamicCall(sonare.fourierTempogram),
      request: { onsetEnvelope: SIG2, sampleRate: 0, center: 'yes' },
      message: 'fourierTempogram: center must be a boolean',
    },
    {
      name: 'masterAudio required selector',
      call: dynamicCall(sonare.masterAudio),
      request: { samples: empty, sampleRate: ALT_SR },
      positional: [empty, ALT_SR],
      message: 'masterAudio: give preset or overrides',
    },
    {
      name: 'renderPercussiveEvents events type',
      call: dynamicCall(sonare.renderPercussiveEvents),
      request: { samples: empty, sampleRate: ALT_SR, events: 'bad' },
      positional: [empty, ALT_SR, { events: 'bad' }],
      message: 'renderPercussiveEvents: events must be an array',
    },
    {
      name: 'segmentRecurrenceMatrix sym type',
      call: dynamicCall(sonare.segmentRecurrenceMatrix),
      request: { data: { data: new Float32Array(1), rows: 2, cols: 1 }, sym: 'yes' },
      message: 'segmentRecurrenceMatrix: sym must be a boolean',
    },
    {
      name: 'segmentRecurrenceToLag pad type',
      call: dynamicCall(sonare.segmentRecurrenceToLag),
      request: { recurrence: { data: new Float32Array(2), rows: 1, cols: 2 }, pad: 'yes' },
      message: 'segmentRecurrenceToLag: pad must be a boolean',
    },
    {
      name: 'melFilterbank htk type',
      call: dynamicCall(sonare.melFilterbank),
      request: { sampleRate: ALT_SR, nFft: 32, fmaxHz: -1, htk: 'yes' },
      message: 'melFilterbank: htk must be a boolean',
    },
    {
      name: 'restoreVocalEditSession state type',
      call: dynamicCall(sonare.restoreVocalEditSession),
      request: { samples: SIG2, sampleRate: 0, state: 'bad' },
      message: 'restoreVocalEditSession: state must be a Uint8Array',
    },
    {
      name: 'createVocalEditSession analysis arrays type',
      call: dynamicCall(sonare.createVocalEditSession),
      request: {
        samples: SIG2,
        sampleRate: ALT_SR,
        analysis: {
          frameOriginSample: 0,
          samplesPerFrame: 0,
          frameLengthSamples: 128,
          f0Hz: 'bad',
          voiced: new Uint8Array(0),
        },
      },
      message: 'createVocalEditSession: analysis arrays must be Float32Array and Uint8Array',
    },
    {
      name: 'masteringAudioProfileStereo detectDefects type',
      call: dynamicCall(sonare.masteringAudioProfileStereo),
      request: { left: empty, right: SIG2, sampleRate: ALT_SR, detectDefects: 'yes' },
      message: 'masteringAudioProfileStereo: detectDefects must be a boolean',
    },
    ...booleanOptions.map(([entry, fn, options, field]) => ({
      name: `${entry} ${field} type`,
      call: dynamicCall(fn),
      request: { samples: empty, sampleRate: ALT_SR, ...options },
      positional: [empty, ALT_SR, options],
      message: `${entry}: ${field} must be a boolean`,
    })),
  ])('$name is refused first', async ({ call, request, message, positional }) => {
    for (const args of [[request], ...(positional ? [positional] : [])]) {
      const error = await captureRefusal(() => call(...args));
      expect(error).toBeInstanceOf(TypeError);
      expect(error.message).toBe(message);
    }
  });

  it('checks percussive options keys before the plain-array input budget in both forms', async () => {
    const call = dynamicCall(sonare.extractPercussiveEvents);
    const samples = new Array(64 * 1024 * 1024 + 1);
    const expected = unknownKeyError('extractPercussiveEvents: unknown key bogus');
    expect(await captureRefusal(() => call({ samples, sampleRate: ALT_SR, bogus: true }))).toEqual(
      expected,
    );
    expect(await captureRefusal(() => call(samples, ALT_SR, { bogus: true }))).toEqual(expected);
  });
});
