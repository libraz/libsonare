import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  init,
  masteringRepairAnalyze,
  masteringRepairApply,
  masteringRepairDeclickStereo,
  masteringRepairDeclipStereo,
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
    expect(analysis.channels?.[0]?.click.count).toBe(0);
    expect(analysis.channels?.[1]?.click.count).toBeGreaterThan(0);
    expect(analysis.defects?.noise.bandFloorDbfs).toBeInstanceOf(Float32Array);
    expect('measured' in (analysis.defects as object)).toBe(false);
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

  it('defaults the streaming-safe preference', () => {
    const channels = [clicked(material(440, 3), 2000, 3500)];
    expect(masteringRepairAnalyze({ channels, sampleRate: SR })).toEqual(
      masteringRepairAnalyze({ channels, sampleRate: SR, preferStreamingSafe: true }),
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
    expect(declick.channels[0]).toEqual(pair.channels[0]);
    expect(declick.channels[1]).toEqual(pair.channels[1]);
    expect(declick.reports[0]?.reports[0]).toEqual(pair.reports[0]);

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
      expect(report.reports[0]).toEqual(linked.reports[0]);
    }
  });

  it('declips each channel on its own unless the stage sets linkChannels', () => {
    const left = material(440, 8);
    const right = material(660, 9);
    left.fill(0.95, 3000, 3006);
    right.fill(0.95, 3000, 3020);
    const options = { clipThresholdDb: -0.9151 };
    for (const linkChannels of [undefined, true]) {
      const session = masteringRepairApply({
        channels: [left, right],
        sampleRate: SR,
        stages: [{ stage: 'declip', ...options, ...(linkChannels ? { linkChannels } : {}) }],
      });
      const pair = masteringRepairDeclipStereo({
        left,
        right,
        sampleRate: SR,
        ...options,
        ...(linkChannels ? { linkChannels } : {}),
      });
      expect(session.channels).toEqual(pair.channels);
      const report = session.reports[0];
      expect(report?.stage).toBe('declip');
      if (report?.stage === 'declip') {
        expect(report.reports[0].linkedRuns).toBe(linkChannels ? 1 : 0);
      }
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

  it('refuses a repeated stage and an unknown setting as RangeErrors', () => {
    const channels = [material(440, 11)];
    // The session's own refusals, worded by the core; a binding reader would
    // name the key in its own terms ("takes a number").
    expect(() =>
      masteringRepairApply({
        channels,
        sampleRate: SR,
        stages: [{ stage: 'declick' }, { stage: 'declick' }],
      }),
    ).toThrow(/stages\[1\]\.stage must not repeat a stage given earlier/);
    expect(() =>
      masteringRepairApply({
        channels,
        sampleRate: SR,
        stages: [{ stage: 'declick', lpcOdrer: 8 } as never],
      }),
    ).toThrow(/unknown key stages\[0\]\.lpcOdrer/);
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

describe('a channel list whose lengths disagree', () => {
  const ragged = (): Float32Array[] => [material(440, 1), material(550, 2).slice(1)];
  const refusal = /channels\[1\] must have the same length as channels\[0\]$/;

  it('is refused by index on analysis and on apply', () => {
    expect(() => masteringRepairAnalyze({ channels: ragged(), sampleRate: SR })).toThrow(refusal);
    expect(() =>
      masteringRepairApply({ channels: ragged(), sampleRate: SR, stages: [{ stage: 'declick' }] }),
    ).toThrow(refusal);
  });
});
