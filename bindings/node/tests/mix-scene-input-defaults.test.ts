/**
 * Every field `MixSceneDocumentInput` leaves optional is one the scene loader fills itself: a
 * document omitting it builds the same mixer as one spelling the default. Each case also gives the
 * field a non-default value and expects a different mixer, so a field the loader ignored could not
 * pass by omission.
 */

import { describe, expect, it } from 'vitest';
import { Mixer, type MixSceneDocumentInput } from '../dist/index.js';

const SR = 48000;

// Cases build documents by deleting keys, so they are held untyped and checked by the loader.
function sceneOf(scene: object): string {
  const mixer = new Mixer({ scene: scene as MixSceneDocumentInput, sampleRate: SR });
  try {
    return mixer.toJson();
  } finally {
    mixer.destroy();
  }
}

type Fields = Record<string, unknown>;

/** A scene whose strip, send, bus and VCA group spell every defaulted field at its default. */
function spelledOut(): {
  version: number;
  strips: Fields[];
  buses: Fields[];
  vcaGroups: Fields[];
  connections: Array<{ source: string; destination: string }>;
} {
  return {
    version: 2,
    strips: [
      {
        id: 'a',
        inputTrimDb: 0,
        faderDb: 0,
        vcaOffsetDb: 0,
        pan: 0,
        width: 1,
        muted: false,
        soloed: false,
        soloSafe: false,
        panMode: 'balance',
        dualPanLeft: -1,
        dualPanRight: 1,
        polarityInvertLeft: false,
        polarityInvertRight: false,
        panLaw: 'const3Db',
        channelDelaySamples: 0,
        inserts: [],
        sends: [{ id: 's', destinationBusId: 'fx', sendDb: 0, timing: 'postFader' }],
      },
    ],
    buses: [
      { id: 'm', role: 'master', inserts: [] },
      { id: 'fx', role: 'aux', inserts: [] },
    ],
    vcaGroups: [{ id: 'v', gainDb: 0, members: [] }],
    connections: [
      { source: 'a', destination: 'm' },
      { source: 'fx', destination: 'm' },
    ],
  };
}

const INSERT = { processor: 'dynamics.deesser', slot: 'pre', params: { thresholdDb: -24 } };

// [field, its default, a value other than its default]
const STRIP_FIELDS: Array<[string, unknown, unknown]> = [
  ['inputTrimDb', 0, -3],
  ['faderDb', 0, -6],
  ['vcaOffsetDb', 0, -2],
  ['pan', 0, 0.5],
  ['width', 1, 0.5],
  ['muted', false, true],
  ['soloed', false, true],
  ['soloSafe', false, true],
  ['panMode', 'balance', 'dualPan'],
  ['dualPanLeft', -1, -0.5],
  ['dualPanRight', 1, 0.5],
  ['polarityInvertLeft', false, true],
  ['polarityInvertRight', false, true],
  ['panLaw', 'const3Db', 'const6Db'],
  ['channelDelaySamples', 0, 10],
  ['inserts', [], [INSERT]],
  ['sends', [], [{ id: 's2', destinationBusId: 'fx' }]],
];
const SEND_FIELDS: Array<[string, unknown, unknown]> = [
  ['sendDb', 0, -6],
  ['timing', 'postFader', 'preFader'],
];
const BUS_FIELDS: Array<[string, unknown, unknown]> = [['inserts', [], [INSERT]]];
const VCA_FIELDS: Array<[string, unknown, unknown]> = [
  ['gainDb', 0, -3],
  ['members', [], ['a']],
];

function expectOmissionIsDefault(
  pick: (scene: ReturnType<typeof spelledOut>) => Fields,
  field: string,
  fallback: unknown,
  other: unknown,
): void {
  const spelled = spelledOut();
  pick(spelled)[field] = fallback;
  const reference = sceneOf(spelled);
  const omitted = spelledOut();
  delete pick(omitted)[field];
  expect(sceneOf(omitted)).toBe(reference);
  const changed = spelledOut();
  pick(changed)[field] = other;
  expect(sceneOf(changed)).not.toBe(reference);
}

describe('MixSceneDocumentInput: an omitted field builds the default', () => {
  it.each(STRIP_FIELDS)('strip %s', (field, fallback, other) => {
    expectOmissionIsDefault((s) => s.strips[0], field, fallback, other);
  });

  it.each(SEND_FIELDS)('send %s', (field, fallback, other) => {
    expectOmissionIsDefault((s) => (s.strips[0].sends as Fields[])[0], field, fallback, other);
  });

  it.each(BUS_FIELDS)('bus %s', (field, fallback, other) => {
    expectOmissionIsDefault((s) => s.buses[1], field, fallback, other);
  });

  it.each(VCA_FIELDS)('VCA group %s', (field, fallback, other) => {
    expectOmissionIsDefault((s) => s.vcaGroups[0], field, fallback, other);
  });

  it('document version', () => {
    const reference = sceneOf(spelledOut());
    const omitted: Fields = spelledOut();
    delete omitted.version;
    expect(sceneOf(omitted)).toBe(reference);
    // The loader reads the field: a version it does not know is refused rather than ignored.
    expect(() => sceneOf({ ...spelledOut(), version: 1 })).toThrow(/version must be 2/);
  });

  // The document's arrays default to empty, and a non-empty one needs its own ids in place.
  const EMPTY = { version: 2, strips: [], buses: [], vcaGroups: [], connections: [] };
  const DOCUMENT_ARRAYS: Array<[keyof typeof EMPTY, MixSceneDocumentInput]> = [
    ['strips', { strips: [{ id: 'a' }] }],
    ['buses', { buses: [{ id: 'm', role: 'master' }] }],
    ['vcaGroups', { vcaGroups: [{ id: 'v' }] }],
    [
      'connections',
      {
        strips: [{ id: 'a' }],
        buses: [{ id: 'm', role: 'master' }],
        connections: [{ source: 'a', destination: 'm' }],
      },
    ],
  ];

  it.each(DOCUMENT_ARRAYS)('document %s', (field, populated) => {
    const reference = sceneOf(EMPTY);
    const omitted: Record<string, unknown> = { ...EMPTY };
    delete omitted[field];
    expect(sceneOf(omitted)).toBe(reference);
    const base =
      field === 'connections' ? sceneOf({ ...EMPTY, ...populated, connections: [] }) : reference;
    expect(sceneOf({ ...EMPTY, ...populated })).not.toBe(base);
  });
});
