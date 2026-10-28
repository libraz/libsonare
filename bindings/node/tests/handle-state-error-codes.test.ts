/**
 * Library and object-state failures are a coded SonareError on the Node surface:
 * a released handle (receiver or argument) is InvalidState, a refused library
 * argument keeps its own code, and an argument refusal stays TypeError/RangeError.
 */

import { describe, expect, it } from 'vitest';
import {
  Audio,
  ErrorCode,
  HrtfSet,
  PlaybackRenderer,
  Project,
  type ProjectTimeline,
  RealtimeEngine,
  renderPlayback,
  SampleBank,
  voiceChange,
} from '../src/index.js';
import { addon } from '../src/native.js';

// biome-ignore lint/suspicious/noExplicitAny: the native surface is untyped on purpose here.
type Loose = any;

/** Release methods stay idempotent no-ops once the owner has freed what they release. */
const RELEASE_METHODS: Record<string, readonly string[]> = {
  RealtimeEngine: ['destroyClipPageProvider'],
};

const capture = (run: () => unknown): unknown => {
  try {
    run();
  } catch (error) {
    return error;
  }
  return undefined;
};

const codeOf = (error: unknown): unknown => (error as { code?: unknown } | undefined)?.code;

/** Every method on the prototype except the destructor itself. */
function methodNames(prototype: object): string[] {
  return Object.getOwnPropertyNames(prototype).filter((name) => {
    if (name === 'constructor' || name === 'destroy') {
      return false;
    }
    const descriptor = Object.getOwnPropertyDescriptor(prototype, name);
    return typeof descriptor?.value === 'function';
  });
}

describe('a destroyed native handle reports InvalidState on every method', () => {
  const handles: Array<[string, () => Loose]> = [
    ['Project', () => new addon.Project()],
    ['RealtimeEngine', () => new addon.RealtimeEngine(48000, 128)],
    ['StreamingRetune', () => new addon.StreamingRetune({})],
  ];

  for (const [label, make] of handles) {
    it(label, () => {
      const probe = make();
      const names = methodNames(Object.getPrototypeOf(probe));
      probe.destroy();
      expect(names.length).toBeGreaterThan(3);
      const offenders: string[] = [];
      const exempt = RELEASE_METHODS[label] ?? [];
      for (const name of exempt) {
        expect(() => probe[name](1), name).not.toThrow();
      }
      for (const name of names.filter((n) => !exempt.includes(n))) {
        const error = capture(() => probe[name]());
        if (codeOf(error) !== ErrorCode.InvalidState) {
          offenders.push(`${name}: ${String(error)}`);
        }
      }
      expect(offenders).toEqual([]);
    });
  }

  it('read-only Project accessors do not answer for a released project', () => {
    const project = new addon.Project() as Loose;
    project.destroy();
    expect(codeOf(capture(() => project.assistSidecarCount()))).toBe(ErrorCode.InvalidState);
    expect(codeOf(capture(() => project.assistSidecars()))).toBe(ErrorCode.InvalidState);
  });

  it('RealtimeEngine.tailSamples refuses a destroyed engine', () => {
    const engine = new addon.RealtimeEngine(48000, 128) as Loose;
    engine.destroy();
    expect(codeOf(capture(() => engine.tailSamples()))).toBe(ErrorCode.InvalidState);
  });
});

describe('a released handle passed as an argument reports InvalidState', () => {
  it('a destroyed HrtfSet at PlaybackRenderer construction and the offline render', () => {
    const hrtf = HrtfSet.default();
    hrtf.destroy();
    const config = { target: { kind: 'headphones' } } as never;
    const samples = new Float32Array(960);
    const viaRenderer = capture(() => new PlaybackRenderer({ config, hrtf }));
    const viaOffline = capture(() =>
      renderPlayback({ samples, channels: 1, sampleRate: 48000, config, hrtf }),
    );
    expect(codeOf(viaRenderer)).toBe(ErrorCode.InvalidState);
    expect(codeOf(viaOffline)).toBe(ErrorCode.InvalidState);
  });

  it('a non-HrtfSet stays a TypeError', () => {
    const config = { target: { kind: 'headphones' } } as never;
    const error = capture(() => new PlaybackRenderer({ config, hrtf: {} as never }));
    expect(error).toBeInstanceOf(TypeError);
  });

  it('a disposed ProjectTimeline applied to an engine', () => {
    const project = Project.create();
    const engine = new RealtimeEngine(48000, 128);
    try {
      const track = project.addTrack({ kind: 'midi' });
      project.addClip({ trackId: track, isMidi: true, startPpq: 0, lengthPpq: 4 });
      const { timeline } = project.compileTimeline();
      expect(timeline).not.toBeNull();
      timeline?.dispose();
      const error = capture(() => engine.applyProjectTimeline(timeline as ProjectTimeline));
      expect(codeOf(error)).toBe(ErrorCode.InvalidState);
    } finally {
      engine.destroy();
      project.destroy();
    }
  });

  it('a non-timeline stays a TypeError', () => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      expect(capture(() => engine.applyProjectTimeline({} as never))).toBeInstanceOf(TypeError);
    } finally {
      engine.destroy();
    }
  });

  it('a destroyed SampleBank in an instrument binding', () => {
    const bank = new SampleBank();
    bank.addSample(new Float32Array(64).fill(0.1));
    bank.addZone({ sampleIndex: 0 });
    bank.destroy();
    const engine = new RealtimeEngine(48000, 128);
    try {
      const error = capture(() =>
        engine.setSynthInstrument({ engineMode: 'sample', sampleBank: bank }, 7),
      );
      expect(codeOf(error)).toBe(ErrorCode.InvalidState);
    } finally {
      engine.destroy();
    }
  });
});

