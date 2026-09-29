/**
 * Realtime NativeSynth GM-program routing: one patch can either follow MIDI
 * program changes or stay on the selected patch.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';

const SAMPLE_RATE = 48000;
const BLOCK_SIZE = 128;
const BLOCK_COUNT = 64;

function midi1Word(status: number, channel: number, data0: number, data1: number): number {
  return (
    (0x2 << 28) |
    ((status & 0xf) << 20) |
    ((channel & 0xf) << 16) |
    ((data0 & 0x7f) << 8) |
    (data1 & 0x7f)
  );
}

function maxAbsDifference(left: Float32Array, right: Float32Array): number {
  expect(left.length).toBe(right.length);
  let max = 0;
  for (let i = 0; i < left.length; i += 1) {
    max = Math.max(max, Math.abs(left[i] - right[i]));
  }
  return max;
}

function renderProgram(useGmPrograms: boolean, program: number): Float32Array {
  const engine = new RealtimeEngine(SAMPLE_RATE, BLOCK_SIZE);
  try {
    engine.setSynthInstrument({ preset: 'saw-lead', useGmPrograms }, 0);
    engine.pushMidiUmp(0, [midi1Word(0xc, 0, program, 0)]);
    engine.pushMidiNoteOn(0, 0, 0, 60, 100);
    const rendered = new Float32Array(BLOCK_SIZE * BLOCK_COUNT);
    for (let block = 0; block < BLOCK_COUNT; block += 1) {
      const [left] = engine.process([new Float32Array(BLOCK_SIZE), new Float32Array(BLOCK_SIZE)]);
      rendered.set(left, block * BLOCK_SIZE);
    }
    return rendered;
  } finally {
    engine.destroy();
  }
}

describe('RealtimeEngine NativeSynth GM program routing', () => {
  beforeAll(async () => {
    await init();
  });

  it('follows MIDI programs only when useGmPrograms is enabled', () => {
    const gmPiano = renderProgram(true, 0);
    const gmViolin = renderProgram(true, 40);
    expect(Math.max(...gmPiano.map(Math.abs))).toBeGreaterThan(0);
    expect(Math.max(...gmViolin.map(Math.abs))).toBeGreaterThan(0);
    expect(maxAbsDifference(gmPiano, gmViolin)).toBeGreaterThan(1e-4);

    const fixedPiano = renderProgram(false, 0);
    const fixedViolin = renderProgram(false, 40);
    expect(maxAbsDifference(fixedPiano, fixedViolin)).toBe(0);
  });

  it('requires useGmPrograms to be a boolean when supplied', () => {
    const engine = new RealtimeEngine(SAMPLE_RATE, BLOCK_SIZE);
    try {
      expect(() =>
        engine.setSynthInstrument(
          // @ts-expect-error Runtime validation intentionally covers a wrong JS type.
          { preset: 'saw-lead', useGmPrograms: 1 },
          0,
        ),
      ).toThrow(/useGmPrograms/);
    } finally {
      engine.destroy();
    }
  });
});
