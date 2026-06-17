import type { AutomationCurve, MeterTap, PanLaw, PanMode, SendTiming } from './public_types';

export function automationCurveCode(curve: AutomationCurve): number {
  switch (curve) {
    case 'linear':
      return 0;
    case 'exponential':
      return 1;
    case 'hold':
      return 2;
    case 's-curve':
      return 3;
    default:
      throw new Error(`Invalid automation curve: ${curve}`);
  }
}

export function panLawCode(panLaw: PanLaw | number): number {
  if (typeof panLaw === 'number') {
    return panLaw;
  }
  switch (panLaw) {
    case 'const4.5dB':
      return 1;
    case 'const6dB':
      return 2;
    case 'linear0dB':
      return 3;
    default:
      return 0;
  }
}

export function panModeCode(panMode: PanMode | number): number {
  if (typeof panMode === 'number') {
    return panMode;
  }
  switch (panMode) {
    case 'stereoPan':
    case 'stereo-pan':
      return 1;
    case 'dualPan':
    case 'dual-pan':
      return 2;
    default:
      return 0;
  }
}

export function meterTapCode(tap: MeterTap | number): number {
  return tap === 'preFader' || tap === 0 ? 0 : 1;
}

export function sendTimingCode(timing: SendTiming | number): number {
  // Mirrors SonareSendTiming: post-fader is 0 (so an omitted/zeroed value is
  // post-fader), pre-fader is 1. A raw number is passed through as the C ABI int.
  if (typeof timing === 'number') {
    return timing;
  }
  return timing === 'preFader' ? 1 : 0;
}
