/**
 * Pins the bounce instrument-list argument contract shared with the WASM
 * binding: a list of bindings, each `{ destinationId?, instrument }`, resolved
 * through one normalizer; anything else (a single binding included) or an
 * element that is not a binding (an array included) is refused before any
 * render starts.
 */

import { describe, expect, it } from 'vitest';
import { midiNoteOff, midiNoteOn, Project, type ProjectBounceOptions } from '../src/index.js';

const OPTIONS: ProjectBounceOptions = { totalFrames: 4096, numChannels: 2 };

/** One sustained note routed to destination 0. */
function midiProject(): Project {
  const project = new Project();
  project.setSampleRate(48000);
  const { trackId, clipId } = project.addMidiClip(0, 4);
  project.setTrackMidiDestination(trackId, 0);
  project.setMidiEvents(clipId, [
    midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }),
    midiNoteOff({ ppq: 2, group: 0, channel: 0, note: 60, velocity: 0 }),
  ]);
  return project;
}

const peak = (audio: Float32Array[]): number =>
  audio.reduce((p, plane) => plane.reduce((q, v) => Math.max(q, Math.abs(v)), p), 0);

type Bounce = (project: Project, value?: unknown) => Float32Array[];

interface Family {
  name: string;
  call: Bounce;
  /** Two bindings that must render differently: the control row. */
  binding: unknown;
  other: unknown;
}

const planes = (r: { channels: Float32Array[] }): Float32Array[] => r.channels;

const FAMILIES: Family[] = [
  {
    name: 'builtinInstruments',
    call: (p, v) => planes(p.bounce({ ...OPTIONS, builtinInstruments: v as never })),
    binding: { destinationId: 0, instrument: { waveform: 'saw' } },
    other: { destinationId: 0, instrument: { waveform: 'sine' } },
  },
  {
    name: 'synthInstruments',
    call: (p, v) => planes(p.bounce({ ...OPTIONS, synthInstruments: v as never })),
    binding: { destinationId: 0, instrument: {} },
    other: { destinationId: 0, instrument: { preset: 'sawLead' } },
  },
  {
    name: 'sf2Instruments',
    call: (p, v) => planes(p.bounce({ ...OPTIONS, sf2Instruments: v as never })),
    binding: { destinationId: 0, instrument: { gain: 0.8 } },
    other: { destinationId: 0, instrument: { gain: 0.2 } },
  },
];

describe.each(FAMILIES)('$name', (family) => {
  it('refuses one bare binding and renders the same binding in a list', () => {
    using project = midiProject();
    expect(() => family.call(project, family.binding)).toThrow(TypeError);
    expect(() => family.call(project, family.binding)).toThrow(
      new TypeError(
        `Project.bounce: ${family.name} must be an array of { destinationId?, instrument } pairs`,
      ),
    );
    const listed = family.call(project, [family.binding]);
    expect(peak(listed)).toBeGreaterThan(0);
    // The control: a different binding moves the render, so the list above
    // is not silent for want of the binding.
    expect(family.call(project, [family.other])).not.toEqual(listed);
  });

  it('reads an omitted or empty list as no bindings', () => {
    using project = midiProject();
    const silent = project.bounce(OPTIONS).channels;
    expect(family.call(project, undefined)).toEqual(silent);
    expect(family.call(project, [])).toEqual(silent);
  });

  it('refuses a non-binding element by name', () => {
    using project = midiProject();
    expect(() => family.call(project, [family.binding, [family.binding]])).toThrow(TypeError);
    expect(() => family.call(project, [family.binding, [family.binding]])).toThrow(
      new TypeError(
        `Project.bounce: ${family.name}[1] must be a { destinationId?, instrument } pair`,
      ),
    );
    expect(() => family.call(project, 42)).toThrow(new RegExp(`${family.name} must be an array`));
    expect(() => family.call(project, null)).toThrow(TypeError);
    expect(() => family.call(project, [{ destinationId: 0 }])).toThrow(
      new TypeError(`Project.bounce: ${family.name}[0].instrument must be given`),
    );
  });
});

describe('bounce takes every instrument kind in its options', () => {
  const native = (project: Project) =>
    (
      project as unknown as {
        handle: {
          bounce(
            settings: object,
            builtin?: unknown[],
            synth?: unknown[],
            sf2?: unknown[],
          ): { interleaved: Float32Array; numChannels: number };
        };
      }
    ).handle;

  const interleaved = (planes: Float32Array[]): Float32Array => {
    const out = new Float32Array(planes.length * planes[0].length);
    planes.forEach((plane, c) => {
      plane.forEach((v, i) => {
        out[i * planes.length + c] = v;
      });
    });
    return out;
  };

  const binding = (instrument: object) => [{ destinationId: 0, instrument }];
  const kinds = [
    [
      'builtinInstruments',
      binding({ waveform: 'saw' }),
      (h: ReturnType<typeof native>, l: unknown[]) => h.bounce(OPTIONS, l),
    ],
    [
      'synthInstruments',
      binding({}),
      (h: ReturnType<typeof native>, l: unknown[]) => h.bounce(OPTIONS, undefined, l),
    ],
    [
      'sf2Instruments',
      binding({ gain: 0.8 }),
      (h: ReturnType<typeof native>, l: unknown[]) => h.bounce(OPTIONS, undefined, undefined, l),
    ],
  ] as const;

  it.each(kinds)(
    '%s alone renders what the native call of that kind renders',
    (kind, list, call) => {
      using project = midiProject();
      const result = project.bounce({ ...OPTIONS, [kind]: list });
      expect(peak(result.channels)).toBeGreaterThan(0);
      // The native layer reads each instrument with its destination folded in.
      const flat = list.map((pair) => ({ ...pair.instrument, destinationId: pair.destinationId }));
      const expected = call(native(project), flat);
      expect(interleaved(result.channels)).toEqual(expected.interleaved);
    },
  );

  it('refuses a list of the wrong type by its key', () => {
    using project = midiProject();
    for (const kind of [
      'instruments',
      'builtinInstruments',
      'synthInstruments',
      'sf2Instruments',
    ]) {
      expect(() => project.bounce({ ...OPTIONS, [kind]: 7 } as never)).toThrow(
        new TypeError(
          `Project.bounce: ${kind} must be an array of { destinationId?, instrument } pairs`,
        ),
      );
    }
  });

  it('refuses a destination bound twice, within one kind or across kinds', () => {
    using project = midiProject();
    const within = { ...OPTIONS, builtinInstruments: [...binding({}), ...binding({})] };
    expect(() => project.bounce(within)).toThrow(/must not be bound twice/);
    const across = {
      ...OPTIONS,
      builtinInstruments: binding({ waveform: 'saw' }),
      synthInstruments: binding({}),
    };
    expect(() => project.bounce(across)).toThrow(/must not be bound twice/);
    expect(
      peak(project.bounce({ ...OPTIONS, builtinInstruments: binding({}) }).channels),
    ).toBeGreaterThan(0);
  });
});
