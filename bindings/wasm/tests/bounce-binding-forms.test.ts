/**
 * Pins the `Project.bounce` instrument-option argument contract shared with the Node
 * binding: a list of { destinationId?, instrument } pairs resolves through one
 * normalizer, and anything else (a single pair included) or an element that is
 * not a pair (an array or a bare config included) is refused by name before
 * any render starts.
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
  option: 'builtinInstruments' | 'synthInstruments' | 'sf2Instruments';
  run: Bounce;
  /** Two instruments that must render differently: the control row. */
  instrument: unknown;
  other: unknown;
  /** The keys a binding pair of this family declares, in the refusal's sorted order. */
  pairKeys: string;
}

const FAMILIES: Family[] = [
  {
    option: 'builtinInstruments',
    run: (p, v) => p.bounce({ ...OPTIONS, builtinInstruments: v as never }),
    instrument: { waveform: 'saw' },
    other: { waveform: 'sine' },
    pairKeys: 'destinationId, instrument',
  },
  {
    option: 'synthInstruments',
    run: (p, v) => p.bounce({ ...OPTIONS, synthInstruments: v as never }),
    instrument: {},
    other: { preset: 'sawLead' },
    pairKeys: 'destinationId, instrument',
  },
  {
    option: 'sf2Instruments',
    run: (p, v) => p.bounce({ ...OPTIONS, sf2Instruments: v as never }),
    instrument: { gain: 0.8 },
    other: { gain: 0.2 },
    pairKeys: 'destinationId, instrument, soundFontUri',
  },
];

beforeAll(async () => {
  await init();
});

describe.each(FAMILIES)('bounce $option', (family) => {
  it('refuses a single pair and renders the same pair in a list', () => {
    withProject((project) => {
      const pair = { destinationId: 0, instrument: family.instrument };
      const arrayError = new TypeError(
        `Project.bounce: ${family.option} must be an array of { destinationId?, instrument } pairs`,
      );
      expect(() => family.run(project, pair)).toThrow(arrayError);
      expect(() => family.run(project, { instrument: family.instrument })).toThrow(arrayError);
      const listed = family.run(project, [pair]);
      expect(peak(listed)).toBeGreaterThan(0);
      // An omitted destination is destination 0.
      expect(family.run(project, [{ instrument: family.instrument }])).toEqual(listed);
      // The control: a different instrument moves the render, so the equality
      // above cannot hold because the instrument was dropped.
      expect(family.run(project, [{ instrument: family.other }])).not.toEqual(listed);
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
        new TypeError(`Project.bounce: ${family.option}[1] must be a ${expected}`),
      );
      expect(() => family.run(project, 42)).toThrow(
        new TypeError(
          `Project.bounce: ${family.option} must be an array of { destinationId?, instrument } pairs`,
        ),
      );
      // A bare config, or a name, must be wrapped as a pair.
      expect(() => family.run(project, [family.instrument])).toThrow(
        new RegExp(
          `: (unknown key ${family.option}\\[0\\]\\.\\w+ \\(expected ${family.pairKeys}\\)|${family.option}\\[0\\]\\.instrument must be given)$`,
        ),
      );
      expect(() => family.run(project, ['piano'])).toThrow(
        new TypeError(`Project.bounce: ${family.option}[0] must be a ${expected}`),
      );
    });
  });
});

