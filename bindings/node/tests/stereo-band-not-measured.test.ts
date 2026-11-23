/**
 * A band correlation the analysis could not take is an absent key, never a 0 that reads as a
 * measurement.
 */

import { describe, expect, it } from 'vitest';
import { masteringAnalyzeStereo } from '../src/index.js';

const SR = 48000;

const sine = (hz: number, seconds: number) =>
  Float32Array.from({ length: Math.round(SR * seconds) }, (_, n) =>
    Math.sin((2 * Math.PI * hz * n) / SR),
  );

describe('a stereo band that was not measured', () => {
  it('leaves a band correlation out where a channel is silent', () => {
    const tone = sine(1000, 0.5);
    const analyze = (right: Float32Array) =>
      masteringAnalyzeStereo({
        analysis: 'stereo.monoCompatCheckLogBands',
        left: tone,
        right,
        sampleRate: SR,
      });
    const silentRight = analyze(new Float32Array(tone.length));
    expect(silentRight.bands.length).toBeGreaterThan(0);
    for (const band of silentRight.bands) expect(band).not.toHaveProperty('correlation');
    const sounding = analyze(tone).bands.find((band) => band.lowHz <= 1000 && band.highHz > 1000);
    expect(sounding?.correlation).toBeCloseTo(1, 3);
  });
});
