/**
 * What the one-shot meters report when there is nothing to measure: a silent phase-scope point has
 * no angle, and every other one-shot meter is refused empty input before it could be unmeasured.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  ebur128LoudnessRangeLu,
  init,
  meteringDcOffset,
  meteringDetectClipping,
  meteringPhaseScope,
  meteringSilenceRatio,
  meteringTruePeakDbtp,
} from '../dist/index.js';

beforeAll(async () => {
  await init();
});

const sampleRate = 48000;
const silence = new Float32Array(4800);
/** Silent for the first half, a tone for the second, so only some points have an angle. */
const halfSilent = new Float32Array(4800).map((_, i) =>
  i < 2400 ? 0 : Math.sin((2 * Math.PI * 440 * i) / sampleRate),
);

describe('a phase scope point at exact silence has no angle', () => {
  it('keeps one NaN angle per silent point and omits the mean when every point is silent', () => {
    const result = meteringPhaseScope({ left: silence, right: silence, sampleRate });
    expect(result.angleRad).toHaveLength(silence.length);
    expect(Array.from(result.angleRad).every(Number.isNaN)).toBe(true);
    expect('averageAbsAngleRad' in result).toBe(false);
  });

  it('measures the mean over the points that have an angle', () => {
    const result = meteringPhaseScope({ left: halfSilent, right: halfSilent, sampleRate });
    const angles = Array.from(result.angleRad);
    expect(angles).toHaveLength(halfSilent.length);
    expect(Number.isNaN(angles[0])).toBe(true);
    expect(Number.isFinite(angles[angles.length - 1])).toBe(true);
    expect(result.averageAbsAngleRad).toBeDefined();
    expect(Number.isFinite(result.averageAbsAngleRad as number)).toBe(true);
  });
});

describe('a one-shot meter cannot be handed no samples, so its result is always measured', () => {
  const empty = new Float32Array(0);
  it.each([
    ['meteringSilenceRatio', () => meteringSilenceRatio({ samples: empty, sampleRate })],
    ['meteringDcOffset', () => meteringDcOffset({ samples: empty, sampleRate })],
    ['meteringDetectClipping', () => meteringDetectClipping({ samples: empty, sampleRate })],
    ['meteringTruePeakDbtp', () => meteringTruePeakDbtp({ samples: empty, sampleRate })],
    ['ebur128LoudnessRangeLu', () => ebur128LoudnessRangeLu({ samples: empty, sampleRate })],
  ] as const)('%s refuses empty input', (entry, run) => {
    expect(run).toThrow(new RangeError(`${entry}: samples must not be empty`));
  });

  it('reads silence as measured', () => {
    expect(meteringSilenceRatio({ samples: silence, sampleRate })).toBe(1);
    expect(meteringDcOffset({ samples: silence, sampleRate })).toBe(0);
    expect(meteringDetectClipping({ samples: silence, sampleRate }).clippedRatio).toBe(0);
  });
});
