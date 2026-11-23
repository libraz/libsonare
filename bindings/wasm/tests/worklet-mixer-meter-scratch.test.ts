import type { MeterScratchField } from '../dist/index.js';
import type { WorkletMeterSnapshot } from '../dist/worklet.js';
import {
  createMeterRingBuffer,
  describe,
  expect,
  it,
  type Mixer,
  mixingScenePreset,
  readMeterRingBuffer,
  SONARE_METER_RING_PROTOCOL_VERSION,
  SonareWorkletProcessor,
  setupWorklet,
} from './_worklet_helpers';

// The SAB meter ring exists so the audio render callback allocates nothing. The
// worklet reusing one record is only half of that: `meterSnapshot()` returns a
// freshly built seven-field embind object, so calling it per interval allocates
// on the render thread whatever this file then does with the result. The
// allocation-free form latches the reading inside the mixer and reads it back
// one number at a time, which is what these tests pin down — every publish, not
// just the first.
describe('SonareWorkletProcessor meter scratch', () => {
  setupWorklet();

  const sampleRate = 48000;
  const blockSize = 128;

  /** Records which mixer accessors a render cycle reached for. */
  interface MeterCalls {
    snapshots: number;
    latches: number;
    /** Field indices passed to `meterScratchValue`, one array per latch. */
    fieldsPerLatch: number[][];
    /** Values returned by `meterScratchValue`, one array per latch. */
    valuesPerLatch: number[][];
    restore: () => void;
  }

  function instrumentMeterAccessors(processor: SonareWorkletProcessor): MeterCalls {
    const mixer = (processor as unknown as { mixer: Mixer }).mixer;
    const realSnapshot = mixer.meterSnapshot;
    const realLatch = mixer.latchMeterSnapshot;
    const realValue = mixer.meterScratchValue;
    const calls: MeterCalls = {
      snapshots: 0,
      latches: 0,
      fieldsPerLatch: [],
      valuesPerLatch: [],
      restore: () => {
        mixer.meterSnapshot = realSnapshot;
        mixer.latchMeterSnapshot = realLatch;
        mixer.meterScratchValue = realValue;
      },
    };
    mixer.meterSnapshot = function (this: Mixer) {
      calls.snapshots++;
      return realSnapshot.call(this);
    };
    mixer.latchMeterSnapshot = function (this: Mixer) {
      calls.latches++;
      calls.fieldsPerLatch.push([]);
      calls.valuesPerLatch.push([]);
      return realLatch.call(this);
    };
    mixer.meterScratchValue = function (this: Mixer, field: MeterScratchField) {
      const value = realValue.call(this, field);
      calls.fieldsPerLatch[calls.fieldsPerLatch.length - 1]?.push(field);
      calls.valuesPerLatch[calls.valuesPerLatch.length - 1]?.push(value ?? Number.NaN);
      return value;
    };
    return calls;
  }

  /** One render quantum whose amplitude identifies the block it came from. */
  function block(amplitude: number): Float32Array {
    const samples = new Float32Array(blockSize);
    samples[0] = amplitude;
    return samples;
  }

  it('writes the ring from scalar accessors, never from meterSnapshot', () => {
    const ring = createMeterRingBuffer({ capacity: 64 });
    const posted: unknown[] = [];
    const processor = new SonareWorkletProcessor({
      sceneJson: JSON.stringify(mixingScenePreset('vocalReverbSend')),
      sampleRate,
      maxBlockSize: blockSize,
      meterIntervalFrames: blockSize,
      meterSharedBuffer: ring.sharedBuffer,
      transport: { postMessage: (meter) => posted.push(meter) },
    });
    const calls = instrumentMeterAccessors(processor);
    try {
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      const publishes = 4;
      for (let cycle = 0; cycle < publishes; cycle++) {
        // A different amplitude per cycle, so a snapshot taken once and reused
        // cannot pass for a snapshot taken per cycle.
        const amplitude = 0.1 * (cycle + 1);
        processor.process(
          [
            [block(amplitude), block(amplitude)],
            [block(0), block(0)],
          ],
          [[outL, outR]],
        );
      }

      // The object-returning accessor is the allocation this fix removes; the
      // render thread must not reach it at all, on any cycle.
      expect(calls.snapshots).toBe(0);
      expect(posted).toHaveLength(0);
      // One latch per publish: a fix that only held for the first interval would
      // leave this at 1 while the ring still filled up.
      expect(calls.latches).toBe(publishes);
      for (const fields of calls.fieldsPerLatch) {
        expect(fields).toEqual([0, 1, 2, 3, 4, 5, 6]);
      }

      const read = readMeterRingBuffer(ring);
      expect(read.meters).toHaveLength(publishes);
      read.meters.forEach((meter, index) => {
        expect(meter.frame).toBe((index + 1) * blockSize);
        expect(meter.targetId).toBe(0);
        // The seven scalar scratch readings are folded into per-plane arrays
        // and the stereo correlation field.
        expect([...meter.peakDb, ...meter.rmsDb, meter.correlation, ...meter.truePeakDbtp]).toEqual(
          calls.valuesPerLatch[index],
        );
        expect(meter.numChannels).toBe(2);
        expect(meter.inputPeakDb).toEqual([undefined, undefined]);
        // The mixer worklet measures no LUFS or gain reduction.
        for (const key of ['momentaryLufs', 'shortTermLufs', 'integratedLufs', 'gainReductionDb']) {
          expect(key in meter, key).toBe(false);
        }
      });
      // Reuse must not smear one publish over the next: the rising input makes
      // every block's peak distinct, so identical records would mean the fields
      // were read once and re-serialised.
      expect(new Set(read.meters.map((meter) => meter.peakDb[0])).size).toBe(publishes);

      // What the ring last received is what the mixer still holds, read here
      // through the object API now that the render cycles are over.
      calls.restore();
      const measured = (processor as unknown as { mixer: Mixer }).mixer.meterSnapshot();
      expect(read.meters[publishes - 1]).toMatchObject({
        peakDb: measured.peakDb,
        rmsDb: measured.rmsDb,
        correlation: measured.correlation,
        truePeakDbtp: measured.truePeakDbtp,
      });
    } finally {
      calls.restore();
      processor.destroy();
    }
  });

  it('keeps meterSnapshot off the postMessage fallback path too', () => {
    const meters: WorkletMeterSnapshot[] = [];
    const processor = new SonareWorkletProcessor({
      sceneJson: JSON.stringify(mixingScenePreset('vocalReverbSend')),
      sampleRate,
      maxBlockSize: blockSize,
      meterIntervalFrames: blockSize,
      transport: { onMeter: (meter) => meters.push(meter) },
    });
    const calls = instrumentMeterAccessors(processor);
    try {
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      for (let cycle = 0; cycle < 2; cycle++) {
        processor.process([[block(0.5), block(0.5)], [block(0)]], [[outL, outR]]);
      }
      expect(meters).toHaveLength(2);
      expect(calls.snapshots).toBe(0);
      expect(calls.latches).toBe(2);
      // The posted object is unavoidable — a listener may retain it — but it is
      // built from the latched values, so it is the only object per publish.
      meters.forEach((meter, index) => {
        expect(meter.peakDb[0]).toBe(calls.valuesPerLatch[index][0]);
        expect(meter.peakDb[1]).toBe(calls.valuesPerLatch[index][1]);
      });
    } finally {
      calls.restore();
      processor.destroy();
    }
  });

  it('leaves the last meter record intact when a late scratch getter throws', () => {
    const ring = createMeterRingBuffer({ capacity: 1 });
    expect(ring.header[3]).toBe(0);
    const processor = new SonareWorkletProcessor({
      sceneJson: JSON.stringify(mixingScenePreset('vocalReverbSend')),
      sampleRate,
      maxBlockSize: blockSize,
      meterIntervalFrames: blockSize,
      meterSharedBuffer: ring.sharedBuffer,
      transport: { postMessage: () => undefined },
    });
    const mixer = (processor as unknown as { mixer: Mixer }).mixer;
    const realValue = mixer.meterScratchValue;
    try {
      expect(ring.header[3]).toBe(SONARE_METER_RING_PROTOCOL_VERSION);
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      const input = [block(0.25), block(0.25)];
      expect(processor.process([input], [[outL, outR]])).toBe(true);
      expect(Atomics.load(ring.header, 0) >>> 0).toBe(1);
      expect(Atomics.load(ring.slotStates, 0)).toBe(0);
      const payloadA = Array.from(
        new Uint8Array(ring.sharedBuffer, ring.header.byteLength, ring.records.byteLength),
      );

      mixer.meterScratchValue = function (this: Mixer, field: MeterScratchField) {
        if (field === 6) {
          throw new Error('late mixer scratch getter');
        }
        return realValue.call(this, field);
      };
      expect(() => processor.process([input], [[outL, outR]])).toThrow('late mixer scratch getter');
      // A is a real completed publication, so this failure reached the meter
      // preparation path and proves no claim or partial store followed it.
      expect(Atomics.load(ring.header, 0) >>> 0).toBe(1);
      expect(Atomics.load(ring.slotStates, 0)).toBe(0);
      expect(
        Array.from(
          new Uint8Array(ring.sharedBuffer, ring.header.byteLength, ring.records.byteLength),
        ),
      ).toEqual(payloadA);

      mixer.meterScratchValue = realValue;
      expect(processor.process([input], [[outL, outR]])).toBe(true);
      // The failed preparation does not advance the cursor; the next complete
      // generation is therefore the second publication.
      expect(Atomics.load(ring.header, 0) >>> 0).toBe(2);
      expect(readMeterRingBuffer(ring, 1).meters.at(-1)?.frame).toBe(blockSize * 3);
    } finally {
      mixer.meterScratchValue = realValue;
      processor.destroy();
    }
  });
});
