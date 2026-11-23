import { unknownOrdinal } from './internal/core_tokens.js';
import { refuse, refuseType } from './internal/entry.js';
import type {
  AutomationCurve,
  MeterTap,
  PanLaw,
  PanMode,
  SendTiming,
  SidechainCheck,
  SidechainRefusal,
  SidechainSourceKind,
} from './public_types.js';

/**
 * Resolve a public enum spelling to its C ABI ordinal. Only the exact declared
 * spelling is accepted: anything that is not a string (a number included) is a
 * `TypeError`, an unknown string a `RangeError`.
 */
export function resolveEnumOrdinal(
  value: unknown,
  values: Readonly<Record<string, number>>,
  key: string,
): number {
  const allowed = Object.keys(values).join(', ');
  if (typeof value !== 'string') {
    throw refuseType(`${key} must be one of ${allowed}`);
  }
  // biome-ignore lint/suspicious/noPrototypeBuiltins: Object.hasOwn is newer than the ES2020 target.
  if (Object.prototype.hasOwnProperty.call(values, value)) {
    return values[value];
  }
  throw refuse(`${key} must be one of ${allowed}`);
}

/**
 * Automation curve spellings and their `SonareAutomationCurve` ordinals.
 *
 * Single source of truth for the vocabulary the mixer, the engine and the
 * Project automation lanes resolve against.
 */
export const AUTOMATION_CURVE_VALUES = {
  linear: 0,
  exponential: 1,
  hold: 2,
  sCurve: 3,
} as const;

export const PAN_LAW_VALUES = {
  const3Db: 0,
  'const4.5Db': 1,
  const6Db: 2,
  linear0Db: 3,
} as const;
export const PAN_MODE_VALUES = { balance: 0, stereoPan: 1, dualPan: 2 } as const;
const METER_TAP_VALUES = { preFader: 0, postFader: 1 } as const;
// Keys follow the scene document's token order, which every refusal lists; post-fader stays ordinal 0.
export const SEND_TIMING_VALUES = { preFader: 1, postFader: 0 } as const;
const SIDECHAIN_SOURCE_KIND_VALUES = { track: 0, bus: 1 } as const;
const TRACK_MONITOR_MODE_VALUES = { off: 0, pfl: 1, afl: 2 } as const;

export function automationCurveCode(curve: AutomationCurve, key: string): number {
  return resolveEnumOrdinal(curve, AUTOMATION_CURVE_VALUES, key);
}

export function panLawCode(panLaw: PanLaw, key: string): number {
  return resolveEnumOrdinal(panLaw, PAN_LAW_VALUES, key);
}

export function panModeCode(panMode: PanMode, key: string): number {
  return resolveEnumOrdinal(panMode, PAN_MODE_VALUES, key);
}

export function meterTapCode(tap: MeterTap, key: string): number {
  return resolveEnumOrdinal(tap, METER_TAP_VALUES, key);
}

export function sendTimingCode(timing: SendTiming, key: string): number {
  // Mirrors SonareSendTiming: post-fader is 0 (so an omitted/zeroed value is post-fader).
  return resolveEnumOrdinal(timing, SEND_TIMING_VALUES, key);
}

/** Resolve a sidechain source kind to its `SonareSidechainSourceKind` ordinal. */
export function sidechainSourceKindCode(kind: SidechainSourceKind, key: string): number {
  return resolveEnumOrdinal(kind, SIDECHAIN_SOURCE_KIND_VALUES, key);
}

const SIDECHAIN_REFUSALS: readonly SidechainRefusal[] = [
  'invalidTarget',
  'insertOutOfRange',
  'undeclaredSource',
  'invalidSourceKind',
  'selfKey',
  'cycle',
  'tableFull',
  'planRefused',
];

/** Maps the C ABI `SonareSidechainRefusal` value (0 = accepted) to the facade shape. */
export function sidechainCheckFromCode(code: number): SidechainCheck {
  if (code === 0) {
    return { ok: true };
  }
  if (!Number.isInteger(code) || code < 1 || code > SIDECHAIN_REFUSALS.length) {
    throw unknownOrdinal('sidechain refusal', code);
  }
  return { ok: false, reason: SIDECHAIN_REFUSALS[code - 1] };
}

/** Resolve a per-track PFL/AFL monitor mode to its C-ABI ordinal. */
export function trackMonitorModeCode(mode: unknown, key: string): number {
  return resolveEnumOrdinal(mode, TRACK_MONITOR_MODE_VALUES, key);
}
