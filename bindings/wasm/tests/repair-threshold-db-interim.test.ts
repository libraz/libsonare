/**
 * Interim: the native repair configs still read linear amplitudes, so the facade
 * converts thresholdDb / clipThresholdDb in internal/repair_common.ts. When the
 * C ABI takes the dBFS spelling the first case goes red: delete the conversion
 * table there and this file in the same change.
 */
import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringRepairDetectClipping } from '../src/index';

const HEADER = new URL('../../../include/sonare/sonare_c_mastering.h', import.meta.url).pathname;

beforeAll(async () => {
  await init();
});

/** The body of the C struct typedef'd as `name`. */
function structBody(text: string, name: string): string {
  const end = text.indexOf(`} ${name};`);
  const start = text.lastIndexOf('typedef struct {', end);
  expect(end, name).toBeGreaterThan(0);
  return text.slice(start, end);
}

describe('repair thresholds in dBFS over a linear native config', () => {
  it('the native declick and declip configs still take linear thresholds', () => {
    const header = readFileSync(HEADER, 'utf8');
    expect(structBody(header, 'SonareDeclickConfig')).toMatch(/\bfloat threshold;/);
    expect(structBody(header, 'SonareDeclipConfig')).toMatch(/\bfloat clip_threshold;/);
  });

  it('clipThresholdDb reaches the native config as its linear amplitude', () => {
    const samples = new Float32Array([0.5, 0.6, 0.6, 0.5, 0.5, 0.6, 0.5, 0.5]);
    const detected = masteringRepairDetectClipping({
      samples,
      sampleRate: 48000,
      clipThresholdDb: 20 * Math.log10(0.55),
    });
    expect(detected.sampleCount).toBe(3);
  });
});
