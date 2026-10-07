/**
 * Pins the bounceWith*Instrument(s) argument contract shared with the WASM
 * binding: the singular form takes one binding or a list, the plural form a
 * list, both resolve through one normalizer, and an element that is not a
 * binding (an array included) is refused by name before any render starts.
 */

import { describe, expect, it } from 'vitest';
import { Project, type ProjectBounceOptions } from '../src/index.js';

const OPTIONS: ProjectBounceOptions = { totalFrames: 4096, numChannels: 2, sampleRate: 48000 };

/** One sustained note routed to destination 0. */
function midiProject(): Project {
  const project = Project.create();
  project.setSampleRate(48000);
  const { trackId, clipId } = project.addMidiClip(0, 4);
  project.setTrackMidiDestination(trackId, 0);
  project.setMidiEvents(clipId, [
    Project.midiNoteOn(0, 0, 0, 60, 100),
    Project.midiNoteOff(2, 0, 0, 60, 0),
  ]);
  return project;
}

const peak = (audio: Float32Array): number => audio.reduce((p, v) => Math.max(p, Math.abs(v)), 0);

type Bounce = (project: Project, value: unknown) => Float32Array;

interface Family {
  singular: string;
  plural: string;
  one: Bounce;
  many: Bounce;
  /** Two bindings that must render differently: the control row. */
  binding: unknown;
  other: unknown;
  /** Whether a bare name is a binding of this family. */
  takesName: boolean;
}

const FAMILIES: Family[] = [
  {
    singular: 'bounceWithBuiltinInstrument',
    plural: 'bounceWithBuiltinInstruments',
    one: (p, v) => p.bounceWithBuiltinInstrument(v as never, OPTIONS),
    many: (p, v) => p.bounceWithBuiltinInstruments(v as never, OPTIONS),
    binding: { waveform: 'saw', destinationId: 0 },
    other: 'sine',
    takesName: true,
  },
  {
    singular: 'bounceWithSynthInstrument',
    plural: 'bounceWithSynthInstruments',
    one: (p, v) => p.bounceWithSynthInstrument(v as never, OPTIONS),
    many: (p, v) => p.bounceWithSynthInstruments(v as never, OPTIONS),
    binding: { destinationId: 0 },
    other: 'saw-lead',
    takesName: true,
  },
  {
    singular: 'bounceWithSf2Instrument',
    plural: 'bounceWithSf2Instruments',
    one: (p, v) => p.bounceWithSf2Instrument(v as never, OPTIONS),
    many: (p, v) => p.bounceWithSf2Instruments(v as never, OPTIONS),
    binding: { destinationId: 0, gain: 0.8 },
    other: { destinationId: 0, gain: 0.2 },
    takesName: false,
  },
];

describe.each(FAMILIES)('$singular / $plural', (family) => {
  it('renders one binding identically in the singular, list and plural forms', () => {
    using project = midiProject();
    const single = family.one(project, family.binding);
    expect(peak(single)).toBeGreaterThan(0);
    expect(family.one(project, [family.binding])).toEqual(single);
    expect(family.many(project, [family.binding])).toEqual(single);
    // The control: a different binding moves the render, so the equalities
    // above cannot hold because the binding was dropped.
    expect(family.many(project, [family.other])).not.toEqual(single);
  });

  it('reads null as no bindings in both forms', () => {
    using project = midiProject();
    const silent = project.bounce(OPTIONS);
    expect(family.one(project, null)).toEqual(silent);
    expect(family.many(project, null)).toEqual(silent);
    expect(family.many(project, [])).toEqual(silent);
  });

  it('refuses a non-binding element by name', () => {
    using project = midiProject();
    const expected = family.takesName ? 'an object or a name' : 'an object';
    expect(() => family.one(project, [[family.binding]])).toThrow(
      new TypeError(`${family.singular}: instrument[0] must be ${expected}`),
    );
    expect(() => family.many(project, [family.binding, [family.binding]])).toThrow(
      new TypeError(`${family.plural}: instruments[1] must be ${expected}`),
    );
    expect(() => family.one(project, 42)).toThrow(
      new TypeError(`${family.singular}: instrument must be ${expected}`),
    );
    expect(() => family.many(project, family.binding)).toThrow(
      new TypeError(`${family.plural}: instruments must be an array`),
    );
    if (!family.takesName) {
      expect(() => family.one(project, 'piano')).toThrow(
        new TypeError(`${family.singular}: instrument must be an object`),
      );
    }
  });
});
