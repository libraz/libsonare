/**
 * Wide meters through the live worklet engine: a 5.1 bus delivers every plane on
 * both the postMessage and SharedArrayBuffer meter paths, mono and stereo
 * snapshots carry per-plane arrays, and the interval gate is per target.
 */

import { type MixSceneStrip, RealtimeEngine } from '../dist/index.js';
import {
  SONARE_METER_RING_CHANNEL_COUNT_OFFSET,
  SONARE_METER_RING_MAX_CHANNELS,
  SONARE_METER_RING_PLANES_OFFSET,
} from '../src/worklet/meter-rings';
import type { WorkletMeterSnapshot } from '../src/worklet/protocol';
import {
  createMeterRingBuffer,
  describe,
  expect,
  it,
  readMeterRingBuffer,
  SONARE_METER_RING_PROTOCOL_VERSION,
  SONARE_METER_RING_RECORD_FLOATS,
  SonareRealtimeEngineWorkletProcessor,
  setupWorklet,
} from './_worklet_helpers';

const SR = 48000;
const BLOCK = 256;
const BUS_TARGET = 33;

type EngineLike = Pick<
  RealtimeEngine,
  'setTrackBuses' | 'setTrackLanes' | 'setStrip' | 'setClips' | 'play'
>;

function seedBus(
  engine: EngineLike,
  channelLayout: 'mono' | 'stereo' | '5.1',
  strip?: MixSceneStrip,
): void {
  engine.setClips([
    {
      id: 1,
      trackId: 10,
      channels: [new Float32Array(BLOCK * 8).fill(0.5)],
      startPpq: 0,
      lengthSamples: BLOCK * 8,
    },
  ]);
  engine.setTrackBuses([{ busId: 1, gainDb: 0, channelLayout }]);
  engine.setTrackLanes([{ trackId: 10, outputBusId: 1 }]);
  if (strip) {
    engine.setStrip(10, strip);
  }
  engine.play();
}

const SURROUND_STRIP: MixSceneStrip = {
  id: 's',
  surroundPan: { azimuth: -110, divergence: 0, lfe: 0 },
} as MixSceneStrip;

function silent(channels: number): Float32Array[] {
  return Array.from({ length: channels }, () => new Float32Array(BLOCK));
}

/** The first bus meter a plain engine reads through the engine drain, as the reference. */
function referenceBusMeter(
  channelLayout: 'mono' | 'stereo' | '5.1',
  strip: MixSceneStrip | undefined,
  channels: number,
) {
  const engine = new RealtimeEngine({ sampleRate: SR, maxBlockSize: BLOCK });
  try {
    seedBus(engine, channelLayout, strip);
    engine.process(silent(channels));
    const record = engine.drainMeterTelemetry().find((r) => r.targetId === BUS_TARGET);
    if (!record) {
      throw new Error('expected a bus meter');
    }
    return record;
  } finally {
    engine.destroy();
  }
}

function engineOf(processor: SonareRealtimeEngineWorkletProcessor): RealtimeEngine {
  return (processor as unknown as { engine: RealtimeEngine }).engine;
}

/** Silence is -Infinity and an unmeasured level is undefined. */
function expectLevel(actual: number | undefined, expected: number | undefined): void {
  expect(expected).not.toBeNaN();
  if (expected === undefined || expected === Number.NEGATIVE_INFINITY) {
    expect(actual).toBe(expected);
  } else {
    expect(actual).toBeCloseTo(expected, 4);
  }
}

function expectPlanes(
  actual: (number | undefined)[] | undefined,
  expected: (number | undefined)[],
): void {
  expect(actual).toHaveLength(expected.length);
  expected.forEach((value, index) => {
    expectLevel(actual?.[index], value);
  });
}

