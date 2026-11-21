/**
 * Pins the callback-instrument bounce: a JS instrument driven synchronously
 * through the `instruments` bounce option, its zero-filled
 * outputs, first-throw propagation, the Promise refusal, and
 * a long bounce whose JS stack depth does not grow from block to block.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  type ExternalInstrument,
  type ExternalInstrumentEvent,
  init,
  midiNoteOff,
  midiNoteOn,
  Project,
  type ProjectBounceOptions,
} from '../dist/index.js';

beforeAll(async () => {
  await init();
});

const OPTIONS = {
  totalFrames: 48000,
  blockSize: 128,
  numChannels: 2,
} satisfies ProjectBounceOptions;

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

/** An instrument that adds a constant level and records what it was handed. */
class ConstantInstrument implements ExternalInstrument {
  prepared: [number, number, number] | null = null;
  events: ExternalInstrumentEvent[] = [];
  renderCalls = 0;
  allZeroOnEntry = true;
  shapes = new Set<string>();

  constructor(readonly level = 0.25) {}

  prepare(sampleRate: number, maxBlockFrames: number, channels: number): void {
    this.prepared = [sampleRate, maxBlockFrames, channels];
  }

  onEvent(event: ExternalInstrumentEvent): void {
    this.events.push(event);
  }

  render(outputs: Float32Array[], frames: number): void {
    this.renderCalls += 1;
    this.shapes.add(`${outputs.length}x${outputs[0].length}=${frames}`);
    for (const channel of outputs) {
      if (channel.some((v) => v !== 0)) {
        this.allZeroOnEntry = false;
      }
      for (let i = 0; i < frames; i += 1) {
        channel[i] += this.level;
      }
    }
  }
}

