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
  enumName: string,
): number {
  if (typeof value !== 'string') {
    throw refuseType(`${enumName} must be a name, got ${typeof value}`);
  }
  // biome-ignore lint/suspicious/noPrototypeBuiltins: Object.hasOwn is newer than the ES2020 target.
  if (Object.prototype.hasOwnProperty.call(values, value)) {
    return values[value];
  }
  throw refuse(`Invalid ${enumName}: ${value}`);
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

const PAN_LAW_VALUES = {
  const3Db: 0,
  'const4.5Db': 1,
  const6Db: 2,
  linear0Db: 3,
} as const;
const PAN_MODE_VALUES = { balance: 0, stereoPan: 1, dualPan: 2 } as const;
const METER_TAP_VALUES = { preFader: 0, postFader: 1 } as const;
const SEND_TIMING_VALUES = { postFader: 0, preFader: 1 } as const;
const SIDECHAIN_SOURCE_KIND_VALUES = { track: 0, bus: 1 } as const;
const TRACK_MONITOR_MODE_VALUES = { off: 0, pfl: 1, afl: 2 } as const;

export function automationCurveCode(curve: AutomationCurve): number {
  return resolveEnumOrdinal(curve, AUTOMATION_CURVE_VALUES, 'automation curve');
}

export function panLawCode(panLaw: PanLaw): number {
  return resolveEnumOrdinal(panLaw, PAN_LAW_VALUES, 'pan law');
}

export function panModeCode(panMode: PanMode): number {
  return resolveEnumOrdinal(panMode, PAN_MODE_VALUES, 'pan mode');
}

export function meterTapCode(tap: MeterTap): number {
  return resolveEnumOrdinal(tap, METER_TAP_VALUES, 'meter tap');
}

export function sendTimingCode(timing: SendTiming): number {
  // Mirrors SonareSendTiming: post-fader is 0 (so an omitted/zeroed value is post-fader).
  return resolveEnumOrdinal(timing, SEND_TIMING_VALUES, 'send timing');
}

/** Resolve a sidechain source kind to its `SonareSidechainSourceKind` ordinal. */
export function sidechainSourceKindCode(kind: SidechainSourceKind): number {
  return resolveEnumOrdinal(kind, SIDECHAIN_SOURCE_KIND_VALUES, 'sidechain source kind');
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
  const reason = SIDECHAIN_REFUSALS[code - 1];
  if (reason === undefined) {
    throw refuse(`Unknown sidechain refusal: ${code}`);
  }
  return { ok: false, reason };
}

/** Resolve a per-track PFL/AFL monitor mode to its C-ABI ordinal. */
export function trackMonitorModeCode(mode: unknown): number {
  return resolveEnumOrdinal(mode, TRACK_MONITOR_MODE_VALUES, 'track monitor mode');
}
