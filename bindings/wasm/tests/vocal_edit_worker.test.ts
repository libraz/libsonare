import { describe, expect, it } from 'vitest';
import type {
  VocalAnalysis,
  VocalCapabilities,
  VocalCreateRequest,
  VocalNotesResult,
  VocalPitchEvaluation,
  VocalRenderResult,
  VocalStateToken,
} from '../src/public_types_vocal_edit';
import {
  installVocalEditWorkerEndpoint,
  type VocalEditWorkerDependencies,
  type VocalEditWorkerNativeDraft,
  type VocalEditWorkerNativeSession,
} from '../src/vocal_edit_worker';
import type {
  VocalWorkerMutation,
  VocalWorkerRequestMessage,
  VocalWorkerResponseMessage,
} from '../src/vocal_edit_worker_protocol';

function token(revision = '0'): VocalStateToken {
  return {
    sessionEpoch: '17',
    revision,
    draftId: '0',
    generation: '0',
    requestId: '0',
    profileId: 1,
  };
}

const notes: VocalNotesResult = { notes: [], transitions: [] };
const analysis: VocalAnalysis = {
  frameOriginSample: 0,
  samplesPerFrame: 512,
  frameLengthSamples: 2048,
  f0Hz: new Float32Array([220]),
  voiced: new Uint8Array([1]),
  algorithmId: 'test',
  algorithmVersion: 1,
  sourceLengthSamples: 512,
  sampleRate: 16000,
  sourceSha256: '0'.repeat(64),
  analysisSha256: '1'.repeat(64),
  fminHz: 65,
  fmaxHz: 2093,
  yinThreshold: 0.1,
  voicedThreshold: 0.5,
  centered: true,
  segmentationThresholdCents: 50,
  minNoteMs: 30,
  referenceHz: 440,
};
const capabilities: VocalCapabilities = {
  apiVersion: 1,
  profileId: 1,
  monophonicOnly: true,
  analysisCancellable: false,
  minimumFormantShiftSemitones: -10.3,
  maximumFormantShiftSemitones: 8.7,
};

/** Read-only native operations whose values identify the scope they ran against. */
function readOnlyOps(scope: 'session' | 'draft') {
  const offset = scope === 'draft' ? 0.5 : 0;
  return {
    evaluatePitch: (noteId: number): VocalPitchEvaluation => ({
      sourceSamples: new Float64Array([noteId, offset]),
      measuredMidi: new Float64Array([60]),
      targetMidi: new Float64Array([62]),
      effectiveMidi: new Float64Array([61]),
      voiced: new Uint8Array([1]),
      hasTarget: new Uint8Array([1]),
    }),
    sourceSampleToDestinationSample: (noteId: number, sample: number) =>
      sample * 2 + noteId + offset,
    destinationSampleToSourceSample: (noteId: number, sample: number) =>
      (sample - noteId - offset) / 2,
  };
}

const sessionReadOnlyOps = {
  ...readOnlyOps('session'),
  outputLengthSamples: () => 480,
  history: () => ({ canUndo: true, canRedo: false }),
};

class Endpoint {
  listener: ((event: MessageEvent<VocalWorkerRequestMessage>) => void) | undefined;
  readonly responses: VocalWorkerResponseMessage[] = [];
  addEventListener(
    _type: 'message',
    listener: (event: MessageEvent<VocalWorkerRequestMessage>) => void,
  ): void {
    this.listener = listener;
  }
  postMessage(message: VocalWorkerResponseMessage): void {
    this.responses.push(structuredClone(message));
  }
  dispatch(message: VocalWorkerRequestMessage): void {
    this.listener?.({ data: message } as MessageEvent<VocalWorkerRequestMessage>);
  }
  take(id: number): VocalWorkerResponseMessage | undefined {
    const index = this.responses.findIndex((response) => response.id === id);
    if (index < 0) {
      return undefined;
    }
    return this.responses.splice(index, 1)[0];
  }
  takeFinal(id: number): VocalWorkerResponseMessage | undefined {
    const index = this.responses.findIndex(
      (response) => response.id === id && response.type !== 'sonare:vocal-progress',
    );
    if (index < 0) {
      return undefined;
    }
    return this.responses.splice(index, 1)[0];
  }
}

