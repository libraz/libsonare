import { ErrorCode, SonareError } from './errors.js';
import { addon } from './native.js';
import type {
  VocalAnalysis,
  VocalApplyRequest,
  VocalCapabilities,
  VocalCreateRequest,
  VocalEditOperation,
  VocalEditResult,
  VocalHistory,
  VocalNote,
  VocalPitchEvaluation,
  VocalRange,
  VocalRangeInput,
  VocalRenderRequest,
  VocalRenderResult,
  VocalRestoreRequest,
  VocalStateToken,
  VocalTransition,
  VocalUint64,
} from './types_vocal_edit.js';
import { assertAudioInput, resolveSampleBound } from './validation.js';

interface NativeSession {
  notes(): { notes: VocalNote[]; transitions: VocalTransition[] };
  analysis(): VocalAnalysis;
  capabilities(): VocalCapabilities;
  token(): VocalStateToken;
  revision(): VocalUint64;
  outputLengthSamples(): number;
  history(): VocalHistory;
  beginEdit(expectedRevision?: VocalUint64): NativeDraft;
  undo(expectedRevision?: VocalUint64): VocalEditResult;
  redo(expectedRevision?: VocalUint64): VocalEditResult;
  evaluatePitch(noteId: number): VocalPitchEvaluation;
  sourceSampleToDestinationSample(noteId: number, sourceSample: number): number;
  destinationSampleToSourceSample(noteId: number, destinationSample: number): number;
  captureRenderSnapshot(): NativeSnapshot;
  exportState(): Uint8Array;
  destroy(): void;
}

interface NativeDraft {
  notes(): { notes: VocalNote[]; transitions: VocalTransition[] };
  token(): VocalStateToken;
  apply(
    expectedGeneration: VocalUint64,
    operations: readonly VocalEditOperation[],
  ): VocalEditResult;
  commit(expectedRevision?: VocalUint64): VocalEditResult;
  cancel(): void;
  evaluatePitch(noteId: number): VocalPitchEvaluation;
  sourceSampleToDestinationSample(noteId: number, sourceSample: number): number;
  destinationSampleToSourceSample(noteId: number, destinationSample: number): number;
  captureRenderSnapshot(): NativeSnapshot;
  destroy(): void;
}

interface NativeSnapshot {
  outputLengthSamples(): number;
  render(
    range: { startSample: number; endSample: number },
    requestId?: VocalUint64,
  ): VocalRenderResult;
  renderAsync(
    range: { startSample: number; endSample: number },
    requestId?: VocalUint64,
    signal?: AbortSignal,
  ): Promise<VocalRenderResult>;
  beginRenderJob(
    range: { startSample: number; endSample: number },
    requestId?: VocalUint64,
  ): NativeJob;
  destroy(): void;
}

interface NativeJob {
  next(): { complete: boolean };
  finalize(): VocalRenderResult;
  abort(): void;
  destroy(): void;
}

interface NativeVocalAddon {
  vocalEditAvailable(): boolean;
  vocalEditApiVersion(): number;
  createVocalEditSession(request: VocalCreateRequest): NativeSession;
  restoreVocalEditSession(request: VocalRestoreRequest): NativeSession;
}

const vocalAddon = addon as unknown as NativeVocalAddon;

function cloneNotesResult(value: { notes: VocalNote[]; transitions: VocalTransition[] }): {
  notes: VocalNote[];
  transitions: VocalTransition[];
} {
  return {
    notes: value.notes.slice(),
    transitions: value.transitions.slice(),
  };
}

function cloneAnalysis(value: VocalAnalysis): VocalAnalysis {
  return {
    ...value,
    f0Hz: new Float32Array(value.f0Hz),
    voiced: new Uint8Array(value.voiced),
  };
}

function cloneStateBytes(value: Uint8Array): Uint8Array {
  return new Uint8Array(value);
}

// Sessions whose handle has been closed; their drafts fail as disposed handles.
const disposedSessions = new WeakSet<VocalEditSession>();

/**
 * A native-backed monophonic vocal note editing session.
 *
 * Disposing a session also disposes every live draft it began.
 */
export class VocalEditSession implements Disposable {
  private disposed = false;

  protected constructor(
    private readonly native: NativeSession,
    private readonly sampleRate: number,
  ) {}

  notes(): { notes: VocalNote[]; transitions: VocalTransition[] } {
    this.requireAlive();
    return cloneNotesResult(this.native.notes());
  }

  analysis(): VocalAnalysis {
    this.requireAlive();
    return cloneAnalysis(this.native.analysis());
  }

  capabilities(): VocalCapabilities {
    this.requireAlive();
    return { ...this.native.capabilities() };
  }

