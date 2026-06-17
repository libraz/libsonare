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

export function trackKindValue(kind: ProjectTrackDesc['kind']): number {
  if (kind === undefined || kind === 'audio') {
    return 0;
  }
  if (kind === 'midi') {
    return 1;
  }
  if (kind === 'aux') {
    return 2;
  }
  if (typeof kind === 'number') {
    return kind;
  }
  throw new Error(`Invalid track kind: ${kind}`);
}

export function warpModeValue(mode: WarpMode | number | undefined): number {
  if (mode === undefined || mode === 'off') {
    return 0;
  }
  if (mode === 'repitch') {
    return 1;
  }
  if (mode === 'tempo-sync') {
    return 2;
  }
  if (typeof mode === 'number') {
    return mode;
  }
  throw new Error(`Invalid warp mode: ${mode}`);
}

export function engineAutomationCurveValue(curve: EngineAutomationPointCurve | undefined): number {
  if (curve === undefined) {
    return 0;
  }
  if (typeof curve === 'number') {
    return curve;
  }
  return automationCurveValue(curve);
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
  if (typeof curve === 'number') {
    return curve;
  }
  if (curve === 'linear') {
    return 0;
  }
  if (curve === 'equalPower' || curve === 'equal-power' || curve === 'equal_power') {
    return 1;
  }
  if (curve === 'exponential') {
    return 2;
  }
  if (curve === 'logarithmic') {
    return 3;
  }
  throw new Error(`Invalid project fade curve: ${curve}`);
}

export function projectClipFadeValue(
  fade: ProjectClipFade | undefined,
): ProjectClipFade | undefined {
  if (fade === undefined) {
    return undefined;
  }
  const curve = projectFadeCurveValue(fade.curve);
  return curve === undefined ? { ...fade } : { ...fade, curve: curve as ProjectFadeCurve };
}

export function projectLoopModeValue(mode: ProjectLoopMode): number {
  if (typeof mode === 'number') {
    return mode;
  }
  if (mode === 'off') {
    return 0;
  }
  if (mode === 'loop') {
    return 1;
  }
  throw new Error(`Invalid project loop mode: ${mode}`);
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
  if (curve === 'linear') {
    return 0;
  }
  if (curve === 'exponential') {
    return 1;
  }
  if (curve === 'hold') {
    return 2;
  }
  if (curve === 's-curve') {
    return 3;
  }
  throw new Error(`Invalid automation curve: ${curve}`);
}

export function panLawValue(panLaw: PanLaw | number): number {
  if (typeof panLaw === 'number') {
    return panLaw;
  }
  const value = PAN_LAW_VALUES[panLaw];
  if (value === undefined) {
    throw new Error(`Invalid pan law: ${panLaw}`);
  }
  return value;
}

export function panModeValue(panMode: PanMode): number {
  if (typeof panMode === 'number') {
    return panMode;
  }
  const mode = panMode.replace(/_/g, '-').toLowerCase();
  if (mode === 'stereo-pan' || mode === 'stereopan' || mode === 'pan') {
    return 1; // SONARE_PAN_MODE_STEREO_PAN
  }
  if (mode === 'dual-pan' || mode === 'dualpan') {
    return 2; // SONARE_PAN_MODE_DUAL_PAN
  }
  return 0; // SONARE_PAN_MODE_BALANCE
}

export function meterTapValue(tap: MeterTap | number): number {
  if (typeof tap === 'number') {
    return tap;
  }
  const value = METER_TAP_VALUES[tap];
  if (value === undefined) {
    throw new Error(`Invalid meter tap: ${tap}`);
  }
  return value;
}

export function sendTimingValue(timing: SendTiming | number): number {
  if (typeof timing === 'number') {
    return timing;
  }
  const value = SEND_TIMING_VALUES[timing];
  if (value === undefined) {
    throw new Error(`Invalid send timing: ${timing}`);
  }
  return value;
}