describe('bounce with instruments', () => {
  it('renders the callback instrument into the bounce', () => {
    using project = midiProject();
    const silent = project.bounce(OPTIONS);
    const instrument = new ConstantInstrument(0.25);
    const audio = project.bounce({ ...OPTIONS, instruments: [{ instrument }] });
    expect(audio).not.toHaveProperty('numChannels');
    expect(audio.channels).toHaveLength(2);
    // The control: a silent bounce is all zero, so the DC is the instrument's.
    for (const channel of silent.channels) {
      expect(Math.max(...channel.map(Math.abs))).toBe(0);
    }
    for (let c = 0; c < audio.channels.length; c += 1) {
      expect(audio.channels[c]).toHaveLength(silent.channels[c].length);
      for (let i = 0; i < audio.channels[c].length; i += 1) {
        expect(audio.channels[c][i]).toBeCloseTo(silent.channels[c][i] + 0.25, 6);
      }
    }
    expect(instrument.renderCalls).toBeGreaterThan(0);
  });

  it('refuses a single binding outside a list and matches an omitted destination inside one', () => {
    using project = midiProject();
    const list = project.bounce({
      ...OPTIONS,
      instruments: [{ destinationId: 0, instrument: new ConstantInstrument(0.1) }],
    });
    expect(() =>
      project.bounce({
        ...OPTIONS,
        instruments: { instrument: new ConstantInstrument(0.1) } as never,
      }),
    ).toThrow(
      new TypeError(
        'Project.bounce: instruments must be an array of { destinationId?, instrument } pairs',
      ),
    );
    expect(
      project.bounce({ ...OPTIONS, instruments: [{ instrument: new ConstantInstrument(0.1) }] }),
    ).toEqual(list);
    expect(
      project.bounce({ ...OPTIONS, instruments: [{ instrument: new ConstantInstrument(0.3) }] }),
    ).not.toEqual(list);
    expect(project.bounce({ ...OPTIONS, instruments: [] })).toEqual(project.bounce(OPTIONS));
  });

  it('hands zero-filled outputs sized to the block', () => {
    using project = midiProject();
    const instrument = new ConstantInstrument();
    project.bounce({ ...OPTIONS, instruments: [{ instrument }] });
    expect(instrument.allZeroOnEntry).toBe(true);
    for (const shape of instrument.shapes) {
      const [channels, rest] = shape.split('x');
      const [length, frames] = rest.split('=');
      expect(channels).toBe('2');
      expect(length).toBe(frames);
      expect(Number(frames)).toBeLessThanOrEqual(128);
    }
  });

  it('prepares with the sample rate, largest block and stereo channel count', () => {
    using project = midiProject();
    // The instrument is stereo whatever the bounce's output width.
    for (const numChannels of [1, 2]) {
      const instrument = new ConstantInstrument();
      project.bounce({ ...OPTIONS, numChannels, instruments: [{ instrument }] });
      expect(instrument.prepared?.[0]).toBe(48000);
      expect(instrument.prepared?.[1]).toBeGreaterThan(0);
      expect(instrument.prepared?.[1]).toBeLessThanOrEqual(128);
      expect(instrument.prepared?.[2]).toBe(2);
      expect([...instrument.shapes].every((shape) => shape.startsWith('2x'))).toBe(true);
    }
  });

  it('delivers the note-on and note-off as UMP events in order', () => {
    using project = midiProject();
    const instrument = new ConstantInstrument();
    project.bounce({ ...OPTIONS, instruments: [{ instrument }] });
    const statuses = instrument.events.map((e) => (e.words[0] >>> 20) & 0xf);
    expect(statuses).toContain(0x9);
    expect(statuses).toContain(0x8);
    expect(statuses.indexOf(0x9)).toBeLessThan(statuses.indexOf(0x8));
    for (const event of instrument.events) {
      expect(event.destinationId).toBe(0);
      expect(event.words.length).toBeGreaterThanOrEqual(1);
      expect(event.words.every((w) => Number.isInteger(w) && w >= 0)).toBe(true);
    }
    const frames = instrument.events.map((e) => e.renderFrame);
    expect(frames).toEqual([...frames].sort((a, b) => a - b));
  });

  it('accepts a render-only instrument', () => {
    using project = midiProject();
    const audio = project.bounce({
      ...OPTIONS,
      instruments: [
        {
          instrument: {
            render(outputs, frames) {
              for (const channel of outputs) {
                channel.fill(0.1, 0, frames);
              }
            },
          },
        },
      ],
    });
    expect(Math.max(...audio.channels[0].map(Math.abs))).toBeGreaterThan(0);
  });

  it('extends an auto-length bounce by tailSamples', () => {
    using project = midiProject();
    const { totalFrames: _omitted, ...auto } = OPTIONS;
    const dry = project.bounce({
      ...auto,
      instruments: [{ instrument: new ConstantInstrument(0) }],
    });
    const withTail = project.bounce({
      ...auto,
      instruments: [{ instrument: { render() {}, tailSamples: 4096 } }],
    });
    expect(withTail.channels[0].length).toBe(dry.channels[0].length + 4096);
  });

  describe('a throwing callback', () => {
    it('stops further callbacks and rethrows the first error after the bounce', () => {
      using project = midiProject();
      let calls = 0;
      const boom = new Error('synthesis failed');
      expect(() =>
        project.bounce({
          ...OPTIONS,
          blockSize: 32,
          instruments: [
            {
              instrument: {
                render() {
                  calls += 1;
                  throw boom;
                },
              },
            },
          ],
        }),
      ).toThrow(boom);
      expect(calls).toBe(1);
    });

    it('latches a throw from onEvent and prepare as well', () => {
      using project = midiProject();
      let events = 0;
      expect(() =>
        project.bounce({
          ...OPTIONS,
          instruments: [
            {
              instrument: {
                onEvent() {
                  events += 1;
                  throw new RangeError('bad event');
                },
                render() {},
              },
            },
          ],
        }),
      ).toThrow(new RangeError('bad event'));
      expect(events).toBe(1);

      let afterPrepare = 0;
      expect(() =>
        project.bounce({
          ...OPTIONS,
          instruments: [
            {
              instrument: {
                prepare() {
                  throw new Error('no prepare');
                },
                onEvent() {
                  afterPrepare += 1;
                },
                render() {
                  afterPrepare += 1;
                },
              },
            },
          ],
        }),
      ).toThrow('no prepare');
      expect(afterPrepare).toBe(0);
    });

    it('lets the project bounce again after a throw', () => {
      using project = midiProject();
      expect(() =>
        project.bounce({
          ...OPTIONS,
          instruments: [
            {
              instrument: {
                render() {
                  throw new Error('once');
                },
              },
            },
          ],
        }),
      ).toThrow('once');
      const again = project.bounce({
        ...OPTIONS,
        instruments: [{ instrument: new ConstantInstrument() }],
      });
      expect(again.channels).toHaveLength(2);
      expect(again.channels[0]).toHaveLength(OPTIONS.totalFrames);
    });
  });

  describe('a callback returning a Promise', () => {
    const asyncReturn = (): Promise<void> => Promise.resolve();

    it.each(['render', 'onEvent', 'prepare'] as const)('refuses it from %s', (name) => {
      using project = midiProject();
      let calls = 0;
      const instrument = {
        render() {},
        [name]() {
          calls += 1;
          return asyncReturn();
        },
      } as unknown as ExternalInstrument;
      expect(() => project.bounce({ ...OPTIONS, instruments: [{ instrument }] })).toThrow(
        TypeError,
      );
      expect(() => project.bounce({ ...OPTIONS, instruments: [{ instrument }] })).toThrow(
        new RegExp(`${name} must not return a Promise`),
      );
      expect(calls).toBe(2);
    });

    it('refuses a bare thenable', () => {
      using project = midiProject();
      const instrument = {
        render: () => Object.defineProperty({}, 'then', { value: () => undefined }),
      } as unknown as ExternalInstrument;
      expect(() => project.bounce({ ...OPTIONS, instruments: [{ instrument }] })).toThrow(
        TypeError,
      );
    });
  });

  describe('argument refusals', () => {
    it('names a missing or mistyped callback before any render starts', () => {
      using project = midiProject();
      expect(() =>
        project.bounce({ ...OPTIONS, instruments: [{ instrument: {} as never }] }),
      ).toThrow(
        new TypeError('Project.bounce: instruments[0].instrument.render must be a function'),
      );
      expect(() =>
        project.bounce({
          ...OPTIONS,
          instruments: [{ instrument: { render() {}, prepare: 5 } as never }],
        }),
      ).toThrow(
        new TypeError('Project.bounce: instruments[0].instrument.prepare must be a function'),
      );
      expect(() =>
        project.bounce({
          ...OPTIONS,
          instruments: [{ instrument: { render() {}, onEvent: 'x' } as never }],
        }),
      ).toThrow(
        new TypeError('Project.bounce: instruments[0].instrument.onEvent must be a function'),
      );
    });

    it('refuses a non-binding element and a bare instrument by name', () => {
      using project = midiProject();
      expect(() => project.bounce({ ...OPTIONS, instruments: 7 as never })).toThrow(
        new TypeError(
          'Project.bounce: instruments must be an array of { destinationId?, instrument } pairs',
        ),
      );
      expect(() =>
        project.bounce({ ...OPTIONS, instruments: { instrument: { render() {} } } as never }),
      ).toThrow(
        new TypeError(
          'Project.bounce: instruments must be an array of { destinationId?, instrument } pairs',
        ),
      );
      expect(() =>
        project.bounce({ ...OPTIONS, instruments: [[{ instrument: { render() {} } }]] as never }),
      ).toThrow(
        new TypeError(
          'Project.bounce: instruments[0] must be a { destinationId?, instrument } pair',
        ),
      );
      // A bare instrument must be wrapped as a pair.
      expect(() => project.bounce({ ...OPTIONS, instruments: [{ render() {} }] as never })).toThrow(
        new TypeError(
          'Project.bounce: unknown key instruments[0].render (expected destinationId, instrument)',
        ),
      );
    });
  });

  it('refuses use of the project from inside a callback', () => {
    using project = midiProject();
    const attempts: Array<[string, () => unknown]> = [
      ['setSampleRate', () => project.setSampleRate(44100)],
      ['bounce', () => project.bounce(OPTIONS)],
      ['nested bounce', () => project.bounce({ ...OPTIONS, instruments: [] })],
      ['delete', () => project.destroy()],
    ];
    for (const [name, attempt] of attempts) {
      let refusal: unknown;
      project.bounce({
        ...OPTIONS,
        instruments: [
          {
            instrument: {
              render() {
                try {
                  attempt();
                } catch (error) {
                  refusal ??= error;
                }
              },
            },
          },
        ],
      });
      expect(refusal, name).toBeDefined();
      expect(refusal, name).toMatchObject({ code: ErrorCode.InvalidState });
    }
    // The refusals changed nothing: the project still bounces and keeps its rate.
    expect(project.sampleRate).toBe(48000);
    expect(project.bounce(OPTIONS).channels[0]).toHaveLength(OPTIONS.totalFrames);
  });

  it('reports a native failure ahead of a callback error', () => {
    using project = midiProject();
    let prepared = false;
    const { totalFrames: _omitted, ...auto } = OPTIONS;
    const bounce = () =>
      project.bounce({
        ...auto,
        instruments: [
          {
            instrument: {
              tailSamples: 2147483647,
              prepare() {
                prepared = true;
                throw new Error('callback error');
              },
              render() {},
            },
          },
        ],
      });
    expect(bounce).toThrow(
      "Project.bounce: totalFrames must be positive when a hosted instrument's tail is unbounded",
    );
    // The control: the callback did run and throw before the native failure.
    expect(prepared).toBe(true);
  });

  it('refuses a bad options bag without bouncing', () => {
    using project = midiProject();
    for (const instruments of [[], [{ instrument: new ConstantInstrument() }]]) {
      expect(() =>
        project.bounce({
          ...({
            blockSize: 'x',
          } as unknown as ProjectBounceOptions),
          instruments: instruments,
        }),
      ).toThrow(/blockSize must be an integer/);
    }
  });

  it('survives an instrument that transfers its scratch buffer away', () => {
    using project = midiProject();
    const audio = project.bounce({
      ...OPTIONS,
      instruments: [
        {
          instrument: {
            render(outputs, frames) {
              outputs[0].fill(0.3, 0, frames);
              structuredClone(outputs[0].buffer, { transfer: [outputs[0].buffer] });
            },
          },
        },
      ],
    });
    expect(audio.channels).toHaveLength(2);
    for (const channel of audio.channels) {
      expect(channel).toHaveLength(OPTIONS.totalFrames);
      expect(channel.every(Number.isFinite)).toBe(true);
    }
  });

  it('runs a callback with ordinary JS stack headroom, block after block', () => {
    using project = midiProject();
    // How deep plain JS recurses here; a callback must manage half of it.
    let plain = 0;
    const probe = (): void => {
      plain += 1;
      probe();
    };
    try {
      probe();
    } catch {
      // Overflow ends the probe.
    }
    const budget = Math.floor(plain / 2);
    const descend = (n: number): number => (n === 0 ? 0 : 1 + descend(n - 1));
    let blocks = 0;
    const audio = project.bounce({
      ...OPTIONS,
      totalFrames: 96000,
      blockSize: 16,
      instruments: [
        {
          instrument: {
            render(outputs, frames) {
              blocks += 1;
              if (descend(budget) !== budget) {
                throw new Error('recursion did not complete');
              }
              for (const channel of outputs) {
                channel.fill(0.05, 0, frames);
              }
            },
          },
        },
      ],
    });
    expect(blocks).toBeGreaterThanOrEqual(5000);
    expect(audio.channels).toHaveLength(2);
    for (const channel of audio.channels) {
      expect(channel).toHaveLength(96000);
      expect(channel.every((v) => Math.abs(v - 0.05) < 1e-6)).toBe(true);
    }
  });
});

describe('Project.bounce refusal subjects', () => {
  // RED WHEN: a C-originated subject keeps its `options.` root while the facade's own refusal on
  // the same option list is spelled bare.
  it('spells a core refusal and a facade refusal on one list the same way', () => {
    using project = midiProject();
    const voice = { waveform: 'saw' } as const;
    expect(() =>
      project.bounce({
        ...OPTIONS,
        builtinInstruments: [
          { destinationId: 0, instrument: voice },
          { destinationId: 0, instrument: voice },
        ],
      }),
    ).toThrow(
      new RangeError('Project.bounce: builtinInstruments[1].destinationId must not be bound twice'),
    );
    expect(() =>
      project.bounce({
        ...OPTIONS,
        builtinInstruments: [
          { destinationId: 0, instrument: voice },
          { destinationId: 1.5, instrument: voice },
        ],
      }),
    ).toThrow(/^Project\.bounce: builtinInstruments\[1\]\.destinationId must be an integer/);
  });
});
