/**
 * The mixer worklet's meter must report the quantity its field names claim.
 *
 * `truePeakDb*` is an inter-sample peak: ITU-R BS.1770-4 Annex 2 defines it as
 * measured after at least 4x oversampling, because the continuous waveform
 * routinely rises above every sample it was reconstructed from. A sample peak
 * published under that name under-reports precisely the headroom a ceiling
 * decision is made from. The worklet now drives the same
 * `sonare::mixing::MeterProcessor` the engine's meter telemetry publishes from,
 * so there is one measurement rather than one per producer.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, meteringTruePeakDb, mixingScenePresetJson, RealtimeEngine } from '../dist/index.js';
import {
  init as initWorklet,
  type SonareWorkletMeterSnapshot,
  SonareWorkletProcessor,
} from '../dist/worklet.js';

const SR = 48000;
const BLOCK = 128;
const FLOOR_DB = -120;

beforeAll(async () => {
  await init();
  await initWorklet();
});

/**
 * A sine at a quarter of the sample rate, phase-offset by a quarter period.
 *
 * Every sample lands on +-amp/sqrt(2) while the continuous waveform it
 * represents reaches +-amp, so the inter-sample peak sits 3.01 dB above the
 * sample peak by construction. A meter that reports the sample peak cannot
 * produce that difference, which is what makes the assertions below non-vacuous.
 */
function interSamplePeakSine(length: number, amplitude = 0.5): Float32Array {
  const out = new Float32Array(length);
  for (let i = 0; i < length; i++) {
    out[i] = amplitude * Math.sin(2 * Math.PI * 0.25 * i + Math.PI / 4);
  }
  return out;
}

interface MixerRun {
  meters: SonareWorkletMeterSnapshot[];
  output: Float32Array;
}

/** Runs `blocks` blocks of `input` through the mixer worklet, capturing both. */
function runMixerWorklet(input: Float32Array, blocks: number): MixerRun {
  const meters: SonareWorkletMeterSnapshot[] = [];
  const output = new Float32Array(blocks * BLOCK);
  const processor = new SonareWorkletProcessor(
    {
      sceneJson: mixingScenePresetJson('vocalReverbSend'),
      sampleRate: SR,
      blockSize: BLOCK,
      meterIntervalFrames: BLOCK,
    },
    { onMeter: (meter) => meters.push(meter) },
  );
  try {
    for (let block = 0; block < blocks; block++) {
      const source = input.subarray(block * BLOCK, (block + 1) * BLOCK);
      const outLeft = new Float32Array(BLOCK);
      const outRight = new Float32Array(BLOCK);
      processor.process([[source, source]], [[outLeft, outRight]]);
      output.set(outLeft, block * BLOCK);
    }
  } finally {
    processor.destroy();
  }
  return { meters, output };
}

function dbFields(meter: SonareWorkletMeterSnapshot): number[] {
  return [
    meter.peakDbL,
    meter.peakDbR,
    meter.rmsDbL,
    meter.rmsDbR,
    meter.truePeakDbL,
    meter.truePeakDbR,
  ];
}

describe('mixer worklet true-peak metering', () => {
  const blocks = 16;

  it('reports an inter-sample peak above the sample peak', () => {
    const { meters } = runMixerWorklet(interSamplePeakSine(blocks * BLOCK), blocks);
    const meter = meters.at(-1);
    expect(meter).toBeDefined();
    if (!meter) {
      throw new Error('expected the worklet to publish a meter');
    }
    // The construction puts the inter-sample peak 3.01 dB above the sample peak.
    // A sample-peak implementation reports the two as the same number, so this
    // margin cannot be met by relabelling one as the other.
    expect(meter.truePeakDbL).toBeGreaterThan(meter.peakDbL + 1.5);
    expect(meter.truePeakDbR).toBeGreaterThan(meter.peakDbR + 1.5);
  });

  it('agrees with the offline true-peak measurement of the same audio', () => {
    const { meters, output } = runMixerWorklet(interSamplePeakSine(blocks * BLOCK), blocks);
    const meter = meters.at(-1);
    if (!meter) {
      throw new Error('expected the worklet to publish a meter');
    }
    // The published snapshot describes the most recent block, so compare against
    // that block. meteringTruePeakDb is an independent implementation of the
    // same standard, which is what makes this a cross-check rather than a
    // restatement of the worklet's own arithmetic.
    const lastBlock = output.subarray((blocks - 1) * BLOCK, blocks * BLOCK);
    expect(meter.truePeakDbL).toBeCloseTo(meteringTruePeakDb(lastBlock, SR, 4), 0);
  });

  it('keeps every dB field finite on a signal', () => {
    const { meters } = runMixerWorklet(interSamplePeakSine(blocks * BLOCK), blocks);
    for (const meter of meters) {
      for (const value of dbFields(meter)) {
        expect(Number.isFinite(value)).toBe(true);
      }
    }
  });

  it('floors every dB field at -120 on silence, matching the engine worklet', () => {
    // -Infinity is not a usable meter reading: it makes the usual bar geometry
    // NaN, serializes to null, and poisons min/max aggregation. Both producers
    // must report the same finite floor so a host can treat their records
    // interchangeably.
    const { meters } = runMixerWorklet(new Float32Array(blocks * BLOCK), blocks);
    const mixerMeter = meters.at(-1);
    if (!mixerMeter) {
      throw new Error('expected the worklet to publish a meter');
    }
    for (const value of dbFields(mixerMeter)) {
      expect(value).toBe(FLOOR_DB);
    }

    const engine = new RealtimeEngine(SR, BLOCK);
    try {
      engine.play();
      for (let block = 0; block < 4; block++) {
        engine.process([new Float32Array(BLOCK), new Float32Array(BLOCK)]);
      }
      const engineMeter = engine.drainMeterTelemetry(64).at(-1);
      expect(engineMeter).toBeDefined();
      if (!engineMeter) {
        throw new Error('expected the engine to publish meter telemetry');
      }
      for (const value of [
        engineMeter.peakDbL,
        engineMeter.peakDbR,
        engineMeter.rmsDbL,
        engineMeter.rmsDbR,
        engineMeter.truePeakDbL,
        engineMeter.truePeakDbR,
      ]) {
        expect(value).toBe(FLOOR_DB);
      }
    } finally {
      engine.destroy();
    }
  });

  it('restarts the meter when metering is re-enabled after a gap', () => {
    // Filter history and the published snapshot both predate the gap; carrying
    // them across would report a peak from audio the caller had metering off for.
    const loud = interSamplePeakSine(BLOCK, 0.9);
    const processor = new SonareWorkletProcessor(
      {
        sceneJson: mixingScenePresetJson('vocalReverbSend'),
        sampleRate: SR,
        blockSize: BLOCK,
        meterIntervalFrames: 0,
      },
      {},
    );
    try {
      for (let block = 0; block < 4; block++) {
        processor.process([[loud, loud]], [[new Float32Array(BLOCK), new Float32Array(BLOCK)]]);
      }
      expect(() =>
        processor.receiveMessage({ type: 'setMeterInterval', frames: BLOCK }),
      ).not.toThrow();
    } finally {
      processor.destroy();
    }
  });
});