  token(): VocalStateToken {
    this.requireAlive();
    return { ...this.native.token() };
  }

  revision(): VocalUint64 {
    this.requireAlive();
    return this.native.revision();
  }

  outputLengthSamples(): number {
    this.requireAlive();
    return this.native.outputLengthSamples();
  }

  history(): VocalHistory {
    this.requireAlive();
    return { ...this.native.history() };
  }

  beginEdit(options: { expectedRevision?: VocalUint64 } = {}): VocalEditDraft {
    this.requireAlive();
    return wrapVocalEditDraft(
      this.native.beginEdit(options.expectedRevision),
      this,
      this.sampleRate,
    );
  }

  undo(options: { expectedRevision?: VocalUint64 } = {}): VocalEditResult {
    this.requireAlive();
    return this.native.undo(options.expectedRevision);
  }

  redo(options: { expectedRevision?: VocalUint64 } = {}): VocalEditResult {
    this.requireAlive();
    return this.native.redo(options.expectedRevision);
  }

  evaluatePitch(noteId: number): VocalPitchEvaluation {
    this.requireAlive();
    return clonePitchEvaluation(this.native.evaluatePitch(noteId));
  }

  sourceSampleToDestinationSample(noteId: number, sourceSample: number): number {
    this.requireAlive();
    return this.native.sourceSampleToDestinationSample(noteId, sourceSample);
  }

  destinationSampleToSourceSample(noteId: number, destinationSample: number): number {
    this.requireAlive();
    return this.native.destinationSampleToSourceSample(noteId, destinationSample);
  }

  captureRenderSnapshot(): VocalRenderSnapshot {
    this.requireAlive();
    return wrapVocalRenderSnapshot(this.native.captureRenderSnapshot(), this.sampleRate);
  }

  exportState(): Uint8Array {
    this.requireAlive();
    return cloneStateBytes(this.native.exportState());
  }

  dispose(): void {
    if (this.disposed) {
      return;
    }
    this.native.destroy();
    this.disposed = true;
    disposedSessions.add(this);
  }

  destroy(): void {
    this.dispose();
  }

  [Symbol.dispose](): void {
    this.dispose();
  }

  private requireAlive(): void {
    if (this.disposed) {
      throw new SonareError(
        ErrorCode.InvalidState,
        'InvalidState',
        'VocalEditSession has been disposed',
      );
    }
  }
}

/** A draft that can be edited and rendered before it is committed. */
export class VocalEditDraft implements Disposable {
  private disposed = false;

  protected constructor(
    private readonly native: NativeDraft,
    private readonly owner: VocalEditSession,
    private readonly sampleRate: number,
  ) {}

  notes(): { notes: VocalNote[]; transitions: VocalTransition[] } {
    this.requireAlive();
    return cloneNotesResult(this.native.notes());
  }

  token(): VocalStateToken {
    this.requireAlive();
    return { ...this.native.token() };
  }

  apply(request: VocalApplyRequest): VocalEditResult {
    this.requireAlive();
    return this.native.apply(
      request.expectedGeneration,
      resolveVocalOperationTimes(request.operations, this.sampleRate),
    );
  }

  commit(options: { expectedRevision?: VocalUint64 } = {}): VocalEditResult {
    this.requireAlive();
    const result = this.native.commit(options.expectedRevision);
    this.disposed = true;
    return result;
  }

  cancel(): void {
    if (this.disposed) {
      return;
    }
    this.native.cancel();
    this.disposed = true;
  }

  evaluatePitch(noteId: number): VocalPitchEvaluation {
    this.requireAlive();
    return clonePitchEvaluation(this.native.evaluatePitch(noteId));
  }

  sourceSampleToDestinationSample(noteId: number, sourceSample: number): number {
    this.requireAlive();
    return this.native.sourceSampleToDestinationSample(noteId, sourceSample);
  }

  destinationSampleToSourceSample(noteId: number, destinationSample: number): number {
    this.requireAlive();
    return this.native.destinationSampleToSourceSample(noteId, destinationSample);
  }

  captureRenderSnapshot(): VocalRenderSnapshot {
    this.requireAlive();
    return wrapVocalRenderSnapshot(this.native.captureRenderSnapshot(), this.sampleRate);
  }

  dispose(): void {
    if (this.disposed) {
      return;
    }
    this.native.destroy();
    this.disposed = true;
  }

  destroy(): void {
    this.dispose();
  }

  [Symbol.dispose](): void {
    this.dispose();
  }

  private requireAlive(): void {
    if (this.disposed || disposedSessions.has(this.owner)) {
      throw new SonareError(
        ErrorCode.InvalidState,
        'InvalidState',
        'VocalEditDraft has been disposed',
      );
    }
  }
}

