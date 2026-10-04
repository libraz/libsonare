import { createHash } from 'node:crypto';
import { describe, expect, it } from 'vitest';
import { Project } from '../src/project.js';
import type { VocalCreateRequest } from '../src/types_vocal_edit.js';
import type {
  ProjectVocalEditApplyRequest,
  ProjectVocalOriginalSource,
} from '../src/types_vocal_project.js';
import {
  createVocalEditSession,
  type VocalEditDraft,
  type VocalRenderSnapshot,
} from '../src/vocal_edit.js';
import {
  projectApplyVocalEdit,
  projectGetVocalEditDependencies,
  projectRehydrateVocalEdits,
} from '../src/vocal_project.js';

const SHA = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef';
const TOKEN = {
  sessionEpoch: '1',
  revision: '2',
  draftId: '0',
  generation: '0',
  requestId: '3',
  profileId: 4,
};

function request(
  overrides: Partial<ProjectVocalEditApplyRequest> = {},
): ProjectVocalEditApplyRequest {
  return {
    clipId: 7,
    expectedSourceId: 11,
    expectedSourceSampleRate: 48000,
    expectedSourceSampleCount: 3,
    expectedSourceSha256: SHA,
    expectedClipLengthPpq: 4,
    expectedSourceOffsetPpq: 0,
    renderedMono: new Float32Array([0, -0, 0.25]),
    renderedSampleRate: 48000,
    renderToken: TOKEN,
    sve1: new Uint8Array([0x53, 0x56, 0x45, 0x31]),
    ...overrides,
  };
}

const applyResult = {
  clipId: 7,
  takeId: 0,
  originalSourceId: 11,
  derivedSourceId: 19,
  committedRevision: '8',
  profileId: 4,
  derivedSourceSha256: SHA,
  sidecarKey: 'libsonare.vocal-edit/clip/7/take/0',
};

const REAL_SAMPLE_RATE = 16000;
const REAL_SAMPLE_COUNT = 4096;
const REAL_FRAME_LENGTH = 256;
const REAL_HOP_LENGTH = 128;

function realSamples(): Float32Array {
  const samples = new Float32Array(REAL_SAMPLE_COUNT);
  for (let index = 0; index < samples.length; ++index) {
    samples[index] = 0.25 * Math.sin((2 * Math.PI * 440 * index) / REAL_SAMPLE_RATE);
  }
  return samples;
}

function realVocalRequest(samples: Float32Array): VocalCreateRequest {
  const frameCount = Math.ceil(samples.length / REAL_HOP_LENGTH) + 1;
  return {
    samples,
    sampleRate: REAL_SAMPLE_RATE,
    frameLengthSamples: REAL_FRAME_LENGTH,
    hopLengthSamples: REAL_HOP_LENGTH,
    analysis: {
      frameOriginSample: 0,
      samplesPerFrame: REAL_HOP_LENGTH,
      frameLengthSamples: REAL_FRAME_LENGTH,
      f0Hz: new Float32Array(frameCount).fill(440),
      voiced: new Uint8Array(frameCount).fill(1),
      algorithmId: 'libsonare.pyin',
      algorithmVersion: 1,
    },
  };
}

function pcmSha256(samples: Float32Array): string {
  return createHash('sha256')
    .update(new Uint8Array(samples.buffer, samples.byteOffset, samples.byteLength))
    .digest('hex');
}

const NON_CANONICAL_UINT64 = [
  ['a leading zero', '007'],
  ['a bigint', 7n],
  ['a number', 7],
  ['an empty string', ''],
  ['a sign', '+7'],
  ['a decimal point', '7.0'],
  ['an overflow', '18446744073709551616'],
] as const;

