/**
 * Pins the bounceWith*Instruments argument contract shared with the WASM
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
    name: 'bounceWithBuiltinInstruments',
    call: (p, v) => planes(p.bounceWithBuiltinInstruments(v as never, OPTIONS)),
    binding: { destinationId: 0, instrument: { waveform: 'saw' } },
    other: { destinationId: 0, instrument: { waveform: 'sine' } },
  },
  {
    name: 'bounceWithSynthInstruments',
    call: (p, v) => planes(p.bounceWithSynthInstruments(v as never, OPTIONS)),
    binding: { destinationId: 0, instrument: {} },
    other: { destinationId: 0, instrument: { preset: 'sawLead' } },
  },
  {
    name: 'bounceWithSf2Instruments',
    call: (p, v) => planes(p.bounceWithSf2Instruments(v as never, OPTIONS)),
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
        `${family.name}: instruments must be an array of { destinationId?, instrument } pairs`,
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
      /ProjectInstrumentBinding: request must be an object/,
    );
    expect(() => family.call(project, 42)).toThrow(/instruments must be an array/);
    expect(() => family.call(project, null)).toThrow(TypeError);
    expect(() => family.call(project, [{ destinationId: 0 }])).toThrow(
      new TypeError(`${family.name}: binding.instrument must be an object`),
    );
  });
});
