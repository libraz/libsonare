/* Synthetic sets for the tests: invented names only, never a product or a library. */

import type { Manifest, QueueStep, SetEntry } from '../src/api/schemas';
import { Manifest as ManifestSchema } from '../src/api/schemas';
import { type AppState, getState, setState, type Take } from '../src/state/store';

export const SETS: SetEntry[] = [
  {
    id: 'p000-grand',
    title: 'Grand',
    takes: 2,
    compare: true,
    group: 'piano',
    path: '',
    ships: { state: 'current' },
  },
  {
    id: 'p019-organ',
    title: 'Organ',
    takes: 1,
    compare: false,
    group: 'organ',
    path: '',
    ships: { state: 'stale' },
  },
];

/** A page with a product comparison (matched) and an instrument one (context only). */
export function comparedManifest(over: Partial<Manifest> = {}): Manifest {
  return ManifestSchema.parse({
    title: 'Grand',
    set_generation: 'g'.repeat(64),
    voice: { program: 0, bank: 0, name: 'Grand', rig: 'amp' },
    sources: {
      model: { label: 'library', role: 'model', scope: 'product' },
      'model-di': { label: 'library DI', role: 'model', scope: 'instrument' },
      'cand-a': {
        label: 'candidate a',
        role: 'model',
        scope: 'product',
        title: { en: 'Softer hammer', ja: '柔らかいハンマー' },
      },
      ref: { label: 'Recording One', role: 'reference' },
      'ref-di': { label: 'Recording One DI', role: 'reference', block: 'secondary' },
      ctx: { label: 'Other capture', role: 'comparison' },
    },
    comparisons: [
      {
        id: 'instrument_di',
        scope: 'instrument',
        model_sources: ['model-di'],
        oracle_sources: ['ref-di'],
        status: 'context_only',
        reasons: ['di only'],
      },
      {
        id: 'gm_gs_product',
        scope: 'product',
        model_sources: ['model', 'cand-a'],
        oracle_sources: ['ref'],
        status: 'matched',
        reasons: [],
      },
    ],
    items: [
      {
        id: 'music',
        label: 'Music',
        group: 'phrases',
        tracks: {
          model: 'music/model.wav',
          'model-di': 'music/model-di.wav',
          'cand-a': 'music/cand-a.wav',
          ref: 'music/ref.wav',
          'ref-di': 'music/ref-di.wav',
          ctx: 'music/ctx.wav',
        },
        meta: {
          notes: [
            { start: 0.5, note: 64, velocity: 90 },
            { start: 0.0, note: 60, velocity: 100 },
            { start: 0.02, note: 67, velocity: 80 },
            { start: 1.0, note: 72, velocity: 70 },
          ],
        },
      },
      { id: 'single-c4', label: 'C4', tracks: { model: 'c4/model.wav', ref: 'c4/ref.wav' } },
    ],
    ...over,
  });
}

/** A page holding the model alone, written before comparisons existed. */
export function soloManifest(): Manifest {
  return ManifestSchema.parse({
    title: 'Organ',
    voice: { program: 19, rig: 'none' },
    sources: { model: { label: 'library', role: 'model' } },
    items: [{ id: 'music', tracks: { model: 'music/model.wav' } }],
  });
}

export function takeOf(keys: string[], epoch = 1): Take {
  const rms = Object.fromEntries(keys.map((k, i) => [k, 0.1 * (i + 1)]));
  return {
    id: 'music',
    keys,
    rms,
    rates: Object.fromEntries(keys.map((k) => [k, 48000])),
    duration: 4,
    epoch,
  };
}

/** The store as it stands with the first take of a manifest decoded. */
export function opened(
  manifest: Manifest = comparedManifest(),
  over: Partial<AppState> = {},
): AppState {
  const item = manifest.items[0];
  setState({
    sets: SETS,
    setId: 'p000-grand',
    base: 's/p000-grand/',
    manifest,
    items: manifest.items,
    itemIndex: 0,
    compare: manifest.items.some((it) => Object.keys(it.tracks).length > 1),
    take: item ? takeOf(Object.keys(item.tracks)) : null,
    canSend: true,
    ...over,
  });
  return getState();
}

export function step(over: Partial<QueueStep> = {}): QueueStep {
  return {
    set: 'p000-grand',
    name: 'Grand',
    program: 0,
    bank: 0,
    kit: false,
    tier: 'core',
    group: 'acoustic piano',
    page: 'ready',
    ships: 'current',
    noted: 0,
    log: '',
    error: '',
    ...over,
  };
}