describe('setAssistSidecar validates before it commits', () => {
  const base = { moduleId: 'mod', schemaVersion: 1, targetTrackId: 0 };

  const withProject = (body: (project: Project) => void): void => {
    const project = Project.create();
    try {
      body(project);
    } finally {
      project.destroy();
    }
  };

  for (const field of ['regionStartPpq', 'regionEndPpq'] as const) {
    it(`refuses a string ${field} by name and leaves the sidecar count unchanged`, () => {
      withProject((project) => {
        const before = project.assistSidecarCount();
        const error = capture(() => project.setAssistSidecar({ ...base, [field]: '1' as never }));
        expect(error).toBeInstanceOf(TypeError);
        expect((error as Error).message).toMatch(new RegExp(field));
        expect(project.assistSidecarCount()).toBe(before);
      });
    });
  }

  it('refuses an array payload by name and leaves the project unchanged', () => {
    withProject((project) => {
      const before = project.toJson();
      const error = capture(() =>
        project.setAssistSidecar({ ...base, payload: [1, 2, 3] as never }),
      );
      expect(error).toBeInstanceOf(TypeError);
      expect((error as Error).message).toMatch(/payload/);
      expect(project.assistSidecarCount()).toBe(0);
      expect(project.toJson()).toBe(before);
    });
  });

  it('still accepts an absent payload and a Uint8Array payload', () => {
    withProject((project) => {
      project.setAssistSidecar({ ...base, moduleId: 'empty' });
      project.setAssistSidecar({ ...base, moduleId: 'bytes', payload: new Uint8Array([1, 2]) });
      expect(project.assistSidecarCount()).toBe(2);
    });
  });
});

describe('streaming constructors refuse a refused argument as a RangeError', () => {
  it('StreamingEqualizer refusals are RangeErrors', () => {
    for (const config of [{ sampleRate: 0 }, { sampleRate: -48000 }, { maxBlockSize: -1 }]) {
      const error = capture(() => new addon.StreamingEqualizer(config));
      expect(error, JSON.stringify(config)).toBeInstanceOf(RangeError);
      expect(codeOf(error), JSON.stringify(config)).toBeUndefined();
    }
  });

  it('StreamingMasteringChain refusals are RangeErrors', () => {
    const configs = [
      { loudness: { enabled: true } },
      { loudness: { truePeakOversample: 3 } },
      { loudness: { maxLimiterGainReductionDb: -1 } },
    ];
    for (const config of configs) {
      const error = capture(() => new addon.StreamingMasteringChain(config));
      expect(error, JSON.stringify(config)).toBeInstanceOf(RangeError);
      expect(codeOf(error), JSON.stringify(config)).toBeUndefined();
    }
  });

  it('StreamingEqualizer refuses a maxBlockSize it cannot take exactly, naming the field', () => {
    for (const maxBlockSize of [1.5, Number.NaN, 1e40]) {
      const error = capture(() => new addon.StreamingEqualizer({ maxBlockSize }));
      expect(error, String(maxBlockSize)).toBeInstanceOf(RangeError);
      expect((error as Error).message).toMatch(/maxBlockSize/);
    }
  });
});

describe('voiceChange maps an argument refusal to RangeError in both formant modes', () => {
  const samples = new Float32Array(2205).fill(0.1);

  for (const formantMode of ['relative', 'absolute'] as const) {
    for (const formantFactor of [0, -2, 10, Number.NaN]) {
      it(`${formantMode} formantFactor ${formantFactor}`, () => {
        const error = capture(() =>
          voiceChange({ samples, sampleRate: 22050, formantFactor, formantMode }),
        );
        expect(error).toBeInstanceOf(RangeError);
      });
    }
  }
});

describe('PlaybackRenderer.renderOffline refuses bad samples like its siblings', () => {
  it('RangeError naming samples for empty and NaN input', () => {
    const renderer = new PlaybackRenderer({ config: {} });
    try {
      const empty = capture(() => renderer.renderOffline(new Float32Array(0), 2));
      expect(empty).toBeInstanceOf(RangeError);
      expect((empty as Error).message).toMatch(/samples/);
      const nan = capture(() => renderer.renderOffline(new Float32Array([0, Number.NaN]), 2));
      expect(nan).toBeInstanceOf(RangeError);
      expect((nan as Error).message).toMatch(/samples/);
    } finally {
      renderer.destroy();
    }
  });
});

describe('Audio keeps its closed-handle error', () => {
  it('is InvalidState with the class name in the message', () => {
    const audio = Audio.fromBuffer(new Float32Array(2205), 22050);
    audio.destroy();
    const error = capture(() => audio.rmsDb());
    expect(codeOf(error)).toBe(ErrorCode.InvalidState);
    expect((error as Error).message).toBe('Audio has been destroyed');
  });
});