function fakeFactory(renderUnits = 1): {
  dependencies: VocalEditWorkerDependencies;
  renderNextCalls: () => number;
} {
  const revision = 0;
  let renderNextCalls = 0;
  const factory = {
    create: (_request: VocalCreateRequest): VocalEditWorkerNativeSession => {
      const sessionToken = (): VocalStateToken => token(String(revision));
      const renderSnapshot = () => {
        let calls = 0;
        return {
          beginRenderJob: () => ({
            next: () => {
              renderNextCalls += 1;
              calls += 1;
              return { complete: calls >= renderUnits };
            },
            finalize: (): VocalRenderResult => ({
              samples: new Float32Array([0.25]),
              startSample: 0,
              token: sessionToken(),
              processedRanges: [{ startSample: 0, endSample: 1 }],
              cacheHitUnits: '0',
              dryPassedFrames: '0',
              limitedCorrectionFrames: '0',
            }),
            abort: () => {},
            dispose: () => {},
          }),
          dispose: () => {},
        };
      };
      const draft: VocalEditWorkerNativeDraft = {
        token: sessionToken,
        notes: () => notes,
        ...readOnlyOps('draft'),
        apply: () => ({ token: sessionToken(), dirtyRanges: [], idChanges: [] }),
        commit: () => ({ token: sessionToken(), dirtyRanges: [], idChanges: [] }),
        cancel: () => {},
        captureRenderSnapshot: renderSnapshot as never,
        dispose: () => {},
      };
      return {
        token: sessionToken,
        notes: () => notes,
        analysis: () => analysis,
        capabilities: () => capabilities,
        ...sessionReadOnlyOps,
        beginEdit: () => draft,
        undo: () => ({ token: sessionToken(), dirtyRanges: [], idChanges: [] }),
        redo: () => ({ token: sessionToken(), dirtyRanges: [], idChanges: [] }),
        captureRenderSnapshot: renderSnapshot as never,
        exportState: () => new Uint8Array([1, 2, 3]),
        dispose: () => {},
      };
    },
    restore: () => {
      throw new Error('restore is outside this protocol test');
    },
  };
  return {
    dependencies: { factory, initialize: async () => {} },
    renderNextCalls: () => renderNextCalls,
  };
}

const createMessage = (id: number, sequence: string): VocalWorkerRequestMessage => ({
  type: 'sonare:vocal-create',
  id,
  sessionId: 's',
  clientIntentSequence: sequence,
  request: { kind: 'create', request: { samples: new Float32Array([0.1]), sampleRate: 16000 } },
});

