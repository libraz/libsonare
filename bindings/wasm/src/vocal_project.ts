import { refuse, refuseType, refuseUnknownKey } from './internal/entry.js';
import { vocalReasonFromCode } from './internal/vocal_error_detail.js';
import type { VocalReason, VocalStateToken } from './public_types_vocal_edit.js';
import type {
  ProjectVocalEditApplyRequest,
  ProjectVocalEditApplyResult,
  ProjectVocalEditDependency,
  ProjectVocalOriginalSource,
  ProjectVocalRehydrateItem,
  ProjectVocalRehydrateOptions,
  ProjectVocalRehydrateStatus,
} from './public_types_vocal_project.js';
import { assertSampleRate, requestObject } from './validation.js';

/** The embind Project object is deliberately structural here. */
export interface VocalProjectNative {
  applyVocalEdit: (request: unknown) => unknown;
  getVocalEditDependencies: () => unknown;
  rehydrateVocalEdits: (originals: unknown, cancel?: unknown) => unknown;
}

const UINT32_MAX = 0xffff_ffff;
const PROJECT_ID_MAX = UINT32_MAX - 1;
const UINT64_MAX = '18446744073709551615';

function requireObject(value: unknown, field: string): Record<string, unknown> {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw refuseType(`${field} must be an object`);
  }
  return value as Record<string, unknown>;
}

function uint32(value: unknown, field: string, allowZero = true): number {
  if (
    typeof value !== 'number' ||
    !Number.isSafeInteger(value) ||
    value < (allowZero ? 0 : 1) ||
    value > PROJECT_ID_MAX
  ) {
    throw refuse(`${field} must be a uint32 project id`);
  }
  return value;
}

function requestUint32(value: unknown, field: string, allowZero = true): number {
  if (typeof value !== 'number') {
    throw refuseType(`${field} must be an integer`);
  }
  return uint32(value, field, allowZero);
}

/** A take id of an apply request: a number, and never `0`, which is spelt by leaving it out. */
function requestTakeId(value: unknown): number {
  if (typeof value !== 'number') {
    throw refuseType('takeId must be an integer');
  }
  if (value === 0) {
    throw refuse('takeId must not be 0; omit it for the base source');
  }
  return requestUint32(value, 'takeId', false);
}

function nativeUint32(value: unknown, field: string): number {
  if (
    typeof value !== 'number' ||
    !Number.isSafeInteger(value) ||
    value < 0 ||
    value > UINT32_MAX
  ) {
    throw refuse(`${field} must be a native uint32`);
  }
  return value;
}

function nativeInt64(value: unknown, field: string): number {
  if (typeof value !== 'number' || !Number.isSafeInteger(value)) {
    throw refuse(`${field} must be a safe integer`);
  }
  return value;
}

/** A numeric request field whose range the native side states; only the type is checked here. */
function requestNumber(value: unknown, field: string): number {
  if (typeof value !== 'number') {
    throw refuseType(`${field} must be an integer`);
  }
  return value;
}

function finite(value: unknown, field: string): number {
  if (typeof value !== 'number') {
    throw refuseType(`${field} must be a number`);
  }
  if (!Number.isFinite(value)) {
    throw refuse(`${field} must be finite`);
  }
  return value;
}

function decimalUint64(value: unknown, field: string): string {
  if (typeof value !== 'string' || !/^(0|[1-9]\d*)$/.test(value)) {
    throw refuse(`${field} must be a canonical decimal uint64 string`);
  }
  if (
    value.length > UINT64_MAX.length ||
    (value.length === UINT64_MAX.length && value > UINT64_MAX)
  ) {
    throw refuse(`${field} must fit uint64`);
  }
  return value;
}

function hexSha256(value: unknown, field: string): string {
  if (typeof value !== 'string' || !/^[0-9a-fA-F]{64}$/.test(value)) {
    throw refuseType(`${field} must be 64 hexadecimal characters`);
  }
  return value.toLowerCase();
}

function finiteMono(value: unknown, field: string): Float32Array {
  if (!(value instanceof Float32Array) || value.length === 0) {
    throw refuseType(`${field} must be a non-empty Float32Array`);
  }
  for (let index = 0; index < value.length; index += 1) {
    if (!Number.isFinite(value[index])) {
      throw refuse(`${field} contains a non-finite sample at index ${index}`);
    }
  }
  return new Float32Array(value);
}

function vocalState(value: unknown): Uint8Array {
  if (!(value instanceof Uint8Array) || value.length === 0) {
    throw refuseType('vocalState must be a non-empty Uint8Array');
  }
  return new Uint8Array(value);
}

function copyToken(value: unknown): VocalStateToken {
  const source = requireObject(value, 'renderToken');
  return {
    sessionEpoch: decimalUint64(source.sessionEpoch, 'renderToken.sessionEpoch'),
    revision: decimalUint64(source.revision, 'renderToken.revision'),
    draftId: decimalUint64(source.draftId, 'renderToken.draftId'),
    generation: decimalUint64(source.generation, 'renderToken.generation'),
    requestId: decimalUint64(source.requestId, 'renderToken.requestId'),
    profileId: requestUint32(source.profileId, 'renderToken.profileId'),
  };
}

