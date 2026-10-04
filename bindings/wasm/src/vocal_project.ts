import type { VocalStateToken, VocalUint64 } from './public_types_vocal_edit';
import type {
  ProjectVocalEditApplyRequest,
  ProjectVocalEditApplyResult,
  ProjectVocalEditDependency,
  ProjectVocalOriginalSource,
  ProjectVocalReason,
  ProjectVocalRehydrateItem,
} from './public_types_vocal_project';
import { assertSampleRate } from './validation';

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
    throw new TypeError(`${field} must be an object`);
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
    throw new RangeError(`${field} must be a uint32 project id`);
  }
  return value;
}

function nativeUint32(value: unknown, field: string): number {
  if (
    typeof value !== 'number' ||
    !Number.isSafeInteger(value) ||
    value < 0 ||
    value > UINT32_MAX
  ) {
    throw new RangeError(`${field} must be a native uint32`);
  }
  return value;
}

function nativeInt64(value: unknown, field: string): number {
  if (typeof value !== 'number' || !Number.isSafeInteger(value)) {
    throw new RangeError(`${field} must be a safe integer`);
  }
  return value;
}

function positiveSampleCount(value: unknown, field: string): number {
  if (typeof value !== 'number' || !Number.isSafeInteger(value) || value <= 0) {
    throw new RangeError(`${field} must be a positive safe integer`);
  }
  return value;
}

function finite(value: unknown, field: string): number {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new RangeError(`${field} must be finite`);
  }
  return value;
}

function decimalUint64(value: unknown, field: string): VocalUint64 {
  if (typeof value !== 'string' || !/^(0|[1-9]\d*)$/.test(value)) {
    throw new RangeError(`${field} must be a canonical decimal uint64 string`);
  }
  if (
    value.length > UINT64_MAX.length ||
    (value.length === UINT64_MAX.length && value > UINT64_MAX)
  ) {
    throw new RangeError(`${field} must fit uint64`);
  }
  return value;
}

function hexSha256(value: unknown, field: string): string {
  if (typeof value !== 'string' || !/^[0-9a-fA-F]{64}$/.test(value)) {
    throw new TypeError(`${field} must be 64 hexadecimal characters`);
  }
  return value.toLowerCase();
}

function finiteMono(value: unknown, field: string): Float32Array {
  if (!(value instanceof Float32Array) || value.length === 0) {
    throw new TypeError(`${field} must be a non-empty Float32Array`);
  }
  for (let index = 0; index < value.length; index += 1) {
    if (!Number.isFinite(value[index])) {
      throw new RangeError(`${field} must contain only finite samples`);
    }
  }
  return new Float32Array(value);
}

function sve1(value: unknown): Uint8Array {
  if (!(value instanceof Uint8Array) || value.length === 0) {
    throw new TypeError('sve1 must be a non-empty Uint8Array');
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
    profileId: uint32(source.profileId, 'renderToken.profileId'),
  };
}

function copyApplyRequest(request: ProjectVocalEditApplyRequest): ProjectVocalEditApplyRequest {
  const source = requireObject(request, 'request');
  const renderedMono = finiteMono(source.renderedMono, 'renderedMono');
  const expectedSourceSampleCount = positiveSampleCount(
    source.expectedSourceSampleCount,
    'expectedSourceSampleCount',
  );
  if (renderedMono.length !== expectedSourceSampleCount) {
    throw new RangeError(
      `renderedMono length must equal expectedSourceSampleCount (${renderedMono.length} != ${expectedSourceSampleCount})`,
    );
  }
  const expectedSourceSampleRate = finite(
    source.expectedSourceSampleRate,
    'expectedSourceSampleRate',
  );
  assertSampleRate('Project.applyVocalEdit', expectedSourceSampleRate);
  const renderedSampleRate = finite(source.renderedSampleRate, 'renderedSampleRate');
  assertSampleRate('Project.applyVocalEdit', renderedSampleRate, 'renderedSampleRate');
  if (renderedSampleRate !== expectedSourceSampleRate) {
    throw new RangeError('renderedSampleRate must equal expectedSourceSampleRate');
  }
  const renderedStartSample = source.renderedStartSample ?? 0;
  if (renderedStartSample !== 0) {
    throw new RangeError('renderedStartSample must be zero for the v1 Project vocal API');
  }
  const renderToken = copyToken(source.renderToken);
  if (renderToken.draftId !== '0' || renderToken.generation !== '0') {
    throw new RangeError('renderToken must be a committed session token');
  }
  return {
    clipId: uint32(source.clipId, 'clipId', false),
    takeId: uint32(source.takeId ?? 0, 'takeId'),
    expectedSourceId: uint32(source.expectedSourceId, 'expectedSourceId', false),
    expectedSourceSampleRate,
    expectedSourceSampleCount,
    expectedSourceSha256: hexSha256(source.expectedSourceSha256, 'expectedSourceSha256'),
    expectedClipLengthPpq: finite(source.expectedClipLengthPpq, 'expectedClipLengthPpq'),
    expectedSourceOffsetPpq: finite(source.expectedSourceOffsetPpq, 'expectedSourceOffsetPpq'),
    renderedMono,
    renderedSampleRate,
    renderedStartSample: 0,
    renderToken,
    sve1: sve1(source.sve1),
  };
}

