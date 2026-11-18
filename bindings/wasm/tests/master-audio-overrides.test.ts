import { beforeAll, describe, expect, it } from 'vitest';
import { Audio, ErrorCode, init, masterAudio, masterAudioStereo } from '../src/index';

const sampleRate = 22_050;
const samples = Float32Array.from(
  { length: 8192 },
  (_, i) => 0.5 * Math.sin((2 * Math.PI * 220 * i) / sampleRate),
);

beforeAll(async () => init());

describe('WASM masterAudio nested overrides', () => {
  it('refuses a string on a number override by path instead of dropping it', () => {
    expect(() =>
      masterAudio({
        samples,
        sampleRate,
        preset: 'pop',
        overrides: { loudness: { targetLufs: 'not-a-number' } } as never,
      }),
    ).toThrow(
      expect.objectContaining({
        code: ErrorCode.InvalidFormat,
        message: expect.stringContaining('loudness.targetLufs is a string, but takes no token'),
      }),
    );
  });

  it('applies nested MasteringChainConfig overrides instead of silently dropping them', () => {
    const loud = masterAudio({
      samples,
      sampleRate,
      preset: 'pop',
      overrides: { loudness: { targetLufs: -8 } },
    });
    const quiet = masterAudio({
      samples,
      sampleRate,
      preset: 'pop',
      overrides: { loudness: { targetLufs: -24 } },
    });
    // A dropped override would yield identical results for both targets.
    expect(loud.outputLufs).not.toBeCloseTo(quiet.outputLufs, 1);
    expect(loud.outputLufs).toBeGreaterThan(quiet.outputLufs);
  });

  it('drives output loudness toward the requested target LUFS', () => {
    const target = -18;
    const result = masterAudio({
      samples,
      sampleRate,
      preset: 'pop',
      overrides: { loudness: { targetLufs: target } },
    });
    expect(result.outputLufs).toBeGreaterThan(target - 4);
    expect(result.outputLufs).toBeLessThan(target + 4);
  });

  it('produces identical results for positional and request-object forms', () => {
    const overrides = { loudness: { targetLufs: -16, ceilingDbtp: -1.5 } };
    const positional = masterAudio(samples, sampleRate, { preset: 'pop', overrides });
    const request = masterAudio({ samples, sampleRate, preset: 'pop', overrides });
    expect(request.outputLufs).toBeCloseTo(positional.outputLufs, 6);
    expect(request.samples).toEqual(positional.samples);
    expect(request.stages).toEqual(positional.stages);
  });

  it('applies stereo nested overrides and takes the request form only', () => {
    const overrides = { loudness: { targetLufs: -12 } };
    const request = masterAudioStereo({
      left: samples,
      right: samples,
      sampleRate,
      preset: 'pop',
      overrides,
    });
    expect(Number.isFinite(request.outputLufs)).toBe(true);
    // The rendered audio must be non-silent, otherwise equality would hold for empty buffers.
    expect(Math.max(...request.left.map(Math.abs))).toBeGreaterThan(0);
    expect(() =>
      (masterAudioStereo as (...args: unknown[]) => unknown)(samples, samples, sampleRate),
    ).toThrow(TypeError);
  });
});

describe('WASM canonical mastering-chain config', () => {
  it('routes nested tilt and all repair stages through the core flat parser', () => {
    const input = Float32Array.from(
      { length: 8192 },
      (_, i) => 0.25 * Math.sin((2 * Math.PI * 220 * i) / sampleRate),
    );
    const result = masterAudio({
      samples: input,
      sampleRate,
      overrides: {
        eq: { tilt: { tiltDb: 1 } },
        repair: {
          declip: { clipThresholdDb: -0.45 },
          decrackle: { threshold: 0.25 },
          dehum: { fundamentalHz: 60 },
        },
      },
    });
    expect(result.stages).toEqual(
      expect.arrayContaining(['eq.tilt', 'repair.declip', 'repair.decrackle', 'repair.dehum']),
    );
  });

  it('honors enabled:false without truthy coercion', () => {
    const input = new Float32Array(4096).fill(0.1);
    const result = masterAudio({
      samples: input,
      sampleRate,
      overrides: {
        repair: { denoise: { enabled: false } },
        eq: { tilt: { enabled: false, tiltDb: 3 } },
      },
    });
    expect(result.stages).not.toContain('repair.denoise');
    expect(result.stages).not.toContain('eq.tilt');
  });

  it('reports output true-peak, LRA, and per-stage gain reductions', () => {
    const input = Float32Array.from(
      { length: 8192 },
      (_, i) => 0.3 * Math.sin((2 * Math.PI * 220 * i) / sampleRate),
    );
    const result = masterAudio({
      samples: input,
      sampleRate,
      overrides: {
        dynamics: { compressor: { thresholdDb: -30, ratio: 4 } },
        loudness: { targetLufs: -14, ceilingDbtp: -1 },
      },
    });
    expect(Number.isFinite(result.outputTruePeakDbtp)).toBe(true);
    expect(result.outputTruePeakDbtp).toBeLessThanOrEqual(0);
    expect(Number.isFinite(result.outputLoudnessRangeLu)).toBe(true);
    expect(result.outputLoudnessRangeLu).toBeGreaterThanOrEqual(0);

    const grStages = result.stageGainReductions.map((r) => r.stage);
    expect(grStages).toContain('dynamics.compressor');
    for (const reduction of result.stageGainReductions) {
      expect(result.stages).toContain(reduction.stage);
      expect(reduction.gainReductionDb).toBeLessThanOrEqual(0);
    }

    // masterAudio (preset path) exposes the same fields.
    const preset = masterAudio({ samples: input, sampleRate, preset: 'pop' });
    expect(Number.isFinite(preset.outputTruePeakDbtp)).toBe(true);
    expect(Number.isFinite(preset.outputLoudnessRangeLu)).toBe(true);
    expect(Array.isArray(preset.stageGainReductions)).toBe(true);
  });
});

describe('WASM masterAudio progress wiring', () => {
  it('invokes onProgress from the positional masterAudio form', () => {
    let calls = 0;
    masterAudio(samples, sampleRate, {
      preset: 'pop',
      overrides: { loudness: { targetLufs: -14 } },
      onProgress: () => {
        calls += 1;
      },
    });
    expect(calls).toBeGreaterThan(0);
  });

  it('invokes onProgress from the request-object masterAudio form', () => {
    let calls = 0;
    masterAudio({
      samples,
      sampleRate,
      preset: 'pop',
      onProgress: () => {
        calls += 1;
      },
    });
    expect(calls).toBeGreaterThan(0);
  });

  it('invokes onProgress from Audio.masterAudio, with and without a preset', () => {
    let calls = 0;
    const onProgress = () => {
      calls += 1;
    };
    const audio = Audio.fromBuffer(samples, sampleRate);
    audio.masterAudio({ preset: 'pop', onProgress });
    expect(calls).toBeGreaterThan(0);
    calls = 0;
    audio.masterAudio({ overrides: { loudness: { targetLufs: -14 } }, onProgress });
    expect(calls).toBeGreaterThan(0);
  });
});