/** Request in the shape the native Project reads: take 0 is the base source and the state key is `sve1`. */
interface NativeApplyRequest {
  clipId: number;
  takeId: number;
  expectedSourceId: number;
  expectedSourceSampleRate: number;
  expectedSourceSampleCount: number;
  expectedSourceSha256: string;
  expectedClipLengthPpq: number;
  expectedSourceOffsetPpq: number;
  renderedMono: Float32Array;
  renderedSampleRate: number;
  renderedStartSample: 0;
  renderToken: VocalStateToken;
  sve1: Uint8Array;
}

function copyApplyRequest(request: ProjectVocalEditApplyRequest): NativeApplyRequest {
  const source = requireObject(request, 'request');
  requestObject('applyProjectVocalEdit', request, 'request', true);
  const renderedMono = finiteMono(source.renderedMono, 'renderedMono');
  const expectedSourceSampleCount = requestNumber(
    source.expectedSourceSampleCount,
    'expectedSourceSampleCount',
  );
  if (renderedMono.length !== expectedSourceSampleCount) {
    throw refuse(
      `renderedMono length must equal expectedSourceSampleCount (${renderedMono.length} != ${expectedSourceSampleCount})`,
    );
  }
  const expectedSourceSampleRate = finite(
    source.expectedSourceSampleRate,
    'expectedSourceSampleRate',
  );
  assertSampleRate(expectedSourceSampleRate);
  const renderedSampleRate = finite(source.renderedSampleRate, 'renderedSampleRate');
  assertSampleRate(renderedSampleRate, 'renderedSampleRate');
  if (renderedSampleRate !== expectedSourceSampleRate) {
    throw refuse('renderedSampleRate must equal expectedSourceSampleRate');
  }
  const renderToken = copyToken(source.renderToken);
  if (renderToken.draftId !== '0' || renderToken.generation !== '0') {
    throw refuse('renderToken must be a committed session token');
  }
  return {
    clipId: requestNumber(source.clipId, 'clipId'),
    takeId:
      source.takeId === undefined || source.takeId === null ? 0 : requestTakeId(source.takeId),
    expectedSourceId: requestNumber(source.expectedSourceId, 'expectedSourceId'),
    expectedSourceSampleRate,
    expectedSourceSampleCount,
    expectedSourceSha256: hexSha256(source.expectedSourceSha256, 'expectedSourceSha256'),
    expectedClipLengthPpq: finite(source.expectedClipLengthPpq, 'expectedClipLengthPpq'),
    expectedSourceOffsetPpq: finite(source.expectedSourceOffsetPpq, 'expectedSourceOffsetPpq'),
    renderedMono,
    renderedSampleRate,
    renderedStartSample: 0,
    renderToken,
    sve1: vocalState(source.vocalState),
  };
}

/** A native take id of 0 means the base source, which the facade reports as no take. */
function optionalTake(takeId: number): { takeId?: number } {
  return takeId === 0 ? {} : { takeId };
}

function reasonToken(code: number, field: string): VocalReason {
  const reason = vocalReasonFromCode(code);
  if (reason === undefined) {
    throw refuse(`${field} is not a known vocal reason`);
  }
  return reason;
}

function copyApplyResult(value: unknown): ProjectVocalEditApplyResult {
  const source = requireObject(value, 'applyVocalEdit result');
  return {
    clipId: uint32(source.clipId, 'result.clipId', false),
    ...optionalTake(uint32(source.takeId, 'result.takeId')),
    originalSourceId: uint32(source.originalSourceId, 'result.originalSourceId', false),
    derivedSourceId: uint32(source.derivedSourceId, 'result.derivedSourceId', false),
    committedRevision: decimalUint64(source.committedRevision, 'result.committedRevision'),
    profileId: uint32(source.profileId, 'result.profileId'),
    derivedSourceSha256: hexSha256(source.derivedSourceSha256, 'result.derivedSourceSha256'),
    sidecarKey:
      typeof source.sidecarKey === 'string'
        ? source.sidecarKey
        : (() => {
            throw refuseType('result.sidecarKey must be a string');
          })(),
  };
}

function copyDependency(value: unknown): ProjectVocalEditDependency {
  const source = requireObject(value, 'vocal edit dependency');
  const reason = nativeUint32(source.reason, 'dependency.reason');
  return {
    clipId: nativeUint32(source.clipId, 'dependency.clipId'),
    ...optionalTake(nativeUint32(source.takeId, 'dependency.takeId')),
    originalSourceId: nativeUint32(source.originalSourceId, 'dependency.originalSourceId'),
    derivedSourceId: nativeUint32(source.derivedSourceId, 'dependency.derivedSourceId'),
    sourceSampleRate: nativeUint32(source.sourceSampleRate, 'dependency.sourceSampleRate'),
    profileId: nativeUint32(source.profileId, 'dependency.profileId'),
    sourceSampleCount: nativeInt64(source.sourceSampleCount, 'dependency.sourceSampleCount'),
    committedRevision: decimalUint64(source.committedRevision, 'dependency.committedRevision'),
    originalSourceSha256: hexSha256(source.originalSourceSha256, 'dependency.originalSourceSha256'),
    derivedSourceSha256: hexSha256(source.derivedSourceSha256, 'dependency.derivedSourceSha256'),
    originalPcmAvailable: source.originalPcmAvailable === true,
    derivedPcmAvailable: source.derivedPcmAvailable === true,
    reason: reasonToken(reason, 'dependency.reason'),
    sidecarKey:
      typeof source.sidecarKey === 'string'
        ? source.sidecarKey
        : (() => {
            throw refuseType('dependency.sidecarKey must be a string');
          })(),
  };
}

