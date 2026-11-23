import { ordinalToken, skewError } from './internal/core_tokens.js';
import { assertDeclaredKeys, refuse, refuseType } from './internal/entry.js';
import { requireListBudget } from './internal/list_budget.js';
import { isAbsent, isNonArrayObject } from './internal/value_guards.js';
import type { VocalReason, VocalStateToken } from './types_vocal_edit.js';
import type {
  ProjectVocalEditApplyRequest,
  ProjectVocalEditApplyResult,
  ProjectVocalEditDependency,
  ProjectVocalOriginalSource,
  ProjectVocalRehydrateItem,
  ProjectVocalRehydrateOptions,
  ProjectVocalRehydrateStatus,
} from './types_vocal_project.js';
import { requestObject } from './validation.js';
import { VOCAL_REASONS } from './vocal_edit.js';

const UINT32_MAX = 0xffffffff;
const MAX_PROJECT_ID = UINT32_MAX - 1;
const UINT64_MAX = 0xffffffffffffffffn;
const MIN_SAMPLE_RATE = 8000;
const MAX_SAMPLE_RATE = 384000;

const REHYDRATE_STATUSES: readonly ProjectVocalRehydrateStatus[] = [
  'rehydrated',
  'alreadyReady',
  'unresolved',
];

/** The apply request as the addon reads it. */
interface NativeApplyRequest extends Omit<ProjectVocalEditApplyRequest, 'vocalState'> {
  sve1: Uint8Array;
}

interface NativeProjectVocal {
  applyVocalEdit(request: NativeApplyRequest): unknown;
  getVocalEditDependencies(): unknown;
  rehydrateVocalEdits(
    originals: readonly ProjectVocalOriginalSource[],
    cancel?: () => boolean,
  ): unknown;
}

function objectValue(value: unknown, field: string): Record<string, unknown> {
  if (!isNonArrayObject(value)) {
    throw refuseType(`${field} must be an object`);
  }
  return value as Record<string, unknown>;
}

function finiteNumber(value: unknown, field: string): number {
  if (typeof value !== 'number') {
    throw refuseType(`${field} must be a number`);
  }
  if (!Number.isFinite(value)) {
    throw refuse(`${field} must be finite`);
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
    throw refuse(`${field} must be a safe integer in [${minimum}, ${maximum}]`);
  }
  return value;
}

function requestSafeInteger(
  value: unknown,
  field: string,
  minimum = 0,
  maximum = Number.MAX_SAFE_INTEGER,
): number {
  if (typeof value !== 'number') {
    throw refuseType(`${field} must be an integer`);
  }
  return safeInteger(value, field, minimum, maximum);
}

function requestUint32(value: unknown, field: string, allowZero = true): number {
  return requestSafeInteger(value, field, allowZero ? 0 : 1, UINT32_MAX);
}

/** A numeric request field whose range the native side states; only the type is checked here. */
function requestNumber(value: unknown, field: string): number {
  if (typeof value !== 'number') {
    throw refuseType(`${field} must be an integer`);
  }
  return value;
}

function requestProjectId(value: unknown, field: string, allowZero = false): number {
  return requestSafeInteger(value, field, allowZero ? 0 : 1, MAX_PROJECT_ID);
}

/** A take id of an apply request: a number, and never `0`, which is spelt by leaving it out. */
function requestTakeId(value: unknown): number {
  if (typeof value !== 'number') {
    throw refuseType('takeId must be an integer');
  }
  if (value === 0) {
    throw refuse('takeId must not be 0; omit it for the base source');
  }
  return requestProjectId(value, 'request.takeId');
}

function uint64String(value: unknown, field: string): string {
  if (typeof value !== 'string' || !/^(0|[1-9][0-9]*)$/.test(value)) {
    throw refuseType(`${field} must be a decimal uint64 string`);
  }
  if (BigInt(value) > UINT64_MAX) {
    throw refuse(`${field} is outside the uint64 range`);
  }
  return value;
}

function digest(value: unknown, field: string): string {
  if (typeof value !== 'string' || !/^[0-9a-fA-F]{64}$/.test(value)) {
    throw refuseType(`${field} must be 64 hexadecimal characters`);
  }
  return value.toLowerCase();
}

function float32Copy(value: unknown, field: string, allowEmpty = false): Float32Array {
  if (!(value instanceof Float32Array)) {
    throw refuseType(`${field} must be a Float32Array`);
  }
  if (!allowEmpty && value.length === 0) {
    throw refuse(`${field} must not be empty`);
  }
  for (let index = 0; index < value.length; index += 1) {
    if (!Number.isFinite(value[index])) {
      throw refuse(`${field} contains a non-finite sample at index ${index}`);
    }
  }
  return new Float32Array(value);
}

