import { describe, expect, it } from 'vitest';
import {
  ErrorCode,
  type MasteringRepairStageReports,
  masteringRepairAnalyze,
  masteringRepairApply,
  masteringRepairDeclickStereo,
  masteringRepairDenoiseClassicalLinked,
} from '../src/index.js';

const SR = 22050;
const LENGTH = SR;

/** A tone with deterministic noise, so every detector has a body to measure against. */
function material(freq: number, seed: number): Float32Array {
  const out = new Float32Array(LENGTH);
  let state = seed >>> 0;
  for (let i = 0; i < LENGTH; i += 1) {
    state = (state * 1664525 + 1013904223) >>> 0;
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

describe('masteringRepairAnalyze', () => {
  it('measures every channel and recommends from the assistant rules', () => {
    const channels = [material(440, 1), clicked(material(550, 2), 3000, 7000), material(660, 3)];
    const analysis = masteringRepairAnalyze({ channels, sampleRate: SR });

    expect(analysis.channels).toHaveLength(3);
    expect(analysis.channels[0]?.clickCount).toBe(0);
    expect(analysis.channels[1]?.clickCount).toBeGreaterThan(0);
    expect(analysis.defects.clickCount).toBe(analysis.channels[1]?.clickCount);
    expect(typeof analysis.declipThresholdSafe).toBe('boolean');
    expect(Number.isFinite(analysis.integratedLufs)).toBe(true);
    expect(analysis.recommended[0]?.stage).toBe('declick');
    expect(analysis.recommended.some((stage) => stage.stage === 'dereverb')).toBe(false);
    expect(analysis.explanation.length).toBeGreaterThan(0);

    // The recommendation is a stage list as it stands.
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
    const channels = [clicked(material(440, 4), 2000, 6000)];
    expect(masteringRepairAnalyze({ channels })).toEqual(
      masteringRepairAnalyze({ channels, sampleRate: 22050, preferStreamingSafe: true }),
    );
  });

  it('refuses an empty channel set as an argument error', () => {
    expect(() => masteringRepairAnalyze({ channels: [], sampleRate: SR })).toThrow(
      expect.objectContaining({ code: ErrorCode.InvalidParameter }),
    );
  });
});

describe('masteringRepairApply', () => {
  it('runs the fixed order whatever order the list gives', () => {
    const channels = [clicked(material(440, 5), 2000, 9000), material(660, 6)];
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

  it('matches the per-stage stereo and linked entries', () => {
    const left = clicked(material(440, 7), 1500, 8000);
    const right = material(660, 8);
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
    expect(denoise.reports[0]?.reports[0]).toEqual(linked.report);
  });

  it('reports per channel for channel stages and once for linked ones', () => {
    const channels = [material(440, 9), material(550, 10), material(660, 11)];
    const { reports } = masteringRepairApply({
      channels,
      sampleRate: SR,
      stages: [{ stage: 'dereverb' }, { stage: 'dehum', adaptive: true }],
    });
    const scopes = reports.map((entry: MasteringRepairStageReports) => [
      entry.stage,
      entry.scope,
      entry.reports.length,
    ]);
    expect(scopes).toEqual([
      ['dehum', 'channel', 3],
      ['dereverb', 'linked', 1],
    ]);
  });

  it('refuses a repeated stage, an unknown stage and an unknown setting', () => {
    const channels = [material(440, 12)];
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
        // @ts-expect-error trim is not a repair stage of this application
        stages: [{ stage: 'trimSilence' }],
      }),
    ).toThrow(refused);
    expect(() =>
      masteringRepairApply({
        channels,
        sampleRate: SR,
        stages: [{ stage: 'declick', lpcOdrer: 8 }],
      }),
    ).toThrow(refused);
    expect(() =>
      // @ts-expect-error stages is required
      masteringRepairApply({ channels, sampleRate: SR }),
    ).toThrow(TypeError);
  });

  it('reports progress per stage and stops when cancelled', () => {
    const channels = [material(440, 13), material(550, 14)];
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
    ).toThrow(expect.objectContaining({ code: ErrorCode.Cancelled, codeName: 'Cancelled' }));
  });
});
