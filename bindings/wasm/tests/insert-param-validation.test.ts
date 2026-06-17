/**
 * WASM coverage for insert param validation: masteringInsertParamNames enumerates
 * the keys a processor reads, and Mixer.sceneWarnings surfaces scene-insert params
 * that were silently ignored (matching the C ABI and the other surfaces).
 */

import { beforeAll, describe, expect, it } from 'vitest';
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
    // realtimeInsertable entries form the always-succeeds scene-insert set.
    const insertable = catalog.filter((entry) => entry.realtimeInsertable).map((entry) => entry.id);
    expect(insertable).toContain('dynamics.compressor');
    expect(insertable).not.toContain('maximizer.loudnessOptimize');
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
});