function uint8Copy(value: unknown, field: string): Uint8Array {
  if (!(value instanceof Uint8Array)) {
    throw refuseType(`${field} must be a Uint8Array`);
  }
  if (value.length === 0) {
    throw refuse(`${field} must not be empty`);
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
    profileId: requestUint32(token.profileId, `${field}.profileId`),
  };
}

function normalizeApplyRequest(request: ProjectVocalEditApplyRequest): NativeApplyRequest {
  const input = objectValue(request, 'request');
  requestObject('applyProjectVocalEdit', request, 'request', true);
  const renderedMono = float32Copy(input.renderedMono, 'request.renderedMono');
  const sve1 = uint8Copy(input.vocalState, 'request.vocalState');
  const expectedSourceSampleCount = requestNumber(
    input.expectedSourceSampleCount,
    'request.expectedSourceSampleCount',
  );
  if (renderedMono.length !== expectedSourceSampleCount) {
    throw refuse('request.renderedMono length must equal request.expectedSourceSampleCount');
  }
  const expectedSourceSampleRate = requestSafeInteger(
    input.expectedSourceSampleRate,
    'request.expectedSourceSampleRate',
    MIN_SAMPLE_RATE,
    MAX_SAMPLE_RATE,
  );
  const renderedSampleRate = requestSafeInteger(
    input.renderedSampleRate,
    'request.renderedSampleRate',
    MIN_SAMPLE_RATE,
    MAX_SAMPLE_RATE,
  );
  if (renderedSampleRate !== expectedSourceSampleRate) {
    throw refuse('request.renderedSampleRate must equal request.expectedSourceSampleRate');
  }
  const token = tokenCopy(input.renderToken, 'request.renderToken');
  if (token.draftId !== '0' || token.generation !== '0') {
    throw refuse('request.renderToken must be a committed session token');
  }
  return {
    clipId: requestNumber(input.clipId, 'request.clipId'),
    ...(isAbsent(input.takeId) ? {} : { takeId: requestTakeId(input.takeId) }),
    expectedSourceId: requestNumber(input.expectedSourceId, 'request.expectedSourceId'),
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
    renderToken: token,
    sve1,
  };
}

function resultObject(value: unknown, field: string): Record<string, unknown> {
  if (!isNonArrayObject(value)) {
    throw skewError(`${field} is not an object`);
  }
  return value as Record<string, unknown>;
}

function resultArray(value: unknown, field: string): readonly unknown[] {
  if (!Array.isArray(value)) {
    throw skewError(`${field} is not an array`);
  }
  return value;
}

function resultInteger(
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
    throw skewError(`${field} is not an integer in [${minimum}, ${maximum}]`);
  }
  return value;
}

function resultUint32(value: unknown, field: string): number {
  return resultInteger(value, field, 0, UINT32_MAX);
}

function resultProjectId(value: unknown, field: string, allowZero = false): number {
  return resultInteger(value, field, allowZero ? 0 : 1, MAX_PROJECT_ID);
}

/** The native base-source id 0 becomes an absent `takeId`. */
function resultTakeId(value: unknown, field: string): { takeId?: number } {
  const id = resultProjectId(value, field, true);
  return id === 0 ? {} : { takeId: id };
}

function resultUint64(value: unknown, field: string): string {
  if (typeof value !== 'string' || !/^(0|[1-9][0-9]*)$/.test(value) || BigInt(value) > UINT64_MAX) {
    throw skewError(`${field} is not a decimal uint64 string`);
  }
  return value;
}

function resultDigest(value: unknown, field: string): string {
  if (typeof value !== 'string' || !/^[0-9a-fA-F]{64}$/.test(value)) {
    throw skewError(`${field} is not 64 hexadecimal characters`);
  }
  return value.toLowerCase();
}

function resultString(value: unknown, field: string): string {
  if (typeof value !== 'string') {
    throw skewError(`${field} is not a string`);
  }
  return value;
}

function resultBoolean(value: unknown, field: string): boolean {
  if (typeof value !== 'boolean') {
    throw skewError(`${field} is not a boolean`);
  }
  return value;
}

function applyResult(value: unknown): ProjectVocalEditApplyResult {
  const result = resultObject(value, 'native applyVocalEdit result');
  return {
    clipId: resultProjectId(result.clipId, 'result.clipId'),
    ...resultTakeId(result.takeId, 'result.takeId'),
    originalSourceId: resultProjectId(result.originalSourceId, 'result.originalSourceId'),
    derivedSourceId: resultProjectId(result.derivedSourceId, 'result.derivedSourceId'),
    committedRevision: resultUint64(result.committedRevision, 'result.committedRevision'),
    profileId: resultUint32(result.profileId, 'result.profileId'),
    derivedSourceSha256: resultDigest(result.derivedSourceSha256, 'result.derivedSourceSha256'),
    sidecarKey: resultString(result.sidecarKey, 'result.sidecarKey'),
  };
}

function reasonName(value: unknown): VocalReason {
  return ordinalToken(VOCAL_REASONS, value, 'vocal reason');
}

