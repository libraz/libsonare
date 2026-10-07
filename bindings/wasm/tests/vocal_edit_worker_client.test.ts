import { describe, expect, it } from 'vitest';
import { isSonareError } from '../src/errors';
import type {
  VocalAnalysis,
  VocalCapabilities,
  VocalEditOperation,
  VocalNotesResult,
  VocalPitchEvaluation,
  VocalRenderResult,
  VocalStateToken,
} from '../src/public_types_vocal_edit';
import {
  installVocalEditWorkerEndpoint,
  type VocalEditWorkerNativeDraft,
  type VocalEditWorkerNativeSession,
} from '../src/vocal_edit_worker';
import { type VocalEditWorker, VocalEditWorkerClient } from '../src/vocal_edit_worker_client';
import type {
  VocalWorkerCreateResult,
  VocalWorkerRequestMessage,
  VocalWorkerResponseMessage,
} from '../src/vocal_edit_worker_protocol';

const token: VocalStateToken = {
  sessionEpoch: '7',
  revision: '0',
  draftId: '0',
  generation: '0',
  requestId: '0',
  profileId: 1,
};

const notes: VocalNotesResult = { notes: [], transitions: [] };
const analysis: VocalAnalysis = {
  frameOriginSample: 0,
  samplesPerFrame: 512,
  frameLengthSamples: 2048,
  f0Hz: new Float32Array(),
  voiced: new Uint8Array(),
  algorithmId: 'test',
  algorithmVersion: 1,
  sourceLengthSamples: 1,
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

class FakeWorker implements VocalEditWorker {
  private listener: ((event: MessageEvent<VocalWorkerResponseMessage>) => void) | undefined;
  private errorListener: ((event: ErrorEvent) => void) | undefined;
  constructor(private readonly stalePreview = false) {}

  lastMutation: VocalWorkerRequestMessage | undefined;
  readonly posted: VocalWorkerRequestMessage[] = [];

  addEventListener(type: string, listener: EventListener): void {
    if (type === 'message') {
      this.listener = listener as (event: MessageEvent<VocalWorkerResponseMessage>) => void;
    } else if (type === 'error') {
      this.errorListener = listener as (event: ErrorEvent) => void;
    }
  }

  removeEventListener(type: string): void {
    if (type === 'message') {
      this.listener = undefined;
    } else if (type === 'error') {
      this.errorListener = undefined;
    }
  }

  emitError(message: string): void {
    this.errorListener?.({ message } as ErrorEvent);
  }

  postMessage(message: VocalWorkerRequestMessage): void {
    this.posted.push(message);
    let response: VocalWorkerResponseMessage;
    if (message.type === 'sonare:vocal-create') {
      const result: VocalWorkerCreateResult = {
        sessionId: message.sessionId,
        token,
        notes,
        analysis,
        capabilities,
      };
      response = {
        type: 'sonare:vocal-result',
        id: message.id,
        sessionId: message.sessionId,
        clientIntentSequence: message.clientIntentSequence,
        token,
        result,
      };
    } else if (message.type === 'sonare:vocal-preview') {
      const renderToken = {
        ...token,
        revision: this.stalePreview ? '1' : token.revision,
        requestId: '9',
      };
      response = {
        type: 'sonare:vocal-result',
        id: message.id,
        sessionId: message.sessionId,
        clientIntentSequence: message.clientIntentSequence,
        token: renderToken,
        result: {
          samples: new Float32Array([0]),
          startSample: 0,
          token: renderToken,
          processedRanges: [],
          cacheHitUnits: '0',
          dryPassedFrames: '0',
          limitedCorrectionFrames: '0',
        },
      };
    } else if (message.type === 'sonare:vocal-mutate') {
      this.lastMutation = message;
      const nextToken = { ...token, revision: '1' };
      response = {
        type: 'sonare:vocal-result',
        id: message.id,
        sessionId: message.sessionId,
        clientIntentSequence: message.clientIntentSequence,
        token: nextToken,
        result: nextToken,
      };
    } else {
      response = {
        type: 'sonare:vocal-result',
        id: message.id,
        sessionId: message.sessionId,
        clientIntentSequence: message.clientIntentSequence,
        result: null,
      };
    }
    queueMicrotask(() =>
      this.listener?.({ data: response } as MessageEvent<VocalWorkerResponseMessage>),
    );
  }

  terminate(): void {}
}

describe('vocal edit Worker client', () => {
  it('keeps intent counters increasing across decimal digit boundaries', async () => {
    const worker = new FakeWorker();
    const client = new VocalEditWorkerClient({ worker });
    const session = await client.create({ samples: new Float32Array([0]), sampleRate: 16000 });
    try {
      for (let index = 0; index < 110; index++) {
        await session.undo();
        expect(worker.lastMutation?.clientIntentSequence).toBe(String(index + 2));
      }
    } finally {
      client.dispose();
    }
  });

  it('does not adopt a render request token as the mutable session token', async () => {
    const worker = new FakeWorker();
    const client = new VocalEditWorkerClient({ worker });
    const session = await client.create({
      samples: new Float32Array([0]),
      sampleRate: 16000,
    });
    await session.preview({ range: { startSample: 0, endSample: 1 }, requestId: '9' });
    await session.beginEdit({ expectedRevision: '0' });

    expect(worker.lastMutation?.type).toBe('sonare:vocal-mutate');
    if (worker.lastMutation?.type === 'sonare:vocal-mutate') {
      expect(worker.lastMutation.baseToken?.requestId).toBe('0');
    }
    await session.dispose();
    expect(() => session.notes()).toThrow(/disposed/);
    client.dispose();
  });

  it('rejects a render result whose state token is stale', async () => {
    const worker = new FakeWorker(true);
    const client = new VocalEditWorkerClient({ worker });
    const session = await client.create({
      samples: new Float32Array([0]),
      sampleRate: 16000,
    });

    await expect(
      session.preview({ range: { startSample: 0, endSample: 1 }, requestId: '9' }),
    ).rejects.toMatchObject({ name: 'StaleResultError' });
    await session.dispose();
    client.dispose();
  });

  it('removes a preview abort listener when the Worker emits an error', async () => {
    const worker = new FakeWorker();
    const client = new VocalEditWorkerClient({ worker });
    const session = await client.create({
      samples: new Float32Array([0]),
      sampleRate: 16000,
    });
    const controller = new AbortController();
    const task = session.preview({
      range: { startSample: 0, endSample: 1 },
      requestId: '9',
      signal: controller.signal,
    });

    worker.emitError('worker crashed');
    await expect(task).rejects.toThrow('worker crashed');
    controller.abort();

    expect(worker.posted.filter((message) => message.type === 'sonare:vocal-cancel')).toHaveLength(
      0,
    );
    client.dispose();
  });
});

/** Connects a client to the real Worker endpoint through macrotask delivery. */
class BridgeWorker implements VocalEditWorker {
  private clientListener: ((event: MessageEvent) => void) | undefined;
  private workerListener: ((event: MessageEvent) => void) | undefined;
  readonly posted: VocalWorkerRequestMessage[] = [];
  createdSessions = 0;
  disposedSessions = 0;
  notesError: Error | undefined;

  constructor(
    private readonly initialize: () => Promise<void> = async () => {},
    private readonly responseGate?: Promise<void>,
  ) {
    let revision = 0;
    let generation = 0;
    let draftOpen = false;
    const stateToken = (): VocalStateToken => ({
      ...token,
      revision: String(revision),
      draftId: draftOpen ? '1' : '0',
      generation: String(generation),
    });
    const editResult = () => ({ token: stateToken(), dirtyRanges: [], idChanges: [] });
    const snapshot = () => {
      const captured = stateToken();
      return {
        beginRenderJob: () => ({
          next: () => ({ complete: true }),
          finalize: (): VocalRenderResult => ({
            samples: new Float32Array([0.5]),
            startSample: 0,
            token: { ...captured, requestId: '3' },
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
    const pitch = (scope: number): VocalPitchEvaluation => ({
      sourceSamples: new Float64Array([scope]),
      measuredMidi: new Float64Array([60]),
      targetMidi: new Float64Array([62]),
      effectiveMidi: new Float64Array([61]),
      voiced: new Uint8Array([1]),
      hasTarget: new Uint8Array([1]),
    });
    const draft: VocalEditWorkerNativeDraft = {
      token: stateToken,
      notes: () => notes,
      evaluatePitch: () => pitch(1),
      sourceSampleToDestinationSample: (_noteId, sample) => sample + 100,
      destinationSampleToSourceSample: (_noteId, sample) => sample - 100,
      apply: (request) => {
        if (request.expectedGeneration !== String(generation)) {
          throw new Error(`generation conflict: ${request.expectedGeneration} != ${generation}`);
        }
        generation += 1;
        return editResult();
      },
      commit: () => {
        revision += 1;
        generation = 0;
        draftOpen = false;
        return editResult();
      },
      cancel: () => {
        draftOpen = false;
        generation = 0;
      },
      captureRenderSnapshot: snapshot as never,
      dispose: () => {},
    };
    const session: VocalEditWorkerNativeSession = {
      token: stateToken,
      notes: () => {
        if (this.notesError) {
          throw this.notesError;
        }
        return notes;
      },
      outputLengthSamples: () => 480,
      history: () => ({ canUndo: false, canRedo: true }),
      evaluatePitch: () => pitch(0),
      sourceSampleToDestinationSample: (_noteId, sample) => sample + 10,
      destinationSampleToSourceSample: (_noteId, sample) => sample - 10,
      analysis: () => analysis,
      capabilities: () => capabilities,
      beginEdit: () => {
        draftOpen = true;
        return draft;
      },
      undo: editResult,
      redo: editResult,
      captureRenderSnapshot: snapshot as never,
      exportState: () => new Uint8Array([1]),
      dispose: () => {
        this.disposedSessions += 1;
      },
    };
    installVocalEditWorkerEndpoint(
      {
        postMessage: (message) => {
          const data = structuredClone(message);
          const deliver = () => {
            setTimeout(() => this.clientListener?.({ data } as MessageEvent), 0);
          };
          if (this.responseGate === undefined) {
            deliver();
          } else {
            void this.responseGate.then(deliver);
          }
        },
        addEventListener: (_type, listener) => {
          this.workerListener = listener as (event: MessageEvent) => void;
        },
      },
      {
        factory: {
          create: () => {
            this.createdSessions += 1;
            return session;
          },
          restore: () => {
            this.createdSessions += 1;
            return session;
          },
        },
        initialize: this.initialize,
      },
    );
  }

  addEventListener(type: string, listener: EventListener): void {
    if (type === 'message') {
      this.clientListener = listener as (event: MessageEvent) => void;
    }
  }

  removeEventListener(): void {}

  postMessage(message: VocalWorkerRequestMessage, transfer?: Transferable[]): void {
    this.posted.push(message);
    const data = structuredClone(message, { transfer });
    setTimeout(() => this.workerListener?.({ data } as MessageEvent), 0);
  }

  terminate(): void {}
}

describe('vocal edit Worker client pipelining and failure paths', () => {
  it('disposes tracked native sessions when the client is disposed', async () => {
    const worker = new BridgeWorker();
    const client = new VocalEditWorkerClient({ worker });
    await client.create({ samples: new Float32Array([0]), sampleRate: 16000 });

    client.dispose();
    await new Promise((resolve) => setTimeout(resolve, 0));

    expect(worker.disposedSessions).toBe(1);
  });

  it('disposes a create that finishes while the client is being disposed', async () => {
    let releaseInitialization!: () => void;
    const initialization = new Promise<void>((resolve) => {
      releaseInitialization = resolve;
    });
    let releaseResponse!: () => void;
    const responseGate = new Promise<void>((resolve) => {
      releaseResponse = resolve;
    });
    const worker = new BridgeWorker(() => initialization, responseGate);
    const client = new VocalEditWorkerClient({ worker });
    const task = client.create({ samples: new Float32Array([0]), sampleRate: 16000 });

    // Let the endpoint enter initialize(), then release it while the response
    // remains held, leaving the native session pending here.
    await new Promise((resolve) => setTimeout(resolve, 0));
    releaseInitialization();
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(worker.createdSessions).toBe(1);
    client.dispose();

    await expect(task).rejects.toThrow(/disposed/);
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(worker.disposedSessions).toBe(1);
    releaseResponse();
    await new Promise((resolve) => setTimeout(resolve, 0));
  });

  it('cancels a create still waiting for Worker initialization', async () => {
    let releaseInitialization!: () => void;
    const initialization = new Promise<void>((resolve) => {
      releaseInitialization = resolve;
    });
    const worker = new BridgeWorker(() => initialization);
    const client = new VocalEditWorkerClient({ worker });
    const task = client.create({ samples: new Float32Array([0]), sampleRate: 16000 });

    await new Promise((resolve) => setTimeout(resolve, 0));
    client.dispose();
    releaseInitialization();

    await expect(task).rejects.toThrow(/disposed/);
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(worker.createdSessions).toBe(0);
    expect(worker.disposedSessions).toBe(0);
  });

  it('keeps the session token in sync when calls are pipelined without awaiting', async () => {
    const client = new VocalEditWorkerClient({ worker: new BridgeWorker() });
    try {
      const session = await client.create({ samples: new Float32Array([0]), sampleRate: 16000 });
      await session.beginEdit();
      const apply = session.apply({ expectedGeneration: '0', operations: [] });
      const preview = session.preview({ requestId: '3' });
      const listed = session.notes();
      const settled = await Promise.allSettled([apply.result, preview.result, listed.result]);
      expect(settled[2]).toMatchObject({ status: 'fulfilled', value: notes });
      expect(session.token().generation).toBe('1');

      const next = await session.apply({
        expectedGeneration: session.token().generation,
        operations: [],
      });
      expect(next.token.generation).toBe('2');
      const latestPreview = await session.preview({ requestId: '4' });
      expect(latestPreview.token.generation).toBe('2');
    } finally {
      client.dispose();
    }
  });

  it('settles every pipelined mutation with its own outcome', async () => {
    const client = new VocalEditWorkerClient({ worker: new BridgeWorker() });
    try {
      const session = await client.create({ samples: new Float32Array([0]), sampleRate: 16000 });
      await session.beginEdit();
      const first = session.apply({ expectedGeneration: '0', operations: [] });
      const second = session.apply({ expectedGeneration: '1', operations: [] });
      const settled = await Promise.allSettled([first.result, second.result]);
      expect(settled.map((outcome) => outcome.status)).toEqual(['fulfilled', 'fulfilled']);
      expect(session.token().generation).toBe('2');
    } finally {
      client.dispose();
    }
  });

  it('renders the post-mutation state for a preview pipelined behind an apply', async () => {
    const client = new VocalEditWorkerClient({ worker: new BridgeWorker() });
    try {
      const session = await client.create({ samples: new Float32Array([0]), sampleRate: 16000 });
      await session.beginEdit();
      const apply = session.apply({ expectedGeneration: '0', operations: [] });
      const applyOutcome = apply.result.then(
        (value) => value,
        (error: unknown) => error,
      );
      const preview = await session.preview({ requestId: '3' });
      expect(preview.token.generation).toBe('1');
      // The apply ran in the Worker, so its task settles with that outcome
      // even though the preview queued behind it is the newer intent.
      expect(await applyOutcome).toMatchObject({ token: { generation: '1' } });
      expect(session.token().generation).toBe('1');
    } finally {
      client.dispose();
    }
  });

  it('rejects and forgets a call whose postMessage throws synchronously', async () => {
    const worker = new FakeWorker();
    const client = new VocalEditWorkerClient({ worker });
    const session = await client.create({ samples: new Float32Array([0]), sampleRate: 16000 });
    const failure = new DOMException('could not clone', 'DataCloneError');
    worker.postMessage = () => {
      throw failure;
    };
    const controller = new AbortController();
    let task: PromiseLike<unknown> | undefined;
    expect(() => {
      task = client.create(
        { samples: new Float32Array([0]), sampleRate: 16000 },
        { signal: controller.signal },
      );
    }).not.toThrow();
    await expect(task).rejects.toBe(failure);
    expect(() => {
      task = session.apply({ expectedGeneration: '0', operations: [] });
    }).not.toThrow();
    await expect(task).rejects.toBe(failure);
    expect((client as unknown as { pending: Map<number, unknown> }).pending.size).toBe(0);
    // The abort listener is gone: aborting now posts nothing.
    let posted = 0;
    worker.postMessage = () => {
      posted += 1;
    };
    controller.abort();
    expect(posted).toBe(0);
    client.dispose();
  });

  it('rejects a create aborted after the Worker created it and disposes that session', async () => {
    const worker = new FakeWorker();
    const sent: VocalWorkerRequestMessage[] = [];
    const deliver = worker.postMessage.bind(worker);
    worker.postMessage = (message: VocalWorkerRequestMessage) => {
      sent.push(message);
      if (message.type !== 'sonare:vocal-cancel') {
        deliver(message);
      }
    };
    const client = new VocalEditWorkerClient({ worker });
    const controller = new AbortController();
    const task = client.create(
      { samples: new Float32Array([0]), sampleRate: 16000 },
      { signal: controller.signal },
    );
    controller.abort();
    await expect(task).rejects.toMatchObject({ name: 'AbortError' });
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(sent.some((message) => message.type === 'sonare:vocal-dispose')).toBe(true);
    client.dispose();
  });

  it('transfers apply and preview arrays by default and copies them on request', async () => {
    const worker = new BridgeWorker();
    const client = new VocalEditWorkerClient({ worker });
    try {
      const session = await client.create({ samples: new Float32Array([0]), sampleRate: 16000 });
      await session.beginEdit();
      const operation = (envelope: Float32Array) =>
        ({
          kind: 'setEdit',
          noteId: 1,
          edit: { amplitudeEnvelope: envelope },
        }) as unknown as VocalEditOperation;
      const copied = new Float32Array([1, 2]);
      await session.apply(
        { expectedGeneration: '0', operations: [operation(copied)] },
        { copy: true },
      );
      expect(copied.length).toBe(2);
      const transferred = new Float32Array([1, 2]);
      await session.apply({ expectedGeneration: '1', operations: [operation(transferred)] });
      expect(transferred.length).toBe(0);

      const range = { startSample: 0, endSample: 1, probe: new Float32Array([3]) };
      await session.preview({ range, requestId: '5' }, { copy: true });
      expect(range.probe.length).toBe(1);
    } finally {
      client.dispose();
    }
  });

  it('reads session and draft state through the read-only methods', async () => {
    const client = new VocalEditWorkerClient({ worker: new BridgeWorker() });
    try {
      const session = await client.create({ samples: new Float32Array([0]), sampleRate: 16000 });
      expect(session.revision()).toBe('0');
      expect(await session.outputLengthSamples()).toBe(480);
      expect(await session.history()).toEqual({ canUndo: false, canRedo: true });
      expect((await session.evaluatePitch(1)).sourceSamples[0]).toBe(0);
      expect(await session.sourceSampleToDestinationSample(1, 5)).toBe(15);
      expect(await session.destinationSampleToSourceSample(1, 15)).toBe(5);
      await expect(session.draftToken()).rejects.toThrow(/No vocal edit draft/);

      await session.beginEdit();
      expect((await session.draftToken()).draftId).toBe('1');
      expect((await session.evaluatePitch(1, { draft: true })).sourceSamples[0]).toBe(1);
      expect(await session.sourceSampleToDestinationSample(1, 5, { draft: true })).toBe(105);
      expect(await session.destinationSampleToSourceSample(1, 105, { draft: true })).toBe(5);
    } finally {
      client.dispose();
    }
  });

  it('requires expectedGeneration on apply like the main-thread draft', async () => {
    const worker = new BridgeWorker();
    const client = new VocalEditWorkerClient({ worker });
    try {
      const session = await client.create({ samples: new Float32Array([0]), sampleRate: 16000 });
      await session.beginEdit();
      const before = worker.posted.length;
      expect(() =>
        session.apply({ operations: [] } as unknown as {
          expectedGeneration: string;
          operations: [];
        }),
      ).toThrow(TypeError);
      expect(worker.posted.length).toBe(before);
    } finally {
      client.dispose();
    }
  });

  it('sends the full transition shape and restore limits to the Worker', async () => {
    const worker = new BridgeWorker();
    const client = new VocalEditWorkerClient({ worker });
    try {
      const restored = await client.restore({
        samples: new Float32Array([0]),
        sampleRate: 16000,
        state: new Uint8Array([1]),
        limits: { maxUndoDepth: 4, maxHistoryBytes: '1024' },
      });
      const restoreMessage = worker.posted.find((m) => m.type === 'sonare:vocal-create');
      expect(
        restoreMessage?.type === 'sonare:vocal-create' && restoreMessage.request.kind === 'restore'
          ? restoreMessage.request.request.limits
          : undefined,
      ).toEqual({ maxUndoDepth: 4, maxHistoryBytes: '1024' });

      await restored.beginEdit();
      const transition = {
        leftNoteId: 1,
        rightNoteId: 2,
        leftWindowSamples: 64,
        rightWindowSamples: 32,
        strength: 0.5,
        curve: 'smoothstep',
      } as const;
      await restored.apply({
        expectedGeneration: '0',
        operations: [{ kind: 'setTransition', transition }],
      });
      const applied = worker.posted.at(-1);
      expect(
        applied?.type === 'sonare:vocal-mutate' && applied.mutation.kind === 'apply'
          ? applied.mutation.request.operations
          : undefined,
      ).toEqual([{ kind: 'setTransition', transition }]);
    } finally {
      client.dispose();
    }
  });

  it('rebuilds a structured native error with every detail field', async () => {
    const worker = new BridgeWorker();
    const client = new VocalEditWorkerClient({ worker });
    try {
      const session = await client.create({ samples: new Float32Array([0]), sampleRate: 16000 });
      worker.notesError = Object.assign(new Error('revision conflict'), {
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
      const error = await session.notes().result.then(
        () => undefined,
        (reason: unknown) => reason,
      );
      expect(isSonareError(error)).toBe(true);
      expect(error).toMatchObject({
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
    } finally {
      client.dispose();
    }
  });
});
