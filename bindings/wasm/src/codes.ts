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
    case 'const3dB':
      return 0;
    case 'const4.5dB':
      return 1;
    case 'const6dB':
      return 2;
    case 'linear0dB':
      return 3;
    default:
      throw new Error(`Invalid pan law: ${panLaw}`);
  }
}

export function panModeCode(panMode: PanMode | number): number {
  if (typeof panMode === 'number') {
    return panMode;
  }
  switch (panMode) {
    case 'balance':
      return 0;
    case 'stereoPan':
    case 'stereo-pan':
      return 1;
    case 'dualPan':
    case 'dual-pan':
      return 2;
    default:
      throw new Error(`Invalid pan mode: ${panMode}`);
  }
}

export function meterTapCode(tap: MeterTap | number): number {
  if (typeof tap === 'number') {
    return tap;
  }
  switch (tap) {
    case 'preFader':
      return 0;
    case 'postFader':
      return 1;
    default:
      throw new Error(`Invalid meter tap: ${tap}`);
  }
}

export function sendTimingCode(timing: SendTiming | number): number {
  // Mirrors SonareSendTiming: post-fader is 0 (so an omitted/zeroed value is
  // post-fader), pre-fader is 1. A raw number is passed through as the C ABI int.
  if (typeof timing === 'number') {
    return timing;
  }
  return timing === 'preFader' ? 1 : 0;
}
