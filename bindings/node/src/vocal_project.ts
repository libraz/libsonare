import type { VocalStateToken, VocalUint64 } from './types_vocal_edit.js';
import type {
  ProjectVocalEditApplyRequest,
  ProjectVocalEditApplyResult,
  ProjectVocalEditDependency,
  ProjectVocalOriginalSource,
  ProjectVocalRehydrateItem,
  ProjectVocalRehydrateStatus,
} from './types_vocal_project.js';

const UINT32_MAX = 0xffffffff;
const MAX_PROJECT_ID = UINT32_MAX - 1;
const UINT64_MAX = 0xffffffffffffffffn;

interface NativeProjectVocal {
  applyVocalEdit(request: ProjectVocalEditApplyRequest): unknown;
  getVocalEditDependencies(): unknown;
  rehydrateVocalEdits(
    originals: readonly ProjectVocalOriginalSource[],
    cancel?: () => boolean,
  ): unknown;
}

function objectValue(value: unknown, field: string): Record<string, unknown> {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new TypeError(`${field} must be an object`);
  }
  return value as Record<string, unknown>;
}

function arrayValue(value: unknown, field: string): readonly unknown[] {
  if (!Array.isArray(value)) {
    throw new TypeError(`${field} must be an array`);
  }
  return value;
}

function finiteNumber(value: unknown, field: string): number {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new TypeError(`${field} must be a finite number`);
  }
  return value;
}

function safeInteger(
  value: unknown,
  field: string,
  minimum = 0,
  maximum = Number.MAX_SAFE_INTEGER,
): number {
  if (
    typeof value !== 'number' ||
    !Number.isSafeInteger(value) ||
    value < minimum ||
    value > maximum
  ) {
    throw new RangeError(`${field} must be a safe integer in [${minimum}, ${maximum}]`);
  }
  return value;
}

function uint32(value: unknown, field: string): number {
  return safeInteger(value, field, 0, UINT32_MAX);
}

function projectId(value: unknown, field: string, allowZero = false): number {
  return safeInteger(value, field, allowZero ? 0 : 1, MAX_PROJECT_ID);
}

function uint64String(value: unknown, field: string): VocalUint64 {
  if (typeof value !== 'string' || !/^(0|[1-9][0-9]*)$/.test(value)) {
    throw new TypeError(`${field} must be a decimal uint64 string`);
  }
  if (BigInt(value) > UINT64_MAX) {
    throw new RangeError(`${field} is outside the uint64 range`);
  }
  return value;
}

function digest(value: unknown, field: string): string {
  if (typeof value !== 'string' || !/^[0-9a-fA-F]{64}$/.test(value)) {
    throw new TypeError(`${field} must be 64 hexadecimal characters`);
  }
  return value.toLowerCase();
}

function float32Copy(value: unknown, field: string, allowEmpty = false): Float32Array {
  if (!(value instanceof Float32Array)) {
    throw new TypeError(`${field} must be a Float32Array`);
  }
  if (!allowEmpty && value.length === 0) {
    throw new RangeError(`${field} must not be empty`);
  }
  for (let index = 0; index < value.length; index += 1) {
    if (!Number.isFinite(value[index])) {
      throw new RangeError(`${field} contains NaN or Inf at index ${index}`);
    }
  }
  return new Float32Array(value);
}

function uint8Copy(value: unknown, field: string): Uint8Array {
  if (!(value instanceof Uint8Array)) {
    throw new TypeError(`${field} must be a Uint8Array`);
  }
  if (value.length === 0) {
    throw new RangeError(`${field} must not be empty`);
  }
  return new Uint8Array(value);
}

function tokenCopy(value: unknown, field: string): VocalStateToken {
  const token = objectValue(value, field);
  return {
    sessionEpoch: uint64String(token.sessionEpoch, `${field}.sessionEpoch`),
    revision: uint64String(token.revision, `${field}.revision`),
    draftId: uint64String(token.draftId, `${field}.draftId`),
    generation: uint64String(token.generation, `${field}.generation`),
    requestId: uint64String(token.requestId, `${field}.requestId`),
    profileId: uint32(token.profileId, `${field}.profileId`),
  };
}

