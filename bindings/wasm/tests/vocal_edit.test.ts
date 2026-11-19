import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, isSonareError, type SonareError } from '../src/index';
import { getSonareModule } from '../src/module_state';
import type {
  VocalEditOperation,
  VocalErrorDetail,
  VocalSessionLimits,
} from '../src/public_types_vocal_edit';
import {
  createVocalEditSession,
  restoreVocalEditSession,
  VocalEditDraft,
  VocalEditSession,
  VocalRenderJob,
  VocalRenderSnapshot,
} from '../src/vocal_edit';

const SAMPLE_RATE = 16000;

function tone(length: number, frequency = 220): Float32Array {
  const out = new Float32Array(length);
  for (let i = 0; i < length; i++) {
    out[i] = 0.35 * Math.sin((2 * Math.PI * frequency * i) / SAMPLE_RATE);
  }
  return out;
}

function suppliedOneFrameRequest<L extends object = VocalSessionLimits>(limits?: L) {
  return {
    samples: tone(256),
    sampleRate: SAMPLE_RATE,
    analysis: {
      frameOriginSample: 0,
      samplesPerFrame: 256,
      frameLengthSamples: 256,
      f0Hz: new Float32Array([220]),
      voiced: new Uint8Array([1]),
      algorithmId: 'host',
      algorithmVersion: 1,
    },
    ...(limits === undefined ? {} : { limits }),
  };
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
      expect(session.token.revision).toBe('0');
      expect(session.token.sessionEpoch).toMatch(/^\d+$/);
      expect(session.capabilities().monophonicOnly).toBe(true);
      const notes = session.notes();
      expect(notes.notes.length).toBeGreaterThan(0);
      expect(notes.notes[0]?.amplitude).toBeInstanceOf(Float32Array);
      expect(session.analysis().f0Hz).toBeInstanceOf(Float32Array);
    } finally {
      session.destroy();
    }
  });

  it('rejects non-finite PCM before creating a native handle', () => {
    const samples = tone(512);
    samples[17] = Number.NaN;
    expect(() => createVocalEditSession({ samples, sampleRate: SAMPLE_RATE })).toThrow();
  });

  it('rejects NUL bytes in a supplied analysis algorithm id', () => {
    const request = suppliedOneFrameRequest();
    request.analysis.algorithmId = 'host\0suffix';
    expect(() => createVocalEditSession(request)).toThrow(RangeError);
  });

  it('refuses a present analysis that is not an object rather than skipping it', () => {
    for (const analysis of [0, '', false, 1, 'frames', []]) {
      const request = { samples: tone(256), sampleRate: SAMPLE_RATE, analysis } as never;
      expect(() => createVocalEditSession(request)).toThrow(TypeError);
      expect(() => createVocalEditSession(request)).toThrow(/^analysis must be an object$/);
    }
  });

  it('rejects NUL bytes through the native WASM vocal export', () => {
    const request = suppliedOneFrameRequest();
    request.analysis.algorithmId = 'host\0suffix';
    const native = getSonareModule();
    expect(() =>
      native.vocalEditSessionCreate(request.samples, request.sampleRate, request),
    ).toThrow();
  });

  it('preserves native vocal error detail on a revision conflict', () => {
    const session = createVocalEditSession({ samples: tone(512), sampleRate: SAMPLE_RATE });
    try {
      expect(() => session.beginEdit({ expectedRevision: '1' })).toThrow();
      try {
        session.beginEdit({ expectedRevision: '1' });
        throw new Error('expected beginEdit to reject the stale revision');
      } catch (error) {
        expect(isSonareError(error)).toBe(true);
        expect((error as SonareError).code).toBe(ErrorCode.InvalidState);
        expect((error as SonareError & { detail?: VocalErrorDetail }).detail).toEqual({
          reason: 'revisionConflict',
          field: 'revision',
          expected: '1',
          actual: '0',
          expectedText: '1',
          actualText: '0',
        });
      }
    } finally {
      session.destroy();
    }
  });

  it('closes a draft handle when cancel completes', () => {
    const session = createVocalEditSession({ samples: tone(512), sampleRate: SAMPLE_RATE });
    try {
      const draft = session.beginEdit();
      draft.cancel();
      expect(() => draft.token).toThrow();
      expect(() => session.beginEdit()).not.toThrow();
    } finally {
      session.destroy();
    }
  });

  it('reads the output length as a getter', () => {
    for (const owner of [VocalEditSession.prototype, VocalRenderSnapshot.prototype]) {
      expect(typeof Object.getOwnPropertyDescriptor(owner, 'outputLengthSamples')?.get).toBe(
        'function',
      );
    }
  });

  it('renders the complete destination when range is omitted', () => {
    const source = tone(512);
    const session = createVocalEditSession({ samples: source, sampleRate: SAMPLE_RATE });
    try {
      const snapshot = session.captureRenderSnapshot();
      try {
        expect(snapshot.outputLengthSamples).toBe(source.length);
        const rendered = snapshot.render({});
        expect(rendered.startSample).toBe(0);
        expect(rendered.samples.length).toBe(source.length);
      } finally {
        snapshot.destroy();
      }
    } finally {
      session.destroy();
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
        expect(restored.token.revision).toBe(session.token.revision);
      } finally {
        restored.destroy();
      }
    } finally {
      session.destroy();
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

      let invalid: Promise<unknown> | undefined;
      expect(() => {
        invalid = snapshot.renderAsync({ range: { startSample: -1 } });
      }).not.toThrow();
      await expect(invalid).rejects.toBeInstanceOf(RangeError);

      const aborted = new AbortController();
      aborted.abort();
      await expect(snapshot.renderAsync({ signal: aborted.signal })).rejects.toMatchObject({
        code: ErrorCode.Cancelled,
      });

      const midway = new AbortController();
      const pending = snapshot.renderAsync({ signal: midway.signal });
      midway.abort();
      await expect(pending).rejects.toMatchObject({ code: ErrorCode.Cancelled });

      snapshot.destroy();
      await expect(snapshot.renderAsync({})).rejects.toThrow(/disposed/);
    } finally {
      snapshot.destroy();
      session.destroy();
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
    expect(() => snapshot.outputLengthSamples).toThrow(/disposed/);
    session.destroy();
    expect(() => session.token).toThrow(/disposed/);
    expect(() => session.destroy()).not.toThrow();
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
        snapshot.destroy();
      }
    } finally {
      session.destroy();
    }
  });

  it('honours restore limits and refuses a zero render-job limit', () => {
    const source = tone(SAMPLE_RATE);
    const session = createVocalEditSession({ samples: source, sampleRate: SAMPLE_RATE });
    const state = session.exportState();
    session.destroy();
    expect(() =>
      restoreVocalEditSession({
        samples: source,
        sampleRate: SAMPLE_RATE,
        state,
        limits: { maxRenderJobs: 0 },
      }),
    ).toThrow();
    const restored = restoreVocalEditSession({
      samples: source,
      sampleRate: SAMPLE_RATE,
      state,
      limits: { maxRenderJobs: 1 },
    });
    const snapshot = restored.captureRenderSnapshot();
    try {
      const range = { startSample: 0, endSample: 256 };
      const first = snapshot.beginRenderJob({ range });
      expect(() => snapshot.beginRenderJob({ range })).toThrow();
      first.destroy();
    } finally {
      snapshot.destroy();
      restored.destroy();
    }
  });

  it('refuses a session limit that is not a non-negative safe integer on create and restore', () => {
    const valid = suppliedOneFrameRequest({ maxHistoryBytes: 0, maxCacheBytes: 7 });
    const session = createVocalEditSession(valid);
    const state = session.exportState();
    session.destroy();

    const refused: Array<[VocalSessionLimits, ErrorConstructor]> = [
      [{ maxHistoryBytes: -1 }, RangeError],
      [{ maxCacheBytes: 1.5 }, RangeError],
      [{ maxCacheBytes: Number.NaN }, RangeError],
      [{ maxHistoryBytes: '7' as unknown as number }, TypeError],
    ];
    for (const [limits, errorClass] of refused) {
      expect(() => createVocalEditSession(suppliedOneFrameRequest(limits))).toThrow(errorClass);
      expect(() =>
        restoreVocalEditSession({
          samples: valid.samples,
          sampleRate: SAMPLE_RATE,
          state,
          limits,
        }),
      ).toThrow(errorClass);
    }

    const accepted = createVocalEditSession(suppliedOneFrameRequest({ maxHistoryBytes: 0 }));
    accepted.destroy();
    const restored = restoreVocalEditSession({
      samples: valid.samples,
      sampleRate: SAMPLE_RATE,
      state,
      limits: { maxCacheBytes: 7 },
    });
    restored.destroy();
  });

  it('enforces canonical limits through the native WASM vocal exports', () => {
    const native = getSonareModule();
    const valid = suppliedOneFrameRequest({ maxHistoryBytes: 0, maxCacheBytes: 7 });
    const session = createVocalEditSession(valid);
    const state = session.exportState();
    session.destroy();

    for (const limits of [{ maxHistoryBytes: '007' }, { maxCacheBytes: '007' }]) {
      const createRequest = suppliedOneFrameRequest(limits);
      expect(() =>
        native.vocalEditSessionCreate(
          createRequest.samples,
          createRequest.sampleRate,
          createRequest,
        ),
      ).toThrow();
      expect(() =>
        native.vocalEditSessionRestore(valid.samples, SAMPLE_RATE, state, { limits }),
      ).toThrow();
    }

    const acceptedRequest = suppliedOneFrameRequest({ maxCacheBytes: '7' });
    const handle = native.vocalEditSessionCreate(
      acceptedRequest.samples,
      acceptedRequest.sampleRate,
      acceptedRequest,
    );
    native.vocalEditSessionDestroy(handle);
  });

  it('refuses a sample rate outside the supported range on create and restore', () => {
    for (const sampleRate of [7999, 384001]) {
      expect(() => createVocalEditSession({ samples: tone(512), sampleRate })).toThrow(RangeError);
      expect(() =>
        restoreVocalEditSession({ samples: tone(512), sampleRate, state: new Uint8Array(8) }),
      ).toThrow(RangeError);
    }
  });

  it('disposes live drafts when their session is disposed', () => {
    const session = createVocalEditSession({ samples: tone(512), sampleRate: SAMPLE_RATE });
    const draft = session.beginEdit();
    session.destroy();
    expect(() => draft.token).toThrow(/disposed/);
    expect(() => draft.apply({ expectedGeneration: '0', operations: [] })).toThrow(/disposed/);
    expect(() => draft.cancel()).not.toThrow();
  });

  it('applies against the draft generation when expectedGeneration is omitted', () => {
    const session = createVocalEditSession({ samples: tone(512), sampleRate: SAMPLE_RATE });
    try {
      const draft = session.beginEdit();
      const noteId = session.notes().notes[0]?.id as number;
      const result = draft.apply({ operations: [{ kind: 'reset', noteIds: [noteId] }] });
      expect(result.token.generation).toBe(draft.token.generation);
      expect(() => draft.apply({ operations: [] })).toThrow(RangeError);
      expect(() => draft.apply({ expectedGeneration: '00', operations: [] })).toThrow(RangeError);
      draft.cancel();
    } finally {
      session.destroy();
    }
  });

  it('requires every transition field and a smoothstep curve', () => {
    const session = createVocalEditSession({ samples: tone(SAMPLE_RATE), sampleRate: SAMPLE_RATE });
    try {
      const draft = session.beginEdit();
      const base = {
        leftNoteId: 1,
        rightNoteId: 2,
        leftWindowSamples: 0,
        rightWindowSamples: 0,
        strength: 1,
        curve: 'smoothstep',
      };
      const apply = (transition: Record<string, unknown>) =>
        draft.apply({
          expectedGeneration: draft.token.generation,
          operations: [{ kind: 'setTransition', transition } as unknown as VocalEditOperation],
        });
      expect(() => apply({ ...base, curve: 'linear' })).toThrow(/curve must be smoothstep/);
      const { curve: _curve, ...withoutCurve } = base;
      expect(() => apply(withoutCurve)).toThrow(/curve must be smoothstep/);
      const { strength: _strength, ...withoutStrength } = base;
      expect(() => apply(withoutStrength)).toThrow(/strength/);
      const { leftWindowSamples: _left, ...withoutWindow } = base;
      expect(() => apply(withoutWindow)).toThrow(/leftWindowSamples/);
      expect(() => apply({ ...base, strength: 2 })).toThrow(/strength/);
      draft.cancel();
    } finally {
      session.destroy();
    }
  });

  it('refuses an omitted source-span, split or edit destination position by name', () => {
    const session = createVocalEditSession({ samples: tone(SAMPLE_RATE), sampleRate: SAMPLE_RATE });
    try {
      const draft = session.beginEdit();
      const generation = draft.token.generation;
      const apply = (operation: Record<string, unknown>) =>
        draft.apply({
          expectedGeneration: draft.token.generation,
          operations: [operation as unknown as VocalEditOperation],
        });
      const span = {
        kind: 'setSourceSpan',
        noteId: 1,
        sourceStartSample: 0,
        sourceEndSample: 100,
        destinationStartSample: 0,
        destinationLengthSamples: 100,
      };
      for (const field of [
        'sourceStartSample',
        'sourceEndSample',
        'destinationStartSample',
        'destinationLengthSamples',
      ] as const) {
        const { [field]: _omitted, ...withoutField } = span;
        expect(() => apply(withoutField)).toThrow(TypeError);
        expect(() => apply(withoutField)).toThrow(new RegExp(field));
      }
      expect(() => apply({ kind: 'split', noteId: 1 })).toThrow(TypeError);
      expect(() => apply({ kind: 'split', noteId: 1 })).toThrow(/sourceSample/);
      const edit = {
        pitch: { target: { mode: 'none' } },
        destinationStartSample: 0,
        destinationLengthSamples: 100,
      };
      for (const field of ['destinationStartSample', 'destinationLengthSamples'] as const) {
        const { [field]: _omitted, ...withoutField } = edit;
        const run = () => apply({ kind: 'setEdit', noteId: 1, edit: withoutField });
        expect(run).toThrow(TypeError);
        expect(run).toThrow(new RegExp(`edit\\.${field}`));
      }
      // The refused operations left the draft generation where it was.
      expect(draft.token.generation).toBe(generation);
      draft.cancel();
    } finally {
      session.destroy();
    }
  });

  it('reads the session and draft token and revision as getters', () => {
    const session = createVocalEditSession({ samples: tone(512), sampleRate: SAMPLE_RATE });
    try {
      expect(typeof session.token).toBe('object');
      expect(session.token.revision).toBe('0');
      expect(session.revision).toBe('0');
      const draft = session.beginEdit();
      expect(typeof draft.token).toBe('object');
      expect(draft.token.draftId).toMatch(/^[1-9]\d*$/);
      expect(typeof draft.revision).toBe('string');
      expect(draft.revision).toBe(session.revision);
      draft.cancel();
    } finally {
      session.destroy();
    }
  });

  it('returns the analysisFrameStart and analysisFrameEnd of a note as numbers', () => {
    const session = createVocalEditSession({ samples: tone(SAMPLE_RATE), sampleRate: SAMPLE_RATE });
    try {
      const { notes } = session.notes();
      expect(notes.length).toBeGreaterThan(0);
      for (const note of notes) {
        expect(typeof note.analysisFrameStart).toBe('number');
        expect(typeof note.analysisFrameEnd).toBe('number');
        expect(Number.isInteger(note.analysisFrameStart)).toBe(true);
        expect(note.analysisFrameEnd).toBeGreaterThan(note.analysisFrameStart);
      }
    } finally {
      session.destroy();
    }
  });

  it('returns the cacheHitUnits, dryPassedFrames and limitedCorrectionFrames as numbers', () => {
    const session = createVocalEditSession({ samples: tone(512), sampleRate: SAMPLE_RATE });
    try {
      const snapshot = session.captureRenderSnapshot();
      try {
        const result = snapshot.render({});
        for (const value of [
          result.cacheHitUnits,
          result.dryPassedFrames,
          result.limitedCorrectionFrames,
        ]) {
          expect(typeof value).toBe('number');
          expect(Number.isSafeInteger(value)).toBe(true);
          expect(value).toBeGreaterThanOrEqual(0);
        }
      } finally {
        snapshot.destroy();
      }
    } finally {
      session.destroy();
    }
  });

  it('keeps the handle constructors out of the public type', () => {
    // Never called: the type check is the assertion, the handles would be refused at runtime.
    const constructors = () => [
      // @ts-expect-error the constructor is protected; sessions come from the factories
      new VocalEditSession(0, SAMPLE_RATE),
      // @ts-expect-error the constructor is protected; drafts come from beginEdit
      new VocalEditDraft(0, undefined as unknown as VocalEditSession, SAMPLE_RATE),
      // @ts-expect-error the constructor is protected; snapshots come from captureRenderSnapshot
      new VocalRenderSnapshot(0, SAMPLE_RATE),
      // @ts-expect-error the constructor is protected; jobs come from beginRenderJob
      new VocalRenderJob(0),
    ];
    expect(typeof constructors).toBe('function');
  });

  it('polls a synchronous cancel hook on render', () => {
    const session = createVocalEditSession({ samples: tone(512), sampleRate: SAMPLE_RATE });
    try {
      const snapshot = session.captureRenderSnapshot();
      try {
        const range = { startSample: 0, endSample: 256 };
        let polls = 0;
        const hooked = snapshot.render({
          range,
          cancel: () => {
            polls += 1;
            return false;
          },
        });
        expect(polls).toBeGreaterThan(0);
        expect(hooked.samples).toEqual(snapshot.render({ range }).samples);
        expect(() => snapshot.render({ range, cancel: () => true })).toThrow(
          expect.objectContaining({ code: ErrorCode.Cancelled }),
        );
        expect(() => snapshot.render({ range, cancel: true as unknown as () => boolean })).toThrow(
          TypeError,
        );
      } finally {
        snapshot.destroy();
      }
    } finally {
      session.destroy();
    }
  });

  it('refuses an aborted signal on renderAsync as a Cancelled SonareError', async () => {
    const session = createVocalEditSession({ samples: tone(512), sampleRate: SAMPLE_RATE });
    try {
      const snapshot = session.captureRenderSnapshot();
      try {
        const signal = AbortSignal.abort();
        const cancelled = (error: unknown) =>
          isSonareError(error) && error.code === ErrorCode.Cancelled;
        const rejection = await snapshot.renderAsync({ signal }).then(
          () => undefined,
          (error: unknown) => error,
        );
        expect(cancelled(rejection)).toBe(true);
        expect((rejection as Error).name).not.toBe('AbortError');
      } finally {
        snapshot.destroy();
      }
    } finally {
      session.destroy();
    }
  });
});