describe('vocal edit Worker protocol', () => {
  it('keeps mutations FIFO and rejects an out-of-order client intent', async () => {
    const endpoint = new Endpoint();
    const fake = fakeFactory();
    installVocalEditWorkerEndpoint(endpoint, fake.dependencies);
    endpoint.dispatch(createMessage(1, '1'));
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(endpoint.take(1)?.type).toBe('sonare:vocal-result');

    endpoint.dispatch({
      type: 'sonare:vocal-mutate',
      id: 2,
      sessionId: 's',
      clientIntentSequence: '3',
      mutation: { kind: 'notes' },
    });
    endpoint.dispatch({
      type: 'sonare:vocal-mutate',
      id: 3,
      sessionId: 's',
      clientIntentSequence: '2',
      mutation: { kind: 'notes' },
    });
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(endpoint.take(3)?.type).toBe('sonare:vocal-error');
    expect(endpoint.take(2)?.type).toBe('sonare:vocal-result');
  });

  it('coalesces only previews that have not started and yields between render units', async () => {
    const endpoint = new Endpoint();
    const fake = fakeFactory(2);
    installVocalEditWorkerEndpoint(endpoint, fake.dependencies);
    endpoint.dispatch(createMessage(1, '1'));
    await new Promise((resolve) => setTimeout(resolve, 0));
    endpoint.take(1);

    endpoint.dispatch({
      type: 'sonare:vocal-preview',
      id: 4,
      sessionId: 's',
      clientIntentSequence: '2',
      request: { range: { startSample: 0, endSample: 1 } },
    });
    endpoint.dispatch({
      type: 'sonare:vocal-preview',
      id: 5,
      sessionId: 's',
      clientIntentSequence: '3',
      request: { range: { startSample: 0, endSample: 1 } },
    });
    endpoint.dispatch({
      type: 'sonare:vocal-preview',
      id: 6,
      sessionId: 's',
      clientIntentSequence: '4',
      request: { range: { startSample: 0, endSample: 1 } },
    });
    await new Promise((resolve) => setTimeout(resolve, 10));
    expect(endpoint.takeFinal(4)?.type).toBe('sonare:vocal-result');
    expect(endpoint.takeFinal(5)?.type).toBe('sonare:vocal-error');
    expect(endpoint.takeFinal(6)?.type).toBe('sonare:vocal-result');
    expect(fake.renderNextCalls()).toBe(4);
  });

  it('cancels an active preview between job_next calls without publishing a result', async () => {
    const endpoint = new Endpoint();
    const fake = fakeFactory(3);
    installVocalEditWorkerEndpoint(endpoint, fake.dependencies);
    endpoint.dispatch(createMessage(1, '1'));
    await new Promise((resolve) => setTimeout(resolve, 0));
    endpoint.take(1);
    endpoint.dispatch({
      type: 'sonare:vocal-preview',
      id: 6,
      sessionId: 's',
      clientIntentSequence: '2',
      request: { range: { startSample: 0, endSample: 1 } },
    });
    await new Promise((resolve) => setTimeout(resolve, 0));
    endpoint.dispatch({
      type: 'sonare:vocal-cancel',
      id: 6,
      sessionId: 's',
      clientIntentSequence: '2',
    });
    await new Promise((resolve) => setTimeout(resolve, 10));
    const response = endpoint.takeFinal(6);
    expect(response?.type).toBe('sonare:vocal-error');
    if (response?.type === 'sonare:vocal-error') {
      expect(response.error.name).toBe('AbortError');
    }
  });

  it('cancels a preview that is waiting behind an active render', async () => {
    const endpoint = new Endpoint();
    const fake = fakeFactory(3);
    installVocalEditWorkerEndpoint(endpoint, fake.dependencies);
    endpoint.dispatch(createMessage(1, '1'));
    await new Promise((resolve) => setTimeout(resolve, 0));
    endpoint.take(1);

    endpoint.dispatch({
      type: 'sonare:vocal-preview',
      id: 7,
      sessionId: 's',
      clientIntentSequence: '2',
      request: { range: { startSample: 0, endSample: 1 } },
    });
    await new Promise((resolve) => setTimeout(resolve, 0));
    endpoint.dispatch({
      type: 'sonare:vocal-preview',
      id: 8,
      sessionId: 's',
      clientIntentSequence: '3',
      request: { range: { startSample: 0, endSample: 1 } },
    });
    endpoint.dispatch({
      type: 'sonare:vocal-cancel',
      id: 8,
      sessionId: 's',
      clientIntentSequence: '3',
    });
    await new Promise((resolve) => setTimeout(resolve, 10));

    const response = endpoint.takeFinal(8);
    expect(response?.type).toBe('sonare:vocal-error');
    if (response?.type === 'sonare:vocal-error') {
      expect(response.error.name).toBe('AbortError');
    }
    expect(endpoint.takeFinal(7)?.type).toBe('sonare:vocal-result');
    expect(fake.renderNextCalls()).toBe(3);
  });

  it('rejects queued work before disposing a session with an active render', async () => {
    const endpoint = new Endpoint();
    const fake = fakeFactory(3);
    installVocalEditWorkerEndpoint(endpoint, fake.dependencies);
    endpoint.dispatch(createMessage(1, '1'));
    await new Promise((resolve) => setTimeout(resolve, 0));
    endpoint.take(1);

    endpoint.dispatch({
      type: 'sonare:vocal-preview',
      id: 9,
      sessionId: 's',
      clientIntentSequence: '2',
      request: { range: { startSample: 0, endSample: 1 } },
    });
    await new Promise((resolve) => setTimeout(resolve, 0));
    endpoint.dispatch({
      type: 'sonare:vocal-mutate',
      id: 10,
      sessionId: 's',
      clientIntentSequence: '3',
      mutation: { kind: 'notes' },
    });
    endpoint.dispatch({
      type: 'sonare:vocal-dispose',
      id: 11,
      sessionId: 's',
      clientIntentSequence: '4',
    });
    await new Promise((resolve) => setTimeout(resolve, 10));

    const queued = endpoint.takeFinal(10);
    expect(queued?.type).toBe('sonare:vocal-error');
    if (queued?.type === 'sonare:vocal-error') {
      expect(queued.error.message).toContain('session disposed');
    }
    expect(endpoint.takeFinal(9)?.type).toBe('sonare:vocal-error');
    expect(endpoint.takeFinal(11)?.type).toBe('sonare:vocal-result');
  });
});