/** An immutable render view whose lifetime is independent of its session. */
export class VocalRenderSnapshot implements Disposable {
  private disposed = false;

  protected constructor(
    private readonly native: NativeSnapshot,
    private readonly sampleRate: number,
  ) {}

  outputLengthSamples(): number {
    this.requireAlive();
    return this.native.outputLengthSamples();
  }

  render(request: VocalRenderRequest): VocalRenderResult {
    this.requireAlive();
    return cloneRenderResult(this.native.render(this.resolveRange(request), request.requestId));
  }

  renderAsync(request: VocalRenderRequest): Promise<VocalRenderResult> {
    this.requireAlive();
    return this.native
      .renderAsync(this.resolveRange(request), request.requestId, request.signal)
      .then(cloneRenderResult);
  }

  beginRenderJob(request: VocalRenderRequest): VocalRenderJob {
    this.requireAlive();
    return wrapVocalRenderJob(
      this.native.beginRenderJob(this.resolveRange(request), request.requestId),
    );
  }

  dispose(): void {
    if (this.disposed) {
      return;
    }
    this.native.destroy();
    this.disposed = true;
  }

  destroy(): void {
    this.dispose();
  }

  [Symbol.dispose](): void {
    this.dispose();
  }

  private requireAlive(): void {
    if (this.disposed) {
      throw new SonareError(
        ErrorCode.InvalidState,
        'InvalidState',
        'VocalRenderSnapshot has been disposed',
      );
    }
  }

  private resolveRange(request: VocalRenderRequest): VocalRange {
    const range: VocalRangeInput = request.range ?? {};
    const start = resolveSampleBound(
      'VocalRenderSnapshot.render',
      range.startSample,
      range.startSec,
      this.sampleRate,
      'range.startSample',
      'range.startSec',
    );
    const end = resolveSampleBound(
      'VocalRenderSnapshot.render',
      range.endSample,
      range.endSec,
      this.sampleRate,
      'range.endSample',
      'range.endSec',
    );
    return {
      startSample: start ?? 0,
      endSample: end ?? this.native.outputLengthSamples(),
    };
  }
}

/** Incremental render job. Each next() call advances one native work unit. */
export class VocalRenderJob implements Disposable {
  private disposed = false;

  protected constructor(private readonly native: NativeJob) {}

  next(): { complete: boolean } {
    this.requireAlive();
    return this.native.next();
  }

  finalize(): VocalRenderResult {
    this.requireAlive();
    const result = cloneRenderResult(this.native.finalize());
    this.dispose();
    return result;
  }

  abort(): void {
    if (!this.disposed) {
      this.native.abort();
    }
  }

  dispose(): void {
    if (this.disposed) {
      return;
    }
    this.native.destroy();
    this.disposed = true;
  }

  destroy(): void {
    this.dispose();
  }

  [Symbol.dispose](): void {
    this.dispose();
  }

  private requireAlive(): void {
    if (this.disposed) {
      throw new SonareError(
        ErrorCode.InvalidState,
        'InvalidState',
        'VocalRenderJob has been disposed',
      );
    }
  }
}

// The public wrappers have protected constructors so their native handle types
// stay out of the package declaration's public surface. These implementation
// subclasses are the only construction path used by the module-level factories
// and by wrapper-to-wrapper methods.
class VocalEditSessionHandle extends VocalEditSession {
  constructor(native: NativeSession, sampleRate: number) {
    super(native, sampleRate);
  }
}

class VocalEditDraftHandle extends VocalEditDraft {
  constructor(native: NativeDraft, owner: VocalEditSession, sampleRate: number) {
    super(native, owner, sampleRate);
  }
}

class VocalRenderSnapshotHandle extends VocalRenderSnapshot {
  constructor(native: NativeSnapshot, sampleRate: number) {
    super(native, sampleRate);
  }
}

class VocalRenderJobHandle extends VocalRenderJob {
  constructor(native: NativeJob) {
    super(native);
  }
}

function wrapVocalEditSession(native: NativeSession, sampleRate: number): VocalEditSession {
  return new VocalEditSessionHandle(native, sampleRate);
}

function wrapVocalEditDraft(
  native: NativeDraft,
  owner: VocalEditSession,
  sampleRate: number,
): VocalEditDraft {
  return new VocalEditDraftHandle(native, owner, sampleRate);
}

function wrapVocalRenderSnapshot(native: NativeSnapshot, sampleRate: number): VocalRenderSnapshot {
  return new VocalRenderSnapshotHandle(native, sampleRate);
}

function wrapVocalRenderJob(native: NativeJob): VocalRenderJob {
  return new VocalRenderJobHandle(native);
}

