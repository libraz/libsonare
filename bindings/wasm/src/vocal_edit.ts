import { getSonareModule } from './module_state';
import type {
  VocalAnalysis,
  VocalApplyRequest,
  VocalCapabilities,
  VocalCreateRequest,
  VocalEditOperation,
  VocalEditResult,
  VocalErrorShape,
  VocalHistoryState,
  VocalJobProgress,
  VocalNote,
  VocalNotesResult,
  VocalPitchEvaluation,
  VocalRange,
  VocalRenderRequest,
  VocalRenderResult,
  VocalRestoreRequest,
  VocalStateToken,
  VocalUint64,
} from './public_types_vocal_edit';
import type { SonareModule } from './sonare.js';

/** Native symbols registered by the full vocal-edit WASM binding. */
export interface VocalWasmExports {
  vocalEditApiVersion: () => number;
  vocalEditAvailable: () => number;
  vocalEditSessionCreate: (samples: Float32Array, sampleRate: number, options: unknown) => number;
  vocalEditSessionRestore: (samples: Float32Array, sampleRate: number, state: Uint8Array) => number;
  vocalEditSessionDestroy: (handle: number) => void;
  vocalEditSessionNotes: (handle: number) => VocalNotesResult;
  vocalEditDraftNotes: (handle: number) => VocalNotesResult;
  vocalEditSessionToken: (handle: number) => VocalStateToken;
  vocalEditDraftToken: (handle: number) => VocalStateToken;
  vocalEditSessionAnalysis: (handle: number) => VocalAnalysis;
  vocalEditSessionCapabilities: (handle: number) => VocalCapabilities;
  vocalEditSessionOutputLength: (handle: number) => number;
  vocalEditSessionHistory: (handle: number) => VocalHistoryState;
  vocalEditSessionBeginEdit: (handle: number, revision: VocalUint64) => number;
  vocalEditDraftApply: (
    handle: number,
    generation: VocalUint64,
    operations: readonly VocalEditOperation[],
  ) => VocalEditResult;
  vocalEditDraftCommit: (handle: number, revision: VocalUint64) => VocalEditResult;
  vocalEditDraftCancel: (handle: number) => void;
  vocalEditDraftDestroy: (handle: number) => void;
  vocalEditSessionApplyHistory: (
    handle: number,
    revision: VocalUint64,
    redo: boolean,
  ) => VocalEditResult;
  vocalEditEvaluatePitch: (handle: number, noteId: number, draft: boolean) => VocalPitchEvaluation;
  vocalEditMapCoordinate: (
    handle: number,
    noteId: number,
    sample: number,
    draft: boolean,
    inverse: boolean,
  ) => number;
  vocalEditCaptureSnapshot: (handle: number, draft: boolean) => number;
  vocalEditSnapshotDestroy: (handle: number) => void;
  vocalEditSnapshotOutputLength: (handle: number) => number;
  vocalEditSnapshotRender: (
    handle: number,
    startSample: number,
    endSample: number,
    requestId: VocalUint64,
  ) => VocalRenderResult;
  vocalEditRenderJobBegin: (
    handle: number,
    startSample: number,
    endSample: number,
    requestId: VocalUint64,
  ) => number;
  vocalEditRenderJobNext: (handle: number) => boolean;
  vocalEditRenderJobFinalize: (handle: number) => VocalRenderResult;
  vocalEditRenderJobAbort: (handle: number) => void;
  vocalEditRenderJobDestroy: (handle: number) => void;
  vocalEditSessionExportState: (handle: number) => Uint8Array;
  vocalEditLastErrorDetail: () => VocalErrorShape & { readonly sequence: VocalUint64 };
}

interface VocalWasmModule extends SonareModule, VocalWasmExports {}

type VocalErrorDetail = VocalErrorShape & { readonly sequence: VocalUint64 };

const vocalErrorFields = [
  'reason',
  'field',
  'expected',
  'actual',
  'expectedText',
  'actualText',
] as const satisfies readonly (keyof VocalErrorShape)[];

let cachedModuleSource: VocalWasmModule | null = null;
let cachedVocalModule: VocalWasmModule | null = null;

function addVocalErrorDetail(error: unknown, detail: VocalErrorDetail): void {
  if (error === null || typeof error !== 'object') {
    return;
  }
  const candidate = error as { name?: unknown; code?: unknown } & Record<string, unknown>;
  if (candidate.name !== 'SonareError' || typeof candidate.code !== 'number') {
    return;
  }
  for (const field of vocalErrorFields) {
    Object.defineProperty(candidate, field, {
      configurable: true,
      enumerable: true,
      value: detail[field],
      writable: false,
    });
  }
}