interface StatefulFake {
  dependencies: VocalEditWorkerDependencies;
  disposedSessions: () => number;
  commits: () => number;
}

function statefulFactory(
  options: { renderUnits?: number; initialize?: () => Promise<void> } = {},
): StatefulFake {
  let disposedSessions = 0;
  let commits = 0;
  const factory = {
    create: (_request: VocalCreateRequest): VocalEditWorkerNativeSession => {
      let revision = 0;
      let generation = 0;
      let draftOpen = false;
      const stateToken = (): VocalStateToken => ({
        ...token(String(revision)),
        draftId: draftOpen ? '1' : '0',
        generation: String(generation),
      });
      const editResult = () => ({ token: stateToken(), dirtyRanges: [], idChanges: [] });
      const renderSnapshot = () => {
        let calls = 0;
        const captured = stateToken();
        return {
          beginRenderJob: () => ({
            next: () => {
              calls += 1;
              return { complete: calls >= (options.renderUnits ?? 1) };
            },
            finalize: (): VocalRenderResult => ({
              samples: new Float32Array([0.25]),
              startSample: 0,
              token: { ...captured, requestId: '5' },
              processedRanges: [],
              cacheHitUnits: '0',
              dryPassedFrames: '0',
              limitedCorrectionFrames: '0',
            }),
            abort: () => {},
            dispose: () => {},
          }),
          dispose: () => {},
        };
      };
      const draft: VocalEditWorkerNativeDraft = {
        token: stateToken,
        notes: () => notes,
        ...readOnlyOps('draft'),
        apply: (request) => {
          if (request.expectedGeneration !== String(generation)) {
            throw Object.assign(new Error('generation conflict'), { field: 'generation' });
          }
          generation += 1;
          return editResult();
        },
        commit: () => {
          commits += 1;
          revision += 1;
          generation = 0;
          draftOpen = false;
          return editResult();
        },
        cancel: () => {
          generation = 0;
          draftOpen = false;
        },
        captureRenderSnapshot: renderSnapshot as never,
        dispose: () => {},
      };
      return {
        token: stateToken,
        notes: () => notes,
        analysis: () => analysis,
        capabilities: () => capabilities,
        ...sessionReadOnlyOps,
        beginEdit: () => {
          draftOpen = true;
          return draft;
        },
        undo: editResult,
        redo: editResult,
        captureRenderSnapshot: renderSnapshot as never,
        exportState: () => new Uint8Array([1, 2, 3]),
        dispose: () => {
          disposedSessions += 1;
        },
      };
    },
    restore: () => {
      throw new Error('restore is outside this protocol test');
    },
  };
  return {
    dependencies: { factory, initialize: options.initialize ?? (async () => {}) },
    disposedSessions: () => disposedSessions,
    commits: () => commits,
  };
}