describe('Vocal uint64 and digest input rules', () => {
  it.each(NON_CANONICAL_UINT64)('refuses %s at the session entry point', (_label, value) => {
    const session = createVocalEditSession(realVocalRequest(realSamples()));
    try {
      expect(() => session.beginEdit({ expectedRevision: value as never })).toThrow(
        /decimal uint64 string/,
      );
    } finally {
      session.dispose();
    }
  });

  it.each(NON_CANONICAL_UINT64)('refuses %s at the Project entry point', (_label, value) => {
    const project = Project.create();
    const native = (project as unknown as { native: { applyVocalEdit(r: unknown): unknown } })
      .native;
    try {
      expect(() =>
        native.applyVocalEdit({ ...request(), renderToken: { ...TOKEN, revision: value } }),
      ).toThrow(/decimal uint64 string/);
    } finally {
      project.destroy();
    }
  });

  it('accepts an upper-case expected digest and passes it on lower-cased', () => {
    let received: ProjectVocalEditApplyRequest | undefined;
    const native = {
      applyVocalEdit(value: ProjectVocalEditApplyRequest): typeof applyResult {
        received = value;
        return applyResult;
      },
      getVocalEditDependencies: () => [],
      rehydrateVocalEdits: () => [],
    } as Parameters<typeof projectApplyVocalEdit>[0];
    projectApplyVocalEdit(native, request({ expectedSourceSha256: SHA.toUpperCase() }));
    expect(received?.expectedSourceSha256).toBe(SHA);
  });

  it('reads an upper-case expected digest in the native reader', () => {
    const project = Project.create();
    const native = (project as unknown as { native: { applyVocalEdit(r: unknown): unknown } })
      .native;
    try {
      expect(() =>
        native.applyVocalEdit({ ...request(), expectedSourceSha256: SHA.toUpperCase() }),
      ).not.toThrow(/hex/);
    } finally {
      project.destroy();
    }
  });
});