function module(): VocalWasmModule {
  const source = getSonareModule() as VocalWasmModule;
  if (source === cachedModuleSource && cachedVocalModule !== null) {
    return cachedVocalModule;
  }
  const detailReader = (source as Partial<VocalWasmModule>).vocalEditLastErrorDetail;
  if (typeof detailReader !== 'function') {
    cachedModuleSource = source;
    cachedVocalModule = source;
    return source;
  }
  const functionCache = new Map<PropertyKey, (...args: unknown[]) => unknown>();
  const wrapped = new Proxy(source, {
    get(target, property, receiver) {
      const member = Reflect.get(target, property, receiver);
      if (
        property === 'vocalEditLastErrorDetail' ||
        typeof property !== 'string' ||
        !property.startsWith('vocalEdit') ||
        typeof member !== 'function'
      ) {
        return member;
      }
      const cached = functionCache.get(property);
      if (cached !== undefined) {
        return cached;
      }
      const wrappedMember = (...args: unknown[]): unknown => {
        let beforeSequence: VocalUint64 | undefined;
        try {
          beforeSequence = detailReader.call(target).sequence;
        } catch {
          // An older module may expose the function but not its complete detail
          // shape. Native errors still follow the generic module conversion.
        }
        try {
          return Reflect.apply(member, target, args);
        } catch (error) {
          if (beforeSequence !== undefined) {
            try {
              const detail = detailReader.call(target);
              if (detail.sequence !== beforeSequence) {
                addVocalErrorDetail(error, detail);
              }
            } catch {
              // Keep the original native error if detail decoding is unavailable.
            }
          }
          throw error;
        }
      };
      functionCache.set(property, wrappedMember);
      return wrappedMember;
    },
  }) as VocalWasmModule;
  cachedModuleSource = source;
  cachedVocalModule = wrapped;
  return wrapped;
}

function requireHandle(handle: number, subject: string): void {
  if (!Number.isSafeInteger(handle) || handle <= 0) {
    throw new TypeError(`${subject} has already been disposed`);
  }
}

function requireFinite(value: number, field: string): number {
  if (!Number.isFinite(value)) {
    throw new RangeError(`${field} must be finite`);
  }
  return value;
}

function requireSample(value: number, field: string): number {
  requireFinite(value, field);
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new RangeError(`${field} must be a non-negative safe integer`);
  }
  return value;
}

function requireUint32(value: number, field: string): number {
  requireSample(value, field);
  if (value > 0xffffffff) {
    throw new RangeError(`${field} must fit uint32`);
  }
  return value;
}

function requireSampleRate(value: number): number {
  requireSample(value, 'sampleRate');
  if (value < 8000 || value > 384000) {
    throw new RangeError('sampleRate must be an integer in [8000, 384000]');
  }
  return value;
}

function requireToken(value: string | undefined, field: string, fallback = '0'): VocalUint64 {
  const token = value ?? fallback;
  if (typeof token !== 'string' || !/^(0|[1-9]\d*)$/.test(token)) {
    throw new RangeError(`${field} must be a canonical decimal uint64 string`);
  }
  if (token.length > 20 || (token.length === 20 && token > '18446744073709551615')) {
    throw new RangeError(`${field} must fit uint64`);
  }
  return token;
}

type RevisionArgument = VocalUint64 | { expectedRevision?: VocalUint64 };

function resolveRevision(
  argument: RevisionArgument | undefined,
  fallback: VocalUint64,
): VocalUint64 {
  const value = typeof argument === 'string' ? argument : argument?.expectedRevision;
  return requireToken(value, 'expectedRevision', fallback);
}

function requireRange(range: VocalRange): VocalRange {
  if (!range || typeof range !== 'object') {
    throw new TypeError('range must be an object');
  }
  const startSample = requireSample(range.startSample, 'range.startSample');
  const endSample = requireSample(range.endSample, 'range.endSample');
  if (endSample < startSample) {
    throw new RangeError('range.endSample must be >= startSample');
  }
  return { startSample, endSample };
}