function normalizeApplyRequest(
  request: ProjectVocalEditApplyRequest,
): ProjectVocalEditApplyRequest {
  const input = objectValue(request, 'request');
  const renderedMono = float32Copy(input.renderedMono, 'request.renderedMono');
  const sve1 = uint8Copy(input.sve1, 'request.sve1');
  const expectedSourceSampleCount = safeInteger(
    input.expectedSourceSampleCount,
    'request.expectedSourceSampleCount',
    1,
  );
  if (renderedMono.length !== expectedSourceSampleCount) {
    throw new RangeError(
      'request.renderedMono length must equal request.expectedSourceSampleCount',
    );
  }
  const expectedSourceSampleRate = uint32(
    input.expectedSourceSampleRate,
    'request.expectedSourceSampleRate',
  );
  if (expectedSourceSampleRate === 0) {
    throw new RangeError('request.expectedSourceSampleRate must be positive');
  }
  const renderedSampleRate = uint32(input.renderedSampleRate, 'request.renderedSampleRate');
  if (renderedSampleRate === 0 || renderedSampleRate !== expectedSourceSampleRate) {
    throw new RangeError(
      'request.renderedSampleRate must equal request.expectedSourceSampleRate and be positive',
    );
  }
  const renderedStartSample = safeInteger(
    input.renderedStartSample ?? 0,
    'request.renderedStartSample',
  );
  if (renderedStartSample !== 0) {
    throw new RangeError('request.renderedStartSample must be zero for the v1 Project vocal API');
  }
  const token = tokenCopy(input.renderToken, 'request.renderToken');
  if (token.draftId !== '0' || token.generation !== '0') {
    throw new RangeError('request.renderToken must be a committed session token');
  }
  return {
    clipId: projectId(input.clipId, 'request.clipId'),
    takeId: projectId(input.takeId ?? 0, 'request.takeId', true),
    expectedSourceId: projectId(input.expectedSourceId, 'request.expectedSourceId'),
    expectedSourceSampleRate,
    expectedSourceSampleCount,
    expectedSourceSha256: digest(input.expectedSourceSha256, 'request.expectedSourceSha256'),
    expectedClipLengthPpq: finiteNumber(
      input.expectedClipLengthPpq,
      'request.expectedClipLengthPpq',
    ),
    expectedSourceOffsetPpq: finiteNumber(
      input.expectedSourceOffsetPpq,
      'request.expectedSourceOffsetPpq',
    ),
    renderedMono,
    renderedSampleRate,
    renderedStartSample,
    renderToken: token,
    sve1,
  };
}

function applyResult(value: unknown): ProjectVocalEditApplyResult {
  const result = objectValue(value, 'native applyVocalEdit result');
  return {
    clipId: projectId(result.clipId, 'result.clipId'),
    takeId: projectId(result.takeId, 'result.takeId', true),
    originalSourceId: projectId(result.originalSourceId, 'result.originalSourceId'),
    derivedSourceId: projectId(result.derivedSourceId, 'result.derivedSourceId'),
    committedRevision: uint64String(result.committedRevision, 'result.committedRevision'),
    profileId: uint32(result.profileId, 'result.profileId'),
    derivedSourceSha256: digest(result.derivedSourceSha256, 'result.derivedSourceSha256'),
    sidecarKey: stringValue(result.sidecarKey, 'result.sidecarKey'),
  };
}

function stringValue(value: unknown, field: string): string {
  if (typeof value !== 'string') {
    throw new TypeError(`${field} must be a string`);
  }
  return value;
}

function dependency(value: unknown, index: number): ProjectVocalEditDependency {
  const result = objectValue(value, `native dependency[${index}]`);
  return {
    clipId: projectId(result.clipId, `dependency[${index}].clipId`),
    takeId: projectId(result.takeId, `dependency[${index}].takeId`, true),
    originalSourceId: projectId(
      result.originalSourceId,
      `dependency[${index}].originalSourceId`,
      true,
    ),
    derivedSourceId: projectId(
      result.derivedSourceId,
      `dependency[${index}].derivedSourceId`,
      true,
    ),
    sourceSampleRate: uint32(result.sourceSampleRate, `dependency[${index}].sourceSampleRate`),
    profileId: uint32(result.profileId, `dependency[${index}].profileId`),
    sourceSampleCount: safeInteger(
      result.sourceSampleCount,
      `dependency[${index}].sourceSampleCount`,
    ),
    committedRevision: uint64String(
      result.committedRevision,
      `dependency[${index}].committedRevision`,
    ),
    originalSourceSha256: digest(
      result.originalSourceSha256,
      `dependency[${index}].originalSourceSha256`,
    ),
    derivedSourceSha256: digest(
      result.derivedSourceSha256,
      `dependency[${index}].derivedSourceSha256`,
    ),
    originalPcmAvailable: booleanValue(
      result.originalPcmAvailable,
      `dependency[${index}].originalPcmAvailable`,
    ),
    derivedPcmAvailable: booleanValue(
      result.derivedPcmAvailable,
      `dependency[${index}].derivedPcmAvailable`,
    ),
    reason: safeInteger(result.reason, `dependency[${index}].reason`, 0, UINT32_MAX),
    sidecarKey: stringValue(result.sidecarKey, `dependency[${index}].sidecarKey`),
  };
}

