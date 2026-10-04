import { describe, expect, it } from 'vitest';
import {
  createVocalEditSession,
  type VocalCreateRequest,
  type VocalEditSession,
  type VocalNoteEdit,
  vocalEditApiVersion,
  vocalEditAvailable,
} from '../src/index.js';

const SAMPLE_RATE = 16000;
const FRAME_LENGTH = 256;
const HOP_LENGTH = 128;
const SAMPLE_COUNT = 4096;

function sine(): Float32Array {
  const samples = new Float32Array(SAMPLE_COUNT);
  for (let i = 0; i < samples.length; ++i) {
    samples[i] = 0.25 * Math.sin((2 * Math.PI * 440 * i) / SAMPLE_RATE);
  }
  return samples;
}

function suppliedAnalysis(): { f0Hz: Float32Array; voiced: Uint8Array } {
  const count = Math.ceil(SAMPLE_COUNT / HOP_LENGTH) + 1;
  return { f0Hz: new Float32Array(count).fill(440), voiced: new Uint8Array(count).fill(1) };
}

function request(samples = sine()): VocalCreateRequest {
  const analysis = suppliedAnalysis();
  return {
    samples,
    sampleRate: SAMPLE_RATE,
    frameLengthSamples: FRAME_LENGTH,
    hopLengthSamples: HOP_LENGTH,
    analysis: {
      frameOriginSample: 0,
      samplesPerFrame: HOP_LENGTH,
      frameLengthSamples: FRAME_LENGTH,
      f0Hz: analysis.f0Hz,
      voiced: analysis.voiced,
      algorithmId: 'libsonare.pyin',
      algorithmVersion: 1,
    },
  };
}

function identityEdit(note?: {
  sourceStartSample: number;
  sourceEndSample: number;
}): VocalNoteEdit {
  return {
    pitch: {
      target: { mode: 'none' },
      amount: 0,
      speedMs: 0,
      maxCorrectionSemitones: 12,
      transposeSemitones: 0,
      driftScale: 1,
      vibratoScale: 1,
    },
    destinationStartSample: note?.sourceStartSample ?? 0,
    destinationLengthSamples:
      note === undefined ? 0 : note.sourceEndSample - note.sourceStartSample,
    gainDb: 0,
    muted: false,
    amplitudeEnvelope: [],
    formant: { mode: 'preserve', shiftSemitones: 0 },
  };
}

function create(): VocalEditSession {
  return createVocalEditSession(request());
}