function copyToken(token: VocalStateToken): VocalStateToken {
  return {
    sessionEpoch: requireToken(token.sessionEpoch, 'sessionEpoch'),
    revision: requireToken(token.revision, 'revision'),
    draftId: requireToken(token.draftId, 'draftId'),
    generation: requireToken(token.generation, 'generation'),
    requestId: requireToken(token.requestId, 'requestId'),
    profileId: token.profileId,
  };
}

function copyRange(range: VocalRange): VocalRange {
  return { startSample: range.startSample, endSample: range.endSample };
}

function copyNotes(value: VocalNotesResult): VocalNotesResult {
  const notes: VocalNote[] = Array.from(value.notes, (note) => ({
    ...note,
    amplitude: new Float32Array(note.amplitude),
    edit: {
      ...note.edit,
      pitch: {
        ...note.edit.pitch,
        target:
          note.edit.pitch.target.mode === 'curve'
            ? {
                mode: 'curve' as const,
                points: note.edit.pitch.target.points.map((point) => ({ ...point })),
              }
            : { ...note.edit.pitch.target },
      },
      amplitudeEnvelope: Array.from(note.edit.amplitudeEnvelope),
      formant: { ...note.edit.formant },
    },
  }));
  return {
    notes,
    transitions: Array.from(value.transitions, (transition) => ({ ...transition })),
  };
}

function copyAnalysis(value: VocalAnalysis): VocalAnalysis {
  return {
    frameOriginSample: value.frameOriginSample,
    samplesPerFrame: value.samplesPerFrame,
    frameLengthSamples: value.frameLengthSamples,
    f0Hz: new Float32Array(value.f0Hz),
    voiced: new Uint8Array(value.voiced),
    algorithmId: value.algorithmId,
    algorithmVersion: value.algorithmVersion,
    sourceLengthSamples: value.sourceLengthSamples,
    sampleRate: value.sampleRate,
    sourceSha256: value.sourceSha256,
    analysisSha256: value.analysisSha256,
    fminHz: value.fminHz,
    fmaxHz: value.fmaxHz,
    yinThreshold: value.yinThreshold,
    voicedThreshold: value.voicedThreshold,
    centered: value.centered,
    segmentationThresholdCents: value.segmentationThresholdCents,
    minNoteMs: value.minNoteMs,
    referenceHz: value.referenceHz,
  };
}

function copyPitch(value: VocalPitchEvaluation): VocalPitchEvaluation {
  return {
    sourceSamples: new Float64Array(value.sourceSamples),
    measuredMidi: new Float64Array(value.measuredMidi),
    targetMidi: new Float64Array(value.targetMidi),
    effectiveMidi: new Float64Array(value.effectiveMidi),
    voiced: new Uint8Array(value.voiced),
    hasTarget: new Uint8Array(value.hasTarget),
  };
}

function copyRender(value: VocalRenderResult): VocalRenderResult {
  return {
    samples: new Float32Array(value.samples),
    startSample: value.startSample,
    token: copyToken(value.token),
    processedRanges: Array.from(value.processedRanges, copyRange),
    cacheHitUnits: requireToken(value.cacheHitUnits, 'cacheHitUnits'),
    dryPassedFrames: requireToken(value.dryPassedFrames, 'dryPassedFrames'),
    limitedCorrectionFrames: requireToken(value.limitedCorrectionFrames, 'limitedCorrectionFrames'),
  };
}

function copyEditResult(value: VocalEditResult): VocalEditResult {
  return {
    token: copyToken(value.token),
    dirtyRanges: Array.from(value.dirtyRanges, copyRange),
    idChanges: Array.from(value.idChanges, (change) => ({
      operationIndex: change.operationIndex,
      retiredId: change.retiredId,
      newIds: Array.from(change.newIds),
    })),
  };
}

function validateCreate(request: VocalCreateRequest): void {
  if (!request || typeof request !== 'object') {
    throw new TypeError('request must be an object');
  }
  if (!(request.samples instanceof Float32Array) || request.samples.length === 0) {
    throw new TypeError('samples must be a non-empty Float32Array');
  }
  for (const sample of request.samples) {
    requireFinite(sample, 'samples');
  }
  requireSampleRate(request.sampleRate);
  if (request.outputLengthSamples !== undefined) {
    requireSample(request.outputLengthSamples, 'outputLengthSamples');
  }
  if (request.analysis) {
    const analysis = request.analysis;
    requireFinite(analysis.frameOriginSample, 'analysis.frameOriginSample');
    requireFinite(analysis.samplesPerFrame, 'analysis.samplesPerFrame');
    if (analysis.samplesPerFrame <= 0) {
      throw new RangeError('analysis.samplesPerFrame must be positive');
    }
    requireSample(analysis.frameLengthSamples, 'analysis.frameLengthSamples');
    if (analysis.frameLengthSamples === 0) {
      throw new RangeError('analysis.frameLengthSamples must be positive');
    }
    if (!(analysis.f0Hz instanceof Float32Array) || !(analysis.voiced instanceof Uint8Array)) {
      throw new TypeError('analysis arrays must be Float32Array and Uint8Array');
    }
    if (analysis.f0Hz.length !== analysis.voiced.length) {
      throw new RangeError('analysis arrays must have equal lengths');
    }
  }
}

