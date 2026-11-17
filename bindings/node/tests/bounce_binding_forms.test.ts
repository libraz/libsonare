/**
 * Pins the bounceWith*Instruments argument contract shared with the WASM
 * binding: one binding or a list of bindings, each `{ destinationId?, instrument }`,
 * resolved through one normalizer; an element that is not a binding (an array
 * included) is refused before any render starts.
 */

import { describe, expect, it } from 'vitest';
import { Project, type ProjectBounceOptions } from '../src/index.js';

const OPTIONS: ProjectBounceOptions = { totalFrames: 4096, numChannels: 2 };

/** One sustained note routed to destination 0. */
function midiProject(): Project {
  const project = new Project();
  project.setSampleRate(48000);
  const { trackId, clipId } = project.addMidiClip(0, 4);
  project.setTrackMidiDestination(trackId, 0);
  project.setMidiEvents(clipId, [
    Project.midiNoteOn(0, 0, 0, 60, 100),
    Project.midiNoteOff(2, 0, 0, 60, 0),
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
    other: { destinationId: 0, instrument: { preset: 'saw-lead' } },
  },
  {
    name: 'bounceWithSf2Instruments',
    call: (p, v) => planes(p.bounceWithSf2Instruments(v as never, OPTIONS)),
    binding: { destinationId: 0, instrument: { gain: 0.8 } },
    other: { destinationId: 0, instrument: { gain: 0.2 } },
  },
];

describe.each(FAMILIES)('$name', (family) => {
  it('renders one binding identically bare and in a list', () => {
    using project = midiProject();
    const single = family.call(project, family.binding);
    expect(peak(single)).toBeGreaterThan(0);
    expect(family.call(project, [family.binding])).toEqual(single);
    // The control: a different binding moves the render, so the equality
    // above cannot hold because the binding was dropped.
    expect(family.call(project, [family.other])).not.toEqual(single);
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
    expect(() => family.call(project, 42)).toThrow(/request must be an object/);
    expect(() => family.call(project, null)).toThrow(TypeError);
    expect(() => family.call(project, [{ destinationId: 0 }])).toThrow(
      new TypeError(`${family.name}: binding.instrument must be an object`),
    );
  });
});
