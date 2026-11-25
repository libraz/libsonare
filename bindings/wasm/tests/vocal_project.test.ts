import { spawnSync } from 'node:child_process';
import { describe, expect, it } from 'vitest';
import { ErrorCode } from '../src/errors';
import type { VocalStateToken } from '../src/public_types_vocal_edit';
import type {
  ProjectVocalEditApplyRequest,
  ProjectVocalOriginalSource,
} from '../src/public_types_vocal_project';
import {
  projectApplyVocalEdit,
  projectGetVocalEditDependencies,
  projectRehydrateVocalEdits,
  type VocalProjectNative,
} from '../src/vocal_project';

const token: VocalStateToken = {
  sessionEpoch: '17',
  revision: '9',
  draftId: '0',
  generation: '0',
  requestId: '0',
  profileId: 1,
};

function request(): ProjectVocalEditApplyRequest {
  return {
    clipId: 3,
    expectedSourceId: 4,
    expectedSourceSampleRate: 16000,
    expectedSourceSampleCount: 4,
    expectedSourceSha256: 'AB'.repeat(32),
    expectedClipLengthPpq: 4,
    expectedSourceOffsetPpq: 0,
    renderedMono: new Float32Array([0.1, -0.2, 0.3, -0.4]),
    renderedSampleRate: 16000,
    renderToken: token,
    vocalState: new Uint8Array([83, 86, 69, 49]),
  };
}

class FakeNative implements VocalProjectNative {
  applyRequest: unknown;
  originalSources: unknown;
  cancel: unknown;

  applyVocalEdit(value: unknown): unknown {
    this.applyRequest = value;
    const source = value as { renderedMono: Float32Array; sve1: Uint8Array };
    source.renderedMono[0] = 1;
    source.sve1[0] = 0;
    return {
      clipId: 3,
      takeId: 0,
      originalSourceId: 4,
      derivedSourceId: 8,
      committedRevision: '9',
      profileId: 1,
      derivedSourceSha256: 'CD'.repeat(32),
      sidecarKey: 'libsonare.vocal-edit/clip/3/take/0',
    };
  }

  getVocalEditDependencies(): unknown {
    return [
      {
        clipId: 3,
        takeId: 0,
        originalSourceId: 4,
        derivedSourceId: 8,
        sourceSampleRate: 16000,
        profileId: 1,
        sourceSampleCount: 4,
        committedRevision: '9',
        originalSourceSha256: '00'.repeat(32),
        derivedSourceSha256: 'CD'.repeat(32),
        originalPcmAvailable: true,
        derivedPcmAvailable: false,
        reason: 0,
        sidecarKey: 'libsonare.vocal-edit/clip/3/take/0',
      },
    ];
  }

  rehydrateVocalEdits(originals: unknown, cancel?: unknown): unknown {
    this.originalSources = originals;
    this.cancel = cancel;
    (originals as ProjectVocalOriginalSource[])[0].mono[0] = 1;
    return [{ clipId: 3, takeId: 0, derivedSourceId: 8, status: 0, reason: 0 }];
  }
}

