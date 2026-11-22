import {
  PAN_LAW_VALUES,
  PAN_MODE_VALUES,
  resolveEnumOrdinal,
  SEND_TIMING_VALUES,
} from '../codes.js';
import type {
  MixSceneBusInput,
  MixSceneSendInput,
  MixSceneStripInput,
} from '../public_types_mixing.js';
import { refuse, refuseType } from './entry.js';
import { requireListBudget } from './list_budget.js';
import { isAbsent, isNonArrayObject } from './value_guards.js';

// The per-strip and per-bus setters take these settings as call arguments, so a wrong type or an unknown token is
// refused here as TypeError / RangeError. A whole scene document is left to the native reader, which refuses its
// content as InvalidFormat.

type Loose = Record<string, unknown>;

/** A pan-mode token as the native ordinal; a non-string is a `TypeError`, an unknown token a `RangeError`. */
export function panModeToNative(value: unknown, argName = 'panMode'): number {
  return resolveEnumOrdinal(value, PAN_MODE_VALUES, argName);
}

function sendToNative(send: MixSceneSendInput, path: string): Loose {
  if (send === null || typeof send !== 'object') {
    throw refuseType(`${path} must be an object`);
  }
  if (isAbsent(send.timing)) {
    return { ...send };
  }
  resolveEnumOrdinal(send.timing, SEND_TIMING_VALUES, `${path}.timing`);
  return { ...send };
}

function panFieldsToNative(out: Loose, path: string): void {
  if (!isAbsent(out.panMode)) {
    resolveEnumOrdinal(out.panMode, PAN_MODE_VALUES, `${path}.panMode`);
  }
  if (!isAbsent(out.panLaw)) {
    resolveEnumOrdinal(out.panLaw, PAN_LAW_VALUES, `${path}.panLaw`);
  }
}

function listOf(value: unknown, path: string): unknown[] | undefined {
  if (isAbsent(value)) {
    return undefined;
  }
  if (!Array.isArray(value)) {
    throw refuseType(`${path} must be an array`);
  }
  return value;
}

function objectAt(value: unknown, path: string): Loose {
  if (!isNonArrayObject(value)) {
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

/** A strip entry of a scene document as the native document spells it. */
function stripToNative(strip: unknown, path: string): Loose {
  const out: Loose = { ...objectAt(strip, path) };
  panFieldsToNative(out, path);
  assertStripMetering(out.metering, `${path}.metering`);
  const sends = listOf(out.sends, `${path}.sends`);
  if (sends !== undefined) {
    requireListBudget(sends, `${path}.sends`);
    out.sends = sends.map((send, i) =>
      sendToNative(send as MixSceneSendInput, `${path}.sends[${i}]`),
    );
  }
  return out;
}

function busToNative(bus: unknown, path: string): Loose {
  const out: Loose = { ...objectAt(bus, path) };
  panFieldsToNative(out, path);
  return out;
}

/** The native scene document for a public strip, as the engine's per-strip setters read it. */
export function mixSceneStripToNative(strip: MixSceneStripInput, path = 'strip'): Loose {
  return stripToNative(strip, path);
}

/** The native scene document for a public bus, as the engine's per-bus setters read it. */
export function mixSceneBusToNative(bus: MixSceneBusInput, path = 'bus'): Loose {
  return busToNative(bus, path);
}
