/**
 * A caller list longer than the input length budget is refused by the facade
 * before the facade walks it, with the text the addon uses for the same
 * refusal. The list is a proxy over an empty array whose `length` is past the
 * budget and whose entries count their reads, so a read count of zero shows the
 * refusal came first, whatever the machine's speed.
 */

import { describe, expect, it } from 'vitest';
import { Mixer, Project, RealtimeEngine } from '../src/index.js';

const OVER_BUDGET = 64 * 1024 * 1024 + 1;

interface HugeList {
  list: never;
  reads: () => number;
}

function hugeList(): HugeList {
  let reads = 0;
  const isIndex = (key: string | symbol) => typeof key === 'string' && /^\d+$/.test(key);
  const list = new Proxy([] as unknown[], {
    get(target, key, receiver) {
      if (key === 'length') {
        return OVER_BUDGET;
      }
      if (isIndex(key)) {
        reads += 1;
      }
      return Reflect.get(target, key, receiver);
    },
    has(target, key) {
      if (isIndex(key)) {
        reads += 1;
      }
      return Reflect.has(target, key);
    },
  });
  return { list: list as never, reads: () => reads };
}

type Row<H> = readonly [name: string, subject: string, call: (handle: H, list: never) => void];

function expectRefused<H>(make: () => H, destroy: (handle: H) => void, row: Row<H>): void {
  const [, subject, call] = row;
  const { list, reads } = hugeList();
  const handle = make();
  try {
    expect(() => call(handle, list)).toThrow(RangeError);
    expect(() => call(handle, list)).toThrow(`${subject} exceeds the input length budget`);
    expect(reads()).toBe(0);
  } finally {
    destroy(handle);
  }
}

const ENGINE_ROWS: ReadonlyArray<Row<RealtimeEngine>> = [
  ['setMarkers', 'markers', (e, l) => e.setMarkers(l)],
  ['setAutomationLane', 'points', (e, l) => e.setAutomationLane(1, l)],
  ['setClips', 'clips', (e, l) => e.setClips(l)],
  ['setTrackLanes', 'lanes', (e, l) => e.setTrackLanes(l)],
  ['setTrackLanes sends', 'lanes[0].sends', (e, l) => e.setTrackLanes([{ trackId: 1, sends: l }])],
  ['setTrackSends', 'sends', (e, l) => e.setTrackSends(1, l)],
  ['setTrackBuses', 'buses', (e, l) => e.setTrackBuses(l)],
  ['setTrackBuses sends', 'buses[0].sends', (e, l) => e.setTrackBuses([{ busId: 1, sends: l }])],
  ['setStrip sends', 'strip.sends', (e, l) => e.setStrip(1, { sends: l } as never)],
  ['setStrip inserts', 'strip', (e, l) => e.setStrip(1, { inserts: l } as never)],
  ['setBusStrip', 'bus', (e, l) => e.setBusStrip(1, { inserts: l } as never)],
  ['setStripEqBand', 'band', (e, l) => e.setStripEqBand(1, 0, { gains: l } as never)],
  ['setGraph nodes', 'nodes', (e, l) => e.setGraph({ nodes: l, connections: [] } as never)],
  ['setGraph connections', 'connections', (e, l) => e.setGraph({ connections: l } as never)],
  [
    'setGraph parameterBindings',
    'parameterBindings',
    (e, l) => e.setGraph({ parameterBindings: l } as never),
  ],
  ['renderOffline', 'channels', (e, l) => e.renderOffline({ channels: l } as never)],
  ['setTempoSegments', 'segments', (e, l) => e.setTempoSegments(l)],
  ['setTimeSignatureSegments', 'segments', (e, l) => e.setTimeSignatureSegments(l)],
  ['setSynthInstrument', 'modRoutings', (e, l) => e.setSynthInstrument(0, { modRoutings: l })],
  [
    'setPartRig',
    'inserts',
    (e, l) => e.setPartRig({ destinationId: 0, part: 0, mode: 'chain', inserts: l }),
  ],
  ['setMidiFx', 'config', (e, l) => e.setMidiFx(0, { chordIntervals: l })],
  ['setMidiClips', 'clips', (e, l) => e.setMidiClips(l)],
  ['pushMidiUmp', 'words', (e, l) => e.pushMidiUmp(0, l)],
  ['pushMidiInputUmp', 'words', (e, l) => e.pushMidiInputUmp(l)],
];

const PROJECT_ROWS: ReadonlyArray<Row<Project>> = [
  ['setClipTakes', 'takes', (p, l) => p.setClipTakes(1, l)],
  ['setClipCompSegments', 'segments', (p, l) => p.setClipCompSegments(1, l)],
  ['setMidiEvents', 'events', (p, l) => p.setMidiEvents(1, l)],
  ['annotateKeys', 'keys', (p, l) => p.annotateKeys(l)],
  ['annotateChords', 'chords', (p, l) => p.annotateChords(l)],
  [
    'setPartRig',
    'inserts',
    (p, l) => p.setPartRig({ destinationId: 0, part: 0, mode: 'chain', inserts: l }),
  ],
  ['setTempoSegments', 'segments', (p, l) => p.setTempoSegments(l)],
  ['setTimeSignatures', 'segments', (p, l) => p.setTimeSignatures(l)],
  ['setWarpMap', 'anchors', (p, l) => p.setWarpMap({ id: 1, anchors: l })],
  [
    'addClip channels',
    'channels',
    (p, l) => p.addClip({ trackId: 1, channels: l, sampleRate: 48000 } as never),
  ],
  ['importExternalStems', 'stems', (p, l) => p.importExternalStems({ stems: l } as never)],
  [
    'bounce builtinInstruments',
    'builtinInstruments',
    (p, l) => void p.bounce({ builtinInstruments: l }),
  ],
  ['bounce instruments', 'instruments', (p, l) => void p.bounce({ instruments: l })],
  [
    'addAutomationLane',
    'points',
    (p, l) => p.addAutomationLane(1, { targetParamId: 1, points: l } as never),
  ],
];

const MIXER_ROWS: ReadonlyArray<Row<Mixer>> = [
  ['addVcaGroup', 'members', (m, l) => m.addVcaGroup('v', 0, l)],
  ['setVcaGroupMembers', 'members', (m, l) => m.setVcaGroupMembers('v', l)],
];

describe('facades refuse an oversized caller list before walking it', () => {
  const makeEngine = () => new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
  it.each(ENGINE_ROWS)('RealtimeEngine.%s', (...row) => {
    expectRefused(makeEngine, (e) => e.destroy(), row);
  });

  it.each(PROJECT_ROWS)('Project.%s', (...row) => {
    expectRefused(
      () => new Project(),
      (p) => p.destroy(),
      row,
    );
  });

  it.each(MIXER_ROWS)('Mixer.%s', (...row) => {
    expectRefused(
      () => new Mixer({ sampleRate: 48000 }),
      (m) => m.destroy(),
      row,
    );
  });

  it('Mixer scene document', () => {
    const { list, reads } = hugeList();
    expect(
      () => new Mixer({ sampleRate: 48000, scene: { strips: list, buses: [] } as never }),
    ).toThrow('scene exceeds the input length budget');
    expect(reads()).toBe(0);
  });

  it('the request-key walk refuses a nested list before reading an entry', () => {
    const { list, reads } = hugeList();
    const engine = makeEngine();
    try {
      expect(() => engine.configureCapture({ punch: list } as never)).toThrow(
        'punch exceeds the input length budget',
      );
      expect(reads()).toBe(0);
    } finally {
      engine.destroy();
    }
  });
});
