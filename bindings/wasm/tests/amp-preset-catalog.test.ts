import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringAmpPresetCatalog, masteringInsertParamInfo } from '../dist/index.js';

const PARAM_KEYS = [
  'topology',
  'inputDb',
  'drive',
  'bassDb',
  'midDb',
  'trebleDb',
  'presenceDb',
  'cab',
  'cabModel',
  'ampModel',
  'levelDb',
  'power',
  'sag',
  'transformer',
  'nfb',
  'micModel',
  'micAxis',
  'micDistanceCm',
  'micBlend',
  'micBModel',
  'micBAxis',
  'micBDistanceCm',
  'micBInvert',
  'cone',
  'doppler',
  'preampStages',
  'biasShift',
  'crossover',
  'powerTube',
].sort();

function expectParams(
  params: Record<string, number | boolean | string> | undefined,
  expected: Record<string, number | boolean | string>,
) {
  expect(params).toBeDefined();
  for (const [key, value] of Object.entries(expected)) {
    const actual = params?.[key];
    if (typeof value !== 'number') {
      expect(actual).toBe(value);
    } else {
      expect(actual).toBeCloseTo(value, 5);
    }
  }
}

describe('amp preset catalog', () => {
  beforeAll(async () => {
    await init();
  });

  it('exposes all ten core presets in stable index order', () => {
    const catalog = masteringAmpPresetCatalog();
    expect(catalog).toHaveLength(10);
    expect(catalog.map((entry) => [entry.index, entry.name])).toEqual([
      [0, 'cleanCombo'],
      [1, 'chimeEdge'],
      [2, 'classicCrunch'],
      [3, 'vintageGrind'],
      [4, 'crunchStack'],
      [5, 'modernLead'],
      [6, 'heavyChug'],
      [7, 'coldBiasBuzz'],
      [8, 'bassDi'],
      [9, 'bassRig'],
    ]);
    for (const entry of catalog) {
      expect(Object.keys(entry.params).sort()).toEqual(PARAM_KEYS);
      for (const value of Object.values(entry.params)) {
        if (typeof value === 'number') {
          expect(Number.isFinite(value)).toBe(true);
        }
      }
    }
  });

  it('matches canonical clean combo, modern lead, and bass DI values', () => {
    const byName = new Map(masteringAmpPresetCatalog().map((entry) => [entry.name, entry.params]));
    const clean = byName.get('cleanCombo');
    const modern = byName.get('modernLead');
    const bass = byName.get('bassDi');
    expect(clean).toBeDefined();
    expect(modern).toBeDefined();
    expect(bass).toBeDefined();

    expectParams(clean, {
      topology: 'voiced',
      inputDb: 0,
      drive: 0.25,
      bassDb: 2,
      midDb: -1,
      trebleDb: 3,
      presenceDb: 1.5,
      cab: true,
      ampModel: 'cleanCombo',
      power: 0.15,
      nfb: 0.4,
      micModel: 'dynamic',
      micAxis: 0.35,
      micDistanceCm: 3,
    });
    expectParams(modern, {
      topology: 'circuit',
      drive: 0.8,
      bassDb: 1,
      midDb: -3,
      trebleDb: 4,
      presenceDb: 3,
      cab: true,
      ampModel: 'modernHiGain',
      power: 0.55,
      nfb: 0.45,
      micModel: 'dynamic',
      micAxis: 0.2,
      micDistanceCm: 2.5,
      preampStages: 4,
    });
    expectParams(bass, {
      topology: 'voiced',
      drive: 0.15,
      bassDb: 2,
      midDb: 0,
      trebleDb: 1,
      cab: false,
      ampModel: 'cleanCombo',
      power: 0.2,
      nfb: 0.3,
      levelDb: -1,
    });
  });

  it('carries each parameter with the type the amp-sim insert declares', () => {
    const declared = new Map(
      masteringInsertParamInfo({ insert: 'saturation.ampSim' }).map((p) => [p.name, p]),
    );
    const misfits: string[] = [];
    for (const entry of masteringAmpPresetCatalog()) {
      for (const [key, value] of Object.entries(entry.params)) {
        const param = declared.get(key);
        const fits =
          param?.type === 'enum'
            ? (param.choices ?? []).some((choice) => choice.name === value)
            : param?.type === 'boolean'
              ? typeof value === 'boolean'
              : typeof value === 'number' && Number.isFinite(value);
        if (!fits) {
          misfits.push(`${entry.name}.${key}: ${JSON.stringify(value)}`);
        }
      }
    }
    expect(misfits).toEqual([]);
  });

  it('returns fresh parameter objects for read-only consumers', () => {
    const first = masteringAmpPresetCatalog();
    first[0].params.drive = 0;
    first[0].params.cab = false;
    const second = masteringAmpPresetCatalog();
    expect(second[0].params.drive).toBe(0.25);
    expect(second[0].params.cab).toBe(true);
  });
});
