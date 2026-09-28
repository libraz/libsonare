import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringAmpPresetCatalog } from '../dist/index.js';

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
  params: Record<string, number | boolean> | undefined,
  expected: Record<string, number | boolean>,
) {
  expect(params).toBeDefined();
  for (const [key, value] of Object.entries(expected)) {
    const actual = params?.[key];
    if (typeof value === 'boolean') {
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
      [3, 'tweedGrind'],
      [4, 'britStack'],
      [5, 'modernLead'],
      [6, 'rectifierChug'],
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
      topology: 0,
      inputDb: 0,
      drive: 0.25,
      bassDb: 2,
      midDb: -1,
      trebleDb: 3,
      presenceDb: 1.5,
      cab: true,
      ampModel: 1,
      power: 0.15,
      nfb: 0.4,
      micModel: 1,
      micAxis: 0.35,
      micDistanceCm: 3,
    });
    expectParams(modern, {
      topology: 1,
      drive: 0.8,
      bassDb: 1,
      midDb: -3,
      trebleDb: 4,
      presenceDb: 3,
      cab: true,
      ampModel: 2,
      power: 0.55,
      nfb: 0.45,
      micModel: 1,
      micAxis: 0.2,
      micDistanceCm: 2.5,
      preampStages: 4,
    });
    expectParams(bass, {
      topology: 0,
      drive: 0.15,
      bassDb: 2,
      midDb: 0,
      trebleDb: 1,
      cab: false,
      ampModel: 1,
      power: 0.2,
      nfb: 0.3,
      levelDb: -1,
    });
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
