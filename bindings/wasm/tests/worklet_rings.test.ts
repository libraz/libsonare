import {
  createSonareEngineCommandRingBuffer,
  createSonareEngineTelemetryRingBuffer,
  createSonareMeterRingBuffer,
  describe,
  expect,
  it,
  Mixer,
  mixingScenePresetJson,
  popSonareEngineCommandRingBuffer,
  pushSonareEngineCommandRingBuffer,
  readSonareEngineTelemetryRingBuffer,
  readSonareMeterRingBuffer,
  SONARE_ENGINE_COMMAND_RECORD_BYTES,
  SONARE_ENGINE_RING_HEADER_INTS,
  SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
  SONARE_METER_RING_HEADER_INTS,
  SONARE_METER_RING_RECORD_FLOATS,
  SonareEngineCommandType,
  SonareEngineTelemetryError,
  SonareEngineTelemetryType,
  SonareWorkletProcessor,
  setupWorklet,
  sonareEngineCommandRingBufferByteLength,
  sonareEngineTelemetryRingBufferByteLength,
  sonareMeterRingBufferByteLength,
  writeSonareEngineTelemetryRingBuffer,
} from './_worklet_helpers';

describe('Sonare worklet ring buffers', () => {
  setupWorklet();

  describe('meter ring-buffer round-trip (pure, no audio thread)', () => {
    const sampleRate = 48000;
    const blockSize = 128;

    function makeRingProcessor(ringSharedBuffer: SharedArrayBuffer): {
      processor: SonareWorkletProcessor;
      stripCount: number;
    } {
      const sceneJson = mixingScenePresetJson('vocalReverbSend');
      // A transport must be present for the processor to compute and publish
      // meters; with a meterSharedBuffer the records go to the ring (not postMessage).
      const processor = new SonareWorkletProcessor(
        {
          sceneJson,
          sampleRate,
          blockSize,
          meterIntervalFrames: blockSize,
          meterSharedBuffer: ringSharedBuffer,
        },
        {},
      );
      // One meter record is published per processed block.
      const reference = Mixer.fromSceneJson(sceneJson, sampleRate, blockSize);
      const stripCount = reference.stripCount();
      reference.delete();
      return { processor, stripCount };
    }

    function fabricateInputs(stripCount: number, impulseStrip = 0): Float32Array[][] {
      const inputs: Float32Array[][] = [];
      for (let strip = 0; strip < stripCount; strip++) {
        const left = new Float32Array(blockSize);
        const right = new Float32Array(blockSize);
        if (strip === impulseStrip) {
          left[0] = 1.0;
          right[0] = 1.0;
        }
        inputs.push([left, right]);
      }
      return inputs;
    }

    function driveBlocks(
      processor: SonareWorkletProcessor,
      stripCount: number,
      blocks: number,
    ): void {
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      for (let block = 0; block < blocks; block++) {
        const inputs = fabricateInputs(stripCount, block === 0 ? 0 : -1);
        expect(processor.process(inputs, [[outL, outR]])).toBe(true);
      }
    }

    it('publishes one finite record per block and advances the read cursor', () => {
      const ring = createSonareMeterRingBuffer(8);
      const { processor, stripCount } = makeRingProcessor(ring.sharedBuffer);
      try {
        const blocks = 5;
        driveBlocks(processor, stripCount, blocks);

        const read = readSonareMeterRingBuffer(ring, 0);
        expect(read.meters).toHaveLength(blocks);
        expect(read.nextReadIndex).toBe(blocks);

        let previousFrame = -1;
        for (const meter of read.meters) {
          expect(meter.type).toBe('meter');
          expect(meter.targetId).toBe(0);
          expect(Number.isFinite(meter.frame)).toBe(true);
          // frame is the cumulative processed-frame count; strictly increasing.
          expect(meter.frame).toBeGreaterThan(previousFrame);
          previousFrame = meter.frame;
          // dB fields are finite or -Infinity for digital silence (the documented
          // toDb() convention); correlation is always finite in [-1, 1].
          for (const field of ['peakDbL', 'peakDbR', 'rmsDbL', 'rmsDbR'] as const) {
            expect(meter[field] === Number.NEGATIVE_INFINITY || Number.isFinite(meter[field])).toBe(
              true,
            );
          }
          expect(Number.isFinite(meter.correlation)).toBe(true);
          expect(Number.isNaN(meter.momentaryLufs)).toBe(true);
        }
        // First record corresponds to the first published block of frames.
        expect(read.meters[0].frame).toBe(blockSize);
        expect(read.meters[blocks - 1].frame).toBe(blockSize * blocks);
      } finally {
        processor.destroy();
      }
    });

    it('returns only new records on incremental reads', () => {
      const ring = createSonareMeterRingBuffer(8);
      const { processor, stripCount } = makeRingProcessor(ring.sharedBuffer);
      try {
        driveBlocks(processor, stripCount, 2);
        const first = readSonareMeterRingBuffer(ring, 0);
        expect(first.meters).toHaveLength(2);
        expect(first.nextReadIndex).toBe(2);

        // Re-reading from the returned cursor yields nothing new.
        const empty = readSonareMeterRingBuffer(ring, first.nextReadIndex);
        expect(empty.meters).toHaveLength(0);
        expect(empty.nextReadIndex).toBe(2);

        // After more blocks, only the new records are returned.
        driveBlocks(processor, stripCount, 3);
        const incremental = readSonareMeterRingBuffer(ring, first.nextReadIndex);
        expect(incremental.meters).toHaveLength(3);
        expect(incremental.nextReadIndex).toBe(5);
        expect(incremental.meters[0].frame).toBe(blockSize * 3);
      } finally {
        processor.destroy();
      }
    });

    it('wraps around and returns at most capacity most-recent records', () => {
      const capacity = 4;
      const ring = createSonareMeterRingBuffer(capacity);
      const { processor, stripCount } = makeRingProcessor(ring.sharedBuffer);
      try {
        const blocks = 10; // more than capacity
        driveBlocks(processor, stripCount, blocks);

        const read = readSonareMeterRingBuffer(ring, 0);
        // The reader never returns more than capacity records.
        expect(read.meters.length).toBeLessThanOrEqual(capacity);
        expect(read.meters).toHaveLength(capacity);
        // nextReadIndex tracks the total number of published records.
        expect(read.nextReadIndex).toBe(blocks);

        // Returned records are the most-recent, coherent and finite.
        let previousFrame = -1;
        for (const meter of read.meters) {
          expect(Number.isFinite(meter.frame)).toBe(true);
          expect(meter.frame).toBeGreaterThan(previousFrame);
          previousFrame = meter.frame;
          // -Infinity is the documented dB value for digital silence.
          expect(meter.peakDbL === Number.NEGATIVE_INFINITY || Number.isFinite(meter.peakDbL)).toBe(
            true,
          );
          expect(Number.isFinite(meter.correlation)).toBe(true);
        }
        // Oldest readable record is (writeIndex - capacity).
        expect(read.meters[0].frame).toBe(blockSize * (blocks - capacity + 1));
        expect(read.meters[capacity - 1].frame).toBe(blockSize * blocks);
      } finally {
        processor.destroy();
      }
    });

    it('round-trips a manually populated SharedArrayBuffer using the documented layout', () => {
      const capacity = 4;
      const sab = new SharedArrayBuffer(sonareMeterRingBufferByteLength(capacity));
      const header = new Int32Array(sab, 0, SONARE_METER_RING_HEADER_INTS);
      const records = new Float32Array(
        sab,
        SONARE_METER_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT,
        capacity * SONARE_METER_RING_RECORD_FLOATS,
      );
      // Header layout: [writeIndex, capacity, recordFloats, overflow].
      header[1] = capacity;
      header[2] = SONARE_METER_RING_RECORD_FLOATS;

      const writeRecord = (
        frame: number,
        targetId: number,
        peakDbL: number,
        peakDbR: number,
        rmsDbL: number,
        rmsDbR: number,
        correlation: number,
      ): void => {
        const writeIndex = header[0];
        const offset = (writeIndex % capacity) * SONARE_METER_RING_RECORD_FLOATS;
        records[offset] = frame;
        records[offset + 1] = 0;
        records[offset + 2] = targetId;
        records[offset + 3] = peakDbL;
        records[offset + 4] = peakDbR;
        records[offset + 5] = rmsDbL;
        records[offset + 6] = rmsDbR;
        records[offset + 7] = correlation;
        records[offset + 8] = peakDbL + 0.5;
        records[offset + 9] = peakDbR + 0.5;
        records[offset + 10] = -23;
        records[offset + 11] = -24;
        records[offset + 12] = -25;
        records[offset + 13] = -2;
        header[0] = writeIndex + 1;
      };

      const ring = { sharedBuffer: sab, header, records, capacity };

      writeRecord(128, 0, -1, -2, -3, -4, 0.5);
      writeRecord(256, 1, -5, -6, -7, -8, 0.25);

      const first = readSonareMeterRingBuffer(ring, 0);
      expect(first.meters).toHaveLength(2);
      expect(first.nextReadIndex).toBe(2);
      expect(first.meters[0]).toMatchObject({
        type: 'meter',
        targetId: 0,
        frame: 128,
        peakDbL: -1,
        peakDbR: -2,
        rmsDbL: -3,
        rmsDbR: -4,
        correlation: 0.5,
        truePeakDbL: -0.5,
        truePeakDbR: -1.5,
        momentaryLufs: -23,
        shortTermLufs: -24,
        integratedLufs: -25,
        gainReductionDb: -2,
      });

      // Overflow the ring; only the most-recent `capacity` survive.
      writeRecord(384, 0, -9, -10, -11, -12, 0);
      writeRecord(512, 1, -13, -14, -15, -16, 0);
      writeRecord(640, 33, -17, -18, -19, -20, 0);
      writeRecord(768, 0xffff, -21, -22, -23, -24, 0);

      const wrapped = readSonareMeterRingBuffer(ring, first.nextReadIndex);
      expect(wrapped.meters.length).toBeLessThanOrEqual(capacity);
      expect(wrapped.nextReadIndex).toBe(6);
      // Frames 128/256 were overwritten; 384..768 remain (most-recent capacity).
      expect(wrapped.meters.map((m) => m.frame)).toEqual([384, 512, 640, 768]);
      expect(wrapped.meters.map((m) => m.targetId)).toEqual([0, 1, 33, 0xffff]);
      for (const meter of wrapped.meters) {
        expect(Number.isFinite(meter.peakDbL)).toBe(true);
        expect(Number.isFinite(meter.correlation)).toBe(true);
      }
    });
  });

  describe('engine SAB command and telemetry rings', () => {
    it('round-trips fixed-layout command records and detects overflow', () => {
      const ring = createSonareEngineCommandRingBuffer(2);
      expect(ring.header[2]).toBe(2);
      expect(ring.header[3]).toBe(SONARE_ENGINE_COMMAND_RECORD_BYTES);
      expect(ring.sharedBuffer.byteLength).toBe(sonareEngineCommandRingBufferByteLength(2));

      expect(
        pushSonareEngineCommandRingBuffer(ring, {
          type: SonareEngineCommandType.TransportPlay,
          sampleTime: 128,
        }),
      ).toBe(true);
      expect(
        pushSonareEngineCommandRingBuffer(ring, {
          type: SonareEngineCommandType.TransportSeekSample,
          targetId: 7,
          sampleTime: 256n,
          argInt: 48000n,
        }),
      ).toBe(true);
      expect(
        pushSonareEngineCommandRingBuffer(ring, {
          type: SonareEngineCommandType.TransportStop,
          sampleTime: 384,
        }),
      ).toBe(false);
      expect(ring.header[4]).toBe(1);

      expect(popSonareEngineCommandRingBuffer(ring)).toMatchObject({
        type: SonareEngineCommandType.TransportPlay,
        targetId: 0,
        sampleTime: 128,
      });
      expect(popSonareEngineCommandRingBuffer(ring)).toMatchObject({
        type: SonareEngineCommandType.TransportSeekSample,
        targetId: 7,
        sampleTime: 256,
        argInt: 48000,
      });
      expect(popSonareEngineCommandRingBuffer(ring)).toBeNull();
    });

    it('round-trips fixed-layout telemetry records with wraparound', () => {
      const ring = createSonareEngineTelemetryRingBuffer(2);
      expect(ring.header[2]).toBe(2);
      expect(ring.header[3]).toBe(SONARE_ENGINE_TELEMETRY_RECORD_BYTES);
      expect(ring.sharedBuffer.byteLength).toBe(sonareEngineTelemetryRingBufferByteLength(2));

      writeSonareEngineTelemetryRingBuffer(ring, {
        type: SonareEngineTelemetryType.ProcessBlock,
        error: SonareEngineTelemetryError.None,
        renderFrame: 0,
        timelineSample: 128,
        audibleTimelineSample: 128,
        graphLatencySamplesQ8: 0,
        value: 128,
      });
      const first = readSonareEngineTelemetryRingBuffer(ring);
      expect(first.nextReadIndex).toBe(1);
      expect(first.telemetry).toHaveLength(1);
      expect(first.telemetry[0]).toMatchObject({
        type: SonareEngineTelemetryType.ProcessBlock,
        error: SonareEngineTelemetryError.None,
        timelineSample: 128,
      });

      writeSonareEngineTelemetryRingBuffer(ring, {
        type: SonareEngineTelemetryType.Error,
        error: SonareEngineTelemetryError.MaxBlockExceeded,
        renderFrame: 128,
        timelineSample: 256,
        audibleTimelineSample: 256,
        graphLatencySamplesQ8: 0,
        value: 512,
      });
      writeSonareEngineTelemetryRingBuffer(ring, {
        type: SonareEngineTelemetryType.ProcessBlock,
        error: SonareEngineTelemetryError.None,
        renderFrame: 256,
        timelineSample: 384,
        audibleTimelineSample: 384,
        graphLatencySamplesQ8: 0,
        value: 128,
      });

      const wrapped = readSonareEngineTelemetryRingBuffer(ring, first.nextReadIndex);
      expect(wrapped.nextReadIndex).toBe(3);
      expect(wrapped.telemetry).toHaveLength(2);
      expect(wrapped.telemetry.map((item) => item.timelineSample)).toEqual([256, 384]);
      expect(ring.header[4]).toBe(1);
    });

    it('documents the shared ring header layout', () => {
      const capacity = 4;
      const sab = new SharedArrayBuffer(sonareEngineCommandRingBufferByteLength(capacity));
      const header = new Int32Array(sab, 0, SONARE_ENGINE_RING_HEADER_INTS);
      header[0] = 3; // writeIndex
      header[1] = 1; // readIndex
      header[2] = capacity;
      header[3] = SONARE_ENGINE_COMMAND_RECORD_BYTES;
      header[4] = 2; // overflow/drop count

      expect(Array.from(header)).toEqual([3, 1, capacity, SONARE_ENGINE_COMMAND_RECORD_BYTES, 2]);
    });
  });
});
