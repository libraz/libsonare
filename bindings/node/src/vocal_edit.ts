import { ErrorCode, isSonareError, type SonareError } from './errors.js';
import {
  codedError,
  entryClass,
  invalidState,
  mergeRespelled,
  type Respelled,
  refuse,
  refuseType,
  withEntry,
} from './internal/entry.js';
import { withRespelled } from './internal/respell.js';
import { addon } from './native.js';
import type {
  VocalAnalysis,
  VocalApplyRequest,
  VocalCapabilities,
  VocalCreateRequest,
  VocalEditOperation,
  VocalEditResult,
  VocalErrorDetail,
  VocalHistory,
  VocalNote,
  VocalNotesResult,
  VocalPitchEvaluation,
  VocalRange,
  VocalRangeInput,
  VocalReason,
  VocalRenderAsyncRequest,
  VocalRenderJobRequest,
  VocalRenderRequest,
  VocalRenderResult,
  VocalRestoreRequest,
  VocalSessionLimits,
  VocalStateToken,
  VocalTransition,
} from './types_vocal_edit.js';
import {
  assertAudioInput,
  requestObject,
  requireSampleBound,
  resolveSampleBound,
  type SampleBound,
} from './validation.js';

/** A note as the addon returns it: frame indices are decimal strings. */
type NativeNote = Omit<VocalNote, 'analysisFrameStart' | 'analysisFrameEnd'> & {
  analysisFrameStart: string;
  analysisFrameEnd: string;
};

interface NativeNotes {
  notes: NativeNote[];
  transitions: VocalTransition[];
}

/** A render result as the addon returns it: work counters are decimal strings. */
type NativeRenderResult = Omit<
  VocalRenderResult,
  'cacheHitUnits' | 'dryPassedFrames' | 'limitedCorrectionFrames'
> & {
  cacheHitUnits: string;
  dryPassedFrames: string;
  limitedCorrectionFrames: string;
};

interface NativeSession {
  notes(): NativeNotes;
  analysis(): VocalAnalysis;
  capabilities(): VocalCapabilities;
  token(): VocalStateToken;
  revision(): string;
  outputLengthSamples(): number;
  history(): VocalHistory;
  beginEdit(expectedRevision?: string): NativeDraft;
  undo(expectedRevision?: string): VocalEditResult;
  redo(expectedRevision?: string): VocalEditResult;
  evaluatePitch(noteId: number): VocalPitchEvaluation;
  sourceSampleToDestinationSample(noteId: number, sourceSample: number): number;
  destinationSampleToSourceSample(noteId: number, destinationSample: number): number;
  captureRenderSnapshot(): NativeSnapshot;
  exportState(): Uint8Array;
  destroy(): void;
}

interface NativeDraft {
  notes(): NativeNotes;
  token(): VocalStateToken;
  apply(expectedGeneration: string, operations: readonly VocalEditOperation[]): VocalEditResult;
  commit(expectedRevision?: string): VocalEditResult;
  cancel(): void;
  evaluatePitch(noteId: number): VocalPitchEvaluation;
  sourceSampleToDestinationSample(noteId: number, sourceSample: number): number;
  destinationSampleToSourceSample(noteId: number, destinationSample: number): number;
  captureRenderSnapshot(): NativeSnapshot;
  destroy(): void;
}

interface NativeSnapshot {
  outputLengthSamples(): number;
  render(range: { startSample: number; endSample: number }, requestId?: string): NativeRenderResult;
  renderAsync(
    range: { startSample: number; endSample: number },
    requestId?: string,
    signal?: AbortSignal,
  ): Promise<NativeRenderResult>;
  beginRenderJob(range: { startSample: number; endSample: number }, requestId?: string): NativeJob;
  destroy(): void;
}

interface NativeJob {
  next(): { complete: boolean };
  finalize(): NativeRenderResult;
  abort(): void;
  destroy(): void;
}

interface NativeVocalAddon {
  vocalEditAbiVersion?(): number;
  createVocalEditSession(request: object): NativeSession;
  restoreVocalEditSession(request: object): NativeSession;
}

