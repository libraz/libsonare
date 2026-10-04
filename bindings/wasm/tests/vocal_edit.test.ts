import { beforeAll, describe, expect, it } from 'vitest';
import { init } from '../src/index';
import { createVocalEditSession, restoreVocalEditSession } from '../src/vocal_edit';

const SAMPLE_RATE = 16000;

function tone(length: number, frequency = 220): Float32Array {
  const out = new Float32Array(length);
  for (let i = 0; i < length; i++) {
    out[i] = 0.35 * Math.sin((2 * Math.PI * frequency * i) / SAMPLE_RATE);
  }
  return out;
}

describe('WASM vocal edit facade', () => {
  beforeAll(async () => {
    await init();
  });

  it('creates a mono session and keeps uint64 state as decimal strings', () => {
    const session = createVocalEditSession({
      samples: tone(SAMPLE_RATE),
      sampleRate: SAMPLE_RATE,
    });
    try {
      expect(session.token().revision).toBe('0');
      expect(session.token().sessionEpoch).toMatch(/^\d+$/);
      expect(session.capabilities().monophonicOnly).toBe(true);
      const notes = session.notes();
      expect(notes.notes.length).toBeGreaterThan(0);
      expect(notes.notes[0]?.amplitude).toBeInstanceOf(Float32Array);
      expect(session.analysis().f0Hz).toBeInstanceOf(Float32Array);
    } finally {
      session.dispose();
    }
  });

  it('rejects non-finite PCM before creating a native handle', () => {
    const samples = tone(512);
    samples[17] = Number.NaN;
    expect(() => createVocalEditSession({ samples, sampleRate: SAMPLE_RATE })).toThrow();
  });

  it('preserves native vocal error detail on a revision conflict', () => {
    const session = createVocalEditSession({ samples: tone(512), sampleRate: SAMPLE_RATE });
    try {
      expect(() => session.beginEdit({ expectedRevision: '1' })).toThrow();
      try {
        session.beginEdit({ expectedRevision: '1' });
        throw new Error('expected beginEdit to reject the stale revision');
      } catch (error) {
        const detail = error as Error & {
          code?: number;
          reason?: number;
          field?: string;
          expected?: string;
          actual?: string;
          expectedText?: string;
          actualText?: string;
        };
        expect(detail.code).toBe(7);
        expect(detail.reason).toBe(2);
        expect(detail.field).toBe('revision');
        expect(detail.expected).toBe('1');
        expect(detail.actual).toBe('0');
        expect(detail.expectedText).toBe('1');
        expect(detail.actualText).toBe('0');
      }
    } finally {
      session.dispose();
    }
  });

  it('closes a draft handle when cancel completes', () => {
    const session = createVocalEditSession({ samples: tone(512), sampleRate: SAMPLE_RATE });
    try {
      const draft = session.beginEdit();
      draft.cancel();
      expect(() => draft.token()).toThrow();
      expect(() => session.beginEdit()).not.toThrow();
    } finally {
      session.dispose();
    }
  });

  it('renders the complete destination when range is omitted', () => {
    const source = tone(512);
    const session = createVocalEditSession({ samples: source, sampleRate: SAMPLE_RATE });
    try {
      const snapshot = session.captureRenderSnapshot();
      try {
        expect(snapshot.outputLengthSamples()).toBe(source.length);
        const rendered = snapshot.render({});
        expect(rendered.startSample).toBe(0);
        expect(rendered.samples.length).toBe(source.length);
      } finally {
        snapshot.dispose();
      }
    } finally {
      session.dispose();
    }
  });

  it('round-trips a state blob against the same source bytes', () => {
    const source = tone(SAMPLE_RATE);
    const session = createVocalEditSession({ samples: source, sampleRate: SAMPLE_RATE });
    try {
      const state = session.exportState();
      expect(state).toBeInstanceOf(Uint8Array);
      const restored = restoreVocalEditSession({
        samples: source,
        sampleRate: SAMPLE_RATE,
        state,
      });
      try {
        expect(restored.notes().notes.map((note) => note.id)).toEqual(
          session.notes().notes.map((note) => note.id),
        );
        expect(restored.token().revision).toBe(session.token().revision);
      } finally {
        restored.dispose();
      }
    } finally {
      session.dispose();
    }
  });

  it('renders asynchronously one unit per macrotask with the synchronous result', async () => {
    const session = createVocalEditSession({ samples: tone(4096), sampleRate: SAMPLE_RATE });
    const snapshot = session.captureRenderSnapshot();
    try {
      const expected = snapshot.render({ requestId: '4' });
      const rendered = await snapshot.renderAsync({ requestId: '4' });
      expect(rendered.samples).toEqual(expected.samples);
      expect(rendered.token).toEqual(expected.token);

      const aborted = new AbortController();
      aborted.abort();
      await expect(snapshot.renderAsync({ signal: aborted.signal })).rejects.toMatchObject({
        name: 'AbortError',
      });

      const midway = new AbortController();
      const pending = snapshot.renderAsync({ signal: midway.signal });
      midway.abort();
      await expect(pending).rejects.toMatchObject({ name: 'AbortError' });
    } finally {
      snapshot.dispose();
      session.dispose();
    }
  });

  it('matches the WASM handle family and treats a second cancel as a no-op', () => {
    const session = createVocalEditSession({ samples: tone(512), sampleRate: SAMPLE_RATE });
    const draft = session.beginEdit();
    draft.cancel();
    expect(() => draft.cancel()).not.toThrow();
    draft.destroy();
    const snapshot = session.captureRenderSnapshot();
    snapshot.destroy();
    expect(() => snapshot.outputLengthSamples()).toThrow(/disposed/);
    session.destroy();
    expect(() => session.token()).toThrow(/disposed/);
    expect(() => session.delete()).not.toThrow();
  });

  it('accepts only canonical decimal uint64 strings for state-token inputs', () => {
    const session = createVocalEditSession({ samples: tone(512), sampleRate: SAMPLE_RATE });
    try {
      for (const revision of ['00', '007', '+0', '0.0', '', ' 0', '18446744073709551616']) {
        expect(() => session.beginEdit({ expectedRevision: revision }), revision).toThrow(
          RangeError,
        );
      }
      for (const revision of [0, 0n]) {
        expect(() => session.undo({ expectedRevision: revision as unknown as string })).toThrow(
          RangeError,
        );
      }
      const draft = session.beginEdit({ expectedRevision: '0' });
      expect(() => draft.apply({ expectedGeneration: '00', operations: [] })).toThrow(RangeError);
      draft.cancel();
      const snapshot = session.captureRenderSnapshot();
      try {
        expect(() => snapshot.render({ requestId: '01' })).toThrow(RangeError);
      } finally {
        snapshot.dispose();
      }
    } finally {
      session.dispose();
    }
  });
});
