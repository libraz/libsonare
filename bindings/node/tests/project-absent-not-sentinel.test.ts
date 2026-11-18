import { describe, expect, it } from 'vitest';
import {
  midiCc,
  midiCcLearn,
  midiCcToBreakpoint,
  midiNoteOn,
  midiParamToCc,
  midiRouteEvents,
  Project,
  type ProjectChordSymbol,
  type ProjectKeySegment,
  type ProjectMidiCcBinding,
} from '../src/index.js';
import { expectRefusal } from './_helpers.js';

// An unknown, absent or "any" value is spelled by omitting the field. The C ABI
// keeps its 255 / -1 sentinels, and an explicit sentinel from a caller is refused
// rather than read as the absence it used to stand for.

function annotation(fill: (project: Project) => void): {
  keys: Array<{ tonicPc: number }>;
  chords: Array<{ rootPc: number; slashBassPc: number }>;
} {
  const project = new Project();
  try {
    fill(project);
    return JSON.parse(project.toJson()).annotation;
  } finally {
    project.destroy();
  }
}

const key = (extra: Partial<ProjectKeySegment>): ProjectKeySegment => ({
  startPpq: 0,
  endPpq: 4,
  mode: 'major',
  ...extra,
});
const chord = (extra: Partial<ProjectChordSymbol>): ProjectChordSymbol => ({
  startPpq: 0,
  endPpq: 4,
  quality: 'major',
  ...extra,
});

function expectFieldRefusal(call: () => unknown, field: string): void {
  expectRefusal(call, RangeError, new RegExp(field));
}

describe('project annotation pitch classes are absent, never 255', () => {
  it('stores an omitted tonic, root and slash bass as unknown / none', () => {
    const stored = annotation((project) => {
      project.annotateKeys([key({})]);
      project.annotateChords([chord({})]);
    });
    expect(stored.keys[0].tonicPc).toBe(255);
    expect(stored.chords[0].rootPc).toBe(255);
    expect(stored.chords[0].slashBassPc).toBe(255);
  });

  it('stores a given pitch class as given', () => {
    const stored = annotation((project) => {
      project.annotateKeys([key({ tonicPc: 9 })]);
      project.annotateChords([chord({ rootPc: 0, slashBassPc: 4 })]);
    });
    expect(stored.keys[0].tonicPc).toBe(9);
    expect(stored.chords[0].rootPc).toBe(0);
    expect(stored.chords[0].slashBassPc).toBe(4);
  });

  it('refuses an explicit 255 or any value past 11, naming the field', () => {
    for (const value of [12, 254, 255]) {
      const project = new Project();
      try {
        expectFieldRefusal(() => project.annotateKeys([key({ tonicPc: value })]), 'tonicPc');
        expectFieldRefusal(() => project.annotateChords([chord({ rootPc: value })]), 'rootPc');
        expectFieldRefusal(
          () => project.annotateChords([chord({ rootPc: 0, slashBassPc: value })]),
          'slashBassPc',
        );
      } finally {
        project.destroy();
      }
    }
  });
});

describe('MIDI CC binding channel is absent for any channel, never 255', () => {
  const binding = (channel?: number): ProjectMidiCcBinding => ({
    ccNumber: 7,
    ...(channel === undefined ? {} : { channel }),
    paramId: 1,
    minValue: 0,
    maxValue: 1,
  });
  const toCc = (channel?: number) =>
    midiParamToCc({ bindings: [binding(channel)], paramId: 1, unitValue: 0.5, group: 0 });

  it('accepts an omitted channel and a channel in 0..15', () => {
    expect(toCc()).not.toBeNull();
    expect(((toCc(15)?.word0 ?? 0) >>> 16) & 0xf).toBe(15);
  });

  it('refuses an explicit 255 or a channel past 15, naming the field', () => {
    for (const value of [16, 254, 255]) {
      expectFieldRefusal(() => toCc(value), 'channel');
    }
  });
});

describe('MIDI binding lookups return null only for an input the bindings do not map', () => {
  const binding: ProjectMidiCcBinding = { ccNumber: 7, paramId: 1, minValue: 0, maxValue: 1 };
  const cc = (controller: number) =>
    midiCc({ ppq: 0, group: 0, channel: 0, controller, value: 64 });

  it('returns null when nothing maps', () => {
    expect(midiParamToCc({ bindings: [binding], paramId: 2, unitValue: 0.5, group: 0 })).toBeNull();
    expect(midiCcToBreakpoint({ bindings: [binding], event: cc(8) })).toBeNull();
    expect(
      midiCcToBreakpoint({
        bindings: [binding],
        event: midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
      }),
    ).toBeNull();
    expect(midiCcLearn({ events: [], paramId: 1 })).toBeNull();
  });

  it('throws RangeError for an out-of-domain argument instead of returning null', () => {
    expect(() =>
      midiParamToCc({ bindings: [binding], paramId: 0, unitValue: 0.5, group: 0 }),
    ).toThrow(RangeError);
    expect(() =>
      midiParamToCc({ bindings: [binding], paramId: 1, unitValue: 0.5, group: 16 }),
    ).toThrow(RangeError);
    expect(() => midiCcLearn({ events: [cc(7)], paramId: 1, minMovement: 128 })).toThrow(
      RangeError,
    );
  });
});

describe('midiRouteEvents filters are absent for any / no remap, never -1', () => {
  const events = [
    midiNoteOn({ ppq: 0, group: 0, channel: 3, note: 60, velocity: 100 }),
    midiNoteOn({ ppq: 1, group: 0, channel: 2, note: 61, velocity: 100 }),
  ];

  it('passes every event when the filters are omitted', () => {
    expect(midiRouteEvents({ events }).events).toHaveLength(2);
  });

  it('refuses an explicit -1 on each filter, naming the field', () => {
    for (const field of ['filterGroup', 'filterChannel', 'remapChannel'] as const) {
      expectFieldRefusal(() => midiRouteEvents({ events, [field]: -1 }), field);
    }
  });
});