const vocalAddon = addon as unknown as NativeVocalAddon;

/** Reason names indexed by the addon's numeric reason value. */
export const VOCAL_REASONS: readonly VocalReason[] = [
  'none',
  'invalidInput',
  'revisionConflict',
  'sourceMismatch',
  'unsupported',
  'cancelled',
  'counterExhausted',
  'invalidState',
];

function vocalReasonName(value: unknown): VocalReason {
  return typeof value === 'number' ? (VOCAL_REASONS[value] ?? 'invalidState') : 'invalidState';
}

/** Moves the addon's flat reason fields on a vocal failure into one `detail` record. */
function attachVocalDetail(error: unknown): unknown {
  if (typeof error !== 'object' || error === null) {
    return error;
  }
  const raw = error as Record<string, unknown>;
  if (typeof raw.reason !== 'number' || typeof raw.field !== 'string') {
    return error;
  }
  const detail: VocalErrorDetail = Object.freeze({
    reason: vocalReasonName(raw.reason),
    field: raw.field,
    expected: String(raw.expected ?? ''),
    actual: String(raw.actual ?? ''),
    expectedText: String(raw.expectedText ?? ''),
    actualText: String(raw.actualText ?? ''),
  });
  for (const key of ['reason', 'field', 'expected', 'actual', 'expectedText', 'actualText']) {
    delete raw[key];
  }
  Object.defineProperty(error, 'detail', { value: detail, enumerable: true });
  return error;
}

/** Wraps a native handle so every failure, sync or async, carries `detail`. */
function guardNative<T extends object>(native: T): T {
  return new Proxy(native, {
    get(target, prop) {
      const value = Reflect.get(target, prop, target);
      if (typeof value !== 'function') {
        return value;
      }
      return (...args: unknown[]) => {
        try {
          const result = value.apply(target, args);
          return result instanceof Promise
            ? result.catch((error: unknown) => {
                throw attachVocalDetail(error);
              })
            : result;
        } catch (error) {
          throw attachVocalDetail(error);
        }
      };
    },
  });
}

function convertNotesResult(value: NativeNotes): VocalNotesResult {
  return {
    notes: value.notes.map((note) => ({
      ...note,
      analysisFrameStart: Number(note.analysisFrameStart),
      analysisFrameEnd: Number(note.analysisFrameEnd),
    })),
    transitions: value.transitions.slice(),
  };
}

const LIMIT_BYTE_FIELDS = ['maxHistoryBytes', 'maxCacheBytes'] as const;

/** Spells the byte limits as the decimal strings the addon reads. */
function nativeLimits(limits: VocalSessionLimits): object {
  if (typeof limits !== 'object' || limits === null) {
    return limits;
  }
  const out: Record<string, unknown> = { ...limits };
  for (const key of LIMIT_BYTE_FIELDS) {
    const value = limits[key];
    if (value === undefined || value === null) {
      continue;
    }
    if (typeof value !== 'number') {
      throw refuseType(`limits.${key} must be a number`);
    }
    if (!Number.isSafeInteger(value) || value < 0) {
      throw refuse(`limits.${key} must be an integer in [0, ${Number.MAX_SAFE_INTEGER}]`);
    }
    out[key] = String(value);
  }
  return out;
}