/**
 * Fold the seconds spellings of an operation's sample positions into the sample
 * fields the addon reads. Operations that carry no seconds field pass through.
 */
function resolveVocalOperationTimes(
  operations: readonly VocalEditOperation[],
  sampleRate: number,
): readonly VocalEditOperation[] {
  if (!Array.isArray(operations)) {
    return operations;
  }
  return operations.map((operation, index) => {
    const at = `operations[${index}]`;
    const fn = 'VocalEditDraft.apply';
    if (operation?.kind === 'setSourceSpan') {
      const { sourceStartSec, sourceEndSec, destinationStartSec, destinationLengthSec, ...rest } =
        operation;
      const bound = (
        sample: number | undefined,
        sec: number | undefined,
        sampleName: string,
        secName: string,
      ) =>
        resolveSampleBound(fn, sample, sec, sampleRate, `${at}.${sampleName}`, `${at}.${secName}`);
      return {
        ...rest,
        sourceStartSample: bound(
          rest.sourceStartSample,
          sourceStartSec,
          'sourceStartSample',
          'sourceStartSec',
        ),
        sourceEndSample: bound(
          rest.sourceEndSample,
          sourceEndSec,
          'sourceEndSample',
          'sourceEndSec',
        ),
        destinationStartSample: bound(
          rest.destinationStartSample,
          destinationStartSec,
          'destinationStartSample',
          'destinationStartSec',
        ),
        destinationLengthSamples: bound(
          rest.destinationLengthSamples,
          destinationLengthSec,
          'destinationLengthSamples',
          'destinationLengthSec',
        ),
      } as VocalEditOperation;
    }
    if (operation?.kind === 'split') {
      const { sourceSec, ...rest } = operation;
      return {
        ...rest,
        sourceSample: resolveSampleBound(
          fn,
          rest.sourceSample,
          sourceSec,
          sampleRate,
          `${at}.sourceSample`,
          `${at}.sourceSec`,
        ),
      } as VocalEditOperation;
    }
    return operation;
  });
}

/** Whether this build has the C vocal-edit surface linked. */
export function vocalEditAvailable(): boolean {
  return vocalAddon.vocalEditAvailable();
}

/** Version of the C vocal-edit ABI exposed by the loaded addon. */
export function vocalEditApiVersion(): number {
  return vocalAddon.vocalEditApiVersion();
}

export function createVocalEditSession(request: VocalCreateRequest): VocalEditSession {
  assertAudioInput('createVocalEditSession', request?.samples, request?.sampleRate);
  return wrapVocalEditSession(vocalAddon.createVocalEditSession(request), request.sampleRate);
}

export function restoreVocalEditSession(request: VocalRestoreRequest): VocalEditSession {
  assertAudioInput('restoreVocalEditSession', request?.samples, request?.sampleRate);
  return wrapVocalEditSession(vocalAddon.restoreVocalEditSession(request), request.sampleRate);
}

function clonePitchEvaluation(value: VocalPitchEvaluation): VocalPitchEvaluation {
  return {
    sourceSamples: new Float64Array(value.sourceSamples),
    measuredMidi: new Float64Array(value.measuredMidi),
    targetMidi: new Float64Array(value.targetMidi),
    effectiveMidi: new Float64Array(value.effectiveMidi),
    voiced: new Uint8Array(value.voiced),
    hasTarget: new Uint8Array(value.hasTarget),
  };
}

function cloneRenderResult(value: VocalRenderResult): VocalRenderResult {
  return {
    ...value,
    samples: new Float32Array(value.samples),
    processedRanges: value.processedRanges.map((range) => ({ ...range })),
    token: { ...value.token },
  };
}

export type {
  VocalAnalysis,
  VocalAnalysisInput,
  VocalApplyRequest,
  VocalCapabilities,
  VocalCreateOptions,
  VocalCreateRequest,
  VocalEditOperation,
  VocalEditResult,
  VocalFormantEdit,
  VocalFormantMode,
  VocalHistory,
  VocalIdChange,
  VocalMergeOperation,
  VocalNote,
  VocalNoteEdit,
  VocalPitchEdit,
  VocalPitchEvaluation,
  VocalPitchPoint,
  VocalPitchTarget,
  VocalRange,
  VocalRangeInput,
  VocalRemoveTransitionOperation,
  VocalRenderRequest,
  VocalRenderResult,
  VocalResetOperation,
  VocalRestoreRequest,
  VocalSetEditOperation,
  VocalSetSourceSpanOperation,
  VocalSetTransitionOperation,
  VocalSplitOperation,
  VocalStateBytes,
  VocalStateToken,
  VocalTransition,
  VocalUint64,
} from './types_vocal_edit.js';