describe('Node vocal edit facade', () => {
  it('reports the linked vocal-edit surface and its ABI version', () => {
    expect(vocalEditAvailable()).toBe(true);
    expect(Number.isInteger(vocalEditApiVersion())).toBe(true);
    expect(vocalEditApiVersion()).toBeGreaterThan(0);
  });

  it('copies source and supplied analysis before returning the session', () => {
    const source = sine();
    const original = source[0];
    const input = request(source);
    const firstF0 = input.analysis?.f0Hz[0] ?? 0;
    const session = createVocalEditSession(input);
    source[0] = 0.9;
    input.analysis?.f0Hz.fill(220);
    try {
      expect(session.analysis().f0Hz[0]).toBe(firstF0);
      expect(source[0]).not.toBe(original);
      expect(session.analysis().sourceLengthSamples).toBe(SAMPLE_COUNT);
    } finally {
      session.dispose();
    }
  });

  it('returns copies of note and analysis arrays and decimal uint64 tokens', () => {
    const session = create();
    try {
      const first = session.notes();
      const second = session.notes();
      expect(first).not.toBe(second);
      expect(session.token().revision).toMatch(/^\d+$/);
      const analysis = session.analysis();
      analysis.f0Hz[0] = 220;
      expect(session.analysis().f0Hz[0]).toBe(440);
      if (first.notes.length !== 0) {
        first.notes[0].amplitude[0] = 0;
        expect(session.notes().notes[0].amplitude[0]).not.toBe(0);
      }
    } finally {
      session.dispose();
    }
  });

  it('keeps authoritative analysis settings ahead of create defaults', () => {
    const input = request();
    const analysis = input.analysis;
    if (analysis === undefined) {
      throw new Error('test request must include analysis');
    }
    Object.assign(input, {
      fminHz: 80,
      fmaxHz: 2000,
      yinThreshold: 0.15,
      voicedThreshold: 0.35,
      centered: true,
      segmentationThresholdCents: 90,
      minNoteMs: 45,
      referenceHz: 440,
    });
    input.analysis = {
      ...analysis,
      fminHz: 120,
      fmaxHz: 1800,
      yinThreshold: 0.23,
      voicedThreshold: 0.7,
      centered: false,
      segmentationThresholdCents: 37,
      minNoteMs: 21,
      referenceHz: 442,
    };
    const session = createVocalEditSession(input);
    try {
      expect(session.analysis()).toMatchObject({
        fminHz: 120,
        fmaxHz: 1800,
        yinThreshold: 0.23,
        voicedThreshold: 0.7,
        centered: false,
        segmentationThresholdCents: 37,
        minNoteMs: 21,
        referenceHz: 442,
      });
    } finally {
      session.dispose();
    }
  });

  it('exposes destination length and history state and defaults a snapshot to the full output', () => {
    const session = create();
    const snapshot = session.captureRenderSnapshot();
    try {
      expect(session.outputLengthSamples()).toBe(SAMPLE_COUNT);
      expect(session.history()).toEqual({ canUndo: false, canRedo: false });
      expect(snapshot.outputLengthSamples()).toBe(SAMPLE_COUNT);
      expect(snapshot.render({}).samples.length).toBe(SAMPLE_COUNT);
    } finally {
      snapshot.dispose();
      session.dispose();
    }
  });

  it('applies a piano-roll center target through one draft transaction', () => {
    const session = create();
    try {
      const note = session.notes().notes[0];
      expect(note).toBeDefined();
      const draft = session.beginEdit({ expectedRevision: session.revision() });
      const edit = identityEdit(note);
      edit.pitch = { ...edit.pitch, target: { mode: 'center', midi: 69 }, amount: 1 };
      const generation = draft.token().generation;
      const result = draft.apply({
        expectedGeneration: generation,
        operations: [{ kind: 'setEdit', noteId: note.id, edit }],
      });
      expect(result.token.generation).not.toBe(generation);
      const evaluation = draft.evaluatePitch(note.id);
      expect(evaluation.hasTarget.some((value) => value === 1)).toBe(true);
      draft.cancel();
    } finally {
      session.dispose();
    }
  });

  it('applies an adjacent-note transition through the tagged operation parser', () => {
    const input = request();
    const f0Hz = new Float32Array(input.analysis?.f0Hz ?? []);
    for (let frame = Math.floor(f0Hz.length / 2); frame < f0Hz.length; ++frame) {
      f0Hz[frame] = 523.2511;
    }
    const analysis = input.analysis;
    if (analysis === undefined) {
      throw new Error('test request must include analysis');
    }
    input.analysis = { ...analysis, f0Hz };
    const session = createVocalEditSession(input);
    try {
      const notes = session.notes().notes;
      expect(notes).toHaveLength(2);
      const draft = session.beginEdit();
      const result = draft.apply({
        expectedGeneration: draft.token().generation,
        operations: [
          {
            kind: 'setTransition',
            transition: {
              leftNoteId: notes[0].id,
              rightNoteId: notes[1].id,
              leftWindowSamples: 128,
              rightWindowSamples: 128,
              strength: 0.5,
              curve: 'smoothstep',
            },
          },
        ],
      });
      expect(result.dirtyRanges.length).toBeGreaterThan(0);
      expect(draft.notes().transitions).toHaveLength(1);
      draft.cancel();
    } finally {
      session.dispose();
    }
  });

  it('rejects a stale draft generation without changing committed state', () => {
    const session = create();
    try {
      const note = session.notes().notes[0];
      const draft = session.beginEdit();
      let error: unknown;
      try {
        draft.apply({
          expectedGeneration: '999999999999999999',
          operations: [{ kind: 'reset', noteIds: [note.id] }],
        });
      } catch (caught) {
        error = caught;
      }
      expect(error).toMatchObject({
        reason: 2,
        field: 'generation',
        expectedText: '999999999999999999',
        actualText: '1',
      });
      expect(session.token().revision).toBe('0');
      draft.cancel();
    } finally {
      session.dispose();
    }
  });

  it('renders an exact requested range and retains the snapshot after session disposal', async () => {
    const session = create();
    const snapshot = session.captureRenderSnapshot();
    session.dispose();
    try {
      const range = { startSample: 64, endSample: 320 };
      const sync = snapshot.render({ range, requestId: '184467440737095516' });
      const asyncResult = await snapshot.renderAsync({ range, requestId: '184467440737095516' });
      expect(sync.samples.length).toBe(range.endSample - range.startSample);
      expect(asyncResult.samples).toEqual(sync.samples);
      expect(asyncResult.token.profileId).toBe(1);
      expect(asyncResult.token.requestId).toBe('184467440737095516');
    } finally {
      snapshot.dispose();
    }
  });

  it('cancels an asynchronous render on the worker and rejects with vocal detail', async () => {
    const session = createVocalEditSession({ ...request(), outputLengthSamples: 2_000_000 });
    const snapshot = session.captureRenderSnapshot();
    const controller = new AbortController();
    try {
      const render = snapshot.renderAsync({ signal: controller.signal });
      controller.abort();
      await expect(render).rejects.toMatchObject({ reason: 5, field: 'render' });
    } finally {
      snapshot.dispose();
      session.dispose();
    }
  });

  it('keeps a successful render settled when abort-listener cleanup throws', async () => {
    const session = createVocalEditSession({ ...request(), limits: { maxRenderJobs: 1 } });
    const snapshot = session.captureRenderSnapshot();
    const signal = {
      aborted: false,
      addEventListener: () => undefined,
      removeEventListener: () => {
        throw new Error('removeEventListener failed');
      },
    } as unknown as AbortSignal;
    try {
      await expect(
        snapshot.renderAsync({
          range: { startSample: 0, endSample: 256 },
          signal,
        }),
      ).resolves.toMatchObject({ samples: expect.any(Float32Array) });
      await expect(
        snapshot.renderAsync({ range: { startSample: 0, endSample: 256 } }),
      ).resolves.toMatchObject({ samples: expect.any(Float32Array) });
    } finally {
      snapshot.dispose();
      session.dispose();
    }
  });

  it('advances and finalizes the incremental render job with copied output', () => {
    const session = create();
    const snapshot = session.captureRenderSnapshot();
    try {
      const job = snapshot.beginRenderJob({ range: { startSample: 0, endSample: 256 } });
      let complete = false;
      for (let i = 0; i < 100 && !complete; ++i) {
        complete = job.next().complete;
      }
      expect(complete).toBe(true);
      const result = job.finalize();
      expect(result.samples).toBeInstanceOf(Float32Array);
      expect(result.samples.length).toBe(256);
    } finally {
      snapshot.dispose();
      session.dispose();
    }
  });

  it('rejects an analysis mask whose length does not match its F0 track', () => {
    const invalid = request();
    const analysis = invalid.analysis;
    if (analysis === undefined) {
      throw new Error('test request must include analysis');
    }
    invalid.analysis = {
      ...analysis,
      voiced: new Uint8Array(1),
    };
    expect(() => createVocalEditSession(invalid)).toThrow();
  });
});