describe('WASM Project vocal edit adapter', () => {
  it('copies apply inputs, canonicalizes hex values, and copies the result', () => {
    const native = new FakeNative();
    const source = request();
    const rendered = source.renderedMono;
    const state = source.vocalState;
    const result = projectApplyVocalEdit(native, source);

    expect(rendered[0]).toBeCloseTo(0.1);
    expect(state[0]).toBe(83);
    const sent = native.applyRequest as {
      renderToken: VocalStateToken;
      expectedSourceSha256: string;
      takeId: number;
      renderedStartSample: number;
    };
    expect(sent.renderToken.revision).toBe('9');
    expect(sent.expectedSourceSha256).toBe('ab'.repeat(32));
    // An omitted take addresses the base source natively.
    expect(sent.takeId).toBe(0);
    expect(sent.renderedStartSample).toBe(0);
    expect(result).toEqual({
      clipId: 3,
      originalSourceId: 4,
      derivedSourceId: 8,
      committedRevision: '9',
      profileId: 1,
      derivedSourceSha256: 'cd'.repeat(32),
      sidecarKey: 'libsonare.vocal-edit/clip/3/take/0',
    });
  });

  it('refuses an explicit take 0 and forwards a real take id', () => {
    const native = new FakeNative();
    expect(() => projectApplyVocalEdit(native, { ...request(), takeId: 0 })).toThrow(RangeError);
    expect(native.applyRequest).toBeUndefined();

    projectApplyVocalEdit(native, { ...request(), takeId: 2 });
    expect((native.applyRequest as { takeId: number }).takeId).toBe(2);
  });

  it('reports a native take id only when it is not the base source', () => {
    const row = {
      clipId: 3,
      takeId: 2,
      originalSourceId: 4,
      derivedSourceId: 8,
      sourceSampleRate: 16000,
      profileId: 1,
      sourceSampleCount: 4,
      committedRevision: '9',
      originalSourceSha256: '00'.repeat(32),
      derivedSourceSha256: 'cd'.repeat(32),
      originalPcmAvailable: true,
      derivedPcmAvailable: true,
      reason: 0,
      sidecarKey: 'libsonare.vocal-edit/clip/3/take/2',
    };
    const native: VocalProjectNative = {
      applyVocalEdit: () => undefined,
      getVocalEditDependencies: () => [row],
      rehydrateVocalEdits: () => [
        { clipId: 3, takeId: 2, derivedSourceId: 8, status: 1, reason: 0 },
      ],
    };
    expect(projectGetVocalEditDependencies(native)[0]).toMatchObject({ takeId: 2 });
    expect(projectRehydrateVocalEdits(native, [])).toEqual([
      { clipId: 3, takeId: 2, derivedSourceId: 8, status: 'alreadyReady', reason: 'none' },
    ]);
  });

  it('keeps takeId absent from the apply result for the base source and carries a real one', () => {
    const native: VocalProjectNative = {
      applyVocalEdit: (value) => ({
        clipId: 3,
        takeId: (value as { takeId: number }).takeId,
        originalSourceId: 4,
        derivedSourceId: 8,
        committedRevision: '9',
        profileId: 1,
        derivedSourceSha256: 'cd'.repeat(32),
        sidecarKey: 'libsonare.vocal-edit/clip/3/take/0',
      }),
      getVocalEditDependencies: () => [],
      rehydrateVocalEdits: () => [],
    };
    expect(projectApplyVocalEdit(native, request())).not.toHaveProperty('takeId');
    expect(projectApplyVocalEdit(native, { ...request(), takeId: 2 })).toMatchObject({ takeId: 2 });
    expect(() => projectApplyVocalEdit(native, { ...request(), takeId: 0 })).toThrow(RangeError);
  });

  it('reads dependency and rehydrate takeId as absent for the base source, with reason and status tokens', () => {
    const row = {
      clipId: 3,
      originalSourceId: 4,
      derivedSourceId: 8,
      sourceSampleRate: 16000,
      profileId: 1,
      sourceSampleCount: 4,
      committedRevision: '9',
      originalSourceSha256: '00'.repeat(32),
      derivedSourceSha256: 'cd'.repeat(32),
      originalPcmAvailable: true,
      derivedPcmAvailable: true,
      sidecarKey: 'libsonare.vocal-edit/clip/3/take/0',
    };
    const native: VocalProjectNative = {
      applyVocalEdit: () => undefined,
      getVocalEditDependencies: () => [
        { ...row, takeId: 0, reason: 0 },
        { ...row, takeId: 2, reason: 1 },
      ],
      rehydrateVocalEdits: () => [
        { clipId: 3, takeId: 0, derivedSourceId: 8, status: 0, reason: 0 },
        { clipId: 3, takeId: 2, derivedSourceId: 9, status: 1, reason: 0 },
        { clipId: 4, takeId: 0, derivedSourceId: 10, status: 2, reason: 1 },
      ],
    };

    const dependencies = projectGetVocalEditDependencies(native);
    expect(dependencies[0]).not.toHaveProperty('takeId');
    expect(dependencies[1]).toMatchObject({ takeId: 2 });
    expect(dependencies.map((item) => item.reason)).toEqual(['none', 'invalidInput']);

    const items = projectRehydrateVocalEdits(native, []);
    expect(items[0]).not.toHaveProperty('takeId');
    expect(items[1]).toMatchObject({ takeId: 2 });
    expect(items[2]).not.toHaveProperty('takeId');
    expect(items.map((item) => item.status)).toEqual(['rehydrated', 'alreadyReady', 'unresolved']);
    expect(items.map((item) => item.reason)).toEqual(['none', 'none', 'invalidInput']);
  });

  it('returns copied dependency rows in native order', () => {
    const dependencies = projectGetVocalEditDependencies(new FakeNative());
    expect(dependencies).toHaveLength(1);
    expect(dependencies[0]).not.toHaveProperty('takeId');
    expect(dependencies[0]).toMatchObject({
      clipId: 3,
      reason: 'none',
      originalPcmAvailable: true,
      derivedPcmAvailable: false,
      committedRevision: '9',
      originalSourceSha256: '00'.repeat(32),
    });
  });

  it('keeps malformed dependency rows visible for UI diagnostics', () => {
    const native: VocalProjectNative = {
      applyVocalEdit: () => undefined,
      getVocalEditDependencies: () => [
        {
          clipId: 0,
          takeId: 0,
          originalSourceId: 0,
          derivedSourceId: 0,
          sourceSampleRate: 0,
          profileId: 0,
          sourceSampleCount: 0,
          committedRevision: '0',
          originalSourceSha256: '00'.repeat(32),
          derivedSourceSha256: '00'.repeat(32),
          originalPcmAvailable: false,
          derivedPcmAvailable: false,
          reason: 7,
          sidecarKey: 'libsonare.vocal-edit/clip/3/take/0',
        },
      ],
      rehydrateVocalEdits: () => [],
    };

    expect(projectGetVocalEditDependencies(native)).toEqual([
      expect.objectContaining({ reason: 'invalidState', sourceSampleCount: 0 }),
    ]);
  });

  it('copies mono originals, rejects duplicate ids, and forwards cancellation', () => {
    const native = new FakeNative();
    const original: ProjectVocalOriginalSource = {
      sourceId: 4,
      mono: new Float32Array([0.1, 0.2]),
      sampleRate: 16000,
    };
    const cancel = () => false;
    const result = projectRehydrateVocalEdits(native, [original], { cancel });

    expect(result[0]).toEqual({
      clipId: 3,
      derivedSourceId: 8,
      status: 'rehydrated',
      reason: 'none',
    });
    expect(native.cancel).toEqual(expect.any(Function));
    expect(native.cancel).not.toBe(cancel);
    expect((native.cancel as () => boolean)()).toBe(false);
    expect((native.originalSources as ProjectVocalOriginalSource[])[0].mono).not.toBe(
      original.mono,
    );
    expect(original.mono[0]).toBeCloseTo(0.1);
    expect(() => projectRehydrateVocalEdits(native, [original, original])).toThrow(
      /duplicate sourceId/,
    );
  });

  it('rethrows a cancellation callback failure after native cancellation', () => {
    const native: VocalProjectNative = {
      applyVocalEdit: () => undefined,
      getVocalEditDependencies: () => [],
      rehydrateVocalEdits: (_originals, cancel) => {
        try {
          if (typeof cancel === 'function' && cancel()) {
            throw new Error('native cancellation');
          }
        } catch {
          // A native cancellation path can consume the callback boundary error.
        }
        throw new Error('native cancellation');
      },
    };
    const callbackFailure = new Error('cancel callback failed');
    expect(() =>
      projectRehydrateVocalEdits(
        native,
        [{ sourceId: 4, mono: new Float32Array([0.1]), sampleRate: 16000 }],
        {
          cancel: () => {
            throw callbackFailure;
          },
        },
      ),
    ).toThrow(callbackFailure);
  });

  it('rethrows a cancel callback failure the native adapter returns as data', () => {
    const callbackFailure = new Error('raw cancel callback failed');
    const native: VocalProjectNative = {
      applyVocalEdit: () => undefined,
      getVocalEditDependencies: () => [],
      rehydrateVocalEdits: () => ({ cancelFailure: callbackFailure }),
    };
    expect(() => projectRehydrateVocalEdits(native, [], { cancel: () => false })).toThrow(
      callbackFailure,
    );
  });

  it('accepts only canonical decimal uint64 strings', () => {
    const native = new FakeNative();
    for (const revision of ['007', '00', '+7', '7.0', '', ' 7', '18446744073709551616']) {
      expect(
        () => projectApplyVocalEdit(native, { ...request(), renderToken: { ...token, revision } }),
        revision,
      ).toThrow(RangeError);
    }
    for (const revision of [7, 7n]) {
      expect(() =>
        projectApplyVocalEdit(native, {
          ...request(),
          renderToken: { ...token, revision: revision as unknown as string },
        }),
      ).toThrow(RangeError);
    }
    expect(() =>
      projectApplyVocalEdit(native, {
        ...request(),
        renderToken: { ...token, revision: '18446744073709551615' },
      }),
    ).not.toThrow();
    const leadingZeroResult: VocalProjectNative = {
      applyVocalEdit: () => ({
        ...(native.applyVocalEdit({
          renderedMono: new Float32Array(1),
          sve1: new Uint8Array(1),
        }) as object),
        committedRevision: '09',
      }),
      getVocalEditDependencies: () => [],
      rehydrateVocalEdits: () => [],
    };
    expect(() => projectApplyVocalEdit(leadingZeroResult, request())).toThrow(
      /native vocal project result.committedRevision is not a uint64 string/,
    );
  });

  it('rejects non-committed tokens, invalid digest, and non-finite mono', () => {
    const native = new FakeNative();
    expect(() =>
      projectApplyVocalEdit(native, {
        ...request(),
        expectedSourceSha256: 'bad',
      }),
    ).toThrow(/64 hexadecimal/);
    expect(() =>
      projectApplyVocalEdit(native, {
        ...request(),
        renderToken: { ...token, draftId: '1' },
      }),
    ).toThrow(/committed/);
    expect(() =>
      projectRehydrateVocalEdits(native, [
        { sourceId: 4, mono: new Float32Array([Number.NaN]), sampleRate: 16000 },
      ]),
    ).toThrow(/finite/);
  });

  it('applies a full vocal render and exposes its persisted dependency', async () => {
    const runtime = (await import('../dist/index.js')) as unknown as {
      init: () => Promise<unknown>;
      Project: {
        fromJson(json: string): {
          rehydrateVocalEdits(
            originals: unknown[],
            options?: { cancel?: () => boolean },
          ): unknown[];
          vocalEditDependencies(): unknown[];
          destroy(): void;
        };
      } & (new () => {
        setSampleRate(sampleRate: number): void;
        addTrack(desc: { kind: string; name: string }): number;
        setAudioSourceMetadata(input: { sourceId: number; contentHash: string }): void;
        addClip(desc: {
          trackId: number;
          startPpq: number;
          lengthPpq: number;
          channels: Float32Array[];
          sampleRate: number;
        }): number;
        clips(): Array<{
          sourceId: number;
          sourceOffsetPpq: number;
          lengthPpq: number;
        }>;
        applyVocalEdit(request: ProjectVocalEditApplyRequest): {
          clipId: number;
          derivedSourceId: number;
          sidecarKey: string;
        };
        vocalEditDependencies(): Array<{ derivedPcmAvailable: boolean }>;
        rehydrateVocalEdits(
          originals: ProjectVocalOriginalSource[],
          options?: { cancel?: () => boolean },
        ): unknown;
        toJson(): string;
        native: {
          rehydrateVocalEdits(originals: unknown, cancel: unknown): unknown;
        };
        destroy(): void;
      });
      createVocalEditSession(request: unknown): {
        analysis(): { sourceSha256: string };
        captureRenderSnapshot(): {
          render(request: { requestId: string }): {
            samples: Float32Array;
            token: VocalStateToken;
          };
          destroy(): void;
        };
        exportState(): Uint8Array;
        destroy(): void;
      };
    };
    await runtime.init();
    const sampleRate = 16000;
    const samples = new Float32Array(8192);
    for (let index = 0; index < samples.length; index += 1) {
      samples[index] = Math.sin((2 * Math.PI * 440 * index) / sampleRate) * 0.2;
    }
    const session = runtime.createVocalEditSession({
      samples,
      sampleRate,
      analysis: {
        frameOriginSample: 0,
        samplesPerFrame: 512,
        frameLengthSamples: 2048,
        f0Hz: new Float32Array(16).fill(440),
        voiced: new Uint8Array(16).fill(1),
        algorithmId: 'host',
        algorithmVersion: 1,
      },
    });
    const snapshot = session.captureRenderSnapshot();
    const rendered = snapshot.render({ requestId: '7' });
    const project = new runtime.Project();
    try {
      project.setSampleRate(sampleRate);
      const trackId = project.addTrack({ kind: 'audio', name: 'vocal' });
      const clipId = project.addClip({
        trackId,
        startPpq: 0,
        lengthPpq: 4,
        channels: [samples],
        sampleRate,
      });
      const clip = project.clips()[0];
      project.setAudioSourceMetadata({
        sourceId: clip.sourceId,
        contentHash: `sha256:${session.analysis().sourceSha256}`,
      });
      const applyRequest = {
        clipId,
        expectedSourceId: clip.sourceId,
        expectedSourceSampleRate: sampleRate,
        expectedSourceSampleCount: samples.length,
        expectedSourceSha256: session.analysis().sourceSha256,
        expectedClipLengthPpq: clip.lengthPpq,
        expectedSourceOffsetPpq: clip.sourceOffsetPpq,
        renderedMono: rendered.samples,
        renderedSampleRate: sampleRate,
        renderToken: rendered.token,
        vocalState: session.exportState(),
      };
      const oversizedVocalState = new Uint8Array(32 * 1024 * 1024 + 1);
      let oversizedError: unknown;
      try {
        project.applyVocalEdit({ ...applyRequest, vocalState: oversizedVocalState });
      } catch (error) {
        oversizedError = error;
      }
      expect(oversizedError).toBeInstanceOf(RangeError);
      expect(oversizedError).toMatchObject({
        message: 'Project.applyVocalEdit: vocalState.length must be an integer in [1, 33554432]',
      });

      const malformed = applyRequest.vocalState.slice();
      malformed[0] = 0x58;
      expect(() => project.applyVocalEdit({ ...applyRequest, vocalState: malformed })).toThrow(
        expect.objectContaining({ code: ErrorCode.InvalidFormat }),
      );
      expect(project.vocalEditDependencies()).toEqual([]);
      const applied = project.applyVocalEdit(applyRequest);
      expect(applied).toMatchObject({
        clipId,
        sidecarKey: `libsonare.vocal-edit/clip/${clipId}/take/0`,
      });
      expect(applied.derivedSourceId).not.toBe(clip.sourceId);
      expect(project.vocalEditDependencies()[0].derivedPcmAvailable).toBe(true);

      // A destroy requested from the cancel callback is deferred until the call returns.
      const doomed = runtime.Project.fromJson(project.toJson());
      let destroyThrew: unknown;
      const deferred = doomed.rehydrateVocalEdits([], {
        cancel: () => {
          try {
            doomed.destroy();
          } catch (error) {
            destroyThrew = error;
          }
          return false;
        },
      });
      expect(destroyThrew).toBeUndefined();
      expect(deferred).toHaveLength(1);
      expect(() => doomed.vocalEditDependencies()).toThrow();
      expect(() => doomed.destroy()).not.toThrow();
      expect(project.vocalEditDependencies()[0].derivedPcmAvailable).toBe(true);

      // Embind's raw `native.delete()` bypasses the TypeScript active-call guard.
      // Keep this in a child process: before the native adapter keeps its shared
      // Project alive, returning false lets the C ABI touch the freed object and
      // can terminate the host instead of producing a normal assertion failure.
      const rawDeleteChild = spawnSync(
        process.execPath,
        [
          '--input-type=module',
          '-e',
          `
const runtime = await import(process.env.LIBSONARE_WASM_INDEX);
await runtime.init();
const project = runtime.Project.fromJson(
  Buffer.from(process.env.LIBSONARE_PROJECT_JSON_B64, 'base64').toString('utf8'),
);
const replacements = [];
let callbackCalled = false;
try {
  const result = project.native.rehydrateVocalEdits([], () => {
    callbackCalled = true;
    project.native.delete();
    for (let index = 0; index < 10000; index += 1) {
      replacements.push(new runtime.Project());
    }
    return false;
  });
  if (!callbackCalled || !Array.isArray(result) || result.length !== 1) {
    throw new Error('raw embind callback did not retain the original project');
  }
  process.stdout.write(JSON.stringify({ callbackCalled, resultLength: result.length }));
} finally {
  for (const replacement of replacements) replacement.destroy();
}
`,
        ],
        {
          encoding: 'utf8',
          env: {
            ...process.env,
            LIBSONARE_WASM_INDEX: new URL('../dist/index.js', import.meta.url).href,
            LIBSONARE_PROJECT_JSON_B64: Buffer.from(project.toJson(), 'utf8').toString('base64'),
          },
        },
      );
      expect(rawDeleteChild.status, rawDeleteChild.stderr).toBe(0);
      expect(JSON.parse(rawDeleteChild.stdout)).toMatchObject({ callbackCalled: true });

      // A JS throw through the adapter would skip its C++ destructors, so the
      // raw binding returns the callback's failure and the facade rethrows it.
      const rawCallbackFailure = new Error('raw cancel callback failed');
      const rawOutcome = project.native.rehydrateVocalEdits([], () => {
        throw rawCallbackFailure;
      }) as { cancelFailure?: unknown };
      expect(rawOutcome.cancelFailure).toBe(rawCallbackFailure);
    } finally {
      project.destroy();
      snapshot.destroy();
      session.destroy();
    }
  });

  it('defers a Project destroy requested during rehydration until the outermost call returns', async () => {
    const { init } = await import('../src/index');
    const { Project } = await import('../src/project_class');
    await init();
    const project = new Project();
    const internals = project as unknown as { native: { delete(): void } };
    internals.native.delete();
    let nativeDeletes = 0;
    let deletesSeenInside = -1;
    let nestedResult: unknown;
    const fake: VocalProjectNative & { delete(): void } = {
      applyVocalEdit: () => undefined,
      getVocalEditDependencies: () => [],
      rehydrateVocalEdits: (_originals, cancel) => {
        (cancel as () => boolean)();
        deletesSeenInside = nativeDeletes;
        return [];
      },
      delete: () => {
        nativeDeletes += 1;
      },
    };
    internals.native = fake;

    const result = project.rehydrateVocalEdits([], {
      cancel: () => {
        nestedResult = project.rehydrateVocalEdits([], {
          cancel: () => {
            project.destroy();
            return false;
          },
        });
        expect(nativeDeletes).toBe(0);
        return false;
      },
    });
    expect(result).toEqual([]);
    expect(nestedResult).toEqual([]);
    expect(deletesSeenInside).toBe(0);
    expect(nativeDeletes).toBe(1);
    project.destroy();
    project.destroy();
    expect(nativeDeletes).toBe(1);

    const failing = new Project();
    const failingInternals = failing as unknown as { native: { delete(): void } };
    failingInternals.native.delete();
    let failingDeletes = 0;
    const failure = new Error('native rehydrate failed');
    failingInternals.native = {
      ...fake,
      rehydrateVocalEdits: (_originals: unknown, cancel: unknown) => {
        (cancel as () => boolean)();
        throw failure;
      },
      delete: () => {
        failingDeletes += 1;
      },
    } as VocalProjectNative & { delete(): void };
    expect(() =>
      failing.rehydrateVocalEdits([], {
        cancel: () => {
          failing.destroy();
          return false;
        },
      }),
    ).toThrow(failure);
    expect(failingDeletes).toBe(1);
  });
});
