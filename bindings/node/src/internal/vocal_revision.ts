import type { VocalRevisionOptions } from '../types_vocal_edit.js';
import { assertDeclaredKeys, refuseType } from './entry.js';
import { isPlainRecord } from './value_guards.js';

/** The `expectedRevision` of a revision options object; anything but a declared-key object is a `TypeError`. */
export function expectedRevisionOption(options: VocalRevisionOptions): string | undefined {
  if (!isPlainRecord(options)) {
    throw refuseType('options must be a { expectedRevision } object');
  }
  assertDeclaredKeys(options, ['expectedRevision'], 'options');
  return (options as VocalRevisionOptions).expectedRevision;
}
