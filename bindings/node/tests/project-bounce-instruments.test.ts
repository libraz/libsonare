/**
 * Pins the callback-instrument bounce: a JS instrument driven synchronously
 * through bounceWithInstruments / bounceWithInstrument, its zero-filled
 * zero-filled scratch outputs, first-throw propagation, the Promise refusal, and
 * a long bounce whose JS stack depth does not grow from block to block.
 */

import { describe, expect, it } from 'vitest';
import {
  ErrorCode,
  type ExternalInstrument,
  type ExternalInstrumentEvent,
  Project,
  type ProjectBounceOptions,
} from '../src/index.js';

const OPTIONS: ProjectBounceOptions = {
  totalFrames: 48000,
  blockSize: 128,
  numChannels: 2,
  sampleRate: 48000,
};

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

describe('bounceWithInstruments', () => {
  it('renders the callback instrument into the bounce', () => {
    using project = midiProject();
    const silent = project.bounce(OPTIONS);
    const instrument = new ConstantInstrument(0.25);
    const audio = project.bounceWithInstruments([instrument], OPTIONS);
    expect(audio).toHaveLength(silent.length);
    // The control: a silent bounce is all zero, so the DC is the instrument's.
    expect(Math.max(...silent.map(Math.abs))).toBe(0);
    for (let i = 0; i < audio.length; i += 1) {
      expect(audio[i]).toBeCloseTo(silent[i] + 0.25, 6);
    }
    expect(instrument.renderCalls).toBeGreaterThan(0);
  });

  it('matches across the singular, list and plural forms', () => {
    using project = midiProject();
    const plural = project.bounceWithInstruments([new ConstantInstrument(0.1)], OPTIONS);
    expect(project.bounceWithInstrument(new ConstantInstrument(0.1), OPTIONS)).toEqual(plural);
    expect(project.bounceWithInstrument([new ConstantInstrument(0.1)], OPTIONS)).toEqual(plural);
    expect(project.bounceWithInstruments([new ConstantInstrument(0.3)], OPTIONS)).not.toEqual(
      plural,
    );
    expect(project.bounceWithInstruments([], OPTIONS)).toEqual(project.bounce(OPTIONS));
  });

  it('hands zero-filled outputs sized to the block', () => {
    using project = midiProject();
    const instrument = new ConstantInstrument();
    project.bounceWithInstruments([instrument], OPTIONS);
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
      project.bounceWithInstruments([instrument], { ...OPTIONS, numChannels });
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
    project.bounceWithInstruments([instrument], OPTIONS);
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
    const audio = project.bounceWithInstruments(
      [
        {
          render(outputs, frames) {
            for (const channel of outputs) {
              channel.fill(0.1, 0, frames);
            }
          },
        },
      ],
      OPTIONS,
    );
    expect(Math.max(...audio.map(Math.abs))).toBeGreaterThan(0);
  });

  it('extends an auto-length bounce by tailSamples', () => {
    using project = midiProject();
    const auto = { ...OPTIONS, totalFrames: 0 };
    const dry = project.bounceWithInstruments([new ConstantInstrument(0)], auto);
    const withTail = project.bounceWithInstruments([{ render() {}, tailSamples: 4096 }], auto);
    expect(withTail.length).toBe(dry.length + 4096 * 2);
  });

  describe('a throwing callback', () => {
    it('stops further callbacks and rethrows the first error after the bounce', () => {
      using project = midiProject();
      let calls = 0;
      const boom = new Error('synthesis failed');
      expect(() =>
        project.bounceWithInstruments(
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
        project.bounceWithInstruments(
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
        project.bounceWithInstruments(
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
        project.bounceWithInstruments(
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
      expect(project.bounceWithInstruments([new ConstantInstrument()], OPTIONS)).toHaveLength(
        OPTIONS.totalFrames! * 2,
      );
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
      expect(() => project.bounceWithInstruments([instrument], OPTIONS)).toThrow(TypeError);
      expect(() => project.bounceWithInstruments([instrument], OPTIONS)).toThrow(
        new RegExp(`${name} must not return a Promise`),
      );
      expect(calls).toBe(2);
    });

    it('refuses a bare thenable', () => {
      using project = midiProject();
      const instrument = {
        render: () => Object.defineProperty({}, 'then', { value: () => undefined }),
      } as unknown as ExternalInstrument;
      expect(() => project.bounceWithInstruments([instrument], OPTIONS)).toThrow(TypeError);
    });
  });

  describe('argument refusals', () => {
    it('names a missing or mistyped callback before any render starts', () => {
      using project = midiProject();
      expect(() => project.bounceWithInstruments([{} as never], OPTIONS)).toThrow(
        new TypeError('bounceWithInstruments: instruments[0].render must be a function'),
      );
      expect(() =>
        project.bounceWithInstruments([{ render() {}, prepare: 5 } as never], OPTIONS),
      ).toThrow(new TypeError('bounceWithInstruments: instruments[0].prepare must be a function'));
      expect(() =>
        project.bounceWithInstruments([{ render() {}, onEvent: 'x' } as never], OPTIONS),
      ).toThrow(new TypeError('bounceWithInstruments: instruments[0].onEvent must be a function'));
    });

    it('refuses a non-binding element and a non-array list by name', () => {
      using project = midiProject();
      expect(() => project.bounceWithInstruments(7 as never, OPTIONS)).toThrow(
        new TypeError('bounceWithInstruments: instruments must be an array'),
      );
      expect(() => project.bounceWithInstruments([[{ render() {} }]] as never, OPTIONS)).toThrow(
        new TypeError('bounceWithInstruments: instruments[0] must be an object'),
      );
    });
  });

  it('refuses use of the project from inside a callback', () => {
    using project = midiProject();
    const attempts: Array<[string, () => unknown]> = [
      ['setSampleRate', () => project.setSampleRate(44100)],
      ['bounce', () => project.bounce(OPTIONS)],
      ['nested bounceWithInstruments', () => project.bounceWithInstruments([], OPTIONS)],
      ['destroy', () => project.destroy()],
    ];
    for (const [name, attempt] of attempts) {
      let refusal: unknown;
      project.bounceWithInstruments(
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
    expect(project.getSampleRate()).toBe(48000);
    expect(project.bounce(OPTIONS)).toHaveLength(OPTIONS.totalFrames! * 2);
  });

  it('reports a native failure ahead of a callback error', () => {
    using project = midiProject();
    let prepared = false;
    const bounce = () =>
      project.bounceWithInstruments(
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
        { ...OPTIONS, totalFrames: 0 },
      );
    expect(bounce).toThrow(/Invalid parameter/);
    // The control: the callback did run and throw before the native failure.
    expect(prepared).toBe(true);
  });

  it('refuses a bad options bag without bouncing', () => {
    using project = midiProject();
    for (const instruments of [[], [new ConstantInstrument()]]) {
      expect(() =>
        project.bounceWithInstruments(instruments, {
          blockSize: 'x',
        } as unknown as ProjectBounceOptions),
      ).toThrow(TypeError);
    }
  });

  it('survives an instrument that transfers its scratch buffer away', () => {
    using project = midiProject();
    const audio = project.bounceWithInstruments(
      [
        {
          render(outputs, frames) {
            outputs[0].fill(0.3, 0, frames);
            structuredClone(outputs[0].buffer, { transfer: [outputs[0].buffer] });
          },
        },
      ],
      OPTIONS,
    );
    expect(audio).toHaveLength(OPTIONS.totalFrames! * 2);
    expect(audio.every(Number.isFinite)).toBe(true);
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
    const audio = project.bounceWithInstruments(
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
    expect(audio).toHaveLength(96000 * 2);
    expect(audio.every((v) => Math.abs(v - 0.05) < 1e-6)).toBe(true);
  });
});

describe('every bounce entry point refuses a bad options bag', () => {
  const bad = { blockSize: 'x' } as unknown as ProjectBounceOptions;
  const calls: Array<[string, (project: Project) => unknown]> = [
    ['bounce', (p) => p.bounce(bad)],
    ['bounceWithBuiltinInstruments', (p) => p.bounceWithBuiltinInstruments([], bad)],
    ['bounceWithBuiltinInstrument', (p) => p.bounceWithBuiltinInstrument({}, bad)],
    ['bounceWithSynthInstruments', (p) => p.bounceWithSynthInstruments([], bad)],
    ['bounceWithSf2Instruments', (p) => p.bounceWithSf2Instruments([], bad)],
  ];
  it.each(calls)('%s', (_name, call) => {
    using project = midiProject();
    expect(() => call(project)).toThrow(/blockSize/);
  });
});
