import type {
  MixSceneBusInput,
  MixSceneSendInput,
  MixSceneStripInput,
  SendTiming,
} from '../types_mixing.js';
import { refuse, refuseType } from './entry.js';
import { requireListBudget } from './list_budget.js';
import { isAbsent } from './value_guards.js';

// The per-strip and per-bus setters take these settings as call arguments, so a wrong type or an unknown token is
// refused here as TypeError / RangeError. A whole scene document is left to the native reader, which refuses its
// content as InvalidFormat.

/** Pan-mode tokens, indexed by the native ordinal. */
const PAN_MODES: readonly string[] = ['balance', 'stereoPan', 'dualPan'];
/** Pan-law tokens, indexed by the native ordinal. */
const PAN_LAWS: readonly string[] = ['const3Db', 'const4.5Db', 'const6Db', 'linear0Db'];
const SEND_TIMINGS: readonly SendTiming[] = ['preFader', 'postFader'];

type Loose = Record<string, unknown>;

function tokenOrdinal(value: unknown, argName: string, tokens: readonly string[]): number {
  if (typeof value !== 'string') {
    throw refuseType(`${argName} must be one of ${tokens.join(', ')}`);
  }
  const ordinal = tokens.indexOf(value);
  if (ordinal < 0) {
    throw refuse(`${argName} must be one of ${tokens.join(', ')}`);
  }
  return ordinal;
}

/** A pan-mode token as the native ordinal; a non-string is a `TypeError`, an unknown token a `RangeError`. */
export function panModeToNative(value: unknown, argName = 'panMode'): number {
  return tokenOrdinal(value, argName, PAN_MODES);
}

function sendToNative(send: MixSceneSendInput, path: string): Loose {
  if (send === null || typeof send !== 'object') {
    throw refuseType(`${path} must be an object`);
  }
  if (!isAbsent(send.timing)) {
    tokenOrdinal(send.timing, `${path}.timing`, SEND_TIMINGS);
  }
  return { ...send };
}

function panFieldsToNative(out: Loose, path: string): void {
  if (!isAbsent(out.panMode)) {
    tokenOrdinal(out.panMode, `${path}.panMode`, PAN_MODES);
  }
  if (!isAbsent(out.panLaw)) {
    tokenOrdinal(out.panLaw, `${path}.panLaw`, PAN_LAWS);
  }
}

function listOf(value: unknown, path: string): unknown[] | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  if (!Array.isArray(value)) {
    throw refuseType(`${path} must be an array`);
  }
  requireListBudget(value, path);
  return value;
}

function objectAt(value: unknown, path: string): Loose {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw refuseType(`${path} must be an object`);
  }
  return value as Loose;
}

/** Refuses an explicit 0 oversampling factor: omit the field for the library default. */
export function assertStripMetering(metering: unknown, path = 'metering'): void {
  if (isAbsent(metering)) {
    return;
  }
  const { truePeakOversample } = objectAt(metering, path);
  if (truePeakOversample === 0) {
    throw refuse(`${path}.truePeakOversample must be 4 or 8 (omit for the default)`);
  }
}

/** The native scene document for a public strip, as the engine's per-strip setters read it. */
export function mixSceneStripToNative(strip: MixSceneStripInput, path = 'strip'): Loose {
  const out: Loose = { ...objectAt(strip, path) };
  panFieldsToNative(out, path);
  assertStripMetering(out.metering, `${path}.metering`);
  const sends = listOf(out.sends, `${path}.sends`);
  if (sends !== undefined) {
    out.sends = sends.map((send, i) =>
      sendToNative(send as MixSceneSendInput, `${path}.sends[${i}]`),
    );
  }
  return out;
}

/** The native scene document for a public bus, as the engine's per-bus setters read it. */
export function mixSceneBusToNative(bus: MixSceneBusInput, path = 'bus'): Loose {
  const out: Loose = { ...objectAt(bus, path) };
  panFieldsToNative(out, path);
  return out;
}
