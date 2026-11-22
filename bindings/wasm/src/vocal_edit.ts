import { ErrorCode } from './errors.js';
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
import { assertHandleLive } from './internal/handle_guard.js';
import { withRespelled } from './internal/respell.js';
import { attachVocalErrorDetail, vocalReasonFromCode } from './internal/vocal_error_detail.js';
import { expectedRevisionOption } from './internal/vocal_revision.js';
import { getSonareModule } from './module_state.js';
import type {
  VocalAnalysis,
  VocalApplyRequest,
  VocalCapabilities,
  VocalCreateRequest,
  VocalEditOperation,
  VocalEditResult,
  VocalHistory,
  VocalJobProgress,
  VocalNote,
  VocalNotesResult,
  VocalPitchEvaluation,
  VocalRange,
  VocalRangeInput,
  VocalRenderAsyncRequest,
  VocalRenderJobRequest,
  VocalRenderRequest,
  VocalRenderResult,
  VocalRestoreRequest,
  VocalRevisionOptions,
  VocalSessionLimits,
  VocalStateToken,
} from './public_types_vocal_edit.js';
import type { SonareModule } from './sonare.js';
import {
  assertAudioInput,
  requestObject,
  requireSampleBound,
  resolveSampleBound,
  type SampleBound,
} from './validation.js';

/** Native symbols registered by the full vocal-edit WASM binding. */
export interface VocalWasmExports {
  vocalEditAbiVersion: () => number;
  vocalEditSessionCreate: (samples: Float32Array, sampleRate: number, options: unknown) => number;
  vocalEditSessionRestore: (
    samples: Float32Array,
    sampleRate: number,
    state: Uint8Array,
    options: unknown,
  ) => number;
  vocalEditSessionDestroy: (handle: number) => void;
  vocalEditSessionNotes: (handle: number) => VocalNotesResult;
  vocalEditDraftNotes: (handle: number) => VocalNotesResult;
  vocalEditSessionToken: (handle: number) => VocalStateToken;
  vocalEditDraftToken: (handle: number) => VocalStateToken;
  vocalEditSessionAnalysis: (handle: number) => VocalAnalysis;
  vocalEditSessionCapabilities: (handle: number) => VocalCapabilities;
  vocalEditSessionOutputLength: (handle: number) => number;
  vocalEditSessionHistory: (handle: number) => VocalHistory;
  vocalEditSessionBeginEdit: (handle: number, revision: string) => number;
  vocalEditDraftApply: (
    handle: number,
    generation: string,
    operations: readonly VocalEditOperation[],
  ) => VocalEditResult;
  vocalEditDraftCommit: (handle: number, revision: string) => VocalEditResult;
  vocalEditDraftCancel: (handle: number) => void;
  vocalEditDraftDestroy: (handle: number) => void;
  vocalEditSessionApplyHistory: (
    handle: number,
    revision: string,
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
    requestId: string,
  ) => VocalRenderResult;
  vocalEditRenderJobBegin: (
    handle: number,
    startSample: number,
    endSample: number,
    requestId: string,
  ) => number;
  vocalEditRenderJobNext: (handle: number) => boolean;
  vocalEditRenderJobFinalize: (handle: number) => VocalRenderResult;
  vocalEditRenderJobAbort: (handle: number) => void;
  vocalEditRenderJobDestroy: (handle: number) => void;
  vocalEditSessionExportState: (handle: number) => Uint8Array;
  vocalEditLastErrorDetail: () => NativeVocalErrorDetail;
}

interface VocalWasmModule extends SonareModule, VocalWasmExports {}

/** The last-error record as the native module returns it. */
interface NativeVocalErrorDetail {
  readonly sequence: string;
  readonly reason: number;
  readonly field: string;
  readonly expected: string;
  readonly actual: string;
  readonly expectedText: string;
  readonly actualText: string;
}

let cachedModuleSource: VocalWasmModule | null = null;
let cachedVocalModule: VocalWasmModule | null = null;