function copyApplyResult(value: unknown): ProjectVocalEditApplyResult {
  const source = requireObject(value, 'applyVocalEdit result');
  return {
    clipId: uint32(source.clipId, 'result.clipId', false),
    takeId: uint32(source.takeId, 'result.takeId'),
    originalSourceId: uint32(source.originalSourceId, 'result.originalSourceId', false),
    derivedSourceId: uint32(source.derivedSourceId, 'result.derivedSourceId', false),
    committedRevision: decimalUint64(source.committedRevision, 'result.committedRevision'),
    profileId: uint32(source.profileId, 'result.profileId'),
    derivedSourceSha256: hexSha256(source.derivedSourceSha256, 'result.derivedSourceSha256'),
    sidecarKey:
      typeof source.sidecarKey === 'string'
        ? source.sidecarKey
        : (() => {
            throw new TypeError('result.sidecarKey must be a string');
          })(),
  };
}

function copyDependency(value: unknown): ProjectVocalEditDependency {
  const source = requireObject(value, 'vocal edit dependency');
  const reason = nativeUint32(source.reason, 'dependency.reason');
  return {
    clipId: nativeUint32(source.clipId, 'dependency.clipId'),
    takeId: nativeUint32(source.takeId, 'dependency.takeId'),
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
    reason: reason as ProjectVocalReason | number,
    sidecarKey:
      typeof source.sidecarKey === 'string'
        ? source.sidecarKey
        : (() => {
            throw new TypeError('dependency.sidecarKey must be a string');
          })(),
  };
}

function copyRehydrateItem(value: unknown): ProjectVocalRehydrateItem {
  const source = requireObject(value, 'vocal rehydrate item');
  const status = source.status;
  if (typeof status !== 'number' || !Number.isSafeInteger(status) || status < 0 || status > 2) {
    throw new RangeError('rehydrate.status must be one of 0, 1, or 2');
  }
  const reason = nativeUint32(source.reason, 'rehydrate.reason');
  return {
    clipId: nativeUint32(source.clipId, 'rehydrate.clipId'),
    takeId: nativeUint32(source.takeId, 'rehydrate.takeId'),
    derivedSourceId: nativeUint32(source.derivedSourceId, 'rehydrate.derivedSourceId'),
    status: status as ProjectVocalRehydrateItem['status'],
    reason: reason as ProjectVocalReason | number,
  };
}

function copyOriginalSources(
  originals: readonly ProjectVocalOriginalSource[],
): ProjectVocalOriginalSource[] {
  if (!Array.isArray(originals)) {
    throw new TypeError('originals must be an array');
  }
  const seen = new Set<number>();
  return originals.map((original, index) => {
    const source = requireObject(original, `originals[${index}]`);
    const sourceId = uint32(source.sourceId, `originals[${index}].sourceId`, false);
    if (seen.has(sourceId)) {
      throw new RangeError(`originals contains duplicate sourceId ${sourceId}`);
    }
    seen.add(sourceId);
    const sampleRate = finite(source.sampleRate, `originals[${index}].sampleRate`);
    assertSampleRate('Project.rehydrateVocalEdits', sampleRate, `originals[${index}].sampleRate`);
    return {
      sourceId,
      mono: finiteMono(source.mono, `originals[${index}].mono`),
      sampleRate,
    };
  });
}

/** Apply a complete committed vocal render to a native Project object. */
export function projectApplyVocalEdit(
  native: VocalProjectNative,
  request: ProjectVocalEditApplyRequest,
): ProjectVocalEditApplyResult {
  const normalized = copyApplyRequest(request);
  return copyApplyResult(native.applyVocalEdit(normalized));
}

/** Return copied vocal sidecar dependencies in native storage order. */
export function projectGetVocalEditDependencies(
  native: VocalProjectNative,
): ProjectVocalEditDependency[] {
  const value = native.getVocalEditDependencies();
  if (!Array.isArray(value)) {
    throw new TypeError('native vocal dependencies must be an array');
  }
  return value.map(copyDependency);
}

/** Rehydrate unresolved vocal PCM without creating a Project history entry. */
export function projectRehydrateVocalEdits(
  native: VocalProjectNative,
  originals: readonly ProjectVocalOriginalSource[],
  cancel?: () => boolean,
): ProjectVocalRehydrateItem[] {
  if (cancel !== undefined && typeof cancel !== 'function') {
    throw new TypeError('cancel must be a function');
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
    throw new TypeError('native vocal rehydrate result must be an array');
  }
  return value.map(copyRehydrateItem);
}