const REHYDRATE_STATUSES: readonly ProjectVocalRehydrateStatus[] = [
  'rehydrated',
  'alreadyReady',
  'unresolved',
];

function copyRehydrateItem(value: unknown): ProjectVocalRehydrateItem {
  const source = requireObject(value, 'vocal rehydrate item');
  const status = REHYDRATE_STATUSES[source.status as number];
  if (status === undefined) {
    throw refuse('rehydrate.status is not a known rehydrate status');
  }
  const reason = nativeUint32(source.reason, 'rehydrate.reason');
  return {
    clipId: nativeUint32(source.clipId, 'rehydrate.clipId'),
    ...optionalTake(nativeUint32(source.takeId, 'rehydrate.takeId')),
    derivedSourceId: nativeUint32(source.derivedSourceId, 'rehydrate.derivedSourceId'),
    status,
    reason: reasonToken(reason, 'rehydrate.reason'),
  };
}

function copyOriginalSources(
  originals: readonly ProjectVocalOriginalSource[],
): ProjectVocalOriginalSource[] {
  if (!Array.isArray(originals)) {
    throw refuseType('originals must be an array');
  }
  const seen = new Set<number>();
  return originals.map((original, index) => {
    const source = requireObject(original, `originals[${index}]`);
    const sourceId = requestNumber(source.sourceId, `originals[${index}].sourceId`);
    if (seen.has(sourceId)) {
      throw refuse(`originals contains duplicate sourceId ${sourceId}`);
    }
    seen.add(sourceId);
    const sampleRate = finite(source.sampleRate, `originals[${index}].sampleRate`);
    assertSampleRate(sampleRate, `originals[${index}].sampleRate`);
    return {
      sourceId,
      mono: finiteMono(source.mono, `originals[${index}].mono`),
      sampleRate,
    };
  });
}

/** Applies a complete committed vocal render to a native Project object. */
export function projectApplyVocalEdit(
  native: VocalProjectNative,
  request: ProjectVocalEditApplyRequest,
): ProjectVocalEditApplyResult {
  const normalized = copyApplyRequest(request);
  return copyApplyResult(native.applyVocalEdit(normalized));
}

/** Returns copied vocal sidecar dependencies in native storage order. */
export function projectGetVocalEditDependencies(
  native: VocalProjectNative,
): ProjectVocalEditDependency[] {
  const value = native.getVocalEditDependencies();
  if (!Array.isArray(value)) {
    throw refuseType('native vocal dependencies must be an array');
  }
  return value.map(copyDependency);
}

/**
 * Rehydrate unresolved vocal PCM without creating a Project history entry.
 * A `true` return from `options.cancel` cancels the call.
 */
export function projectRehydrateVocalEdits(
  native: VocalProjectNative,
  originals: readonly ProjectVocalOriginalSource[],
  options: ProjectVocalRehydrateOptions = {},
): ProjectVocalRehydrateItem[] {
  if (options === null || typeof options !== 'object' || Array.isArray(options)) {
    throw refuseType('options must be an object');
  }
  for (const key of Object.keys(options)) {
    if (key !== 'cancel') {
      throw refuseUnknownKey(key, ['cancel']);
    }
  }
  const { cancel } = options;
  if (cancel !== undefined && typeof cancel !== 'function') {
    throw refuseType('cancel must be a function');
  }
  const normalized = copyOriginalSources(originals);
  let callbackFailed = false;
  let callbackFailure: unknown;
  const guardedCancel =
    cancel === undefined
      ? undefined
      : () => {
          try {
            return cancel();
          } catch (error) {
            callbackFailed = true;
            callbackFailure = error;
            // Stop native work without allowing a JS exception to cross the C ABI.
            return true;
          }
        };
  let value: unknown;
  try {
    value = native.rehydrateVocalEdits(normalized, guardedCancel);
  } catch (error) {
    if (callbackFailed) {
      throw callbackFailure;
    }
    throw error;
  }
  if (callbackFailed) {
    throw callbackFailure;
  }
  // The adapter reports a throwing callback as data instead of throwing
  // through C++ frames, whose destructors a JS exception would skip.
  if (value !== null && typeof value === 'object' && 'cancelFailure' in value) {
    throw (value as { cancelFailure: unknown }).cancelFailure;
  }
  if (!Array.isArray(value)) {
    throw refuseType('native vocal rehydrate result must be an array');
  }
  return value.map(copyRehydrateItem);
}