describe('wide meters on the live worklet engine', () => {
  setupWorklet();

  it('delivers every plane of a 5.1 bus over postMessage', () => {
    const reference = referenceBusMeter('5.1', SURROUND_STRIP, 6);
    const meters: WorkletMeterSnapshot[] = [];
    const processor = new SonareRealtimeEngineWorkletProcessor({
      sampleRate: SR,
      maxBlockSize: BLOCK,
      numChannels: 6,
      meterIntervalFrames: BLOCK,
      transport: { onMeter: (meter) => meters.push(meter) },
    });
    try {
      seedBus(engineOf(processor), '5.1', SURROUND_STRIP);
      processor.process([[]], [silent(6)]);
      const bus = meters.find((m) => m.targetId === BUS_TARGET);
      expect(bus?.numChannels).toBe(6);
      expectPlanes(bus?.peakDb, reference.peakDb);
      expectPlanes(bus?.rmsDb, reference.rmsDb);
      expectPlanes(bus?.truePeakDbtp, reference.truePeakDbtp);
      expectPlanes(bus?.inputPeakDb, reference.inputPeakDb);
      // Ls (plane 4) carries the panned lane, above the silent front-left plane.
      expect((bus?.peakDb?.[4] ?? 0) - (bus?.peakDb?.[0] ?? 0)).toBeGreaterThan(10);
    } finally {
      processor.destroy();
    }
  });

  it('delivers every plane of a 5.1 bus over the shared meter ring', () => {
    const reference = referenceBusMeter('5.1', SURROUND_STRIP, 6);
    const ring = createMeterRingBuffer({ capacity: 64 });
    const processor = new SonareRealtimeEngineWorkletProcessor({
      sampleRate: SR,
      maxBlockSize: BLOCK,
      numChannels: 6,
      meterIntervalFrames: BLOCK,
      meterSharedBuffer: ring.sharedBuffer,
      transport: { postMessage: () => undefined },
    });
    try {
      seedBus(engineOf(processor), '5.1', SURROUND_STRIP);
      processor.process([[]], [silent(6)]);
      expect(ring.header[3]).toBe(SONARE_METER_RING_PROTOCOL_VERSION);
      const bus = readMeterRingBuffer(ring).meters.find((m) => m.targetId === BUS_TARGET);
      expect(bus?.numChannels).toBe(6);
      expectPlanes(bus?.peakDb, reference.peakDb);
      expectPlanes(bus?.rmsDb, reference.rmsDb);
      expectPlanes(bus?.truePeakDbtp, reference.truePeakDbtp);
      expectPlanes(bus?.inputPeakDb, reference.inputPeakDb);
    } finally {
      processor.destroy();
    }
  });

  it('delivers both stereo planes across both paths', () => {
    const reference = referenceBusMeter('stereo', undefined, 2);
    const posted: WorkletMeterSnapshot[] = [];
    const viaMessage = new SonareRealtimeEngineWorkletProcessor({
      sampleRate: SR,
      maxBlockSize: BLOCK,
      numChannels: 2,
      meterIntervalFrames: BLOCK,
      transport: { onMeter: (meter) => posted.push(meter) },
    });
    const ring = createMeterRingBuffer({ capacity: 64 });
    const viaRing = new SonareRealtimeEngineWorkletProcessor({
      sampleRate: SR,
      maxBlockSize: BLOCK,
      numChannels: 2,
      meterIntervalFrames: BLOCK,
      meterSharedBuffer: ring.sharedBuffer,
      transport: { postMessage: () => undefined },
    });
    try {
      seedBus(engineOf(viaMessage), 'stereo');
      seedBus(engineOf(viaRing), 'stereo');
      viaMessage.process([[]], [silent(2)]);
      viaRing.process([[]], [silent(2)]);
      const message = posted.find((m) => m.targetId === BUS_TARGET);
      const shared = readMeterRingBuffer(ring).meters.find((m) => m.targetId === BUS_TARGET);
      for (const meter of [message, shared]) {
        expect(meter).toBeDefined();
        expect(meter?.numChannels).toBe(2);
        expectPlanes(meter?.peakDb, reference.peakDb);
        expectPlanes(meter?.rmsDb, reference.rmsDb);
        expectPlanes(meter?.truePeakDbtp, reference.truePeakDbtp);
        expectPlanes(meter?.inputPeakDb, reference.inputPeakDb);
      }
      expect(message?.peakDb).toEqual(shared?.peakDb);
      expect(message?.rmsDb).toEqual(shared?.rmsDb);
      expect(message?.truePeakDbtp).toEqual(shared?.truePeakDbtp);
      expect(message?.inputPeakDb).toEqual(shared?.inputPeakDb);
    } finally {
      viaMessage.destroy();
      viaRing.destroy();
    }
  });

  it('offers mono per-plane arrays and filters unmeasured ring values', () => {
    const reference = referenceBusMeter('mono', undefined, 1);
    const posted: WorkletMeterSnapshot[] = [];
    const viaMessage = new SonareRealtimeEngineWorkletProcessor({
      sampleRate: SR,
      maxBlockSize: BLOCK,
      numChannels: 1,
      meterIntervalFrames: BLOCK,
      transport: { onMeter: (meter) => posted.push(meter) },
    });
    const ring = createMeterRingBuffer({ capacity: 64 });
    const viaRing = new SonareRealtimeEngineWorkletProcessor({
      sampleRate: SR,
      maxBlockSize: BLOCK,
      numChannels: 1,
      meterIntervalFrames: BLOCK,
      meterSharedBuffer: ring.sharedBuffer,
      transport: { postMessage: () => undefined },
    });
    try {
      seedBus(engineOf(viaMessage), 'mono');
      seedBus(engineOf(viaRing), 'mono');
      viaMessage.process([[]], [silent(1)]);
      viaRing.process([[]], [silent(1)]);
      const message = posted.find((meter) => meter.targetId === BUS_TARGET);
      expect(message?.numChannels).toBe(1);
      expectPlanes(message?.peakDb, reference.peakDb);
      expectPlanes(message?.rmsDb, reference.rmsDb);
      expectPlanes(message?.truePeakDbtp, reference.truePeakDbtp);
      expectPlanes(message?.inputPeakDb, reference.inputPeakDb);
      for (const key of ['peakDb', 'rmsDb', 'truePeakDbtp', 'inputPeakDb'] as const) {
        const values = message?.[key];
        expect(values).toHaveLength(1);
        expect(values).not.toContain(-120);
        expect(values?.some(Number.isNaN)).toBe(false);
      }

      const writeIndex = Atomics.load(ring.header, 0) >>> 0;
      const firstReadable = Math.max(0, writeIndex - ring.capacity);
      let recordOffset = -1;
      for (let index = firstReadable; index < writeIndex; index++) {
        const offset = (index % ring.capacity) * SONARE_METER_RING_RECORD_FLOATS;
        if (ring.records[offset + 2] === BUS_TARGET) {
          recordOffset = offset;
          break;
        }
      }
      if (recordOffset < 0) {
        throw new Error('expected a mono bus meter in the shared ring');
      }
      ring.records[recordOffset + 3] = Number.NaN;
      ring.records[recordOffset + 5] = Number.NaN;
      ring.records[recordOffset + 8] = Number.NaN;
      ring.records[recordOffset + 14] = Number.NaN;
      for (const scalarOffset of [4, 6, 9, 15]) {
        ring.records[recordOffset + scalarOffset] = -120;
      }
      for (let kind = 0; kind < 4; kind++) {
        ring.records[
          recordOffset + SONARE_METER_RING_PLANES_OFFSET + kind * SONARE_METER_RING_MAX_CHANNELS + 1
        ] = -120;
      }
      expect(ring.records[recordOffset + SONARE_METER_RING_CHANNEL_COUNT_OFFSET]).toBe(1);
      const shared = readMeterRingBuffer(ring).meters.find(
        (meter) => meter.targetId === BUS_TARGET,
      );
      expect(shared?.numChannels).toBe(1);
      for (const key of ['peakDb', 'rmsDb', 'truePeakDbtp', 'inputPeakDb'] as const) {
        const values = shared?.[key];
        expect(values).toEqual([undefined]);
        expect(values).not.toContain(-120);
        expect(values?.some(Number.isNaN)).toBe(false);
      }
    } finally {
      viaMessage.destroy();
      viaRing.destroy();
    }
  });

  it('widens the meter ring record to carry the per-plane block', () => {
    expect(SONARE_METER_RING_RECORD_FLOATS).toBe(49);
    expect(createMeterRingBuffer({ capacity: 4 }).header[3]).toBe(0);
  });

  it('gates the meter interval per target', () => {
    const processor = new SonareRealtimeEngineWorkletProcessor({
      sampleRate: SR,
      maxBlockSize: BLOCK,
      numChannels: 2,
      meterIntervalFrames: 1000,
      transport: { onMeter: () => undefined },
    });
    try {
      const gate = (
        processor as unknown as { meterDue(id: number, frame: number): boolean }
      ).meterDue.bind(processor);
      expect(gate(1, 5000)).toBe(true);
      // Another target's first record is not held back by the busy one.
      expect(gate(2, 5100)).toBe(true);
      expect(gate(1, 5600)).toBe(false);
      expect(gate(2, 5600)).toBe(false);
      expect(gate(1, 6000)).toBe(true);
      expect(gate(2, 6100)).toBe(true);
      // Every target past the bus range shares one slot.
      expect(gate(0xffff, 6200)).toBe(true);
    } finally {
      processor.destroy();
    }
  });

  it('keeps each target on its own cadence while rendering', () => {
    const meters: WorkletMeterSnapshot[] = [];
    const processor = new SonareRealtimeEngineWorkletProcessor({
      sampleRate: SR,
      maxBlockSize: BLOCK,
      numChannels: 2,
      meterIntervalFrames: BLOCK * 4,
      transport: { onMeter: (meter) => meters.push(meter) },
    });
    try {
      seedBus(engineOf(processor), 'mono');
      for (let block = 0; block < 9; ++block) {
        processor.process([[]], [silent(2)]);
      }
      for (const target of [0, 1, BUS_TARGET]) {
        const frames = meters.filter((m) => m.targetId === target).map((m) => m.frame);
        expect(frames).toEqual([0, BLOCK * 4, BLOCK * 8]);
      }
    } finally {
      processor.destroy();
    }
  });
});