function normalizeRequest(
  request: VocalRenderRequest,
  outputLengthSamples: number,
): {
  range: VocalRange;
  requestId: VocalUint64;
} {
  const range =
    request.range === undefined
      ? { startSample: 0, endSample: requireSample(outputLengthSamples, 'outputLengthSamples') }
      : requireRange(request.range);
  const requestId = requireToken(request.requestId, 'requestId');
  return { range, requestId };
}

/** Whether this build has the C vocal-edit surface linked. */
export function vocalEditAvailable(): boolean {
  const raw = module();
  return typeof raw.vocalEditAvailable === 'function' && raw.vocalEditAvailable() !== 0;
}

/** Version of the C vocal-edit ABI exposed by the loaded WASM module. */
export function vocalEditApiVersion(): number {
  const raw = module();
  return typeof raw.vocalEditApiVersion === 'function' ? raw.vocalEditApiVersion() : 0;
}

/** Analyse mono PCM, or adopt a host analysis, and open an editing session. */
export function createVocalEditSession(request: VocalCreateRequest): VocalEditSession {
  validateCreate(request);
  if (!vocalEditAvailable()) {
    throw new Error('vocal edit is unavailable in this WASM build');
  }
  return new VocalEditSession(
    module().vocalEditSessionCreate(request.samples, request.sampleRate, request),
  );
}

/** Open a new session from an exported state blob and the same source PCM. */
export function restoreVocalEditSession(request: VocalRestoreRequest): VocalEditSession {
  if (!(request.samples instanceof Float32Array) || request.samples.length === 0) {
    throw new TypeError('samples must be a non-empty Float32Array');
  }
  if (!(request.state instanceof Uint8Array)) {
    throw new TypeError('state must be a Uint8Array');
  }
  for (const sample of request.samples) {
    requireFinite(sample, 'samples');
  }
  requireSampleRate(request.sampleRate);
  if (!vocalEditAvailable()) {
    throw new Error('vocal edit is unavailable in this WASM build');
  }
  return new VocalEditSession(
    module().vocalEditSessionRestore(request.samples, request.sampleRate, request.state),
  );
}

export class VocalEditSession {
  private handle: number;
  private readonly drafts = new Set<VocalEditDraft>();
  private disposed = false;

  /** @internal Instances are created by {@link createVocalEditSession} and {@link restoreVocalEditSession}. */
  constructor(handle: number) {
    requireHandle(handle, 'session');
    this.handle = handle;
  }

  dispose(): void {
    if (this.disposed) {
      return;
    }
    for (const draft of this.drafts) {
      draft.dispose();
    }
    this.drafts.clear();
    module().vocalEditSessionDestroy(this.handle);
    this.handle = 0;
    this.disposed = true;
  }

  delete(): void {
    this.dispose();
  }

  destroy(): void {
    this.dispose();
  }

  private native(): number {
    if (this.disposed) {
      throw new TypeError('session has already been disposed');
    }
    requireHandle(this.handle, 'session');
    return this.handle;
  }

  token(): VocalStateToken {
    return copyToken(module().vocalEditSessionToken(this.native()));
  }

  revision(): VocalUint64 {
    return this.token().revision;
  }

  notes(): VocalNotesResult {
    return copyNotes(module().vocalEditSessionNotes(this.native()));
  }

  analysis(): VocalAnalysis {
    return copyAnalysis(module().vocalEditSessionAnalysis(this.native()));
  }

  capabilities(): VocalCapabilities {
    return { ...module().vocalEditSessionCapabilities(this.native()) };
  }

  outputLengthSamples(): number {
    return requireSample(
      module().vocalEditSessionOutputLength(this.native()),
      'outputLengthSamples',
    );
  }

  history(): VocalHistoryState {
    const value = module().vocalEditSessionHistory(this.native());
    return { canUndo: value.canUndo === true, canRedo: value.canRedo === true };
  }

