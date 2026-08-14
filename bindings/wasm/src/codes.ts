import type { AutomationCurve, MeterTap, PanLawInput, PanMode, SendTiming } from './public_types';

/** Resolve a numeric ordinal in an inclusive range without coercion. */
export function resolveOrdinalInRange(
  value: unknown,
  min: number,
  max: number,
  enumName: string,
): number {
  if (typeof value !== 'number' || !Number.isSafeInteger(value) || value < min || value > max) {
    throw new RangeError(`Invalid ${enumName}: ${String(value)}`);
  }
  return value;
}

/** Resolve a public enum spelling or ordinal without permitting unknown values. */
export function resolveEnumOrdinal(
  value: unknown,
  values: Readonly<Record<string, number>>,
  enumName: string,
): number {
  if (typeof value === 'number') {
    const ordinals = Object.values(values);
    const ordinal = resolveOrdinalInRange(
      value,
      Math.min(...ordinals),
      Math.max(...ordinals),
      enumName,
    );
    if (!ordinals.includes(ordinal)) {
      throw new RangeError(`Invalid ${enumName}: ${String(value)}`);
    }
    return ordinal;
  }
  if (typeof value === 'string') {
    const ordinal = values[value];
    if (ordinal !== undefined) {
      return ordinal;
    }
  }
  throw new RangeError(`Invalid ${enumName}: ${String(value)}`);
}

const AUTOMATION_CURVE_VALUES = {
  linear: 0,
  exponential: 1,
  hold: 2,
  's-curve': 3,
} as const;
const PAN_LAW_VALUES: Readonly<Record<string, number>> = {
  const3db: 0,
  'const-3db': 0,
  '-3db': 0,
  'const4.5db': 1,
  'const-4.5db': 1,
  '-4.5db': 1,
  const6db: 2,
  'const-6db': 2,
  '-6db': 2,
  linear0db: 3,
  'linear-0db': 3,
  linear: 3,
  '0db': 3,
} as const;
const PAN_MODE_VALUES = {
  balance: 0,
  pan: 1,
  stereopan: 1,
  'stereo-pan': 1,
  dualpan: 2,
  'dual-pan': 2,
} as const;
const METER_TAP_VALUES = { preFader: 0, postFader: 1 } as const;
const SEND_TIMING_VALUES = { postFader: 0, preFader: 1 } as const;
const TRACK_MONITOR_MODE_VALUES = { off: 0, pfl: 1, afl: 2 } as const;

export function automationCurveCode(curve: AutomationCurve): number {
  return resolveEnumOrdinal(curve, AUTOMATION_CURVE_VALUES, 'automation curve');
}

export function panLawCode(panLaw: PanLawInput): number {
  const normalized = typeof panLaw === 'string' ? panLaw.toLowerCase().replace(/_/g, '-') : panLaw;
  return resolveEnumOrdinal(normalized, PAN_LAW_VALUES, 'pan law');
}

export function panModeCode(panMode: PanMode | number): number {
  const normalized =
    typeof panMode === 'string' ? panMode.replace(/_/g, '-').toLowerCase() : panMode;
  return resolveEnumOrdinal(normalized, PAN_MODE_VALUES, 'pan mode');
}

export function meterTapCode(tap: MeterTap | number): number {
  return resolveEnumOrdinal(tap, METER_TAP_VALUES, 'meter tap');
}

export function sendTimingCode(timing: SendTiming | number): number {
  // Mirrors SonareSendTiming: post-fader is 0 (so an omitted/zeroed value is
  // post-fader), pre-fader is 1. A raw number is passed through as the C ABI int.
  // An unknown string is rejected rather than silently routed to post-fader,
  // matching the sibling enum-code helpers and Node's sendTimingValue.
  return resolveEnumOrdinal(timing, SEND_TIMING_VALUES, 'send timing');
}

/** Resolve a per-track PFL/AFL monitor mode to its C-ABI ordinal. */
export function trackMonitorModeCode(mode: unknown): number {
  return resolveEnumOrdinal(mode, TRACK_MONITOR_MODE_VALUES, 'track monitor mode');
}