describe('Node Project vocal edit helpers', () => {
  it('enumerates an empty Project through the native facade', () => {
    const project = Project.create();
    try {
      expect(project.getVocalEditDependencies()).toEqual([]);
    } finally {
      project.destroy();
    }
  });

  it('normalizes defaults and copies apply buffers before entering native code', () => {
    let received: ProjectVocalEditApplyRequest | undefined;
    const native = {
      applyVocalEdit(value: ProjectVocalEditApplyRequest): typeof applyResult {
        received = value;
        return applyResult;
      },
      getVocalEditDependencies: () => [],
      rehydrateVocalEdits: () => [],
    } as Parameters<typeof projectApplyVocalEdit>[0];
    const input = request();
    const result = projectApplyVocalEdit(native, input);

    expect(received).toBeDefined();
    expect(received?.takeId).toBe(0);
    expect(received?.renderedStartSample).toBe(0);
    expect(received?.renderedMono).not.toBe(input.renderedMono);
    expect(received?.sve1).not.toBe(input.sve1);
    input.renderedMono[0] = 0.9;
    input.sve1[0] = 0;
    expect(received?.renderedMono[0]).toBe(0);
    expect(received?.sve1[0]).toBe(0x53);
    expect(result).toEqual(applyResult);
  });

  it('rejects non-hex SHA, draft tokens, partial renders, and rate changes', () => {
    const native = {
      applyVocalEdit: () => applyResult,
      getVocalEditDependencies: () => [],
      rehydrateVocalEdits: () => [],
    } as Parameters<typeof projectApplyVocalEdit>[0];
    expect(() =>
      projectApplyVocalEdit(native, request({ expectedSourceSha256: `${SHA.slice(2)}zz` })),
    ).toThrow(/64 hexadecimal/);
    expect(() =>
      projectApplyVocalEdit(native, request({ renderToken: { ...TOKEN, generation: '1' } })),
    ).toThrow(/committed session token/);
    expect(() =>
      projectApplyVocalEdit(native, request({ renderedMono: new Float32Array([0, 0]) })),
    ).toThrow(/length must equal/);
    expect(() => projectApplyVocalEdit(native, request({ renderedSampleRate: 44100 }))).toThrow(
      /must equal/,
    );
  });

  it('returns copied dependency records with decimal revisions and lower-case digests', () => {
    const dependencies = [
      {
        clipId: 7,
        takeId: 0,
        originalSourceId: 11,
        derivedSourceId: 19,
        sourceSampleRate: 48000,
        profileId: 4,
        sourceSampleCount: 3,
        committedRevision: '8',
        originalSourceSha256: SHA,
        derivedSourceSha256: SHA,
        originalPcmAvailable: true,
        derivedPcmAvailable: false,
        reason: 0,
        sidecarKey: 'libsonare.vocal-edit/clip/7/take/0',
      },
    ];
    const native = {
      applyVocalEdit: () => applyResult,
      getVocalEditDependencies: () => dependencies,
      rehydrateVocalEdits: () => [],
    } as Parameters<typeof projectGetVocalEditDependencies>[0];
    const result = projectGetVocalEditDependencies(native);

    expect(result).toEqual(dependencies);
    expect(result).not.toBe(dependencies);
    expect(result[0]).not.toBe(dependencies[0]);
    dependencies[0].sourceSampleCount = 99;
    expect(result[0]?.sourceSampleCount).toBe(3);
  });

  it('preserves malformed dependency and unresolved rehydrate records', () => {
    const malformed = {
      clipId: 7,
      takeId: 0,
      originalSourceId: 0,
      derivedSourceId: 0,
      sourceSampleRate: 0,
      profileId: 0,
      sourceSampleCount: 0,
      committedRevision: '0',
      originalSourceSha256: '0'.repeat(64),
      derivedSourceSha256: '0'.repeat(64),
      originalPcmAvailable: false,
      derivedPcmAvailable: false,
      reason: 4,
      sidecarKey: 'libsonare.vocal-edit/clip/7/take/0',
    };
    const native = {
      applyVocalEdit: () => applyResult,
      getVocalEditDependencies: () => [malformed],
      rehydrateVocalEdits: () => [
        { clipId: 7, takeId: 0, derivedSourceId: 0, status: 2, reason: 1 },
      ],
    } as Parameters<typeof projectGetVocalEditDependencies>[0];

    expect(projectGetVocalEditDependencies(native)).toEqual([malformed]);
    expect(projectRehydrateVocalEdits(native, [])).toEqual([
      { clipId: 7, takeId: 0, derivedSourceId: 0, status: 2, reason: 1 },
    ]);
  });

  it('refuses sample rates outside the supported range', () => {
    const native = {
      applyVocalEdit: () => applyResult,
      getVocalEditDependencies: () => [],
      rehydrateVocalEdits: () => [],
    } as Parameters<typeof projectApplyVocalEdit>[0];
    for (const rate of [7999, 384001]) {
      expect(() =>
        projectApplyVocalEdit(
          native,
          request({ expectedSourceSampleRate: rate, renderedSampleRate: rate }),
        ),
      ).toThrow(RangeError);
      expect(() =>
        projectRehydrateVocalEdits(native, [
          { sourceId: 11, mono: new Float32Array([0]), sampleRate: rate },
        ]),
      ).toThrow(RangeError);
    }
  });

  it('copies originals, rejects duplicate ids, and keeps cancel call-scoped', () => {
    let received: readonly ProjectVocalOriginalSource[] | undefined;
    let callbackSeen: (() => boolean) | undefined;
    const native = {
      applyVocalEdit: () => applyResult,
      getVocalEditDependencies: () => [],
      rehydrateVocalEdits(
        sources: readonly ProjectVocalOriginalSource[],
        cancel?: () => boolean,
      ): unknown {
        received = sources;
        callbackSeen = cancel;
        return [{ clipId: 7, takeId: 0, derivedSourceId: 19, status: 0, reason: 0 }];
      },
    } as Parameters<typeof projectRehydrateVocalEdits>[0];
    const mono = new Float32Array([0, -0, 0.25]);
    const cancel = (): boolean => true;
    const result = projectRehydrateVocalEdits(
      native,
      [{ sourceId: 11, mono, sampleRate: 48000 }],
      cancel,
    );

    expect(received?.[0]?.mono).not.toBe(mono);
    expect(received?.[0]?.mono[1]).toBe(-0);
    expect(callbackSeen).toBe(cancel);
    expect(callbackSeen?.()).toBe(true);
    expect(result).toEqual([{ clipId: 7, takeId: 0, derivedSourceId: 19, status: 0, reason: 0 }]);
    expect(() =>
      projectRehydrateVocalEdits(native, [
        { sourceId: 11, mono: new Float32Array([0]), sampleRate: 48000 },
        { sourceId: 11, mono: new Float32Array([1]), sampleRate: 48000 },
      ]),
    ).toThrow(/duplicate sourceId/);
  });

  it('applies a nonidentity vocal edit and restores its derived PCM after JSON round-trip', () => {
    const samples = realSamples();
    const session = createVocalEditSession(realVocalRequest(samples));
    let snapshot: VocalRenderSnapshot | undefined;
    let draft: VocalEditDraft | undefined;
    let project: Project | undefined;
    let restored: Project | undefined;
    try {
      const note = session.notes().notes[0];
      expect(note).toBeDefined();
      draft = session.beginEdit({ expectedRevision: session.revision() });
      draft.apply({
        expectedGeneration: draft.token().generation,
        operations: [
          {
            kind: 'setEdit',
            noteId: note.id,
            edit: {
              ...note.edit,
              pitch: {
                ...note.edit.pitch,
                target: { mode: 'center', midi: 72 },
                amount: 1,
              },
              amplitudeEnvelope: Array.from(note.edit.amplitudeEnvelope),
            },
          },
        ],
      });
      draft.commit({ expectedRevision: session.revision() });

      snapshot = session.captureRenderSnapshot();
      const rendered = snapshot.render({
        range: { startSample: 0, endSample: samples.length },
      }).samples;
      expect(rendered).toHaveLength(samples.length);
      expect(rendered.some((sample, index) => sample !== samples[index])).toBe(true);

      const sourceDigest = pcmSha256(samples);
      project = Project.create();
      project.setSampleRate(REAL_SAMPLE_RATE);
      const trackId = project.addTrack({ kind: 'audio', name: 'lead' });
      const clipId = project.addClip({
        trackId,
        startPpq: 0,
        lengthPpq: 4,
        audio: samples,
        audioChannels: 1,
        audioSampleRate: REAL_SAMPLE_RATE,
      });
      const sourceId = project.sourceByIndex(0).id;
      project.setAudioSourceMetadata(sourceId, `sha256:${sourceDigest}`, '');

      const applied = project.applyVocalEdit({
        clipId,
        expectedSourceId: sourceId,
        expectedSourceSampleRate: REAL_SAMPLE_RATE,
        expectedSourceSampleCount: samples.length,
        expectedSourceSha256: sourceDigest,
        expectedClipLengthPpq: 4,
        expectedSourceOffsetPpq: 0,
        renderedMono: rendered,
        renderedSampleRate: REAL_SAMPLE_RATE,
        renderToken: session.token(),
        sve1: session.exportState(),
      });
      expect(applied).toMatchObject({
        clipId,
        takeId: 0,
        originalSourceId: sourceId,
        committedRevision: '1',
        profileId: 1,
      });
      expect(applied.derivedSourceId).not.toBe(sourceId);

      expect(project.getVocalEditDependencies()).toEqual([
        expect.objectContaining({
          clipId,
          takeId: 0,
          originalSourceId: sourceId,
          derivedSourceId: applied.derivedSourceId,
          sourceSampleRate: REAL_SAMPLE_RATE,
          sourceSampleCount: samples.length,
          originalPcmAvailable: true,
          derivedPcmAvailable: true,
          reason: 0,
        }),
      ]);
      const beforeRestoreBounce = project.bounce({
        totalFrames: samples.length,
        numChannels: 1,
      });

      restored = Project.fromJson(project.toJson());
      expect(restored.getVocalEditDependencies()).toEqual([
        expect.objectContaining({
          originalPcmAvailable: false,
          derivedPcmAvailable: false,
          reason: 0,
        }),
      ]);
      expect(
        restored.rehydrateVocalEdits([{ sourceId, mono: samples, sampleRate: REAL_SAMPLE_RATE }]),
      ).toEqual([
        {
          clipId,
          takeId: 0,
          derivedSourceId: applied.derivedSourceId,
          status: 0,
          reason: 0,
        },
      ]);
      expect(restored.getVocalEditDependencies()).toEqual([
        expect.objectContaining({
          originalPcmAvailable: true,
          derivedPcmAvailable: true,
          reason: 0,
        }),
      ]);
      expect(restored.bounce({ totalFrames: samples.length, numChannels: 1 })).toEqual(
        beforeRestoreBounce,
      );
      expect(
        restored.rehydrateVocalEdits(
          [{ sourceId, mono: samples, sampleRate: REAL_SAMPLE_RATE }],
          () => {
            restored?.destroy();
            return false;
          },
        ),
      ).toEqual([
        {
          clipId,
          takeId: 0,
          derivedSourceId: applied.derivedSourceId,
          status: 1,
          reason: 0,
        },
      ]);
      expect(() => restored?.getVocalEditDependencies()).toThrow(/disposed/);

      // Any truthy cancel return cancels, not only boolean true.
      const cancelled = Project.fromJson(project.toJson());
      try {
        expect(() =>
          cancelled.rehydrateVocalEdits(
            [{ sourceId, mono: samples, sampleRate: REAL_SAMPLE_RATE }],
            (() => 1) as unknown as () => boolean,
          ),
        ).toThrow(/Cancelled/);
      } finally {
        cancelled.destroy();
      }
    } finally {
      draft?.cancel();
      snapshot?.dispose();
      restored?.destroy();
      project?.destroy();
      session.dispose();
    }
  });
});
