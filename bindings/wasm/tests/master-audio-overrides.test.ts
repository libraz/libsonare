import { beforeAll, describe, expect, it } from 'vitest';
import { Audio, init, masterAudio, masterAudioStereo } from '../src/index';

const sampleRate = 22_050;
const samples = Float32Array.from(
  { length: 8192 },
  (_, i) => 0.5 * Math.sin((2 * Math.PI * 220 * i) / sampleRate),
);

beforeAll(async () => init());

describe('WASM masterAudio nested overrides (H-1)', () => {
  it('applies nested MasteringChainConfig overrides instead of silently dropping them', () => {
    const loud = masterAudio(samples, sampleRate, 'pop', { loudness: { targetLufs: -8 } });
    const quiet = masterAudio(samples, sampleRate, 'pop', { loudness: { targetLufs: -24 } });
    // A dropped override would yield identical results for both targets.
    expect(loud.outputLufs).not.toBeCloseTo(quiet.outputLufs, 1);
    expect(loud.outputLufs).toBeGreaterThan(quiet.outputLufs);
  });

  it('drives output loudness toward the requested target LUFS', () => {
    const target = -18;
    const result = masterAudio(samples, sampleRate, 'pop', { loudness: { targetLufs: target } });
    expect(result.outputLufs).toBeGreaterThan(target - 4);
    expect(result.outputLufs).toBeLessThan(target + 4);
  });

  it('produces identical results for positional and request-object forms', () => {
    const overrides = { loudness: { targetLufs: -16, ceilingDb: -1.5 } };
    const positional = masterAudio(samples, sampleRate, 'pop', overrides);
    const request = masterAudio({ samples, sampleRate, preset: 'pop', overrides });
    expect(request.outputLufs).toBeCloseTo(positional.outputLufs, 6);
    expect(request.samples).toEqual(positional.samples);
    expect(request.stages).toEqual(positional.stages);
  });

  it('applies stereo nested overrides for both call forms', () => {
    const overrides = { loudness: { targetLufs: -12 } };
    const positional = masterAudioStereo(samples, samples, sampleRate, 'pop', overrides);
    const request = masterAudioStereo({
      left: samples,
      right: samples,
      sampleRate,
      preset: 'pop',
      overrides,
    });
    expect(request.outputLufs).toBeCloseTo(positional.outputLufs, 6);
    expect(request.samples).toEqual(positional.samples);
  });
});

describe('WASM canonical mastering-chain config (H-1)', () => {
  it('routes nested tilt and all repair stages through the core flat parser', async () => {
    const { masteringChain } = await import('../src/index');
    const input = Float32Array.from(
      { length: 8192 },
      (_, i) => 0.25 * Math.sin((2 * Math.PI * 220 * i) / sampleRate),
    );
    const result = masteringChain(input, sampleRate, {
      eq: { tilt: { tiltDb: 1 } },
      repair: {
        declip: { clipThreshold: 0.95 },
        decrackle: { threshold: 0.25 },
        dehum: { fundamentalHz: 60 },
      },
    });
    expect(result.stages).toEqual(
      expect.arrayContaining(['eq.tilt', 'repair.declip', 'repair.decrackle', 'repair.dehum']),
    );
  });

  it('honors enabled:false without truthy coercion', async () => {
    const { masteringChain } = await import('../src/index');
    const input = new Float32Array(4096).fill(0.1);
    const result = masteringChain(input, sampleRate, {
      repair: { denoise: { enabled: false } },
      eq: { tilt: { enabled: false, tiltDb: 3 } },
    });
    expect(result.stages).not.toContain('repair.denoise');
    expect(result.stages).not.toContain('eq.tilt');
  });

  it('reports output true-peak, LRA, and per-stage gain reductions (M-1)', async () => {
    const { masteringChain } = await import('../src/index');
    const input = Float32Array.from(
      { length: 8192 },
      (_, i) => 0.3 * Math.sin((2 * Math.PI * 220 * i) / sampleRate),
    );
    const result = masteringChain(input, sampleRate, {
      dynamics: { compressor: { thresholdDb: -30, ratio: 4 } },
      loudness: { targetLufs: -14, ceilingDb: -1 },
    });
    expect(Number.isFinite(result.outputTruePeakDbtp)).toBe(true);
    expect(result.outputTruePeakDbtp).toBeLessThanOrEqual(0);
    expect(Number.isFinite(result.outputLra)).toBe(true);
    expect(result.outputLra).toBeGreaterThanOrEqual(0);

    const grStages = result.stageGainReductions.map((r) => r.stage);
    expect(grStages).toContain('dynamics.compressor');
    for (const reduction of result.stageGainReductions) {
      expect(result.stages).toContain(reduction.stage);
      expect(reduction.gainReductionDb).toBeLessThanOrEqual(0);
    }

    // masterAudio (preset path) exposes the same fields.
    const preset = masterAudio(input, sampleRate, 'pop');
    expect(Number.isFinite(preset.outputTruePeakDbtp)).toBe(true);
    expect(Number.isFinite(preset.outputLra)).toBe(true);
    expect(Array.isArray(preset.stageGainReductions)).toBe(true);
  });
});

describe('WASM masterAudio / masteringChain progress wiring (M-1)', () => {
  it('invokes onProgress from the positional masterAudio form', () => {
    let calls = 0;
    masterAudio(samples, sampleRate, 'pop', { loudness: { targetLufs: -14 } }, () => {
      calls += 1;
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

  it('invokes onProgress from Audio.masterAudio', () => {
    let calls = 0;
    Audio.fromBuffer(samples, sampleRate).masterAudio('pop', null, () => {
      calls += 1;
    });
    expect(calls).toBeGreaterThan(0);
  });

  it('invokes onProgress from Audio.masteringChain', () => {
    let calls = 0;
    Audio.fromBuffer(samples, sampleRate).masteringChain({ loudness: { targetLufs: -14 } }, () => {
      calls += 1;
    });
    expect(calls).toBeGreaterThan(0);
  });
});
