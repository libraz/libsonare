import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  init,
  masteringRepairAnalyze,
  masteringRepairApply,
  masteringRepairDeclickStereo,
  masteringRepairDenoiseClassicalLinked,
} from '../src/index';

const SR = 22050;
const LENGTH = SR / 2;

/** A tone with deterministic noise, so every detector has a body to measure against. */
function material(freq: number, seed: number): Float32Array {
  const out = new Float32Array(LENGTH);
  let state = seed >>> 0;
  for (let i = 0; i < LENGTH; i += 1) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    out[i] = 0.2 * Math.sin((2 * Math.PI * freq * i) / SR) + 0.003 * (state / 0x7fffffff - 1);
  }
  return out;
}

function clicked(signal: Float32Array, first: number, stride: number): Float32Array {
  const out = Float32Array.from(signal);
  for (let i = first; i < out.length; i += stride) {
    out[i] = 0.95;
  }
  return out;
}

beforeAll(async () => {
  await init();
});

describe('masteringRepairAnalyze', () => {
  it('measures every channel and recommends from the assistant rules', () => {
    const channels = [material(440, 1), clicked(material(550, 2), 2000, 3500)];
    const analysis = masteringRepairAnalyze({ channels, sampleRate: SR });

    expect(analysis.channels).toHaveLength(2);
    expect(analysis.channels[0]?.clickCount).toBe(0);
    expect(analysis.channels[1]?.clickCount).toBeGreaterThan(0);
    expect(typeof analysis.declipThresholdSafe).toBe('boolean');
    expect(analysis.recommended[0]?.stage).toBe('declick');
    expect(analysis.recommended.some((stage) => stage.stage === 'dereverb')).toBe(false);

    const applied = masteringRepairApply({
      channels,
      sampleRate: SR,
      stages: analysis.recommended,
    });
    expect(applied.reports.map((entry) => entry.stage)).toEqual(
      analysis.recommended.map((stage) => stage.stage),
    );
  });

  it('defaults the sample rate and streaming-safe preference', () => {
    const channels = [clicked(material(440, 3), 2000, 3500)];
    expect(masteringRepairAnalyze({ channels })).toEqual(
      masteringRepairAnalyze({ channels, sampleRate: 22050, preferStreamingSafe: true }),
    );
  });
});

describe('masteringRepairApply', () => {
  it('runs the fixed order whatever order the list gives', () => {
    const channels = [clicked(material(440, 4), 1500, 4000), material(660, 5)];
    const forward = masteringRepairApply({
      channels,
      sampleRate: SR,
      stages: [{ stage: 'decrackle' }, { stage: 'declick' }],
    });
    const reversed = masteringRepairApply({
      channels,
      sampleRate: SR,
      stages: [{ stage: 'declick' }, { stage: 'decrackle' }],
    });
    expect(forward).toEqual(reversed);
    expect(forward.reports.map((entry) => entry.stage)).toEqual(['declick', 'decrackle']);
  });

  it('matches the per-stage stereo and linked entries, band arrays included', () => {
    const left = clicked(material(440, 6), 1500, 4000);
    const right = material(660, 7);
    const declick = masteringRepairApply({
      channels: [left, right],
      sampleRate: SR,
      stages: [{ stage: 'declick', lpcOrder: 16 }],
    });
    const pair = masteringRepairDeclickStereo({ left, right, sampleRate: SR, lpcOrder: 16 });
    expect(declick.channels[0]).toEqual(pair.left);
    expect(declick.channels[1]).toEqual(pair.right);
    expect(declick.reports[0]?.reports[0]).toEqual(pair.leftReport);

    const denoise = masteringRepairApply({
      channels: [left, right, left],
      sampleRate: SR,
      stages: [{ stage: 'denoise', mode: 'mmseStsa', reductionDb: 18 }],
    });
    const linked = masteringRepairDenoiseClassicalLinked({
      channels: [left, right, left],
      sampleRate: SR,
      mode: 'mmseStsa',
      reductionDb: 18,
    });
    expect(denoise.channels).toEqual(linked.channels);
    const report = denoise.reports[0];
    expect(report?.stage).toBe('denoise');
    if (report?.stage === 'denoise') {
      expect(report.reports[0].detected.bandFloorDbfs).toBeInstanceOf(Float32Array);
      expect(report.reports[0]).toEqual(linked.report);
    }
  });

  it('reports per channel for channel stages and once for linked ones', () => {
    const channels = [material(440, 8), material(550, 9), material(660, 10)];
    const { reports } = masteringRepairApply({
      channels,
      sampleRate: SR,
      stages: [{ stage: 'dereverb' }, { stage: 'dehum', adaptive: true }],
    });
    expect(reports.map((entry) => [entry.stage, entry.scope, entry.reports.length])).toEqual([
      ['dehum', 'channel', 3],
      ['dereverb', 'linked', 1],
    ]);
  });

  it('refuses a repeated stage and an unknown setting as coded errors', () => {
    const channels = [material(440, 11)];
    const refused = expect.objectContaining({ code: ErrorCode.InvalidParameter });
    expect(() =>
      masteringRepairApply({
        channels,
        sampleRate: SR,
        stages: [{ stage: 'declick' }, { stage: 'declick' }],
      }),
    ).toThrow(refused);
    expect(() =>
      masteringRepairApply({
        channels,
        sampleRate: SR,
        stages: [{ stage: 'declick', lpcOdrer: 8 }],
      }),
    ).toThrow(refused);
  });

  it('reports progress per stage and stops when cancelled', () => {
    const channels = [material(440, 12), material(550, 13)];
    const seen: Array<[number, string]> = [];
    masteringRepairApply({
      channels,
      sampleRate: SR,
      stages: [{ stage: 'decrackle' }, { stage: 'declick' }],
      onProgress: (progress, stage) => seen.push([progress, stage]),
    });
    expect(seen).toEqual([
      [0.5, 'repair.declick'],
      [1, 'repair.decrackle'],
    ]);
    expect(() =>
      masteringRepairApply({
        channels,
        sampleRate: SR,
        stages: [{ stage: 'declick' }],
        cancel: () => true,
      }),
    ).toThrow(expect.objectContaining({ code: ErrorCode.Cancelled }));
  });
});
