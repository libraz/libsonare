/**
 * Pins the bounceWith*Instruments argument contract shared with the Node
 * binding: a single { destinationId?, instrument } pair or a list resolves
 * through one normalizer, and an element that is not a pair (an array or a
 * bare config included) is refused by name before any render starts.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  midiNoteOff,
  midiNoteOn,
  Project,
  type ProjectBounceOptions,
} from '../dist/index.js';

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

function withProject(body: (project: Project) => void): void {
  const project = midiProject();
  try {
    body(project);
  } finally {
    project.destroy();
  }
}

const peak = (audio: { channels: Float32Array[] }): number =>
  audio.channels[0].reduce((p, v) => Math.max(p, Math.abs(v)), 0);

type Bounce = (project: Project, value: unknown) => { channels: Float32Array[] };

interface Family {
  method: string;
  run: Bounce;
  /** Two instruments that must render differently: the control row. */
  instrument: unknown;
  other: unknown;
}

const FAMILIES: Family[] = [
  {
    method: 'bounceWithBuiltinInstruments',
    run: (p, v) => p.bounceWithBuiltinInstruments(v as never, OPTIONS),
    instrument: { waveform: 'saw' },
    other: { waveform: 'sine' },
  },
  {
    method: 'bounceWithSynthInstruments',
    run: (p, v) => p.bounceWithSynthInstruments(v as never, OPTIONS),
    instrument: {},
    other: { preset: 'sawLead' },
  },
  {
    method: 'bounceWithSf2Instruments',
    run: (p, v) => p.bounceWithSf2Instruments(v as never, OPTIONS),
    instrument: { gain: 0.8 },
    other: { gain: 0.2 },
  },
];

beforeAll(async () => {
  await init();
});

describe.each(FAMILIES)('$method', (family) => {
  it('renders one binding identically as a pair and as a list', () => {
    withProject((project) => {
      const pair = { destinationId: 0, instrument: family.instrument };
      const single = family.run(project, pair);
      expect(peak(single)).toBeGreaterThan(0);
      expect(family.run(project, [pair])).toEqual(single);
      // An omitted destination is destination 0.
      expect(family.run(project, { instrument: family.instrument })).toEqual(single);
      // The control: a different instrument moves the render, so the equalities
      // above cannot hold because the instrument was dropped.
      expect(family.run(project, [{ instrument: family.other }])).not.toEqual(single);
    });
  });

  it('reads no bindings as silence', () => {
    withProject((project) => {
      const silent = project.bounce(OPTIONS);
      expect(family.run(project, null)).toEqual(silent);
      expect(family.run(project, [])).toEqual(silent);
    });
  });

  it('refuses a non-pair element by name', () => {
    withProject((project) => {
      const pair = { instrument: family.instrument };
      const expected = '{ destinationId?, instrument } pair';
      expect(() => family.run(project, [pair, [pair]])).toThrow(
        new TypeError(`${family.method}: instruments[1] must be a ${expected}`),
      );
      expect(() => family.run(project, 42)).toThrow(
        new TypeError(`${family.method}: instrument must be a ${expected}`),
      );
      // A bare config, or a name, must be wrapped as a pair.
      expect(() => family.run(project, [family.instrument])).toThrow(
        /instruments\[0\](: unknown request key| needs an 'instrument' key)/,
      );
      expect(() => family.run(project, ['piano'])).toThrow(
        new TypeError(`${family.method}: instruments[0] must be a ${expected}`),
      );
    });
  });
});
