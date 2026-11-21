/**
 * Pins the callback-instrument bounce: a JS instrument driven synchronously
 * through Project.bounce with instruments, its zero-filled scratch outputs, first-throw propagation, the Promise refusal, and
 * a long bounce whose JS stack depth does not grow from block to block.
 */

import { describe, expect, it } from 'vitest';
import {
  ErrorCode,
  type ExternalInstrument,
  type ExternalInstrumentEvent,
  midiNoteOff,
  midiNoteOn,
  Project,
  type ProjectBounceOptions,
  type ProjectBounceResult,
} from '../src/index.js';

const TOTAL_FRAMES = 48000;
const OPTIONS: ProjectBounceOptions = {
  totalFrames: TOTAL_FRAMES,
  blockSize: 128,
  numChannels: 2,
};

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

/** Binds each instrument to destination 0 and bounces through the callback host. */
function run(
  project: Project,
  instruments: ExternalInstrument[],
  options: ProjectBounceOptions = OPTIONS,
): ProjectBounceResult {
  return project.bounce({
    ...options,
    instruments: instruments.map((instrument) => ({ instrument })),
  });
}

/** Every plane of a bounce, back to back. */
function flat(result: ProjectBounceResult): Float32Array {
  const out = new Float32Array(result.channels.reduce((n, c) => n + c.length, 0));
  let at = 0;
  for (const plane of result.channels) {
    out.set(plane, at);
    at += plane.length;
  }
  return out;
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
    const silent = flat(project.bounce(OPTIONS));
    const instrument = new ConstantInstrument(0.25);
    const audio = flat(run(project, [instrument]));
    expect(audio).toHaveLength(silent.length);
    // The control: a silent bounce is all zero, so the DC is the instrument's.
    expect(Math.max(...silent.map(Math.abs))).toBe(0);
    for (let i = 0; i < audio.length; i += 1) {
      expect(audio[i]).toBeCloseTo(silent[i] + 0.25, 6);
    }
    expect(instrument.renderCalls).toBeGreaterThan(0);
  });

  it('refuses a bare binding and matches an omitted destination inside a list', () => {
    using project = midiProject();
    const listed = run(project, [new ConstantInstrument(0.1)]);
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
      project.bounce({
        ...OPTIONS,
        instruments: [{ destinationId: 0, instrument: new ConstantInstrument(0.1) }],
      }),
    ).toEqual(listed);
    expect(run(project, [new ConstantInstrument(0.3)])).not.toEqual(listed);
    expect(run(project, [])).toEqual(project.bounce(OPTIONS));
  });

  it('hands zero-filled outputs sized to the block', () => {
    using project = midiProject();
    const instrument = new ConstantInstrument();
    run(project, [instrument]);
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
      run(project, [instrument], { ...OPTIONS, numChannels });
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
    run(project, [instrument]);
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
    const audio = flat(
      run(project, [
        {
          render(outputs, frames) {
            for (const channel of outputs) {
              channel.fill(0.1, 0, frames);
            }
          },
        },
      ]),
    );
    expect(Math.max(...audio.map(Math.abs))).toBeGreaterThan(0);
  });

  it('extends an auto-length bounce by tailSamples', () => {
    using project = midiProject();
    const { totalFrames: _derived, ...auto } = OPTIONS;
    const dry = run(project, [new ConstantInstrument(0)], auto);
    const withTail = run(project, [{ render() {}, tailSamples: 4096 }], auto);
    expect(withTail.channels.map((c) => c.length)).toEqual(
      dry.channels.map((c) => c.length + 4096),
    );
  });

  describe('a throwing callback', () => {
    it('stops further callbacks and rethrows the first error after the bounce', () => {
      using project = midiProject();
      let calls = 0;
      const boom = new Error('synthesis failed');
      expect(() =>
        run(
          project,
          [
            {
              render() {
                calls += 1;
                throw boom;
              },
            },
          ],
          { ...OPTIONS, blockSize: 32 },
        ),
      ).toThrow(boom);
      expect(calls).toBe(1);
    });

    it('latches a throw from onEvent and prepare as well', () => {
      using project = midiProject();
      let events = 0;
      expect(() =>
        run(
          project,
          [
            {
              onEvent() {
                events += 1;
                throw new RangeError('bad event');
              },
              render() {},
            },
          ],
          OPTIONS,
        ),
      ).toThrow(new RangeError('bad event'));
      expect(events).toBe(1);

      let afterPrepare = 0;
      expect(() =>
        run(
          project,
          [
            {
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
          ],
          OPTIONS,
        ),
      ).toThrow('no prepare');
      expect(afterPrepare).toBe(0);
    });

    it('lets the project bounce again after a throw', () => {
      using project = midiProject();
      expect(() =>
        run(
          project,
          [
            {
              render() {
                throw new Error('once');
              },
            },
          ],
          OPTIONS,
        ),
      ).toThrow('once');
      expect(flat(run(project, [new ConstantInstrument()]))).toHaveLength(TOTAL_FRAMES * 2);
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
      expect(() => run(project, [instrument])).toThrow(TypeError);
      expect(() => run(project, [instrument])).toThrow(
        new RegExp(`${name} must not return a Promise`),
      );
      expect(calls).toBe(2);
    });

    it('refuses a bare thenable', () => {
      using project = midiProject();
      const instrument = {
        render: () => Object.defineProperty({}, 'then', { value: () => undefined }),
      } as unknown as ExternalInstrument;
      expect(() => run(project, [instrument])).toThrow(TypeError);
    });
  });

  describe('argument refusals', () => {
    const bounceOne = (project: Project, instrument: unknown) =>
      project.bounce({ ...OPTIONS, instruments: [{ instrument } as never] });

    it('names a missing or mistyped callback before any render starts', () => {
      using project = midiProject();
      expect(() => bounceOne(project, {})).toThrow(
        new TypeError('Project.bounce: instruments[0].render must be a function'),
      );
      expect(() => bounceOne(project, { render() {}, prepare: 5 })).toThrow(
        new TypeError('Project.bounce: instruments[0].prepare must be a function'),
      );
      expect(() => bounceOne(project, { render() {}, onEvent: 'x' })).toThrow(
        new TypeError('Project.bounce: instruments[0].onEvent must be a function'),
      );
    });

    it('refuses a non-binding element and a binding without an instrument by name', () => {
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
        project.bounce({ ...OPTIONS, instruments: [[{ render() {} }]] as never }),
      ).toThrow(
        new TypeError(
          'Project.bounce: instruments[0] must be a { destinationId?, instrument } pair',
        ),
      );
      expect(() =>
        project.bounce({ ...OPTIONS, instruments: [{ destinationId: 0 }] as never }),
      ).toThrow(new TypeError('Project.bounce: instruments[0].instrument must be given'));
      expect(() => bounceOne(project, [{ render() {} }])).toThrow(
        new TypeError('Project.bounce: instruments[0].instrument must be an object'),
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
      ['nested bounce with instruments', () => run(project, [])],
      ['destroy', () => project.destroy()],
    ];
    for (const [name, attempt] of attempts) {
      let refusal: unknown;
      run(
        project,
        [
          {
            render() {
              try {
                attempt();
              } catch (error) {
                refusal ??= error;
              }
            },
          },
        ],
        OPTIONS,
      );
      expect(refusal, name).toBeDefined();
      expect(refusal, name).toMatchObject({ code: ErrorCode.InvalidState });
    }
    // The refusals changed nothing: the project still bounces and keeps its rate.
    expect(project.sampleRate).toBe(48000);
    expect(flat(project.bounce(OPTIONS))).toHaveLength(TOTAL_FRAMES * 2);
  });

  it('reports a native failure ahead of a callback error', () => {
    using project = midiProject();
    let prepared = false;
    const bounce = () =>
      run(
        project,
        [
          {
            tailSamples: 2147483647,
            prepare() {
              prepared = true;
              throw new Error('callback error');
            },
            render() {},
          },
        ],
        { ...OPTIONS, totalFrames: undefined },
      );
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
      ).toThrow(TypeError);
    }
  });

  it('survives an instrument that transfers its scratch buffer away', () => {
    using project = midiProject();
    const audio = run(
      project,
      [
        {
          render(outputs, frames) {
            outputs[0].fill(0.3, 0, frames);
            structuredClone(outputs[0].buffer, { transfer: [outputs[0].buffer as ArrayBuffer] });
          },
        },
      ],
      OPTIONS,
    );
    expect(flat(audio)).toHaveLength(TOTAL_FRAMES * 2);
    expect(flat(audio).every(Number.isFinite)).toBe(true);
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
    const audio = run(
      project,
      [
        {
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
      ],
      { ...OPTIONS, totalFrames: 96000, blockSize: 16 },
    );
    expect(blocks).toBeGreaterThanOrEqual(5000);
    expect(flat(audio)).toHaveLength(96000 * 2);
    expect(flat(audio).every((v) => Math.abs(v - 0.05) < 1e-6)).toBe(true);
  });
});

describe('every bounce entry point refuses a bad options bag', () => {
  const bad = { blockSize: 'x' } as unknown as ProjectBounceOptions;
  const calls: Array<[string, (project: Project) => unknown]> = [
    ['bounce', (p) => p.bounce(bad)],
    ['builtinInstruments', (p) => p.bounce({ ...bad, builtinInstruments: [] })],
    [
      'builtinInstruments (one binding)',
      (p) => p.bounce({ ...bad, builtinInstruments: [{ instrument: {} }] }),
    ],
    ['synthInstruments', (p) => p.bounce({ ...bad, synthInstruments: [] })],
    ['sf2Instruments', (p) => p.bounce({ ...bad, sf2Instruments: [] })],
  ];
  it.each(calls)('%s', (_name, call) => {
    using project = midiProject();
    expect(() => call(project)).toThrow(/blockSize/);
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