describe('bounce instrument options', () => {
  it('bounce takes every instrument kind in its options', () => {
    withProject((project) => {
      const native = (
        project as unknown as {
          native: {
            bounce: (...args: unknown[]) => { interleaved: Float32Array; numChannels: number };
          };
        }
      ).native;
      const planes = (raw: { interleaved: Float32Array; numChannels: number }): Float32Array[] => {
        const frames = raw.interleaved.length / raw.numChannels;
        return Array.from({ length: raw.numChannels }, (_, c) =>
          Float32Array.from({ length: frames }, (_, i) => raw.interleaved[i * raw.numChannels + c]),
        );
      };
      const kinds = [
        { option: 'builtinInstruments', slot: 1, instrument: { waveform: 'saw' } },
        { option: 'synthInstruments', slot: 3, instrument: { preset: 'sawLead' } },
        { option: 'sf2Instruments', slot: 4, instrument: { gain: 0.8 } },
      ] as const;
      for (const { option, slot, instrument } of kinds) {
        const pairs = [{ destinationId: 0, instrument }];
        const viaOptions = project.bounce({ ...OPTIONS, [option]: pairs });
        const args: unknown[] = [OPTIONS, undefined, undefined, undefined, undefined];
        args[slot] = [{ ...instrument, destinationId: 0 }];
        expect(viaOptions.channels).toEqual(planes(native.bounce(...args)));
        expect(peak(viaOptions)).toBeGreaterThan(0);
        expect(() => project.bounce({ ...OPTIONS, [option]: 'saw' as never })).toThrow(TypeError);
        expect(() =>
          project.bounce({
            ...OPTIONS,
            [option]: [
              { destinationId: 0, instrument },
              { destinationId: 0, instrument },
            ],
          }),
        ).toThrow(RangeError);
      }
      expect(() => project.bounce({ ...OPTIONS, instruments: 7 as never })).toThrow(TypeError);
      expect(() =>
        project.bounce({ ...OPTIONS, instruments: [{ instrument: {} as never }] }),
      ).toThrow(TypeError);
      expect(project.bounce({ ...OPTIONS, instruments: [] })).toEqual(project.bounce(OPTIONS));
    });
  });

  it('refuses a destination bound by two kinds', () => {
    withProject((project) => {
      expect(() =>
        project.bounce({
          ...OPTIONS,
          builtinInstruments: [{ destinationId: 0, instrument: {} }],
          synthInstruments: [{ destinationId: 0, instrument: {} }],
        }),
      ).toThrow(RangeError);
    });
  });

  // RED WHEN: a refusal names the C binding member (`patch` / `config`) where the caller wrote
  // `instrument`, or an undeclared key inside the instrument is dropped without a refusal.
  it.each([
    [
      'an unknown synth preset',
      { synthInstruments: [{ instrument: { preset: 'saw-lead' } }] },
      RangeError,
      'synthInstruments[0].instrument.preset must name a synth catalog preset',
    ],
    [
      'a synth preset longer than the field holds',
      { synthInstruments: [{ instrument: { preset: 'x'.repeat(32) } }] },
      RangeError,
      'synthInstruments[0].instrument.preset must be at most 31 bytes',
    ],
    [
      'a synth field out of range',
      { synthInstruments: [{ instrument: { polyphony: 999 } }] },
      RangeError,
      'synthInstruments[0].instrument.polyphony must be an integer in [1, 64]',
    ],
    [
      'a synth token outside its set',
      { synthInstruments: [{ instrument: { waveform: 'nope' } }] },
      RangeError,
      'synthInstruments[0].instrument.waveform must be one of default, sine, saw, square, triangle, noise',
    ],
    [
      'a synth field of the wrong type',
      { synthInstruments: [{ instrument: { cutoffHz: 'x' } }] },
      TypeError,
      'synthInstruments[0].instrument.cutoffHz must be a number',
    ],
    [
      'an undeclared synth key',
      { synthInstruments: [{ instrument: { filterCutoffHz: 1 } }] },
      TypeError,
      'unknown key synthInstruments[0].instrument.filterCutoffHz',
    ],
    [
      'an engine param key the engine does not declare',
      {
        synthInstruments: [
          { instrument: { engineMode: 'bowedString', engineParams: { noSuchKey: 1 } } },
        ],
      },
      TypeError,
      'unknown key synthInstruments[0].instrument.engineParams.noSuchKey (expected ',
    ],
    [
      'an engine param outside its domain',
      {
        synthInstruments: [
          { instrument: { engineMode: 'bowedString', engineParams: { bowForce: 2 } } },
        ],
      },
      RangeError,
      'synthInstruments[0].instrument.engineParams.bowForce must be finite and in [0, 1]',
    ],
    [
      'a builtin field out of range',
      { builtinInstruments: [{ instrument: { polyphony: 999 } }] },
      RangeError,
      'builtinInstruments[0].instrument.polyphony must be an integer in [1, 64]',
    ],
    [
      'an undeclared builtin key',
      { builtinInstruments: [{ instrument: { cutoffHz: 1 } }] },
      TypeError,
      'unknown key builtinInstruments[0].instrument.cutoffHz',
    ],
    [
      'a SoundFont field out of range',
      { sf2Instruments: [{ instrument: { polyphony: 999 } }] },
      RangeError,
      'sf2Instruments[0].instrument.polyphony must be an integer in [1, 64]',
    ],
    [
      'an undeclared SoundFont key',
      { sf2Instruments: [{ instrument: { volume: 1 } }] },
      TypeError,
      'unknown key sf2Instruments[0].instrument.volume',
    ],
  ] as const)('names %s at the instrument the caller wrote', (_name, bindings, cls, text) => {
    withProject((project) => {
      const call = () => project.bounce({ ...OPTIONS, ...(bindings as ProjectBounceOptions) });
      expect(call).toThrow(cls);
      expect(call).toThrow(`Project.bounce: ${text}`);
    });
  });

  // RED WHEN: the preset-length refusal falls back to the bare `synth patch` label on a patch the
  // caller placed at a path.
  it('names a preset longer than the field at the destination instrument', () => {
    withProject((project) => {
      const call = () =>
        project.setDestinationInstrument(0, { kind: 'synth', patch: { preset: 'x'.repeat(32) } });
      expect(call).toThrow(RangeError);
      expect(call).toThrow(
        'Project.setDestinationInstrument: instrument.patch.preset must be at most 31 bytes',
      );
    });
  });
});