function dependency(value: unknown, index: number): ProjectVocalEditDependency {
  const result = resultObject(value, `native dependency[${index}]`);
  return {
    clipId: resultProjectId(result.clipId, `dependency[${index}].clipId`),
    ...resultTakeId(result.takeId, `dependency[${index}].takeId`),
    originalSourceId: resultProjectId(
      result.originalSourceId,
      `dependency[${index}].originalSourceId`,
      true,
    ),
    derivedSourceId: resultProjectId(
      result.derivedSourceId,
      `dependency[${index}].derivedSourceId`,
      true,
    ),
    sourceSampleRate: resultUint32(
      result.sourceSampleRate,
      `dependency[${index}].sourceSampleRate`,
    ),
    profileId: resultUint32(result.profileId, `dependency[${index}].profileId`),
    sourceSampleCount: resultInteger(
      result.sourceSampleCount,
      `dependency[${index}].sourceSampleCount`,
    ),
    committedRevision: resultUint64(
      result.committedRevision,
      `dependency[${index}].committedRevision`,
    ),
    originalSourceSha256: resultDigest(
      result.originalSourceSha256,
      `dependency[${index}].originalSourceSha256`,
    ),
    derivedSourceSha256: resultDigest(
      result.derivedSourceSha256,
      `dependency[${index}].derivedSourceSha256`,
    ),
    originalPcmAvailable: resultBoolean(
      result.originalPcmAvailable,
      `dependency[${index}].originalPcmAvailable`,
    ),
    derivedPcmAvailable: resultBoolean(
      result.derivedPcmAvailable,
      `dependency[${index}].derivedPcmAvailable`,
    ),
    reason: reasonName(result.reason),
    sidecarKey: resultString(result.sidecarKey, `dependency[${index}].sidecarKey`),
  };
}

function originalSource(value: unknown, index: number): ProjectVocalOriginalSource {
  const source = objectValue(value, `originals[${index}]`);
  const mono = float32Copy(source.mono, `originals[${index}].mono`);
  const sampleRate = requestSafeInteger(
    source.sampleRate,
    `originals[${index}].sampleRate`,
    MIN_SAMPLE_RATE,
    MAX_SAMPLE_RATE,
  );
  return {
    sourceId: requestNumber(source.sourceId, `originals[${index}].sourceId`),
    mono,
    sampleRate,
  };
}

function rehydrateItem(value: unknown, index: number): ProjectVocalRehydrateItem {
  const item = resultObject(value, `native rehydrate item[${index}]`);
  const status = ordinalToken(REHYDRATE_STATUSES, item.status, 'rehydrate status');
  return {
    clipId: resultProjectId(item.clipId, `rehydrate[${index}].clipId`),
    ...resultTakeId(item.takeId, `rehydrate[${index}].takeId`),
    derivedSourceId: resultProjectId(
      item.derivedSourceId,
      `rehydrate[${index}].derivedSourceId`,
      true,
    ),
    status,
    reason: reasonName(item.reason),
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
  const values = resultArray(native.getVocalEditDependencies(), 'native vocal dependencies');
  const result: ProjectVocalEditDependency[] = [];
  for (let index = 0; index < values.length; index += 1) {
    result.push(dependency(values[index], index));
  }
  return result;
}

/** Rehydrate unresolved vocal PCM; any truthy return from `options.cancel` cancels the call. */
export function projectRehydrateVocalEdits(
  native: NativeProjectVocal,
  originals: readonly ProjectVocalOriginalSource[],
  options: ProjectVocalRehydrateOptions = {},
): ProjectVocalRehydrateItem[] {
  if (!Array.isArray(originals)) {
    throw refuseType('originals must be an array');
  }
  requireListBudget(originals, 'originals');
  const opts = objectValue(options, 'options');
  assertDeclaredKeys(opts, ['cancel'], '');
  const cancel = opts.cancel as (() => boolean) | undefined;
  if (cancel !== undefined && typeof cancel !== 'function') {
    throw refuseType('cancel must be a function');
  }
  const sources: ProjectVocalOriginalSource[] = [];
  const seen = new Set<number>();
  for (let index = 0; index < originals.length; index += 1) {
    const source = originalSource(originals[index], index);
    if (seen.has(source.sourceId)) {
      throw refuse(`originals contains duplicate sourceId ${source.sourceId}`);
    }
    seen.add(source.sourceId);
    sources.push(source);
  }
  const value =
    cancel === undefined
      ? native.rehydrateVocalEdits(sources)
      : native.rehydrateVocalEdits(sources, cancel);
  const values = resultArray(value, 'native vocal rehydrate result');
  const result: ProjectVocalRehydrateItem[] = [];
  for (let index = 0; index < values.length; index += 1) {
    result.push(rehydrateItem(values[index], index));
  }
  return result;
}