function booleanValue(value: unknown, field: string): boolean {
  if (typeof value !== 'boolean') {
    throw new TypeError(`${field} must be a boolean`);
  }
  return value;
}

function originalSource(value: unknown, index: number): ProjectVocalOriginalSource {
  const source = objectValue(value, `originals[${index}]`);
  const mono = float32Copy(source.mono, `originals[${index}].mono`);
  const sampleRate = uint32(source.sampleRate, `originals[${index}].sampleRate`);
  if (sampleRate === 0) {
    throw new RangeError(`originals[${index}].sampleRate must be positive`);
  }
  return {
    sourceId: projectId(source.sourceId, `originals[${index}].sourceId`),
    mono,
    sampleRate,
  };
}

function rehydrateItem(value: unknown, index: number): ProjectVocalRehydrateItem {
  const item = objectValue(value, `native rehydrate item[${index}]`);
  const status = safeInteger(item.status, `rehydrate[${index}].status`, 0, 2);
  return {
    clipId: projectId(item.clipId, `rehydrate[${index}].clipId`),
    takeId: projectId(item.takeId, `rehydrate[${index}].takeId`, true),
    derivedSourceId: projectId(item.derivedSourceId, `rehydrate[${index}].derivedSourceId`, true),
    status: status as ProjectVocalRehydrateStatus,
    reason: safeInteger(item.reason, `rehydrate[${index}].reason`, 0, UINT32_MAX),
  };
}

/** Apply one complete, committed vocal render to a project clip or take. */
export function projectApplyVocalEdit(
  native: NativeProjectVocal,
  request: ProjectVocalEditApplyRequest,
): ProjectVocalEditApplyResult {
  return applyResult(native.applyVocalEdit(normalizeApplyRequest(request)));
}

/** Return copied vocal sidecar dependencies in native storage order. */
export function projectGetVocalEditDependencies(
  native: NativeProjectVocal,
): ProjectVocalEditDependency[] {
  const values = arrayValue(native.getVocalEditDependencies(), 'native vocal dependencies');
  const result: ProjectVocalEditDependency[] = [];
  for (let index = 0; index < values.length; index += 1) {
    result.push(dependency(values[index], index));
  }
  return result;
}

/** Rehydrate unresolved vocal PCM with a call-scoped cancellation callback. */
export function projectRehydrateVocalEdits(
  native: NativeProjectVocal,
  originals: readonly ProjectVocalOriginalSource[],
  cancel?: () => boolean,
): ProjectVocalRehydrateItem[] {
  if (!Array.isArray(originals)) {
    throw new TypeError('originals must be an array');
  }
  if (cancel !== undefined && typeof cancel !== 'function') {
    throw new TypeError('cancel must be a function');
  }
  const sources: ProjectVocalOriginalSource[] = [];
  const seen = new Set<number>();
  for (let index = 0; index < originals.length; index += 1) {
    const source = originalSource(originals[index], index);
    if (seen.has(source.sourceId)) {
      throw new RangeError(`originals contains duplicate sourceId ${source.sourceId}`);
    }
    seen.add(source.sourceId);
    sources.push(source);
  }
  const value =
    cancel === undefined
      ? native.rehydrateVocalEdits(sources)
      : native.rehydrateVocalEdits(sources, cancel);
  const values = arrayValue(value, 'native vocal rehydrate result');
  const result: ProjectVocalRehydrateItem[] = [];
  for (let index = 0; index < values.length; index += 1) {
    result.push(rehydrateItem(values[index], index));
  }
  return result;
}
