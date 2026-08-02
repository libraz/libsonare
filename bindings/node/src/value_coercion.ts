import type {
  AutomationCurve,
  EngineAutomationPoint,
  EngineAutomationPointCurve,
  MeterTap,
  PanLaw,
  PanMode,
  ProjectAutomationLaneDesc,
  ProjectAutomationPoint,
  ProjectClipFade,
  ProjectFadeCurve,
  ProjectLoopMode,
  ProjectTrackDesc,
  SendTiming,
  WarpMode,
} from './types.js';

/**
 * Internal enum/string -> numeric value coercion helpers shared by the Project
 * and Mixer facades. These map the public string/union enum spellings to the
 * integer ordinals the C ABI expects. Pure functions over type-only inputs; not
 * part of the public surface (consumed by index.ts, never re-exported).
 */

/** Resolve only declared enum spellings and ordinals at the JS/C ABI boundary. */
export function resolveEnumOrdinal(
  value: unknown,
  values: Readonly<Record<string, number>>,
  enumName: string,
): number {
  if (typeof value === 'number') {
    if (!Number.isSafeInteger(value) || !Object.values(values).includes(value)) {
      throw new RangeError(`Invalid ${enumName}: ${String(value)}`);
    }
    return value;
  }
  if (typeof value === 'string') {
    const ordinal = values[value];
    if (ordinal !== undefined) {
      return ordinal;
    }
  }
  throw new RangeError(`Invalid ${enumName}: ${String(value)}`);
}

export function trackKindValue(kind: ProjectTrackDesc['kind']): number {
  return resolveEnumOrdinal(kind ?? 'audio', { audio: 0, midi: 1, aux: 2 }, 'track kind');
}

export function warpModeValue(mode: WarpMode | number | undefined): number {
  return resolveEnumOrdinal(mode ?? 'off', { off: 0, repitch: 1, 'tempo-sync': 2 }, 'warp mode');
}

export function engineAutomationCurveValue(curve: EngineAutomationPointCurve | undefined): number {
  return resolveEnumOrdinal(
    curve ?? 'linear',
    { linear: 0, exponential: 1, hold: 2, 's-curve': 3 },
    'automation curve',
  );
}

export function engineAutomationPointValue(point: EngineAutomationPoint): EngineAutomationPoint {
  return {
    ...point,
    curveToNext: engineAutomationCurveValue(point.curveToNext) as EngineAutomationPointCurve,
  };
}

export function projectFadeCurveValue(
  curve: ProjectFadeCurve | undefined | null,
): number | undefined {
  if (curve === undefined || curve === null) {
    return undefined;
  }
  const normalized = typeof curve === 'string' ? curve.replace(/[_-]/g, '').toLowerCase() : curve;
  return resolveEnumOrdinal(
    normalized,
    { linear: 0, equalpower: 1, exponential: 2, exp: 2, logarithmic: 3, log: 3 },
    'project fade curve',
  );
}

export function projectClipFadeValue(
  fade: ProjectClipFade | undefined,
): ProjectClipFade | undefined {
  if (fade === undefined) {
    return undefined;
  }
  // Keep names intact so the addon resolves them through the C ABI's shared
  // fade-curve parser. This prevents the three bindings from drifting.
  return { ...fade };
}

export function projectLoopModeValue(mode: ProjectLoopMode): number {
  return resolveEnumOrdinal(mode, { off: 0, loop: 1 }, 'project loop mode');
}

export function projectAutomationPointValue(point: ProjectAutomationPoint): ProjectAutomationPoint {
  const curve = engineAutomationCurveValue(point.curve ?? point.curveToNext);
  return {
    ...point,
    curve: curve as EngineAutomationPointCurve,
    curveToNext: curve as EngineAutomationPointCurve,
  };
}

export function projectAutomationLaneValue(
  desc: ProjectAutomationLaneDesc,
): ProjectAutomationLaneDesc {
  return { ...desc, points: desc.points.map(projectAutomationPointValue) };
}

const PAN_LAW_VALUES: Record<PanLaw, number> = {
  const3dB: 0,
  'const4.5dB': 1,
  const6dB: 2,
  linear0dB: 3,
};

const METER_TAP_VALUES: Record<MeterTap, number> = {
  preFader: 0,
  postFader: 1,
};

// Mirrors SonareSendTiming: post-fader is 0 so a zero-initialized C ABI send
// defaults to post-fader. (Distinct from METER_TAP_VALUES, which keeps 0=pre.)
const SEND_TIMING_VALUES: Record<SendTiming, number> = {
  postFader: 0,
  preFader: 1,
};

export function automationCurveValue(curve: AutomationCurve): number {
  return resolveEnumOrdinal(
    curve,
    { linear: 0, exponential: 1, hold: 2, 's-curve': 3 },
    'automation curve',
  );
}

export function panLawValue(panLaw: PanLaw | number): number {
  return resolveEnumOrdinal(panLaw, PAN_LAW_VALUES, 'pan law');
}

export function panModeValue(panMode: PanMode): number {
  const mode = typeof panMode === 'string' ? panMode.replace(/_/g, '-').toLowerCase() : panMode;
  return resolveEnumOrdinal(
    mode,
    { balance: 0, pan: 1, stereopan: 1, 'stereo-pan': 1, dualpan: 2, 'dual-pan': 2 },
    'pan mode',
  );
}

export function meterTapValue(tap: MeterTap | number): number {
  return resolveEnumOrdinal(tap, METER_TAP_VALUES, 'meter tap');
}

export function sendTimingValue(timing: SendTiming | number): number {
  return resolveEnumOrdinal(timing, SEND_TIMING_VALUES, 'send timing');
}