function addVocalErrorDetail(error: unknown, detail: NativeVocalErrorDetail): void {
  if (error === null || typeof error !== 'object') {
    return;
  }
  const candidate = error as { name?: unknown; code?: unknown };
  if (candidate.name !== 'SonareError' || typeof candidate.code !== 'number') {
    return;
  }
  const reason = vocalReasonFromCode(detail.reason);
  if (reason === undefined) {
    return;
  }
  attachVocalErrorDetail(candidate, {
    reason,
    field: detail.field,
    expected: detail.expected,
    actual: detail.actual,
    expectedText: detail.expectedText,
    actualText: detail.actualText,
  });
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
        let beforeSequence: string | undefined;
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

function requireFinite(value: number, field: string): number {
  if (!Number.isFinite(value)) {
    throw refuse(`${field} must be finite`);
  }
  return value;
}

function requireSample(value: number, field: string): number {
  requireFinite(value, field);
  if (!Number.isSafeInteger(value) || value < 0) {
    throw refuse(`${field} must be an integer in [0, ${Number.MAX_SAFE_INTEGER}]`);
  }
  return value;
}

function requireUint32(value: number, field: string): number {
  requireSample(value, field);
  if (value > 0xffffffff) {
    throw refuse(`${field} must fit uint32`);
  }
  return value;
}

function requireToken(value: string | undefined, field: string, fallback = '0'): string {
  const token = value ?? fallback;
  if (typeof token !== 'string' || !/^(0|[1-9]\d*)$/.test(token)) {
    throw refuse(`${field} must be a canonical decimal uint64 string`);
  }
  if (token.length > 20 || (token.length === 20 && token > '18446744073709551615')) {
    throw refuse(`${field} must fit uint64`);
  }
  return token;
}

/** A 64-bit counter the native module reports as decimal text, as a safe-integer count. */
function requireCount(value: unknown, field: string): number {
  const count = typeof value === 'string' ? Number(value) : value;
  if (typeof count !== 'number' || !Number.isSafeInteger(count) || count < 0) {
    throw refuse(`${field} must be an integer in [0, ${Number.MAX_SAFE_INTEGER}]`);
  }
  return count;
}

function resolveRevision(options: VocalRevisionOptions, fallback: string): string {
  return requireToken(expectedRevisionOption(options), 'expectedRevision', fallback);
}

function resolveRange(
  range: VocalRangeInput,
  sampleRate: number,
  outputLengthSamples: number,
): { range: VocalRange; respelled: Respelled } {
  if (!range || typeof range !== 'object') {
    throw refuseType('range must be an object');
  }
  const start = resolveSampleBound(
    range.startSample,
    range.startSec,
    sampleRate,
    'range.startSample',
    'range.startSec',
  );
  const end = resolveSampleBound(
    range.endSample,
    range.endSec,
    sampleRate,
    'range.endSample',
    'range.endSec',
  );
  const startSample = requireSample(start.value ?? 0, 'range.startSample');
  const endSample = requireSample(end.value ?? outputLengthSamples, 'range.endSample');
  return {
    range: { startSample, endSample },
    respelled: mergeRespelled(start.respelled, end.respelled),
  };
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
 * fields the core reads. Operations that carry no seconds field pass through.
 */
function resolveOperationTimes(
  operations: readonly VocalEditOperation[],
  sampleRate: number,
): { operations: readonly VocalEditOperation[]; respelled: Respelled } {
  const respellings: Respelled[] = [];
  const bound = (
    at: string,
    sample: number | undefined,
    sec: number | undefined,
    sampleName: string,
    secName: string,
  ): number => {
    const resolvedBound = requiredOperationBound(at, sample, sec, sampleRate, sampleName, secName);
    respellings.push(resolvedBound.respelled);
    return resolvedBound.value;
  };
  const resolved = operations.map((operation, index) => {
    const at = `operations[${index}]`;
    if (operation?.kind === 'setSourceSpan') {
      const { sourceStartSec, sourceEndSec, destinationStartSec, destinationLengthSec, ...rest } =
        operation;
      return {
        ...rest,
        sourceStartSample: bound(
          at,
          rest.sourceStartSample,
          sourceStartSec,
          'sourceStartSample',
          'sourceStartSec',
        ),
        sourceEndSample: bound(
          at,
          rest.sourceEndSample,
          sourceEndSec,
          'sourceEndSample',
          'sourceEndSec',
        ),
        destinationStartSample: bound(
          at,
          rest.destinationStartSample,
          destinationStartSec,
          'destinationStartSample',
          'destinationStartSec',
        ),
        destinationLengthSamples: bound(
          at,
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
        sourceSample: bound(at, rest.sourceSample, sourceSec, 'sourceSample', 'sourceSec'),
      } as VocalEditOperation;
    }
    return operation;
  });
  return { operations: resolved, respelled: mergeRespelled(...respellings) };
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
    analysisFrameStart: requireCount(note.analysisFrameStart, 'analysisFrameStart'),
    analysisFrameEnd: requireCount(note.analysisFrameEnd, 'analysisFrameEnd'),
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
    cacheHitUnits: requireCount(value.cacheHitUnits, 'cacheHitUnits'),
    dryPassedFrames: requireCount(value.dryPassedFrames, 'dryPassedFrames'),
    limitedCorrectionFrames: requireCount(value.limitedCorrectionFrames, 'limitedCorrectionFrames'),
  };
}

/** Validates `limits` and returns the form the native module reads (byte limits as decimal text). */
function nativeSessionLimits(
  limits: VocalSessionLimits | null | undefined,
): Record<string, unknown> | undefined {
  if (limits === undefined || limits === null) {
    return undefined;
  }
  if (typeof limits !== 'object' || Array.isArray(limits)) {
    throw refuseType('limits must be an object');
  }
  const native: Record<string, unknown> = { ...limits };
  for (const field of ['maxHistoryBytes', 'maxCacheBytes'] as const) {
    const bytes = limits[field];
    if (bytes !== undefined) {
      if (typeof bytes !== 'number') {
        throw refuseType(`limits.${field} must be a number`);
      }
      native[field] = String(requireSample(bytes, `limits.${field}`));
    }
  }
  return native;
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
    throw refuseType('request must be an object');
  }
  assertAudioInput(request.samples, request.sampleRate, request);
  if (request.outputLengthSamples !== undefined) {
    requireSample(request.outputLengthSamples, 'outputLengthSamples');
  }
  if (request.analysis !== undefined && request.analysis !== null) {
    const analysis = request.analysis;
    if (typeof analysis !== 'object' || Array.isArray(analysis)) {
      throw refuseType('analysis must be an object');
    }
    if (analysis.algorithmId !== undefined) {
      if (typeof analysis.algorithmId !== 'string') {
        throw refuseType('analysis.algorithmId must be a string');
      }
      if (analysis.algorithmId.includes('\0')) {
        throw refuse('analysis.algorithmId must not contain NUL');
      }
    }
    requireFinite(analysis.frameOriginSample, 'analysis.frameOriginSample');
    requireFinite(analysis.samplesPerFrame, 'analysis.samplesPerFrame');
    if (analysis.samplesPerFrame <= 0) {
      throw refuse('analysis.samplesPerFrame must be greater than 0');
    }
    requireSample(analysis.frameLengthSamples, 'analysis.frameLengthSamples');
    if (analysis.frameLengthSamples === 0) {
      throw refuse(
        `analysis.frameLengthSamples must be an integer in [1, ${Number.MAX_SAFE_INTEGER}]`,
      );
    }
    if (!(analysis.f0Hz instanceof Float32Array) || !(analysis.voiced instanceof Uint8Array)) {
      throw refuseType('analysis arrays must be Float32Array and Uint8Array');
    }
    if (analysis.f0Hz.length !== analysis.voiced.length) {
      throw refuse('analysis arrays must have equal lengths');
    }
  }
}

function normalizeRequest(
  request: VocalRenderJobRequest,
  outputLengthSamples: number,
  sampleRate: number,
): {
  range: VocalRange;
  requestId: string;
  respelled: Respelled;
} {
  const resolved =
    request.range === undefined
      ? {
          range: {
            startSample: 0,
            endSample: requireSample(outputLengthSamples, 'outputLengthSamples'),
          },
          respelled: {},
        }
      : resolveRange(
          request.range,
          sampleRate,
          requireSample(outputLengthSamples, 'outputLengthSamples'),
        );
  const requestId = requireToken(request.requestId, 'requestId');
  return { range: resolved.range, requestId, respelled: resolved.respelled };
}

/** Advances a render job to completion, polling `cancel` before each work unit. */
function renderUntilCancelled(job: VocalRenderJob, cancel: () => boolean): VocalRenderResult {
  try {
    while (true) {
      if (cancel()) {
        throw codedError(ErrorCode.Cancelled, 'The render was cancelled');
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

/** Whether this build has the C vocal-edit surface linked. */
function vocalEditAvailable(): boolean {
  return module().capabilities().features.vocalEdit === true;
}

/**
 * ABI version of the vocal-edit layout in the loaded module. `undefined` when
 * the build has no vocal edit; check `capabilities().features` for presence.
 */
export function vocalEditAbiVersion(): number | undefined {
  return withEntry('vocalEditAbiVersion', () => {
    const raw = module();
    const version = typeof raw.vocalEditAbiVersion === 'function' ? raw.vocalEditAbiVersion() : 0;
    return version > 0 ? version : undefined;
  });
}

/** Analyse mono PCM, or adopt a host analysis, and open an editing session. */
export function createVocalEditSession(request: VocalCreateRequest): VocalEditSession {
  return withEntry('createVocalEditSession', () => {
    requestObject('createVocalEditSession', request, 'request', true);
    validateCreate(request);
    const limits = nativeSessionLimits(request.limits);
    if (!vocalEditAvailable()) {
      throw codedError(ErrorCode.NotSupported, 'vocal edit is unavailable in this WASM build');
    }
    const native = limits === undefined ? request : { ...request, limits };
    return new VocalEditSessionHandle(
      module().vocalEditSessionCreate(request.samples, request.sampleRate, native),
      request.sampleRate,
    );
  });
}

/** Open a new session from an exported state blob and the same source PCM. */
export function restoreVocalEditSession(request: VocalRestoreRequest): VocalEditSession {
  return withEntry('restoreVocalEditSession', () => {
    requestObject('restoreVocalEditSession', request, 'request', true);
    assertAudioInput(request.samples, request.sampleRate, request);
    if (!(request.state instanceof Uint8Array)) {
      throw refuseType('state must be a Uint8Array');
    }
    const limits = nativeSessionLimits(request.limits);
    if (!vocalEditAvailable()) {
      throw codedError(ErrorCode.NotSupported, 'vocal edit is unavailable in this WASM build');
    }
    const native = limits === undefined ? request : { ...request, limits };
    return new VocalEditSessionHandle(
      module().vocalEditSessionRestore(request.samples, request.sampleRate, request.state, native),
      request.sampleRate,
    );
  });
}

/** A vocal editing session. Disposing it also disposes every live draft it began. */
@entryClass('VocalEditSession', ['native'])
export class VocalEditSession {
  private handle: number;
  private readonly drafts = new Set<VocalEditDraft>();
  private disposed = false;
  private readonly sampleRate: number;

  /** @internal Instances are created by {@link createVocalEditSession} and {@link restoreVocalEditSession}. */
  protected constructor(handle: number, sampleRate: number) {
    this.handle = handle;
    this.sampleRate = sampleRate;
  }

  /** Releases the session and every draft it began; calling it again does nothing. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    for (const draft of this.drafts) {
      draft.destroy();
    }
    this.drafts.clear();
    module().vocalEditSessionDestroy(this.handle);
    this.handle = 0;
    this.disposed = true;
  }

  /** Releases the handle through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }

  private native(): number {
    assertHandleLive(this.disposed, 'VocalEditSession');
    return this.handle;
  }

  /** The current committed state token. */
  get token(): VocalStateToken {
    return copyToken(module().vocalEditSessionToken(this.native()));
  }

  /** The committed revision, a decimal string (64-bit counter). */
  get revision(): string {
    return this.token.revision;
  }

  /** The committed notes and transitions. */
  notes(): VocalNotesResult {
    return copyNotes(module().vocalEditSessionNotes(this.native()));
  }

  /** The pitch analysis the session was built from. */
  analysis(): VocalAnalysis {
    return copyAnalysis(module().vocalEditSessionAnalysis(this.native()));
  }

  /** What the loaded engine supports. */
  capabilities(): VocalCapabilities {
    return { ...module().vocalEditSessionCapabilities(this.native()) };
  }

  /** Length of the rendered output in samples. */
  get outputLengthSamples(): number {
    return requireSample(
      module().vocalEditSessionOutputLength(this.native()),
      'outputLengthSamples',
    );
  }

  /** Whether undo and redo have anything to act on. */
  history(): VocalHistory {
    const value = module().vocalEditSessionHistory(this.native());
    return { canUndo: value.canUndo === true, canRedo: value.canRedo === true };
  }

  /** Opens a draft for batching edits. `expectedRevision` defaults to the current revision; a mismatch is refused with a `revisionConflict` detail. */
  beginEdit(options: VocalRevisionOptions = {}): VocalEditDraft {
    const draft = new VocalEditDraftHandle(
      module().vocalEditSessionBeginEdit(this.native(), resolveRevision(options, this.revision)),
      this,
      this.sampleRate,
    );
    this.drafts.add(draft);
    return draft;
  }

  /** Reverts the last commit. `expectedRevision` defaults to the current revision. */
  undo(options: VocalRevisionOptions = {}): VocalEditResult {
    return copyEditResult(
      module().vocalEditSessionApplyHistory(
        this.native(),
        resolveRevision(options, this.revision),
        false,
      ),
    );
  }

  /** Re-applies the last undone commit. `expectedRevision` defaults to the current revision. */
  redo(options: VocalRevisionOptions = {}): VocalEditResult {
    return copyEditResult(
      module().vocalEditSessionApplyHistory(
        this.native(),
        resolveRevision(options, this.revision),
        true,
      ),
    );
  }

  /** Per-frame pitch of one committed note. */
  evaluatePitch(noteId: number): VocalPitchEvaluation {
    return copyPitch(
      module().vocalEditEvaluatePitch(this.native(), requireUint32(noteId, 'noteId'), false),
    );
  }

  /** Maps a source sample of a note to its position in the output. */
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

  /** Maps an output sample of a note back to its source position. */
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

  /** Freezes the committed state for rendering; the snapshot is unaffected by later edits. */
  captureRenderSnapshot(): VocalRenderSnapshot {
    return new VocalRenderSnapshotHandle(
      module().vocalEditCaptureSnapshot(this.native(), false),
      this.sampleRate,
    );
  }

  /** Serialises the edit state; pass it to {@link restoreVocalEditSession} with the same source PCM. */
  exportState(): Uint8Array {
    return new Uint8Array(module().vocalEditSessionExportState(this.native()));
  }

  /** @internal Used by the draft to remove itself from the owner set. */
  _forgetDraft(draft: VocalEditDraft): void {
    this.drafts.delete(draft);
  }
}

/** An open batch of edits on a {@link VocalEditSession}. Commit it to publish the edits or cancel it to drop them. */
@entryClass('VocalEditDraft', ['native'])
export class VocalEditDraft {
  private handle: number;
  private disposed = false;
  /** How the draft ended, completing the refusal a later call receives. */
  private endedAs = 'has been destroyed';
  private readonly owner: VocalEditSession;
  private readonly sampleRate: number;

  /** @internal Instances are created by {@link VocalEditSession.beginEdit}. */
  protected constructor(handle: number, owner: VocalEditSession, sampleRate: number) {
    this.handle = handle;
    this.owner = owner;
    this.sampleRate = sampleRate;
  }

  private native(): number {
    if (this.disposed) {
      throw invalidState(`VocalEditDraft ${this.endedAs}`);
    }
    return this.handle;
  }

  /** The draft's current state token. */
  get token(): VocalStateToken {
    return copyToken(module().vocalEditDraftToken(this.native()));
  }

  /** The committed revision the draft was begun from, a decimal string (64-bit counter). */
  get revision(): string {
    return this.token.revision;
  }

  /** The draft's notes and transitions, including uncommitted edits. */
  notes(): VocalNotesResult {
    return copyNotes(module().vocalEditDraftNotes(this.native()));
  }

  /** Applies the operations as one batch. Omitting `expectedGeneration` uses the draft's current generation. */
  apply(request: VocalApplyRequest): VocalEditResult {
    requestObject('vocalApply', request, 'request', true);
    const generation =
      request.expectedGeneration === undefined
        ? this.token.generation
        : requireToken(request.expectedGeneration, 'expectedGeneration');
    if (!Array.isArray(request.operations)) {
      throw refuseType('operations must be an array');
    }
    const resolved = resolveOperationTimes(request.operations, this.sampleRate);
    return copyEditResult(
      withRespelled(resolved.respelled, () =>
        module().vocalEditDraftApply(this.native(), generation, resolved.operations),
      ),
    );
  }

  /** Commits the draft and releases it. `expectedRevision` defaults to the revision the draft was begun from. */
  commit(options: VocalRevisionOptions = {}): VocalEditResult {
    const result = copyEditResult(
      module().vocalEditDraftCommit(this.native(), resolveRevision(options, this.owner.revision)),
    );
    this.destroy();
    this.endedAs = 'has been committed';
    return result;
  }

  /** Discards the draft and releases it. */
  cancel(): void {
    if (this.disposed) {
      return;
    }
    module().vocalEditDraftCancel(this.native());
    this.destroy();
    this.endedAs = 'has been cancelled';
  }

  /** Per-frame pitch of one note including uncommitted edits. */
  evaluatePitch(noteId: number): VocalPitchEvaluation {
    return copyPitch(
      module().vocalEditEvaluatePitch(this.native(), requireUint32(noteId, 'noteId'), true),
    );
  }

  /** Maps a source sample of a note to its position in the output, including uncommitted edits. */
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

  /** Maps an output sample of a note back to its source position, including uncommitted edits. */
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

  /** Freezes the draft state for rendering. */
  captureRenderSnapshot(): VocalRenderSnapshot {
    return new VocalRenderSnapshotHandle(
      module().vocalEditCaptureSnapshot(this.native(), true),
      this.sampleRate,
    );
  }

  /** Releases the draft without committing it; calling it again does nothing. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    module().vocalEditDraftDestroy(this.handle);
    this.handle = 0;
    this.disposed = true;
    this.owner._forgetDraft(this);
  }

  /** Releases the handle through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

/** A frozen copy of session or draft state that can be rendered repeatedly. */
@entryClass('VocalRenderSnapshot', ['native'])
export class VocalRenderSnapshot {
  private handle: number;
  private disposed = false;
  private readonly sampleRate: number;

  /** @internal Instances are created by a session or draft. */
  protected constructor(handle: number, sampleRate: number) {
    this.handle = handle;
    this.sampleRate = sampleRate;
  }

  private native(): number {
    assertHandleLive(this.disposed, 'VocalRenderSnapshot');
    return this.handle;
  }

  /** Length of the rendered output in samples. */
  get outputLengthSamples(): number {
    return requireSample(
      module().vocalEditSnapshotOutputLength(this.native()),
      'outputLengthSamples',
    );
  }

  /** Renders the requested range synchronously. */
  render(request: VocalRenderRequest): VocalRenderResult {
    const handle = this.native();
    const cancel = request.cancel;
    if (cancel !== undefined) {
      if (typeof cancel !== 'function') {
        throw refuseType('cancel must be a function');
      }
      return renderUntilCancelled(this.beginRenderJob(request), cancel);
    }
    const normalized = normalizeRequest(request, this.outputLengthSamples, this.sampleRate);
    return copyRender(
      withRespelled(normalized.respelled, () =>
        module().vocalEditSnapshotRender(
          handle,
          normalized.range.startSample,
          normalized.range.endSample,
          normalized.requestId,
        ),
      ),
    );
  }

  /** Starts an incremental render that the caller advances with {@link VocalRenderJob.next}. */
  beginRenderJob(request: VocalRenderJobRequest): VocalRenderJob {
    const handle = this.native();
    const normalized = normalizeRequest(request, this.outputLengthSamples, this.sampleRate);
    return new VocalRenderJobHandle(
      withRespelled(normalized.respelled, () =>
        module().vocalEditRenderJobBegin(
          handle,
          normalized.range.startSample,
          normalized.range.endSample,
          normalized.requestId,
        ),
      ),
    );
  }

  /**
   * Render one native work unit per macrotask so `request.signal` can stop the
   * job between units. A cancelled job publishes no partial PCM. Every failure rejects:
   * an aborted signal, before or during the render, with a `Cancelled` SonareError.
   */
  renderAsync(request: VocalRenderAsyncRequest): Promise<VocalRenderResult> {
    let job: VocalRenderJob;
    try {
      this.native();
      if (request.signal?.aborted) {
        throw codedError(ErrorCode.Cancelled, 'The render was cancelled');
      }
      job = this.beginRenderJob(request);
    } catch (error) {
      return Promise.reject(error);
    }
    const signal = request.signal;
    return (async () => {
      try {
        while (true) {
          if (signal?.aborted) {
            job.abort();
            throw codedError(ErrorCode.Cancelled, 'The render was cancelled');
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
        job.destroy();
      }
    })();
  }

  /** Releases the snapshot; calling it again does nothing. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    module().vocalEditSnapshotDestroy(this.handle);
    this.handle = 0;
    this.disposed = true;
  }

  /** Releases the handle through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

/** An incremental render started with {@link VocalRenderSnapshot.beginRenderJob}. */
@entryClass('VocalRenderJob', ['native'])
export class VocalRenderJob {
  private handle: number;
  private disposed = false;
  /** How the job ended, completing the refusal a later call receives. */
  private endedAs = 'has been destroyed';

  /** @internal Instances are created by {@link VocalRenderSnapshot.beginRenderJob}. */
  protected constructor(handle: number) {
    this.handle = handle;
  }

  private native(): number {
    if (this.disposed) {
      throw invalidState(`VocalRenderJob ${this.endedAs}`);
    }
    return this.handle;
  }

  /** Runs one work unit and reports whether the job is ready to finalise. */
  next(): VocalJobProgress {
    return { complete: module().vocalEditRenderJobNext(this.native()) };
  }

  /** Collects the rendered PCM and releases the job. */
  finalize(): VocalRenderResult {
    const result = copyRender(module().vocalEditRenderJobFinalize(this.native()));
    this.destroy();
    this.endedAs = 'has been finalized';
    return result;
  }

  /** Stops the job; no partial PCM is published. */
  abort(): void {
    if (!this.disposed) {
      module().vocalEditRenderJobAbort(this.handle);
    }
  }

  /** Releases the job; calling it again does nothing. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    module().vocalEditRenderJobDestroy(this.handle);
    this.handle = 0;
    this.disposed = true;
  }

  /** Releases the handle through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

export type {
  VocalAnalysis,
  VocalCapabilities,
  VocalCreateRequest,
  VocalEditResult,
  VocalHistory,
} from './public_types_vocal_edit.js';

// The public wrappers have protected constructors so a raw handle never appears in the
// package declaration; these subclasses are the module's only construction path.
class VocalEditSessionHandle extends VocalEditSession {
  constructor(handle: number, sampleRate: number) {
    super(handle, sampleRate);
  }
}

class VocalEditDraftHandle extends VocalEditDraft {
  constructor(handle: number, owner: VocalEditSession, sampleRate: number) {
    super(handle, owner, sampleRate);
  }
}

class VocalRenderSnapshotHandle extends VocalRenderSnapshot {
  constructor(handle: number, sampleRate: number) {
    super(handle, sampleRate);
  }
}

class VocalRenderJobHandle extends VocalRenderJob {
  constructor(handle: number) {
    super(handle);
  }
}
