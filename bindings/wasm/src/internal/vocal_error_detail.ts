import type { VocalErrorDetail, VocalReason } from '../public_types_vocal_edit.js';

/** Native reason codes in `SonareVocalReason` order. */
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

/** Whether a value read off a message is a {@link VocalErrorDetail}. */
export function isVocalErrorDetail(value: unknown): value is VocalErrorDetail {
  if (value === null || typeof value !== 'object') {
    return false;
  }
  const v = value as Record<string, unknown>;
  return (
    typeof v.reason === 'string' &&
    VOCAL_REASONS.includes(v.reason as VocalReason) &&
    typeof v.field === 'string' &&
    typeof v.expected === 'string' &&
    typeof v.actual === 'string' &&
    typeof v.expectedText === 'string' &&
    typeof v.actualText === 'string'
  );
}

/** Copies `detail` onto `error` as a read-only `detail` property. */
export function attachVocalErrorDetail(error: object, detail: VocalErrorDetail): void {
  Object.defineProperty(error, 'detail', {
    configurable: true,
    enumerable: true,
    value: Object.freeze({
      reason: detail.reason,
      field: detail.field,
      expected: detail.expected,
      actual: detail.actual,
      expectedText: detail.expectedText,
      actualText: detail.actualText,
    }),
    writable: false,
  });
}
