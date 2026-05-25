import { beforeAll, describe, expect, it } from 'vitest';
import { init, Mixer, mixingScenePresetJson } from '../dist/index.js';
import {
  createSonareMeterRingBuffer,
  createSonareSpectrumRingBuffer,
  readSonareMeterRingBuffer,
  readSonareSpectrumRingBuffer,
  registerSonareWorkletProcessor,
  SONARE_METER_RING_HEADER_INTS,
  SONARE_METER_RING_RECORD_FLOATS,
  SonareWorkletProcessor,
  sonareMeterRingBufferByteLength,
} from '../dist/worklet.js';

describe('SonareWorkletProcessor', () => {
  beforeAll(async () => {
    await init();
  });

  it('matches the offline mixer for one 128-sample render quantum', () => {
    const sampleRate = 48000;
    const blockSize = 128;
    const sceneJson = mixingScenePresetJson('vocalReverbSend');

    const offline = Mixer.fromSceneJson(sceneJson, sampleRate, blockSize);
    const processor = new SonareWorkletProcessor({ sceneJson, sampleRate, blockSize });
    try {
      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      const returnL = new Float32Array(blockSize);
      const returnR = new Float32Array(blockSize);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;

      const expected = offline.processStereo([vocalL, returnL], [vocalR, returnR]);
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      const alive = processor.process(
        [
          [vocalL, vocalR],
          [returnL, returnR],
        ],
        [[outL, outR]],
      );

      expect(alive).toBe(true);
      expect(Array.from(outL)).toEqual(Array.from(expected.left));
      expect(Array.from(outR)).toEqual(Array.from(expected.right));
    } finally {
      processor.destroy();
      offline.delete();
    }
  });

  it('keeps processing silence when an input strip is missing', () => {
    const sampleRate = 48000;
    const blockSize = 128;
    const processor = new SonareWorkletProcessor({
      sceneJson: mixingScenePresetJson('vocalReverbSend'),
      sampleRate,
      blockSize,
    });
    try {
      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      expect(processor.process([[vocalL, vocalR]], [[outL, outR]])).toBe(true);
      expect(outL.length).toBe(blockSize);
      expect(outR.length).toBe(blockSize);
    } finally {
      processor.destroy();
    }
  });

  it('accepts postMessage-style automation and publishes meters', () => {
    const sampleRate = 48000;
    const blockSize = 128;
    const meters: unknown[] = [];
    const processor = new SonareWorkletProcessor(
      {
        sceneJson: mixingScenePresetJson('vocalReverbSend'),
        sampleRate,
        blockSize,
        meterIntervalFrames: blockSize,
      },
      { onMeter: (meter) => meters.push(meter) },
    );
    try {
      processor.receiveMessage({
        type: 'scheduleInsertAutomation',
        stripIndex: 0,
        insertIndex: 0,
        paramId: 0,
        samplePos: 0,
        value: 0,
        curve: 'linear',
      });

      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      const returnL = new Float32Array(blockSize);
      const returnR = new Float32Array(blockSize);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);

      expect(
        processor.process(
          [
            [vocalL, vocalR],
            [returnL, returnR],
          ],
          [[outL, outR]],
        ),
      ).toBe(true);
      expect(meters).toHaveLength(1);
      expect(meters[0]).toMatchObject({ type: 'meter', frame: blockSize });
    } finally {
      processor.destroy();
    }
  });

  it('can publish meters through a SharedArrayBuffer ring', () => {
    const sampleRate = 48000;
    const blockSize = 128;
    const ring = createSonareMeterRingBuffer(4);
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
    try {
      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      const returnL = new Float32Array(blockSize);
      const returnR = new Float32Array(blockSize);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);

      expect(
        processor.process(
          [
            [vocalL, vocalR],
            [returnL, returnR],
          ],
          [[outL, outR]],
        ),
      ).toBe(true);

      expect(posted).toHaveLength(0);
      const read = readSonareMeterRingBuffer(ring);
      expect(read.nextReadIndex).toBe(1);
      expect(read.meters).toHaveLength(1);
      expect(read.meters[0]).toMatchObject({ type: 'meter', frame: blockSize });
    } finally {
      processor.destroy();
    }
  });

  it('can publish spectrum snapshots through postMessage and SharedArrayBuffer', () => {
    const sampleRate = 48000;
    const blockSize = 128;
    const spectra: unknown[] = [];
    const processor = new SonareWorkletProcessor(
      {
        sceneJson: mixingScenePresetJson('vocalReverbSend'),
        sampleRate,
        blockSize,
        spectrumIntervalFrames: blockSize,
        spectrumBands: 8,
      },
      { onSpectrum: (spectrum) => spectra.push(spectrum) },
    );
    try {
      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      expect(processor.process([[vocalL, vocalR]], [[outL, outR]])).toBe(true);
      expect(spectra).toHaveLength(1);
      expect(spectra[0]).toMatchObject({ type: 'spectrum', frame: blockSize });
      expect((spectra[0] as { bands: Float32Array }).bands).toHaveLength(8);
    } finally {
      processor.destroy();
    }

    const ring = createSonareSpectrumRingBuffer(4, 8);
    const ringProcessor = new SonareWorkletProcessor({
      sceneJson: mixingScenePresetJson('vocalReverbSend'),
      sampleRate,
      blockSize,
      spectrumIntervalFrames: blockSize,
      spectrumSharedBuffer: ring.sharedBuffer,
    });
    try {
      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      expect(ringProcessor.process([[vocalL, vocalR]], [[new Float32Array(blockSize)]])).toBe(true);
      const read = readSonareSpectrumRingBuffer(ring);
      expect(read.nextReadIndex).toBe(1);
      expect(read.spectra).toHaveLength(1);
      expect(read.spectra[0].bands).toHaveLength(8);
    } finally {
      ringProcessor.destroy();
    }
  });

  it('routes messages and meters through a registered processor port', () => {
    const previousProcessor = (
      globalThis as typeof globalThis & { AudioWorkletProcessor?: unknown }
    ).AudioWorkletProcessor;
    const previousRegister = (globalThis as typeof globalThis & { registerProcessor?: unknown })
      .registerProcessor;
    let registeredCtor: unknown;
    try {
      Object.assign(globalThis, {
        AudioWorkletProcessor: class {
          port = {
            posted: [] as unknown[],
            postMessage: (message: unknown) => {
              this.port.posted.push(message);
            },
          };
        },
        registerProcessor: (_name: string, ctor: unknown) => {
          registeredCtor = ctor;
        },
      });
      registerSonareWorkletProcessor('sonare-test-processor');
      const Ctor = registeredCtor as new (options: {
        processorOptions: {
          sceneJson: string;
          sampleRate: number;
          blockSize: number;
          meterIntervalFrames: number;
        };
      }) => {
        port: { posted: unknown[]; onmessage?: (event: { data: unknown }) => void };
        process: (inputs: Float32Array[][], outputs: Float32Array[][][]) => boolean;
      };
      const blockSize = 128;
      const instance = new Ctor({
        processorOptions: {
          sceneJson: mixingScenePresetJson('vocalReverbSend'),
          sampleRate: 48000,
          blockSize,
          meterIntervalFrames: blockSize,
        },
      });
      instance.port.onmessage?.({
        data: {
          type: 'scheduleInsertAutomation',
          stripIndex: 0,
          insertIndex: 0,
          paramId: 0,
          value: 0,
        },
      });

      const vocalL = new Float32Array(blockSize);
      const vocalR = new Float32Array(blockSize);
      vocalL[0] = 1;
      vocalR[0] = 1;
      const outL = new Float32Array(blockSize);
      const outR = new Float32Array(blockSize);
      expect(instance.process([[vocalL, vocalR]], [[outL, outR]])).toBe(true);
      expect(instance.port.posted).toHaveLength(1);
      expect(instance.port.posted[0]).toMatchObject({ type: 'meter', frame: blockSize });
      instance.port.onmessage?.({ data: { type: 'destroy' } });
    } finally {
      Object.assign(globalThis, {
        AudioWorkletProcessor: previousProcessor,
        registerProcessor: previousRegister,
      });
    }
  });

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
        peakDbL: number,
        peakDbR: number,
        rmsDbL: number,
        rmsDbR: number,
        correlation: number,
      ): void => {
        const writeIndex = header[0];
        const offset = (writeIndex % capacity) * SONARE_METER_RING_RECORD_FLOATS;
        records[offset] = frame;
        records[offset + 1] = peakDbL;
        records[offset + 2] = peakDbR;
        records[offset + 3] = rmsDbL;
        records[offset + 4] = rmsDbR;
        records[offset + 5] = correlation;
        header[0] = writeIndex + 1;
      };

      const ring = { sharedBuffer: sab, header, records, capacity };

      writeRecord(128, -1, -2, -3, -4, 0.5);
      writeRecord(256, -5, -6, -7, -8, 0.25);

      const first = readSonareMeterRingBuffer(ring, 0);
      expect(first.meters).toHaveLength(2);
      expect(first.nextReadIndex).toBe(2);
      expect(first.meters[0]).toMatchObject({
        type: 'meter',
        frame: 128,
        peakDbL: -1,
        peakDbR: -2,
        rmsDbL: -3,
        rmsDbR: -4,
        correlation: 0.5,
      });

      // Overflow the ring; only the most-recent `capacity` survive.
      writeRecord(384, -9, -10, -11, -12, 0);
      writeRecord(512, -13, -14, -15, -16, 0);
      writeRecord(640, -17, -18, -19, -20, 0);
      writeRecord(768, -21, -22, -23, -24, 0);

      const wrapped = readSonareMeterRingBuffer(ring, first.nextReadIndex);
      expect(wrapped.meters.length).toBeLessThanOrEqual(capacity);
      expect(wrapped.nextReadIndex).toBe(6);
      // Frames 128/256 were overwritten; 384..768 remain (most-recent capacity).
      expect(wrapped.meters.map((m) => m.frame)).toEqual([384, 512, 640, 768]);
      for (const meter of wrapped.meters) {
        expect(Number.isFinite(meter.peakDbL)).toBe(true);
        expect(Number.isFinite(meter.correlation)).toBe(true);
      }
    });
  });

  it('registers in an AudioWorklet-like global scope', () => {
    const previousProcessor = (
      globalThis as typeof globalThis & { AudioWorkletProcessor?: unknown }
    ).AudioWorkletProcessor;
    const previousRegister = (globalThis as typeof globalThis & { registerProcessor?: unknown })
      .registerProcessor;
    let registeredName = '';
    let registeredCtor: unknown;
    try {
      Object.assign(globalThis, {
        AudioWorkletProcessor: class {},
        registerProcessor: (name: string, ctor: unknown) => {
          registeredName = name;
          registeredCtor = ctor;
        },
      });
      registerSonareWorkletProcessor();
      expect(registeredName).toBe('sonare-worklet-processor');
      expect(typeof registeredCtor).toBe('function');
    } finally {
      Object.assign(globalThis, {
        AudioWorkletProcessor: previousProcessor,
        registerProcessor: previousRegister,
      });
    }
  });
});