  beginEdit(options: RevisionArgument = {}): VocalEditDraft {
    const draft = new VocalEditDraft(
      module().vocalEditSessionBeginEdit(this.native(), resolveRevision(options, this.revision())),
      this,
    );
    this.drafts.add(draft);
    return draft;
  }

  undo(options: RevisionArgument = {}): VocalEditResult {
    return copyEditResult(
      module().vocalEditSessionApplyHistory(
        this.native(),
        resolveRevision(options, this.revision()),
        false,
      ),
    );
  }

  redo(options: RevisionArgument = {}): VocalEditResult {
    return copyEditResult(
      module().vocalEditSessionApplyHistory(
        this.native(),
        resolveRevision(options, this.revision()),
        true,
      ),
    );
  }

  evaluatePitch(noteId: number): VocalPitchEvaluation {
    return copyPitch(
      module().vocalEditEvaluatePitch(this.native(), requireUint32(noteId, 'noteId'), false),
    );
  }

  sourceSampleToDestinationSample(noteId: number, sourceSample: number): number {
    requireFinite(sourceSample, 'sourceSample');
    return module().vocalEditMapCoordinate(
      this.native(),
      requireUint32(noteId, 'noteId'),
      sourceSample,
      false,
      false,
    );
  }

  destinationSampleToSourceSample(noteId: number, destinationSample: number): number {
    requireFinite(destinationSample, 'destinationSample');
    return module().vocalEditMapCoordinate(
      this.native(),
      requireUint32(noteId, 'noteId'),
      destinationSample,
      false,
      true,
    );
  }

  captureRenderSnapshot(): VocalRenderSnapshot {
    return new VocalRenderSnapshot(module().vocalEditCaptureSnapshot(this.native(), false));
  }

  exportState(): Uint8Array {
    return new Uint8Array(module().vocalEditSessionExportState(this.native()));
  }

  /** @internal Used by the draft to remove itself from the owner set. */
  _forgetDraft(draft: VocalEditDraft): void {
    this.drafts.delete(draft);
  }
}

export class VocalEditDraft {
  private handle: number;
  private disposed = false;
  private readonly owner: VocalEditSession;

  /** @internal Instances are created by {@link VocalEditSession.beginEdit}. */
  constructor(handle: number, owner: VocalEditSession) {
    requireHandle(handle, 'draft');
    this.handle = handle;
    this.owner = owner;
  }

  private native(): number {
    if (this.disposed) {
      throw new TypeError('draft has already been disposed');
    }
    requireHandle(this.handle, 'draft');
    return this.handle;
  }

  token(): VocalStateToken {
    return copyToken(module().vocalEditDraftToken(this.native()));
  }

  notes(): VocalNotesResult {
    return copyNotes(module().vocalEditDraftNotes(this.native()));
  }

  apply(request: VocalApplyRequest): VocalEditResult {
    const generation = requireToken(request.expectedGeneration, 'expectedGeneration');
    if (!Array.isArray(request.operations)) {
      throw new TypeError('operations must be an array');
    }
    return copyEditResult(
      module().vocalEditDraftApply(this.native(), generation, request.operations),
    );
  }

  commit(options: RevisionArgument = {}): VocalEditResult {
    const result = copyEditResult(
      module().vocalEditDraftCommit(this.native(), resolveRevision(options, this.owner.revision())),
    );
    this.dispose();
    return result;
  }

  cancel(): void {
    if (this.disposed) {
      return;
    }
    module().vocalEditDraftCancel(this.native());
    this.dispose();
  }

  evaluatePitch(noteId: number): VocalPitchEvaluation {
    return copyPitch(
      module().vocalEditEvaluatePitch(this.native(), requireUint32(noteId, 'noteId'), true),
    );
  }

  sourceSampleToDestinationSample(noteId: number, sourceSample: number): number {
    requireFinite(sourceSample, 'sourceSample');
    return module().vocalEditMapCoordinate(
      this.native(),
      requireUint32(noteId, 'noteId'),
      sourceSample,
      true,
      false,
    );
  }

  destinationSampleToSourceSample(noteId: number, destinationSample: number): number {
    requireFinite(destinationSample, 'destinationSample');
    return module().vocalEditMapCoordinate(
      this.native(),
      requireUint32(noteId, 'noteId'),
      destinationSample,
      true,
      true,
    );
  }

