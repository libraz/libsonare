/* Synthetic bank, sets and manifests for the bank tests; every name here is invented. */

import { type Manifest, Manifest as ManifestSchema, SetEntry } from '../../src/api/schemas';
import { BankVoiceRow } from '../../src/bank/schema';

export function voice(over: Record<string, unknown> = {}): BankVoiceRow {
  return BankVoiceRow.parse({
    slug: 'p000',
    name: 'Glass Harmonica',
    program: 0,
    bank: 0,
    kit: false,
    group: 'Keys',
    engine: 'modal',
    patch: 'glass',
    tone_class: 'struck',
    stage: 0.6,
    next: 'record a listening verdict',
    capture: 'cap-a',
    open_candidates: [],
    ...over,
  });
}

/** Seven voices spanning every slot kind, method, rung and page state. */
export function bank(): BankVoiceRow[] {
  return [
    voice(),
    voice({
      slug: 'p001',
      name: 'Tin Whistle',
      program: 1,
      engine: 'subtractive',
      patch: 'tin',
      stage: 0.2,
      capture: null,
      next: 'capture a reference',
      target: { branch: 'default', timbre: 'instrument' },
    }),
    voice({
      slug: 'p002',
      name: 'Rubber Band',
      program: 2,
      group: 'Strings',
      engine: 'fm',
      patch: 'rubber',
      stage: 1,
      open_candidates: ['rubber.tension', 'rubber.damp'],
      target: { branch: 'machine_defined', timbre: 'machine', state: 'off-target' },
    }),
    voice({
      slug: 'p002b8',
      name: 'Rubber Band',
      program: 2,
      bank: 8,
      group: 'Strings',
      engine: 'fm',
      patch: 'rubber',
      stage: 0.4,
      capture: null,
    }),
    voice({
      slug: 'p002b1',
      name: 'Rubber Band',
      program: 2,
      bank: 1,
      group: 'Strings',
      engine: 'fm',
      patch: 'rubber',
      stage: 0.4,
      capture: null,
    }),
    voice({
      slug: 'p003',
      name: 'Anvil',
      program: 3,
      group: 'Strings',
      engine: null,
      patch: null,
      tone_class: null,
      stage: 0.8,
      axes: {
        timbres: 2,
        profile_rows: 40,
        gate_state: 'green',
        coverage: {
          canonical: 6,
          gated: 4,
          complete: false,
          excused: ['stretch'],
          gaps: ['decay', 'tnr'],
        },
        agreement: { total: 5, inside: 3, outside: { centroid: 2 } },
      },
    }),
    voice({
      slug: 'k000',
      name: 'Cardboard Kit',
      program: 0,
      kit: true,
      group: 'Kits',
      engine: 'drum',
      patch: null,
      stage: 0,
      capture: null,
      axes: { music: { state: 'stale' }, structure: { state: 'current' } },
    }),
  ];
}

export function setEntry(id: string, over: Record<string, unknown> = {}): SetEntry {
  return SetEntry.parse({
    id,
    title: `${id} title`,
    takes: 3,
    compare: true,
    group: 'Keys',
    ...over,
  });
}

export function manifest(over: Record<string, unknown> = {}): Manifest {
  return ManifestSchema.parse({
    title: 'Glass Harmonica',
    voice: { name: 'Glass Harmonica', program: 0, bank: 0, patch: 'glass' },
    ...over,
  });
}
