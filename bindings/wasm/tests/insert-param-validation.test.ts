/**
 * WASM coverage for insert param validation: masteringInsertParamNames enumerates
 * the keys a processor reads, and Mixer.sceneWarnings surfaces scene-insert params
 * that were silently ignored (matching the C ABI and the other surfaces).
 */

import { beforeAll, describe, expect, it } from 'vitest';
import type { MasteringProcessorCatalogEntry } from '../dist/index.js';
import {
  init,
  Mixer,
  masteringInsertParamInfo,
  masteringInsertParamNames,
  masteringProcessorCatalog,
} from '../dist/index.js';

const SR = 48000;
const BLOCK = 512;

describe('insert param validation (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('enumerates the keys an insert processor reads', () => {
    const comp = masteringInsertParamNames('dynamics.compressor');
    expect(comp).toContain('thresholdDb');
    expect(comp).toContain('ratio');
    // Band-indexed processors expose their band{i}.<field> keys.
    expect(masteringInsertParamNames('eq.parametric')).toContain('band0.frequencyHz');
    // Unknown name -> empty list (no throw).
    expect(masteringInsertParamNames('not.a.real.processor')).toEqual([]);
  });

  it('flags a bound the processor itself rejects as exclusive', () => {
    const hpf = masteringInsertParamInfo('dynamics.compressor').find(
      (d) => d.name === 'sidechainHpfHz',
    );
    expect(hpf?.min).toBe(0);
    expect(typeof hpf?.minExclusive).toBe('boolean');
    expect(hpf?.minExclusive).toBe(true);
    expect(hpf?.maxRelativeTo).toBeNull();
    const frequency = masteringInsertParamInfo('eq.parametric').find(
      (d) => d.name === 'band0.frequencyHz',
    );
    expect(frequency?.maxRelativeTo).toBe('nyquist');
  });

  it('resolves a Nyquist-following ceiling for the host rate and leaves other keys alone', () => {
    const frequency = (rate?: number) =>
      masteringInsertParamInfo('eq.parametric', rate).find((d) => d.name === 'band0.frequencyHz');
    expect(frequency()?.max).toBe(24000);
    expect(frequency(44100)?.max).toBe(22050);
    expect(frequency(44100)?.maxExclusive).toBe(true);
    // The insert's build-time cap holds above its build rate.
    expect(frequency(96000)?.max).toBe(24000);
    const ratio = (rate?: number) =>
      masteringInsertParamInfo('dynamics.compressor', rate).find((d) => d.name === 'ratio');
    expect(ratio(44100)).toEqual(ratio());
    expect(() => masteringInsertParamInfo('eq.parametric', 0)).toThrow();
  });

  it('reports realtime-automatable insert param descriptors', () => {
    const info = masteringInsertParamInfo('effects.reverb.fdn');
    if (info.length === 0) {
      return; // FX not built in this configuration.
    }
    const dryWet = info.find((d) => d.name === 'dryWet');
    expect(dryWet).toBeDefined();
    expect(dryWet?.rtSafe).toBe(true);
    expect(typeof dryWet?.id).toBe('number');
    // Dattorro publishes a non-realtime-safe param (modDepthSamples).
    const dat = masteringInsertParamInfo('effects.reverb.dattorro');
    expect(dat.find((d) => d.name === 'modDepthSamples')?.rtSafe).toBe(false);
    // Unknown name -> empty list.
    expect(masteringInsertParamInfo('not.a.real.processor')).toEqual([]);
  });

  it('classifies processors in the realtime/offline/pair catalog', () => {
    const catalog = masteringProcessorCatalog();
    expect(catalog.length).toBeGreaterThan(0);
    const byId = new Map(catalog.map((entry) => [entry.id, entry]));
    // Realtime-insertable id.
    expect(byId.get('dynamics.compressor')).toMatchObject({
      kind: 'realtime',
      realtimeInsertable: true,
      channelPolicy: 'multichannel',
    });
    expect(typeof byId.get('dynamics.compressor')?.latencySamples).toBe('number');
    expect(typeof byId.get('dynamics.compressor')?.tailSamples).toBe('number');
    expect(byId.get('dynamics.compressor')?.realtimeCost).toBe('low');
    // Pair (two-input match.*) id.
    expect(byId.get('match.abCrossfade')?.kind).toBe('pair');
    // Whole-file-only id.
    expect(byId.get('maximizer.loudnessOptimize')).toMatchObject({
      kind: 'offline',
      realtimeInsertable: false,
    });
    // stereoOnly is surfaced independently of kind.
    expect(byId.get('eq.midSide')?.stereoOnly).toBe(true);
    // channelPolicy: inherently-stereo processors are wrapped on the L/R pair.
    expect(byId.get('eq.midSide')?.channelPolicy).toBe('stereoPairOnly');
    expect(byId.get('stereo.imager')?.channelPolicy).toBe('stereoPairOnly');
    expect(byId.get('stereo.haasEnhancer')?.tailSamples).toBe(576);
    expect(byId.get('stereo.phaseAlign')?.tailSamples).toBe(0);
    expect(byId.get('effects.reverb.velvet')?.realtimeCost).toBe('high');
    expect(byId.get('effects.reverb.fdn')?.realtimeCost).toBe('moderate');
    // A repair stage publishes the bounds its own validation enforces, and says whether it can be causal.
    expect(byId.get('repair.declick')?.causal).toBe(false);
    expect(byId.get('repair.declip')?.causal).toBe(false);
    for (const id of [
      'repair.decrackle',
      'repair.dehum',
      'repair.denoiseClassical',
      'repair.dereverbClassical',
    ]) {
      expect(byId.get(id)?.causal).toBe(true);
    }
    expect(byId.get('dynamics.compressor')?.causal).toBe(true);
    const clickRun = byId
      .get('repair.declick')
      ?.params.find((param) => param.name === 'maxClickSamples');
    expect(clickRun).toMatchObject({ min: 1, max: 512, unit: 'samples', id: null, rtSafe: false });
    // realtimeInsertable entries form the always-succeeds scene-insert set.
    const insertable = catalog.filter((entry) => entry.realtimeInsertable).map((entry) => entry.id);
    expect(insertable).toContain('dynamics.compressor');
    expect(insertable).not.toContain('maximizer.loudnessOptimize');
  });

  it('declares every field the catalog payload carries', () => {
    // The registry emits `category` and `params` unconditionally, but the TS
    // interface stopped at `channelPolicy`, so reading either was a TS2339 on a
    // value that was already there. Compare the runtime key set against the
    // declared one rather than spot-checking, so the next added field cannot
    // go undeclared either.
    const catalog = masteringProcessorCatalog();
    const declared: Record<keyof MasteringProcessorCatalogEntry, true> = {
      id: true,
      kind: true,
      realtimeInsertable: true,
      stereoOnly: true,
      latencySamples: true,
      tailSamples: true,
      realtimeCost: true,
      channelPolicy: true,
      category: true,
      causal: true,
      params: true,
      slots: true,
    };
    for (const entry of catalog) {
      expect(Object.keys(entry).sort()).toEqual(Object.keys(declared).sort());
    }
    const compressor = catalog.find((entry) => entry.id === 'dynamics.compressor');
    expect(compressor?.category).toBe('dynamics');
    expect(catalog.find((entry) => entry.id === 'match.abCrossfade')?.category).toBe('reference');
    // The catalog's params for an insertable id are the same list the
    // per-processor query returns.
    expect(compressor?.params.map((param) => param.name).sort()).toEqual(
      masteringInsertParamInfo('dynamics.compressor')
        .map((param) => param.name)
        .sort(),
    );
    // Non-insertable entries carry an empty list, not a missing key.
    expect(catalog.find((entry) => entry.id === 'maximizer.loudnessOptimize')?.params).toEqual([]);
    // A crossover band past the default split is listed with the cutoffs it needs.
    const band3 = catalog
      .find((entry) => entry.id === 'multiband.compressor')
      ?.slots.find((slot) => slot.name === 'band3');
    expect(band3).toEqual({
      name: 'band3',
      parent: null,
      activation: 'always',
      minCrossoverCutoffs: 3,
    });
  });

  it('surfaces silently-ignored insert params as scene warnings', () => {
    // eq.parametric reads only band{i}.* fields, so flat keys take no effect.
    const ignored = JSON.stringify({
      version: 1,
      buses: [{ id: 'master', role: 'master' }],
      strips: [
        {
          id: 'vocal',
          inserts: [
            {
              slot: 'post',
              processor: 'eq.parametric',
              params: JSON.stringify({ highPassHz: 80, presenceDb: 4 }),
            },
          ],
        },
      ],
      connections: [{ source: 'vocal', destination: 'master' }],
    });
    const mixer = Mixer.fromSceneJson(ignored, SR, BLOCK);
    const warnings = mixer.sceneWarnings();
    expect(warnings.length).toBe(1);
    expect(warnings[0]).toContain('eq.parametric');
    expect(warnings[0]).toContain('highPassHz');
    expect(warnings[0]).toContain('presenceDb');

    // A scene whose params are all consumed reports no warnings.
    const clean = JSON.stringify({
      version: 1,
      buses: [{ id: 'master', role: 'master' }],
      strips: [
        {
          id: 'vocal',
          inserts: [
            {
              slot: 'post',
              processor: 'eq.parametric',
              params: JSON.stringify({ 'band0.frequencyHz': 1000, 'band0.gainDb': 3 }),
            },
          ],
        },
      ],
      connections: [{ source: 'vocal', destination: 'master' }],
    });
    expect(Mixer.fromSceneJson(clean, SR, BLOCK).sceneWarnings()).toEqual([]);
  });

  it('reports an unknown scene key by path and exempts $ and x- keys', () => {
    const scene = (extra: Record<string, unknown>, stripExtra: Record<string, unknown>) =>
      JSON.stringify({
        version: 1,
        ...extra,
        buses: [{ id: 'master', role: 'master' }],
        strips: [{ id: 'vocal', ...stripExtra }],
        connections: [{ source: 'vocal', destination: 'master' }],
      });
    expect(Mixer.fromSceneJson(scene({}, { faderDB: -3 }), SR, BLOCK).sceneWarnings()).toEqual([
      "unknown scene key 'strips[0].faderDB'",
    ]);
    const annotated = scene({ $schema: 'x', 'x-note': 1 }, { 'x-color': 'red' });
    expect(Mixer.fromSceneJson(annotated, SR, BLOCK).sceneWarnings()).toEqual([]);
  });
});