  captureRenderSnapshot(): VocalRenderSnapshot {
    return new VocalRenderSnapshot(module().vocalEditCaptureSnapshot(this.native(), true));
  }

  dispose(): void {
    if (this.disposed) {
      return;
    }
    module().vocalEditDraftDestroy(this.handle);
    this.handle = 0;
    this.disposed = true;
    this.owner._forgetDraft(this);
  }

  delete(): void {
    this.dispose();
  }

  destroy(): void {
    this.dispose();
  }
}

export class VocalRenderSnapshot {
  private handle: number;
  private disposed = false;

  /** @internal Instances are created by a session or draft. */
  constructor(handle: number) {
    requireHandle(handle, 'snapshot');
    this.handle = handle;
  }

  private native(): number {
    if (this.disposed) {
      throw new TypeError('snapshot has already been disposed');
    }
    requireHandle(this.handle, 'snapshot');
    return this.handle;
  }

  outputLengthSamples(): number {
    return requireSample(
      module().vocalEditSnapshotOutputLength(this.native()),
      'outputLengthSamples',
    );
  }

  render(request: VocalRenderRequest): VocalRenderResult {
    const handle = this.native();
    if (request.signal?.aborted) {
      throw new DOMException('The render was aborted', 'AbortError');
    }
    const normalized = normalizeRequest(request, this.outputLengthSamples());
    return copyRender(
      module().vocalEditSnapshotRender(
        handle,
        normalized.range.startSample,
        normalized.range.endSample,
        normalized.requestId,
      ),
    );
  }

  beginRenderJob(request: VocalRenderRequest): VocalRenderJob {
    const handle = this.native();
    if (request.signal?.aborted) {
      throw new DOMException('The render was aborted', 'AbortError');
    }
    const normalized = normalizeRequest(request, this.outputLengthSamples());
    return new VocalRenderJob(
      module().vocalEditRenderJobBegin(
        handle,
        normalized.range.startSample,
        normalized.range.endSample,
        normalized.requestId,
      ),
    );
  }

  /**
   * Render one native work unit per macrotask so `request.signal` can stop the
   * job between units. A cancelled job publishes no partial PCM.
   */
  renderAsync(request: VocalRenderRequest): Promise<VocalRenderResult> {
    this.native();
    const signal = request.signal;
    if (signal?.aborted) {
      return Promise.reject(new DOMException('The render was aborted', 'AbortError'));
    }
    const job = this.beginRenderJob(request);
    return (async () => {
      try {
        while (true) {
          if (signal?.aborted) {
            job.abort();
            throw new DOMException('The render was aborted', 'AbortError');
          }
          if (job.next().complete) {
            return job.finalize();
          }
          await new Promise<void>((resolve) => setTimeout(resolve, 0));
        }
      } catch (error) {
        job.abort();
        throw error;
      } finally {
        job.dispose();
      }
    })();
  }

  dispose(): void {
    if (this.disposed) {
      return;
    }
    module().vocalEditSnapshotDestroy(this.handle);
    this.handle = 0;
    this.disposed = true;
  }

  delete(): void {
    this.dispose();
  }

  destroy(): void {
    this.dispose();
  }
}

export class VocalRenderJob {
  private handle: number;
  private disposed = false;

  /** @internal Instances are created by {@link VocalRenderSnapshot.beginRenderJob}. */
  constructor(handle: number) {
    requireHandle(handle, 'render job');
    this.handle = handle;
  }

  private native(): number {
    if (this.disposed) {
      throw new TypeError('render job has already been disposed');
    }
    requireHandle(this.handle, 'render job');
    return this.handle;
  }

  next(): VocalJobProgress {
    return { complete: module().vocalEditRenderJobNext(this.native()) };
  }

  finalize(): VocalRenderResult {
    const result = copyRender(module().vocalEditRenderJobFinalize(this.native()));
    this.dispose();
    return result;
  }

  abort(): void {
    if (!this.disposed) {
      module().vocalEditRenderJobAbort(this.handle);
    }
  }

  dispose(): void {
    if (this.disposed) {
      return;
    }
    module().vocalEditRenderJobDestroy(this.handle);
    this.handle = 0;
    this.disposed = true;
  }

  delete(): void {
    this.dispose();
  }

  destroy(): void {
    this.dispose();
  }
}

export type {
  VocalAnalysis,
  VocalCapabilities,
  VocalCreateRequest,
  VocalEditResult,
  VocalHistory,
} from './public_types_vocal_edit';