const settle = (ms = 0) => new Promise((resolve) => setTimeout(resolve, ms));

describe('vocal edit Worker mutation semantics', () => {
  it('applies queued mutations whose base token predates an earlier queued mutation', async () => {
    const endpoint = new Endpoint();
    installVocalEditWorkerEndpoint(endpoint, statefulFactory().dependencies);
    endpoint.dispatch(createMessage(1, '1'));
    await settle();
    endpoint.take(1);
    const base = token('0');
    endpoint.dispatch({
      type: 'sonare:vocal-mutate',
      id: 2,
      sessionId: 's',
      clientIntentSequence: '2',
      baseToken: base,
      mutation: { kind: 'beginEdit', expectedRevision: '0' },
    });
    for (const [id, generation] of [
      [3, '0'],
      [4, '1'],
    ] as const) {
      endpoint.dispatch({
        type: 'sonare:vocal-mutate',
        id,
        sessionId: 's',
        clientIntentSequence: String(id),
        baseToken: base,
        mutation: { kind: 'apply', request: { expectedGeneration: generation, operations: [] } },
      });
    }
    endpoint.dispatch({
      type: 'sonare:vocal-preview',
      id: 5,
      sessionId: 's',
      clientIntentSequence: '5',
      baseToken: base,
      request: { range: { startSample: 0, endSample: 1 } },
    });
    await settle(10);
    expect(endpoint.takeFinal(2)?.type).toBe('sonare:vocal-result');
    expect(endpoint.takeFinal(3)?.type).toBe('sonare:vocal-result');
    const second = endpoint.takeFinal(4);
    expect(second?.type).toBe('sonare:vocal-result');
    expect(second?.token?.generation).toBe('2');
    const preview = endpoint.takeFinal(5);
    expect(preview?.type).toBe('sonare:vocal-result');
    expect(preview?.token?.generation).toBe('2');
  });

  it('never reports a completed commit as cancelled', async () => {
    const endpoint = new Endpoint();
    const fake = statefulFactory({ renderUnits: 3 });
    installVocalEditWorkerEndpoint(endpoint, fake.dependencies);
    endpoint.dispatch(createMessage(1, '1'));
    await settle();
    endpoint.take(1);
    endpoint.dispatch({
      type: 'sonare:vocal-mutate',
      id: 2,
      sessionId: 's',
      clientIntentSequence: '2',
      mutation: { kind: 'beginEdit', expectedRevision: '0' },
    });
    await settle();
    endpoint.take(2);

    // A shared cancel flag already raised when the commit starts must not
    // turn the committed result into an AbortError.
    const raised = new SharedArrayBuffer(Int32Array.BYTES_PER_ELEMENT);
    Atomics.store(new Int32Array(raised), 0, 1);
    endpoint.dispatch({
      type: 'sonare:vocal-mutate',
      id: 3,
      sessionId: 's',
      clientIntentSequence: '3',
      mutation: { kind: 'commit', expectedRevision: '0' },
      cancelBuffer: raised,
    });
    await settle();
    const committed = endpoint.takeFinal(3);
    expect(committed?.type).toBe('sonare:vocal-result');
    expect(committed?.token?.revision).toBe('1');

    // A cancel message for a mutation queued behind a render is a no-op.
    endpoint.dispatch({
      type: 'sonare:vocal-preview',
      id: 4,
      sessionId: 's',
      clientIntentSequence: '4',
      request: { range: { startSample: 0, endSample: 1 } },
    });
    endpoint.dispatch({
      type: 'sonare:vocal-mutate',
      id: 5,
      sessionId: 's',
      clientIntentSequence: '5',
      mutation: { kind: 'beginEdit', expectedRevision: '1' },
    });
    endpoint.dispatch({
      type: 'sonare:vocal-cancel',
      id: 5,
      sessionId: 's',
      clientIntentSequence: '5',
    });
    await settle(10);
    expect(endpoint.takeFinal(4)?.type).toBe('sonare:vocal-result');
    expect(endpoint.takeFinal(5)?.type).toBe('sonare:vocal-result');
    expect(fake.commits()).toBe(1);
  });

  it('rejects a cancelled create with AbortError and disposes the native session', async () => {
    const endpoint = new Endpoint();
    const fake = statefulFactory({ initialize: () => settle() as Promise<void> });
    const raised = new SharedArrayBuffer(Int32Array.BYTES_PER_ELEMENT);
    const factory = fake.dependencies.factory as NonNullable<
      VocalEditWorkerDependencies['factory']
    >;
    let raiseDuringCreate = false;
    installVocalEditWorkerEndpoint(endpoint, {
      ...fake.dependencies,
      factory: {
        ...factory,
        create: (request) => {
          const created = factory.create(request);
          // The client aborts while the synchronous native create is running.
          if (raiseDuringCreate) {
            Atomics.store(new Int32Array(raised), 0, 1);
          }
          return created;
        },
      },
    });
    endpoint.dispatch(createMessage(1, '1'));
    endpoint.dispatch({
      type: 'sonare:vocal-cancel',
      id: 1,
      sessionId: 's',
      clientIntentSequence: '1',
    });
    await settle(10);
    const response = endpoint.takeFinal(1);
    expect(response?.type).toBe('sonare:vocal-error');
    if (response?.type === 'sonare:vocal-error') {
      expect(response.error.name).toBe('AbortError');
    }

    expect(fake.disposedSessions()).toBe(0);

    raiseDuringCreate = true;
    endpoint.dispatch({
      ...createMessage(2, '1'),
      cancelBuffer: raised,
    } as VocalWorkerRequestMessage);
    await settle(10);
    expect(endpoint.takeFinal(2)?.type).toBe('sonare:vocal-error');
    expect(fake.disposedSessions()).toBe(1);
    raiseDuringCreate = false;

    // The session id stays free for a later create.
    endpoint.dispatch(createMessage(3, '1'));
    await settle(10);
    expect(endpoint.takeFinal(3)?.type).toBe('sonare:vocal-result');
  });

  it('replies to a dispose of an unknown session', async () => {
    const endpoint = new Endpoint();
    installVocalEditWorkerEndpoint(endpoint, statefulFactory().dependencies);
    endpoint.dispatch({
      type: 'sonare:vocal-dispose',
      id: 9,
      sessionId: 'missing',
      clientIntentSequence: '1',
    });
    await settle();
    const response = endpoint.takeFinal(9);
    expect(response?.type).toBe('sonare:vocal-result');
  });

  it('answers malformed messages with an error when they carry an id and never throws', async () => {
    const endpoint = new Endpoint();
    installVocalEditWorkerEndpoint(endpoint, statefulFactory().dependencies);
    endpoint.dispatch(createMessage(1, '1'));
    await settle();
    endpoint.take(1);
    const malformed: unknown[] = [
      null,
      42,
      { type: 'sonare:vocal-mutate' },
      { type: 'sonare:vocal-mutate', id: 20, sessionId: 3, clientIntentSequence: '2' },
      {
        type: 'sonare:vocal-mutate',
        id: 21,
        sessionId: 's',
        clientIntentSequence: 7,
        mutation: { kind: 'notes' },
      },
      { type: 'sonare:vocal-mutate', id: 22, sessionId: 's', clientIntentSequence: '2' },
      {
        type: 'sonare:vocal-mutate',
        id: 23,
        sessionId: 's',
        clientIntentSequence: '2',
        mutation: { kind: 'explode' },
      },
      { type: 'sonare:vocal-create', id: 24, sessionId: 't', clientIntentSequence: '1' },
      { type: 'sonare:vocal-unknown', id: 25, sessionId: 's', clientIntentSequence: '2' },
      {
        type: 'sonare:vocal-mutate',
        id: 27,
        sessionId: 's',
        clientIntentSequence: '02',
        mutation: { kind: 'notes' },
      },
      { type: 'sonare:vocal-cancel', id: 'x', sessionId: 's', clientIntentSequence: '2' },
    ];
    for (const message of malformed) {
      expect(() => endpoint.dispatch(message as VocalWorkerRequestMessage)).not.toThrow();
    }
    await settle(10);
    for (const id of [20, 21, 22, 23, 24, 25, 27]) {
      expect(endpoint.takeFinal(id)?.type, `id ${id}`).toBe('sonare:vocal-error');
    }
    expect(endpoint.responses).toEqual([]);

    // The session still accepts well-formed work afterwards.
    endpoint.dispatch({
      type: 'sonare:vocal-mutate',
      id: 26,
      sessionId: 's',
      clientIntentSequence: '2',
      mutation: { kind: 'notes' },
    });
    await settle();
    expect(endpoint.takeFinal(26)?.type).toBe('sonare:vocal-result');
  });

  it('answers every read-only command against the session or the open draft', async () => {
    const endpoint = new Endpoint();
    installVocalEditWorkerEndpoint(endpoint, statefulFactory().dependencies);
    endpoint.dispatch(createMessage(1, '1'));
    await settle();
    endpoint.take(1);
    let sequence = 1;
    const read = async (id: number, mutation: VocalWorkerMutation) => {
      sequence += 1;
      endpoint.dispatch({
        type: 'sonare:vocal-mutate',
        id,
        sessionId: 's',
        clientIntentSequence: String(sequence),
        mutation,
      });
      await settle();
      return endpoint.takeFinal(id);
    };
    const result = async (id: number, mutation: VocalWorkerMutation) => {
      const response = await read(id, mutation);
      expect(response?.type, `id ${id}`).toBe('sonare:vocal-result');
      return response?.type === 'sonare:vocal-result' ? response.result : undefined;
    };

    expect(await result(2, { kind: 'outputLength' })).toBe(480);
    expect(await result(3, { kind: 'history' })).toEqual({ canUndo: true, canRedo: false });
    expect(await result(4, { kind: 'evaluatePitch', noteId: 3 })).toMatchObject({
      sourceSamples: new Float64Array([3, 0]),
    });
    expect(await result(5, { kind: 'mapCoordinate', noteId: 1, sample: 10, inverse: false })).toBe(
      21,
    );
    expect(await result(6, { kind: 'mapCoordinate', noteId: 1, sample: 21, inverse: true })).toBe(
      10,
    );

    // Without a draft, draft-scoped reads and the draft token are refused.
    for (const [id, mutation] of [
      [7, { kind: 'draftToken' }],
      [8, { kind: 'evaluatePitch', noteId: 3, draft: true }],
      [9, { kind: 'mapCoordinate', noteId: 1, sample: 10, inverse: false, draft: true }],
    ] as const) {
      const response = await read(id, mutation);
      expect(response?.type, `id ${id}`).toBe('sonare:vocal-error');
    }

    await result(10, { kind: 'beginEdit', expectedRevision: '0' });
    expect(await result(11, { kind: 'draftToken' })).toMatchObject({ draftId: '1' });
    expect(await result(12, { kind: 'evaluatePitch', noteId: 3, draft: true })).toMatchObject({
      sourceSamples: new Float64Array([3, 0.5]),
    });
    expect(
      await result(13, {
        kind: 'mapCoordinate',
        noteId: 1,
        sample: 10,
        inverse: false,
        draft: true,
      }),
    ).toBe(21.5);
    expect(
      await result(14, {
        kind: 'mapCoordinate',
        noteId: 1,
        sample: 21.5,
        inverse: true,
        draft: true,
      }),
    ).toBe(10);
    // The default scope stays the committed session while a draft is open.
    expect(await result(15, { kind: 'evaluatePitch', noteId: 3 })).toMatchObject({
      sourceSamples: new Float64Array([3, 0]),
    });
  });

  it('rejects everything that arrives after a dispose while a render is running', async () => {
    const endpoint = new Endpoint();
    const fake = statefulFactory({ renderUnits: 1000 });
    installVocalEditWorkerEndpoint(endpoint, fake.dependencies);
    endpoint.dispatch(createMessage(1, '1'));
    await settle();
    endpoint.take(1);
    endpoint.dispatch({
      type: 'sonare:vocal-preview',
      id: 2,
      sessionId: 's',
      clientIntentSequence: '2',
      request: { range: { startSample: 0, endSample: 1 } },
    });
    await settle();
    endpoint.dispatch({
      type: 'sonare:vocal-dispose',
      id: 3,
      sessionId: 's',
      clientIntentSequence: '3',
    });
    endpoint.dispatch({
      type: 'sonare:vocal-preview',
      id: 4,
      sessionId: 's',
      clientIntentSequence: '4',
      request: { range: { startSample: 0, endSample: 1 } },
    });
    endpoint.dispatch({
      type: 'sonare:vocal-mutate',
      id: 5,
      sessionId: 's',
      clientIntentSequence: '5',
      mutation: { kind: 'beginEdit', expectedRevision: '0' },
    });
    endpoint.dispatch({
      type: 'sonare:vocal-dispose',
      id: 6,
      sessionId: 's',
      clientIntentSequence: '6',
    });
    await settle(20);

    expect(endpoint.takeFinal(2)?.type).toBe('sonare:vocal-error');
    for (const id of [4, 5]) {
      const late = endpoint.takeFinal(id);
      expect(late?.type, `id ${id}`).toBe('sonare:vocal-error');
      if (late?.type === 'sonare:vocal-error') {
        expect(late.error.message).toBe('Unknown vocal session');
      }
    }
    expect(endpoint.takeFinal(3)?.type).toBe('sonare:vocal-result');
    expect(endpoint.takeFinal(6)?.type).toBe('sonare:vocal-result');
    expect(fake.disposedSessions()).toBe(1);
  });

  it('carries every field of a structured native error', async () => {
    const endpoint = new Endpoint();
    const base = statefulFactory();
    const factory = base.dependencies.factory as NonNullable<
      VocalEditWorkerDependencies['factory']
    >;
    const failure = Object.assign(new Error('revision conflict'), {
      name: 'SonareError',
      code: 5,
      codeName: 'InvalidState',
      reason: 2,
      field: 'revision',
      expected: '0',
      actual: '1',
      expectedText: 'revision 0',
      actualText: 'revision 1',
    });
    let armed = false;
    installVocalEditWorkerEndpoint(endpoint, {
      ...base.dependencies,
      factory: {
        ...factory,
        create: (request) => ({
          ...factory.create(request),
          // Armed after create, which itself reads notes().
          notes: () => {
            if (armed) {
              throw failure;
            }
            return notes;
          },
        }),
      },
    });
    endpoint.dispatch(createMessage(1, '1'));
    await settle();
    endpoint.take(1);
    armed = true;
    endpoint.dispatch({
      type: 'sonare:vocal-mutate',
      id: 2,
      sessionId: 's',
      clientIntentSequence: '2',
      mutation: { kind: 'notes' },
    });
    await settle();
    const response = endpoint.takeFinal(2);
    expect(response?.type).toBe('sonare:vocal-error');
    if (response?.type === 'sonare:vocal-error') {
      expect(response.error).toEqual({
        name: 'SonareError',
        message: 'revision conflict',
        code: 5,
        codeName: 'InvalidState',
        reason: 2,
        field: 'revision',
        expected: '0',
        actual: '1',
        expectedText: 'revision 0',
        actualText: 'revision 1',
      });
    }
  });
});
