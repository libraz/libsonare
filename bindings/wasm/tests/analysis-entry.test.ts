import { beforeAll, describe, expect, it } from 'vitest';
import * as analysis from '../dist/analysis.js';

describe('analysis-only WASM entry', () => {
  beforeAll(async () => {
    await analysis.init();
  });

  it('loads the dedicated binary and exposes analysis capabilities', () => {
    expect(analysis.isInitialized()).toBe(true);
    expect(analysis.capabilities().features).toMatchObject({
      mastering: false,
      mixing: false,
      fx: false,
    });
    expect(analysis.meteringPeakDb(new Float32Array([0, 0.5, -0.25]))).toBeCloseTo(-6.0206, 3);
  });

  it('does not expose non-analysis APIs', () => {
    const entry = analysis as Record<string, unknown>;
    for (const name of [
      'masterAudio',
      'mixStereo',
      'Mixer',
      'Project',
      'RealtimeEngine',
      'synthesizeRir',
      'estimateRoom',
      'roomMorph',
    ]) {
      expect(entry[name]).toBeUndefined();
    }
  });
});
