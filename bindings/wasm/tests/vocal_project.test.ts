import { spawnSync } from 'node:child_process';
import { describe, expect, it } from 'vitest';
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
    takeId: 0,
    expectedSourceId: 4,
    expectedSourceSampleRate: 16000,
    expectedSourceSampleCount: 4,
    expectedSourceSha256: 'AB'.repeat(32),
    expectedClipLengthPpq: 4,
    expectedSourceOffsetPpq: 0,
    renderedMono: new Float32Array([0.1, -0.2, 0.3, -0.4]),
    renderedSampleRate: 16000,
    renderedStartSample: 0,
    renderToken: token,
    sve1: new Uint8Array([83, 86, 69, 49]),
  };
}

class FakeNative implements VocalProjectNative {
  applyRequest: unknown;
  originalSources: unknown;
  cancel: unknown;

  applyVocalEdit(value: unknown): unknown {
    this.applyRequest = value;
    const source = value as ProjectVocalEditApplyRequest;
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
    const state = source.sve1;
    const result = projectApplyVocalEdit(native, source);

    expect(rendered[0]).toBeCloseTo(0.1);
    expect(state[0]).toBe(83);
    expect((native.applyRequest as ProjectVocalEditApplyRequest).renderToken.revision).toBe('9');
    expect((native.applyRequest as ProjectVocalEditApplyRequest).expectedSourceSha256).toBe(
      'ab'.repeat(32),
    );
    expect(result).toEqual({
      clipId: 3,
      takeId: 0,
      originalSourceId: 4,
      derivedSourceId: 8,
      committedRevision: '9',
      profileId: 1,
      derivedSourceSha256: 'cd'.repeat(32),
      sidecarKey: 'libsonare.vocal-edit/clip/3/take/0',
    });
  });

  it('returns copied dependency rows in native order', () => {
    const dependencies = projectGetVocalEditDependencies(new FakeNative());
    expect(dependencies).toHaveLength(1);
    expect(dependencies[0]).toMatchObject({
      clipId: 3,
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
      expect.objectContaining({ reason: 7, sourceSampleCount: 0 }),
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
    const result = projectRehydrateVocalEdits(native, [original], cancel);

    expect(result[0]).toEqual({ clipId: 3, takeId: 0, derivedSourceId: 8, status: 0, reason: 0 });
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
        () => {
          throw callbackFailure;
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
    expect(() => projectRehydrateVocalEdits(native, [], () => false)).toThrow(callbackFailure);
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
        ...(native.applyVocalEdit(request()) as object),
        committedRevision: '09',
      }),
      getVocalEditDependencies: () => [],
      rehydrateVocalEdits: () => [],
    };
    expect(() => projectApplyVocalEdit(leadingZeroResult, request())).toThrow(RangeError);
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
          rehydrateVocalEdits(originals: unknown[], cancel?: () => boolean): unknown[];
          getVocalEditDependencies(): unknown[];
          delete(): void;
        };
      } & (new () => {
        setSampleRate(sampleRate: number): void;
        addTrack(desc: { kind: string; name: string }): number;
        setAudioSourceMetadata(
          sourceId: number,
          contentHash: string,
          externalStemRole: string,
        ): void;
        addClip(desc: {
          trackId: number;
          startPpq: number;
          lengthPpq: number;
          audio: Float32Array;
          audioChannels: number;
          audioSampleRate: number;
        }): number;
        clipByIndex(index: number): {
          sourceId: number;
          sourceOffsetPpq: number;
          lengthPpq: number;
        };
        applyVocalEdit(request: ProjectVocalEditApplyRequest): {
          clipId: number;
          derivedSourceId: number;
          sidecarKey: string;
        };
        getVocalEditDependencies(): Array<{ derivedPcmAvailable: boolean }>;
        rehydrateVocalEdits(
          originals: ProjectVocalOriginalSource[],
          cancel?: () => boolean,
        ): unknown;
        toJson(): string;
        native: {
          rehydrateVocalEdits(originals: unknown, cancel: unknown): unknown;
        };
        delete(): void;
      });
      createVocalEditSession(request: unknown): {
        analysis(): { sourceSha256: string };
        captureRenderSnapshot(): {
          render(request: { requestId: string }): {
            samples: Float32Array;
            token: VocalStateToken;
          };
          dispose(): void;
        };
        exportState(): Uint8Array;
        dispose(): void;
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
        audio: samples,
        audioChannels: 1,
        audioSampleRate: sampleRate,
      });
      const clip = project.clipByIndex(0);
      project.setAudioSourceMetadata(
        clip.sourceId,
        `sha256:${session.analysis().sourceSha256}`,
        '',
      );
      const applied = project.applyVocalEdit({
        clipId,
        takeId: 0,
        expectedSourceId: clip.sourceId,
        expectedSourceSampleRate: sampleRate,
        expectedSourceSampleCount: samples.length,
        expectedSourceSha256: session.analysis().sourceSha256,
        expectedClipLengthPpq: clip.lengthPpq,
        expectedSourceOffsetPpq: clip.sourceOffsetPpq,
        renderedMono: rendered.samples,
        renderedSampleRate: sampleRate,
        renderedStartSample: 0,
        renderToken: rendered.token,
        sve1: session.exportState(),
      });
      expect(applied).toMatchObject({
        clipId,
        sidecarKey: `libsonare.vocal-edit/clip/${clipId}/take/0`,
      });
      expect(applied.derivedSourceId).not.toBe(clip.sourceId);
      expect(project.getVocalEditDependencies()[0].derivedPcmAvailable).toBe(true);

      // A delete requested from the cancel callback is deferred until the call returns.
      const doomed = runtime.Project.fromJson(project.toJson());
      let deleteThrew: unknown;
      const deferred = doomed.rehydrateVocalEdits([], () => {
        try {
          doomed.delete();
        } catch (error) {
          deleteThrew = error;
        }
        return false;
      });
      expect(deleteThrew).toBeUndefined();
      expect(deferred).toHaveLength(1);
      expect(() => doomed.getVocalEditDependencies()).toThrow();
      expect(() => doomed.delete()).not.toThrow();
      expect(project.getVocalEditDependencies()[0].derivedPcmAvailable).toBe(true);

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
  for (const replacement of replacements) replacement.delete();
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
      project.delete();
      snapshot.dispose();
      session.dispose();
    }
  });

  it('defers a Project delete requested during rehydration until the outermost call returns', async () => {
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

    const result = project.rehydrateVocalEdits([], () => {
      nestedResult = project.rehydrateVocalEdits([], () => {
        project.delete();
        return false;
      });
      expect(nativeDeletes).toBe(0);
      return false;
    });
    expect(result).toEqual([]);
    expect(nestedResult).toEqual([]);
    expect(deletesSeenInside).toBe(0);
    expect(nativeDeletes).toBe(1);
    project.delete();
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
      failing.rehydrateVocalEdits([], () => {
        failing.delete();
        return false;
      }),
    ).toThrow(failure);
    expect(failingDeletes).toBe(1);
  });
});