/** The request the addon reads: facade-only keys dropped, byte limits spelled as strings. */
function nativeRequest(request: VocalCreateRequest | VocalRestoreRequest): object {
  const out: Record<string, unknown> = { ...request };
  if (request.limits !== undefined && request.limits !== null) {
    out.limits = nativeLimits(request.limits);
  }
  return out;
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
@entryClass('VocalEditSession', ['requireAlive'])
export class VocalEditSession implements Disposable {
  private disposed = false;

  protected constructor(
    private readonly native: NativeSession,
    private readonly sampleRate: number,
  ) {}

  /** Notes and transitions of the current state, copied. */
  notes(): VocalNotesResult {
    this.requireAlive();
    return convertNotesResult(this.native.notes());
  }

  /** The pitch analysis the session was built from, copied. */
  analysis(): VocalAnalysis {
    this.requireAlive();
    return cloneAnalysis(this.native.analysis());
  }

  /** What the loaded engine supports. */
  capabilities(): VocalCapabilities {
    this.requireAlive();
    return { ...this.native.capabilities() };
  }

  /** State token of the session's committed state. */
  get token(): VocalStateToken {
    this.requireAlive();
    return { ...this.native.token() };
  }

  /** Decimal revision of the committed state (64-bit counters exceed 2^53). */
  get revision(): string {
    this.requireAlive();
    return this.native.revision();
  }

  /** Length of the rendered output in samples. */
  get outputLengthSamples(): number {
    this.requireAlive();
    return this.native.outputLengthSamples();
  }

  /** Whether undo and redo have an edit to act on. */
  history(): VocalHistory {
    this.requireAlive();
    return { ...this.native.history() };
  }

  /** Opens a draft; with `expectedRevision` the call is refused if the session has moved on. */
  beginEdit(options: { expectedRevision?: string } = {}): VocalEditDraft {
    this.requireAlive();
    return wrapVocalEditDraft(
      this.native.beginEdit(options.expectedRevision),
      this,
      this.sampleRate,
    );
  }

  /** Reverts the last committed edit. */
  undo(options: { expectedRevision?: string } = {}): VocalEditResult {
    this.requireAlive();
    return this.native.undo(options.expectedRevision);
  }

  /** Reapplies the last undone edit. */
  redo(options: { expectedRevision?: string } = {}): VocalEditResult {
    this.requireAlive();
    return this.native.redo(options.expectedRevision);
  }

  /** Per-sample pitch curves of one note. */
  evaluatePitch(noteId: number): VocalPitchEvaluation {
    this.requireAlive();
    return clonePitchEvaluation(this.native.evaluatePitch(noteId));
  }

  /** Maps a source sample of a note to its output sample. */
  sourceSampleToDestinationSample(noteId: number, sourceSample: number): number {
    this.requireAlive();
    return this.native.sourceSampleToDestinationSample(noteId, sourceSample);
  }

  /** Maps an output sample of a note to its source sample. */
  destinationSampleToSourceSample(noteId: number, destinationSample: number): number {
    this.requireAlive();
    return this.native.destinationSampleToSourceSample(noteId, destinationSample);
  }

  /** Captures an immutable render view of the current state. */
  captureRenderSnapshot(): VocalRenderSnapshot {
    this.requireAlive();
    return wrapVocalRenderSnapshot(this.native.captureRenderSnapshot(), this.sampleRate);
  }

  /** Exports the committed state as bytes for {@link restoreVocalEditSession}. */
  exportState(): Uint8Array {
    this.requireAlive();
    return cloneStateBytes(this.native.exportState());
  }

  /** Releases the native handle; idempotent. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.native.destroy();
    this.disposed = true;
    disposedSessions.add(this);
  }

  /** Releases the object through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }

  private requireAlive(): void {
    if (this.disposed) {
      throw invalidState('VocalEditSession has been destroyed');
    }
  }
}

/** A draft that can be edited and rendered before it is committed. */
@entryClass('VocalEditDraft', ['requireAlive'])
export class VocalEditDraft implements Disposable {
  private disposed = false;
  /** How the draft ended, completing the refusal a later call receives. */
  private endedAs = 'has been destroyed';

  protected constructor(
    private readonly native: NativeDraft,
    private readonly owner: VocalEditSession,
    private readonly sampleRate: number,
  ) {}

  /** Notes and transitions of the current state, copied. */
  notes(): VocalNotesResult {
    this.requireAlive();
    return convertNotesResult(this.native.notes());
  }

  /** State token of the draft, including its current generation. */
  get token(): VocalStateToken {
    this.requireAlive();
    return { ...this.native.token() };
  }

  /** Applies operations; an omitted `expectedGeneration` means the draft's current one. */
  apply(request: VocalApplyRequest): VocalEditResult {
    this.requireAlive();
    requestObject('vocalApply', request, 'request', true);
    const resolved = resolveVocalOperationTimes(request.operations, this.sampleRate);
    return withRespelled(resolved.respelled, () =>
      this.native.apply(
        request.expectedGeneration ?? this.native.token().generation,
        resolved.operations,
      ),
    );
  }

  /** Commits the draft; the draft is unusable afterwards. */
  commit(options: { expectedRevision?: string } = {}): VocalEditResult {
    this.requireAlive();
    const result = this.native.commit(options.expectedRevision);
    this.disposed = true;
    this.endedAs = 'has been committed';
    return result;
  }

  /** Discards the draft. */
  cancel(): void {
    if (this.disposed) {
      return;
    }
    this.native.cancel();
    this.disposed = true;
    this.endedAs = 'has been cancelled';
  }

  /** Per-sample pitch curves of one note. */
  evaluatePitch(noteId: number): VocalPitchEvaluation {
    this.requireAlive();
    return clonePitchEvaluation(this.native.evaluatePitch(noteId));
  }

  /** Maps a source sample of a note to its output sample. */
  sourceSampleToDestinationSample(noteId: number, sourceSample: number): number {
    this.requireAlive();
    return this.native.sourceSampleToDestinationSample(noteId, sourceSample);
  }

  /** Maps an output sample of a note to its source sample. */
  destinationSampleToSourceSample(noteId: number, destinationSample: number): number {
    this.requireAlive();
    return this.native.destinationSampleToSourceSample(noteId, destinationSample);
  }

  /** Captures an immutable render view of the current state. */
  captureRenderSnapshot(): VocalRenderSnapshot {
    this.requireAlive();
    return wrapVocalRenderSnapshot(this.native.captureRenderSnapshot(), this.sampleRate);
  }

  /** Releases the native handle; idempotent. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.native.destroy();
    this.disposed = true;
  }

  /** Releases the object through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }

  private requireAlive(): void {
    if (this.disposed) {
      throw invalidState(`VocalEditDraft ${this.endedAs}`);
    }
    if (disposedSessions.has(this.owner)) {
      throw invalidState('VocalEditDraft has been destroyed');
    }
  }
}

function cancelledError(): SonareError {
  return codedError(ErrorCode.Cancelled, 'The render was aborted');
}

/** Refuses a render whose signal is already aborted, as a `Cancelled` error. */
function throwIfAborted(signal: AbortSignal | undefined): void {
  if (signal?.aborted) {
    throw cancelledError();
  }
}

/** Steps a job to completion, polling `cancel` before each work unit; no partial PCM on a stop. */
function renderUntilCancelled(job: VocalRenderJob, cancel: () => boolean): VocalRenderResult {
  try {
    while (true) {
      if (cancel()) {
        throw cancelledError();
      }
      if (job.next().complete) {
        return job.finalize();
      }
    }
  } catch (error) {
    job.abort();
    throw error;
  } finally {
    job.destroy();
  }
}

/** An immutable render view whose lifetime is independent of its session. */
@entryClass('VocalRenderSnapshot', ['requireAlive', 'resolveRange'])
export class VocalRenderSnapshot implements Disposable {
  private disposed = false;

  protected constructor(
    private readonly native: NativeSnapshot,
    private readonly sampleRate: number,
  ) {}

  /** Length of the rendered output in samples. */
  get outputLengthSamples(): number {
    this.requireAlive();
    return this.native.outputLengthSamples();
  }

  /**
   * Renders the requested range synchronously. A `cancel` hook is polled between
   * work units; when it returns `true` the render stops with a `Cancelled` error.
   */
  render(request: VocalRenderRequest): VocalRenderResult {
    this.requireAlive();
    const cancel = request.cancel;
    if (cancel === undefined) {
      const { range, respelled } = this.resolveRange(request);
      return convertRenderResult(
        withRespelled(respelled, () => this.native.render(range, request.requestId)),
      );
    }
    if (typeof cancel !== 'function') {
      throw refuseType('cancel must be a function');
    }
    return renderUntilCancelled(this.beginRenderJob(request), cancel);
  }

  /** Renders the requested range off the calling thread. */
  async renderAsync(request: VocalRenderAsyncRequest): Promise<VocalRenderResult> {
    this.requireAlive();
    throwIfAborted(request.signal);
    try {
      const { range, respelled } = this.resolveRange(request);
      return convertRenderResult(
        await withRespelled(respelled, () =>
          this.native.renderAsync(range, request.requestId, request.signal),
        ),
      );
    } catch (error) {
      if (
        request.signal?.aborted &&
        !(isSonareError(error) && error.code === ErrorCode.Cancelled)
      ) {
        throw cancelledError();
      }
      throw error;
    }
  }

  /** Starts an incremental render job. */
  beginRenderJob(request: VocalRenderJobRequest): VocalRenderJob {
    this.requireAlive();
    const { range, respelled } = this.resolveRange(request);
    return wrapVocalRenderJob(
      withRespelled(respelled, () => this.native.beginRenderJob(range, request.requestId)),
    );
  }

  /** Releases the native handle; idempotent. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.native.destroy();
    this.disposed = true;
  }

  /** Releases the object through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }

  private requireAlive(): void {
    if (this.disposed) {
      throw invalidState('VocalRenderSnapshot has been destroyed');
    }
  }

  private resolveRange(request: VocalRenderJobRequest): {
    range: VocalRange;
    respelled: Respelled;
  } {
    const range: VocalRangeInput = request.range ?? {};
    const start = resolveSampleBound(
      range.startSample,
      range.startSec,
      this.sampleRate,
      'range.startSample',
      'range.startSec',
    );
    const end = resolveSampleBound(
      range.endSample,
      range.endSec,
      this.sampleRate,
      'range.endSample',
      'range.endSec',
    );
    return {
      range: {
        startSample: start.value ?? 0,
        endSample: end.value ?? this.native.outputLengthSamples(),
      },
      respelled: mergeRespelled(start.respelled, end.respelled),
    };
  }
}

/** Incremental render job. Each next() call advances one native work unit. */
@entryClass('VocalRenderJob', ['requireAlive'])
export class VocalRenderJob implements Disposable {
  private disposed = false;
  /** How the job ended, completing the refusal a later call receives. */
  private endedAs = 'has been destroyed';

  protected constructor(private readonly native: NativeJob) {}

  /** Advances the job by one work unit. */
  next(): { complete: boolean } {
    this.requireAlive();
    return this.native.next();
  }

  /** Collects the finished render and releases the job. */
  finalize(): VocalRenderResult {
    this.requireAlive();
    const result = convertRenderResult(this.native.finalize());
    this.destroy();
    this.endedAs = 'has been finalized';
    return result;
  }

  /** Stops the job. */
  abort(): void {
    if (!this.disposed) {
      this.native.abort();
    }
  }

  /** Releases the native handle; idempotent. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.native.destroy();
    this.disposed = true;
  }

  /** Releases the object through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }

  private requireAlive(): void {
    if (this.disposed) {
      throw invalidState(`VocalRenderJob ${this.endedAs}`);
    }
  }
}

// The public wrappers have protected constructors so their native handle types
// stay out of the package declaration's public surface. These implementation
// subclasses are the only construction path used by the module-level factories
// and by wrapper-to-wrapper methods.
class VocalEditSessionHandle extends VocalEditSession {
  constructor(native: NativeSession, sampleRate: number) {
    super(guardNative(native), sampleRate);
  }
}

class VocalEditDraftHandle extends VocalEditDraft {
  constructor(native: NativeDraft, owner: VocalEditSession, sampleRate: number) {
    super(guardNative(native), owner, sampleRate);
  }
}

class VocalRenderSnapshotHandle extends VocalRenderSnapshot {
  constructor(native: NativeSnapshot, sampleRate: number) {
    super(guardNative(native), sampleRate);
  }
}

class VocalRenderJobHandle extends VocalRenderJob {
  constructor(native: NativeJob) {
    super(guardNative(native));
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

/** Resolves one operation position, refusing an omitted one by name rather than reading it as 0. */
function requiredOperationBound(
  at: string,
  sample: number | undefined,
  sec: number | undefined,
  sampleRate: number,
  sampleName: string,
  secName: string,
): SampleBound & { readonly value: number } {
  return requireSampleBound(sample, sec, sampleRate, `${at}.${sampleName}`, `${at}.${secName}`);
}

/**
 * Fold the seconds spellings of an operation's sample positions into the sample
 * fields the addon reads. Operations that carry no seconds field pass through.
 */
function resolveVocalOperationTimes(
  operations: readonly VocalEditOperation[],
  sampleRate: number,
): { operations: readonly VocalEditOperation[]; respelled: Respelled } {
  if (!Array.isArray(operations)) {
    return { operations, respelled: {} };
  }
  const respellings: Respelled[] = [];
  const resolved = operations.map((operation, index) => {
    const at = `operations[${index}]`;
    const bound = (
      sample: number | undefined,
      sec: number | undefined,
      sampleName: string,
      secName: string,
    ): number => {
      const resolvedBound = requiredOperationBound(
        at,
        sample,
        sec,
        sampleRate,
        sampleName,
        secName,
      );
      respellings.push(resolvedBound.respelled);
      return resolvedBound.value;
    };
    if (operation?.kind === 'setSourceSpan') {
      const { sourceStartSec, sourceEndSec, destinationStartSec, destinationLengthSec, ...rest } =
        operation;
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
        sourceSample: bound(rest.sourceSample, sourceSec, 'sourceSample', 'sourceSec'),
      } as VocalEditOperation;
    }
    return operation;
  });
  return { operations: resolved, respelled: mergeRespelled(...respellings) };
}

/**
 * ABI version of the vocal-edit layout in the loaded addon. `undefined` when the build has no
 * vocal editing; check `capabilities().features` for presence.
 */
export function vocalEditAbiVersion(): number | undefined {
  const version = vocalAddon.vocalEditAbiVersion?.() ?? 0;
  return version > 0 ? version : undefined;
}

/**
 * Analyses `request.samples` and opens an editing session over them.
 */
export function createVocalEditSession(request: VocalCreateRequest): VocalEditSession {
  return withEntry('createVocalEditSession', () => {
    requestObject('createVocalEditSession', request, 'request', true);
    assertAudioInput(request?.samples, request?.sampleRate, request);
    return wrapVocalEditSession(
      guardNative(vocalAddon).createVocalEditSession(nativeRequest(request)),
      request.sampleRate,
    );
  });
}

/**
 * Reopens a session from {@link VocalEditSession.exportState} bytes and the
 * samples they were exported from.
 */
export function restoreVocalEditSession(request: VocalRestoreRequest): VocalEditSession {
  return withEntry('restoreVocalEditSession', () => {
    requestObject('restoreVocalEditSession', request, 'request', true);
    assertAudioInput(request?.samples, request?.sampleRate, request);
    return wrapVocalEditSession(
      guardNative(vocalAddon).restoreVocalEditSession(nativeRequest(request)),
      request.sampleRate,
    );
  });
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

function convertRenderResult(value: NativeRenderResult): VocalRenderResult {
  return {
    ...value,
    cacheHitUnits: Number(value.cacheHitUnits),
    dryPassedFrames: Number(value.dryPassedFrames),
    limitedCorrectionFrames: Number(value.limitedCorrectionFrames),
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
  VocalErrorDetail,
  VocalFormantEdit,
  VocalFormantMode,
  VocalHistory,
  VocalIdChange,
  VocalMergeOperation,
  VocalNote,
  VocalNoteEdit,
  VocalNotesResult,
  VocalPitchEdit,
  VocalPitchEvaluation,
  VocalPitchPoint,
  VocalPitchTarget,
  VocalRange,
  VocalRangeInput,
  VocalReason,
  VocalRemoveTransitionOperation,
  VocalRenderAsyncRequest,
  VocalRenderJobRequest,
  VocalRenderRequest,
  VocalRenderResult,
  VocalResetOperation,
  VocalRestoreRequest,
  VocalSessionLimits,
  VocalSetEditOperation,
  VocalSetSourceSpanOperation,
  VocalSetTransitionOperation,
  VocalSplitOperation,
  VocalStateBytes,
  VocalStateToken,
  VocalTransition,
} from './types_vocal_edit.js';
