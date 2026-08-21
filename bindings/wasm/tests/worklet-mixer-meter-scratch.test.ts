import {
  createSonareMeterRingBuffer,
  describe,
  expect,
  it,
  type Mixer,
  mixingScenePresetJson,
  readSonareMeterRingBuffer,
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
    mixer.meterScratchValue = function (this: Mixer, field: number) {
      const value = realValue.call(this, field);
      calls.fieldsPerLatch[calls.fieldsPerLatch.length - 1]?.push(field);
      calls.valuesPerLatch[calls.valuesPerLatch.length - 1]?.push(value);
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
    const ring = createSonareMeterRingBuffer(64);
    const posted: unknown[] = [];
    const processor = new SonareWorkletProcessor(
      {
        sceneJson: mixingScenePresetJson('vocalReverbSend'),
        sampleRate,
        blockSize,
        meterIntervalFrames: blockSize,
        meterSharedBuffer: ring.sharedBuffer,
      },
      { postMessage: (meter) => posted.push(meter) },
    );
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

      const read = readSonareMeterRingBuffer(ring);
      expect(read.meters).toHaveLength(publishes);
      read.meters.forEach((meter, index) => {
        expect(meter.frame).toBe((index + 1) * blockSize);
        expect(meter.targetId).toBe(0);
        // The seven values the accessors returned for this publish are the seven
        // the ring carries, in the documented field order.
        expect([
          meter.peakDbL,
          meter.peakDbR,
          meter.rmsDbL,
          meter.rmsDbR,
          meter.correlation,
          meter.truePeakDbL,
          meter.truePeakDbR,
        ]).toEqual(calls.valuesPerLatch[index]);
        // The mixer worklet runs no K-weighting filters, so LUFS and gain
        // reduction stay declared-unavailable across a reused record.
        expect(meter.momentaryLufs).toBeNaN();
        expect(meter.shortTermLufs).toBeNaN();
        expect(meter.integratedLufs).toBeNaN();
        expect(meter.gainReductionDb).toBeNaN();
      });
      // Reuse must not smear one publish over the next: the rising input makes
      // every block's peak distinct, so identical records would mean the fields
      // were read once and re-serialised.
      expect(new Set(read.meters.map((meter) => meter.peakDbL)).size).toBe(publishes);

      // What the ring last received is what the mixer still holds, read here
      // through the object API now that the render cycles are over.
      calls.restore();
      const measured = (processor as unknown as { mixer: Mixer }).mixer.meterSnapshot();
      expect(read.meters[publishes - 1]).toMatchObject({
        peakDbL: measured.peakDbL,
        peakDbR: measured.peakDbR,
        rmsDbL: measured.rmsDbL,
        rmsDbR: measured.rmsDbR,
        correlation: measured.correlation,
        truePeakDbL: measured.truePeakDbL,
        truePeakDbR: measured.truePeakDbR,
      });
    } finally {
      calls.restore();
      processor.destroy();
    }
  });

  it('keeps meterSnapshot off the postMessage fallback path too', () => {
    const meters: { peakDbL: number }[] = [];
    const processor = new SonareWorkletProcessor(
      {
        sceneJson: mixingScenePresetJson('vocalReverbSend'),
        sampleRate,
        blockSize,
        meterIntervalFrames: blockSize,
      },
      { onMeter: (meter) => meters.push(meter as { peakDbL: number }) },
    );
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
      // built from the latched scalars, so it is the only object per publish.
      meters.forEach((meter, index) => {
        expect(meter.peakDbL).toBe(calls.valuesPerLatch[index][0]);
      });
    } finally {
      calls.restore();
      processor.destroy();
    }
  });
});
