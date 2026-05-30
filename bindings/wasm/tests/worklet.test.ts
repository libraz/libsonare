import { beforeAll, describe, expect, it } from 'vitest';
import { init, Mixer, mixingScenePresetJson } from '../dist/index.js';
import {
  createSonareEngineCommandRingBuffer,
  createSonareEngineTelemetryRingBuffer,
  createSonareMeterRingBuffer,
  createSonareSpectrumRingBuffer,
  init as initWorklet,
  popSonareEngineCommandRingBuffer,
  pushSonareEngineCommandRingBuffer,
  readSonareEngineTelemetryRingBuffer,
  readSonareMeterRingBuffer,
  readSonareSpectrumRingBuffer,
  registerSonareRealtimeEngineWorkletProcessor,
  registerSonareRealtimeVoiceChangerWorkletProcessor,
  registerSonareWorkletProcessor,
  SONARE_ENGINE_COMMAND_RECORD_BYTES,
  SONARE_ENGINE_RING_HEADER_INTS,
  SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
  SONARE_METER_RING_HEADER_INTS,
  SONARE_METER_RING_RECORD_FLOATS,
  SonareEngine,
  SonareEngineCommandType,
  SonareEngineTelemetryError,
  SonareEngineTelemetryType,
  SonareRealtimeEngineNode,
  SonareRealtimeEngineWorkletProcessor,
  SonareRealtimeVoiceChangerWorkletProcessor,
  SonareRtRealtimeEngineRuntime,
  SonareWorkletProcessor,
  sonareEngineCommandRingBufferByteLength,
  sonareEngineTelemetryRingBufferByteLength,
  sonareMeterRingBufferByteLength,
  writeSonareEngineTelemetryRingBuffer,
} from '../dist/worklet.js';

describe('SonareWorkletProcessor', () => {
  beforeAll(async () => {
    // index.js and worklet.js are separate self-contained bundles (code-splitting
    // is disabled), so each owns its module singleton and must be initialized.
    await init();
    await initWorklet();
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

  describe('SonareRealtimeVoiceChangerWorkletProcessor', () => {
    it('processes mono render quanta through the unified realtime voice changer', () => {
      const blockSize = 128;
      const processor = new SonareRealtimeVoiceChangerWorkletProcessor({
        preset: 'bright-idol',
        sampleRate: 48000,
        blockSize,
        channelCount: 1,
      });
      try {
        const input = new Float32Array(blockSize);
        for (let i = 0; i < input.length; i++) {
          input[i] = Math.sin((2 * Math.PI * 220 * i) / 48000) * 0.2;
        }
        let output = new Float32Array(blockSize);
        for (let block = 0; block < 32; block++) {
          output = new Float32Array(blockSize);
          expect(processor.process([[input]], [[output]])).toBe(true);
          expect(output.every((sample) => Number.isFinite(sample))).toBe(true);
        }
        expect(output.some((sample) => sample !== 0)).toBe(true);

        processor.receiveMessage({ type: 'setConfig', preset: 'dark-villain' });
        const nextOutput = new Float32Array(blockSize);
        expect(processor.process([[input]], [[nextOutput]])).toBe(true);
        expect(nextOutput.every((sample) => Number.isFinite(sample))).toBe(true);
      } finally {
        processor.destroy();
      }
    });

    it('processes stereo render quanta without allocating returned buffers', () => {
      const blockSize = 128;
      const processor = new SonareRealtimeVoiceChangerWorkletProcessor({
        preset: 'deep-narrator',
        sampleRate: 48000,
        blockSize,
        channelCount: 2,
      });
      try {
        const left = new Float32Array(blockSize);
        const right = new Float32Array(blockSize);
        for (let i = 0; i < blockSize; i++) {
          left[i] = Math.sin((2 * Math.PI * 180 * i) / 48000) * 0.15;
          right[i] = Math.sin((2 * Math.PI * 220 * i) / 48000) * 0.15;
        }
        const outLeft = new Float32Array(blockSize);
        const outRight = new Float32Array(blockSize);
        expect(processor.process([[left, right]], [[outLeft, outRight]])).toBe(true);
        expect(outLeft.every((sample) => Number.isFinite(sample))).toBe(true);
        expect(outRight.every((sample) => Number.isFinite(sample))).toBe(true);
      } finally {
        processor.destroy();
      }
    });

    it('registers an AudioWorklet processor wrapper', () => {
      const previousProcessor = (
        globalThis as typeof globalThis & { AudioWorkletProcessor?: unknown }
      ).AudioWorkletProcessor;
      const previousRegister = (globalThis as typeof globalThis & { registerProcessor?: unknown })
        .registerProcessor;
      let registeredName = '';
      let registeredCtor: unknown;
      try {
        Object.assign(globalThis, {
          AudioWorkletProcessor: class {
            port = {};
          },
          registerProcessor: (name: string, ctor: unknown) => {
            registeredName = name;
            registeredCtor = ctor;
          },
        });
        registerSonareRealtimeVoiceChangerWorkletProcessor();
        expect(registeredName).toBe('sonare-realtime-voice-changer-processor');
        expect(typeof registeredCtor).toBe('function');
      } finally {
        Object.assign(globalThis, {
          AudioWorkletProcessor: previousProcessor,
          registerProcessor: previousRegister,
        });
      }
    });

    it('channel mismatch is handled gracefully (no throw, output is zeroed or projected)', () => {
      // Configure for 1 channel; send a 2-channel input layout.
      // The worklet clips channels to min(channelCount, output.length) and should
      // not throw regardless of input shape.
      const blockSize = 128;
      const processor = new SonareRealtimeVoiceChangerWorkletProcessor({
        preset: 'neutral-monitor',
        sampleRate: 48000,
        blockSize,
        channelCount: 1,
      });
      try {
        const ch1 = new Float32Array(blockSize).fill(0.1);
        const ch2 = new Float32Array(blockSize).fill(-0.1);
        const out1 = new Float32Array(blockSize);
        const out2 = new Float32Array(blockSize);
        // inputs has 2 channels but the processor was prepared for 1 — the
        // worklet takes Math.min(channelCount, output.length) so it just uses ch1.
        expect(() => processor.process([[ch1, ch2]], [[out1, out2]])).not.toThrow();
        expect(out1.every((s) => Number.isFinite(s))).toBe(true);
      } finally {
        processor.destroy();
      }
    });

    it('destroy then process returns false and does not throw', () => {
      const blockSize = 128;
      const processor = new SonareRealtimeVoiceChangerWorkletProcessor({
        preset: 'neutral-monitor',
        sampleRate: 48000,
        blockSize,
        channelCount: 1,
      });
      const input = new Float32Array(blockSize);
      const output = new Float32Array(blockSize);
      // First cycle works fine.
      expect(processor.process([[input]], [[output]])).toBe(true);
      // Destroy the processor.
      processor.destroy();
      // Post-destroy process() must not throw and must return false.
      const output2 = new Float32Array(blockSize);
      expect(() => processor.process([[input]], [[output2]])).not.toThrow();
      expect(processor.process([[input]], [[output2]])).toBe(false);
    });

    it('does not allocate Float32Array storage on the audio thread (RT-safety)', () => {
      // AudioWorklet's process() runs on the realtime audio thread. Any
      // `new Float32Array(n)` (number form) allocates a fresh ArrayBuffer via
      // the V8 heap allocator and can trigger GC, causing audible glitches.
      // View-form allocations (`new Float32Array(buffer, offset, length)` or
      // subarray()) only allocate a small wrapper object and are acceptable.
      // This test counts only the dangerous size-based allocations.
      const original = globalThis.Float32Array;
      let storageAllocationCount = 0;
      const proxy = new Proxy(original, {
        construct(target, args, newTarget) {
          // Size-based allocation: `new Float32Array(N)` where the first arg
          // is a number. View-based allocation: first arg is an ArrayBuffer
          // (or SharedArrayBuffer) or a typed array — those just create a
          // wrapper over existing storage.
          if (typeof args[0] === 'number') {
            storageAllocationCount++;
          }
          return Reflect.construct(target, args, newTarget);
        },
      });
      const blockSize = 128;
      // Construct with the unmocked Float32Array so the constructor can
      // legitimately allocate its scratch buffers.
      const processor = new SonareRealtimeVoiceChangerWorkletProcessor({
        preset: 'neutral-monitor',
        sampleRate: 48000,
        blockSize,
        channelCount: 2,
      });
      try {
        // Swap in the proxy only for the audio-thread loop.
        (globalThis as { Float32Array: typeof Float32Array }).Float32Array =
          proxy as unknown as typeof Float32Array;
        storageAllocationCount = 0;
        // Pre-allocate I/O buffers BEFORE the audio loop (host-side cost).
        const leftIn = new original(blockSize);
        const rightIn = new original(blockSize);
        const leftOut = new original(blockSize);
        const rightOut = new original(blockSize);
        for (let i = 0; i < blockSize; i++) {
          leftIn[i] = Math.sin((2 * Math.PI * 220 * i) / 48000) * 0.1;
          rightIn[i] = Math.sin((2 * Math.PI * 220 * i) / 48000) * 0.1;
        }
        const allocationsBefore = storageAllocationCount;
        for (let i = 0; i < 5; i++) {
          processor.process([[leftIn, rightIn]], [[leftOut, rightOut]]);
        }
        const allocationsDuringProcess = storageAllocationCount - allocationsBefore;
        expect(allocationsDuringProcess).toBe(0);
      } finally {
        (globalThis as { Float32Array: typeof Float32Array }).Float32Array = original;
        processor.destroy();
      }
    });

    it('does not allocate Float32Array storage in mono mode on the audio thread (RT-safety)', () => {
      const original = globalThis.Float32Array;
      let storageAllocationCount = 0;
      const proxy = new Proxy(original, {
        construct(target, args, newTarget) {
          if (typeof args[0] === 'number') {
            storageAllocationCount++;
          }
          return Reflect.construct(target, args, newTarget);
        },
      });
      const blockSize = 128;
      const processor = new SonareRealtimeVoiceChangerWorkletProcessor({
        preset: 'neutral-monitor',
        sampleRate: 48000,
        blockSize,
        channelCount: 1,
      });
      try {
        (globalThis as { Float32Array: typeof Float32Array }).Float32Array =
          proxy as unknown as typeof Float32Array;
        storageAllocationCount = 0;
        const input = new original(blockSize);
        const output = new original(blockSize);
        for (let i = 0; i < blockSize; i++) {
          input[i] = Math.sin((2 * Math.PI * 220 * i) / 48000) * 0.1;
        }
        const allocationsBefore = storageAllocationCount;
        for (let i = 0; i < 5; i++) {
          processor.process([[input]], [[output]]);
        }
        const allocationsDuringProcess = storageAllocationCount - allocationsBefore;
        expect(allocationsDuringProcess).toBe(0);
      } finally {
        (globalThis as { Float32Array: typeof Float32Array }).Float32Array = original;
        processor.destroy();
      }
    });

    it('applies setConfig synchronously on receiveMessage and never from process() (RT-safety)', () => {
      // setConfig may parse JSON and recompute DSP coefficients; that work
      // must happen on the message-handler side, never on the realtime audio
      // thread. We spy on the underlying RealtimeVoiceChanger.setConfig by
      // proxying the per-instance method on the worklet's private `changer`
      // handle, then assert:
      //   1. receiveMessage({ type: 'setConfig', ... }) invokes setConfig
      //      synchronously (before returning).
      //   2. Subsequent process() calls do NOT invoke setConfig at all.
      const blockSize = 128;
      const processor = new SonareRealtimeVoiceChangerWorkletProcessor({
        preset: 'neutral-monitor',
        sampleRate: 48000,
        blockSize,
        channelCount: 1,
      });
      try {
        const internal = processor as unknown as {
          changer: { setConfig: (preset: unknown) => void };
        };
        const originalSetConfig = internal.changer.setConfig.bind(internal.changer);
        let setConfigCalls = 0;
        internal.changer.setConfig = (preset: unknown) => {
          setConfigCalls++;
          originalSetConfig(preset);
        };

        // (1) Synchronous application on message receipt.
        const beforeRecv = setConfigCalls;
        processor.receiveMessage({ type: 'setConfig', preset: 'dark-villain' });
        expect(setConfigCalls).toBe(beforeRecv + 1);

        // (2) process() does not invoke setConfig.
        const input = new Float32Array(blockSize);
        const output = new Float32Array(blockSize);
        for (let i = 0; i < blockSize; i++) {
          input[i] = Math.sin((2 * Math.PI * 220 * i) / 48000) * 0.1;
        }
        const beforeProcess = setConfigCalls;
        for (let block = 0; block < 8; block++) {
          processor.process([[input]], [[output]]);
        }
        expect(setConfigCalls).toBe(beforeProcess);
      } finally {
        processor.destroy();
      }
    });

    it('ignores control-plane messages after destroy', () => {
      const blockSize = 128;
      const processor = new SonareRealtimeVoiceChangerWorkletProcessor({
        preset: 'neutral-monitor',
        sampleRate: 48000,
        blockSize,
        channelCount: 1,
      });
      const internal = processor as unknown as {
        changer: { setConfig: (preset: unknown) => void };
      };
      let setConfigCalls = 0;
      const originalSetConfig = internal.changer.setConfig.bind(internal.changer);
      internal.changer.setConfig = (preset: unknown) => {
        setConfigCalls++;
        originalSetConfig(preset);
      };
      processor.destroy();
      expect(() =>
        processor.receiveMessage({ type: 'setConfig', preset: 'bright-idol' }),
      ).not.toThrow();
      expect(() => processor.receiveMessage({ type: 'reset' })).not.toThrow();
      expect(setConfigCalls).toBe(0);
    });

    it.skip('invalid preset at construction throws or falls back gracefully (skipped: WASM embind preset validation path not yet exposed at worklet ctor level)', () => {
      // The SonareRealtimeVoiceChangerWorkletProcessor constructor calls
      //   new RealtimeVoiceChanger(options.preset ?? 'neutral-monitor')
      // followed by changer.prepare(). Whether an unknown preset string throws
      // depends on whether the embind RealtimeVoiceChanger constructor validates
      // the preset ID or silently falls back to neutral-monitor. The current WASM
      // binding passes the preset string directly to the C++ constructor which
      // may silently default rather than throw, making this assertion
      // implementation-dependent. Skip until the WASM binding documents the
      // exact contract for unknown preset IDs.
    });
  });

  describe('SonareRealtimeEngineWorkletProcessor', () => {
    it('applies SAB transport commands within the next processed block', () => {
      const blockSize = 128;
      const commandRing = createSonareEngineCommandRingBuffer(8);
      const telemetryRing = createSonareEngineTelemetryRingBuffer(8);
      const processor = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        blockSize,
        channelCount: 2,
        commandSharedBuffer: commandRing.sharedBuffer,
        telemetrySharedBuffer: telemetryRing.sharedBuffer,
      });
      try {
        expect(
          pushSonareEngineCommandRingBuffer(commandRing, {
            type: SonareEngineCommandType.TransportPlay,
            sampleTime: -1,
          }),
        ).toBe(true);
        const outL = new Float32Array(blockSize);
        const outR = new Float32Array(blockSize);
        expect(processor.process([[]], [[outL, outR]])).toBe(true);

        const first = readSonareEngineTelemetryRingBuffer(telemetryRing);
        expect(first.telemetry.length).toBeGreaterThan(0);
        expect(first.telemetry.at(-1)).toMatchObject({
          type: SonareEngineTelemetryType.ProcessBlock,
          error: SonareEngineTelemetryError.None,
          timelineSample: blockSize,
        });
        expect(commandRing.header[1]).toBe(1);

        expect(
          pushSonareEngineCommandRingBuffer(commandRing, {
            type: SonareEngineCommandType.TransportSeekSample,
            sampleTime: -1,
            argInt: 48000,
          }),
        ).toBe(true);
        expect(processor.process([[]], [[outL, outR]])).toBe(true);
        const second = readSonareEngineTelemetryRingBuffer(telemetryRing, first.nextReadIndex);
        expect(second.telemetry.at(-1)?.timelineSample).toBe(48000 + blockSize);
      } finally {
        processor.destroy();
      }
    });

    it('publishes telemetry through postMessage fallback when SAB telemetry is absent', () => {
      const posted: unknown[] = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize: 128, channelCount: 1 },
        { postMessage: (message) => posted.push(message) },
      );
      try {
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });
        expect(processor.process([[]], [[new Float32Array(128)]])).toBe(true);
        expect(posted.length).toBeGreaterThan(0);
        expect(
          posted.find(
            (item) =>
              typeof item === 'object' &&
              item !== null &&
              (item as { type?: unknown }).type === SonareEngineTelemetryType.ProcessBlock,
          ),
        ).toMatchObject({
          type: SonareEngineTelemetryType.ProcessBlock,
          error: SonareEngineTelemetryError.None,
          timelineSample: 128,
        });
      } finally {
        processor.destroy();
      }
    });

    it('publishes real output meters from the realtime engine', () => {
      const meters: unknown[] = [];
      const posted: unknown[] = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize: 128, channelCount: 2, meterIntervalFrames: 128 },
        {
          onMeter: (meter) => meters.push(meter),
          postMessage: (message) => posted.push(message),
        },
      );
      try {
        const inL = new Float32Array(128).fill(0.5);
        const inR = new Float32Array(128).fill(-0.5);
        const outL = new Float32Array(128);
        const outR = new Float32Array(128);
        expect(processor.process([[inL, inR]], [[outL, outR]])).toBe(true);
        expect(meters).toHaveLength(1);
        const meter = meters[0] as { peakDbL: number; peakDbR: number };
        expect(meters[0]).toMatchObject({
          type: 'meter',
          frame: 0,
        });
        expect(meter.peakDbL).toBeCloseTo(-6.0206, 2);
        expect(meter.peakDbR).toBeCloseTo(-6.0206, 2);
        expect(posted).toEqual(
          expect.arrayContaining([expect.objectContaining({ type: 'meter' })]),
        );
      } finally {
        processor.destroy();
      }
    });

    it('registers a realtime engine processor in an AudioWorklet-like global scope', () => {
      const previousProcessor = (
        globalThis as typeof globalThis & { AudioWorkletProcessor?: unknown }
      ).AudioWorkletProcessor;
      const previousRegister = (globalThis as typeof globalThis & { registerProcessor?: unknown })
        .registerProcessor;
      let registeredName = '';
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
          registerProcessor: (name: string, ctor: unknown) => {
            registeredName = name;
            registeredCtor = ctor;
          },
        });
        registerSonareRealtimeEngineWorkletProcessor();
        expect(registeredName).toBe('sonare-realtime-engine-processor');
        expect(typeof registeredCtor).toBe('function');
      } finally {
        Object.assign(globalThis, {
          AudioWorkletProcessor: previousProcessor,
          registerProcessor: previousRegister,
        });
      }
    });

    it('rejects direct embind bridge construction for the dedicated sonare-rt target', () => {
      expect(
        () =>
          new SonareRealtimeEngineWorkletProcessor({
            runtimeTarget: 'sonare-rt',
            sampleRate: 48000,
            blockSize: 128,
          }),
      ).toThrow(/dedicated Emscripten AudioWorklet module/);
    });
  });

  describe('SonareRtRealtimeEngineRuntime', () => {
    async function createRtModule(): Promise<{
      module: {
        _malloc: (size: number) => number;
        _free: (ptr: number) => void;
        _sonare_rt_engine_abi_version: () => number;
        _sonare_rt_engine_create: () => number;
        _sonare_rt_engine_destroy: (engine: number) => void;
        _sonare_rt_engine_prepare: (
          engine: number,
          sampleRate: number,
          maxBlockSize: number,
          commandCapacity: number,
          telemetryCapacity: number,
        ) => number;
        _sonare_rt_engine_play: (engine: number, renderFrame: bigint) => number;
        _sonare_rt_engine_stop: (engine: number, renderFrame: bigint) => number;
        _sonare_rt_engine_seek_sample: (
          engine: number,
          timelineSample: bigint,
          renderFrame: bigint,
        ) => number;
        _sonare_rt_engine_seek_ppq: (engine: number, ppq: number, renderFrame: bigint) => number;
        _sonare_rt_engine_set_tempo: (engine: number, bpm: number) => number;
        _sonare_rt_engine_set_loop: (
          engine: number,
          startPpq: number,
          endPpq: number,
          enabled: number,
        ) => number;
        _sonare_rt_engine_seek_marker: (
          engine: number,
          markerId: number,
          renderFrame: bigint,
        ) => number;
        _sonare_rt_engine_set_metronome_enabled: (
          engine: number,
          enabled: number,
          beatGain: number,
          accentGain: number,
          clickSamples: number,
        ) => number;
        _sonare_rt_engine_set_capture_armed: (engine: number, armed: number) => number;
        _sonare_rt_engine_set_capture_punch: (
          engine: number,
          startSample: bigint,
          endSample: bigint,
          enabled: number,
        ) => number;
        _sonare_rt_engine_process: (
          engine: number,
          channelsPtr: number,
          numChannels: number,
          numFrames: number,
        ) => void;
        _sonare_rt_engine_drain_telemetry: (
          engine: number,
          typesErrorsValuesPtr: number,
          frameValuesPtr: number,
          maxRecords: number,
        ) => number;
      };
      memory: WebAssembly.Memory;
    }> {
      const { default: createSonareRt } = (await import('../dist/sonare-rt.js')) as {
        default: (options?: {
          locateFile?: (path: string) => string;
          wasmMemory?: WebAssembly.Memory;
        }) => Promise<{
          _malloc: (size: number) => number;
          _free: (ptr: number) => void;
          _sonare_rt_engine_abi_version: () => number;
          _sonare_rt_engine_create: () => number;
          _sonare_rt_engine_destroy: (engine: number) => void;
          _sonare_rt_engine_prepare: (
            engine: number,
            sampleRate: number,
            maxBlockSize: number,
            commandCapacity: number,
            telemetryCapacity: number,
          ) => number;
          _sonare_rt_engine_play: (engine: number, renderFrame: bigint) => number;
          _sonare_rt_engine_stop: (engine: number, renderFrame: bigint) => number;
          _sonare_rt_engine_seek_sample: (
            engine: number,
            timelineSample: bigint,
            renderFrame: bigint,
          ) => number;
          _sonare_rt_engine_seek_ppq: (engine: number, ppq: number, renderFrame: bigint) => number;
          _sonare_rt_engine_set_tempo: (engine: number, bpm: number) => number;
          _sonare_rt_engine_set_loop: (
            engine: number,
            startPpq: number,
            endPpq: number,
            enabled: number,
          ) => number;
          _sonare_rt_engine_seek_marker: (
            engine: number,
            markerId: number,
            renderFrame: bigint,
          ) => number;
          _sonare_rt_engine_set_metronome_enabled: (
            engine: number,
            enabled: number,
            beatGain: number,
            accentGain: number,
            clickSamples: number,
          ) => number;
          _sonare_rt_engine_set_capture_armed: (engine: number, armed: number) => number;
          _sonare_rt_engine_set_capture_punch: (
            engine: number,
            startSample: bigint,
            endSample: bigint,
            enabled: number,
          ) => number;
          _sonare_rt_engine_process: (
            engine: number,
            channelsPtr: number,
            numChannels: number,
            numFrames: number,
          ) => void;
          _sonare_rt_engine_drain_telemetry: (
            engine: number,
            typesErrorsValuesPtr: number,
            frameValuesPtr: number,
            maxRecords: number,
          ) => number;
        }>;
      };
      const memory = new WebAssembly.Memory({ initial: 1024, maximum: 1024, shared: true });
      return {
        module: await createSonareRt({
          wasmMemory: memory,
          locateFile: (path) => new URL(`../dist/${path}`, import.meta.url).pathname,
        }),
        memory,
      };
    }

    it('drives the dedicated sonare-rt C ABI from SAB command and telemetry rings', async () => {
      const blockSize = 128;
      const { module: rt, memory } = await createRtModule();
      const commandRing = createSonareEngineCommandRingBuffer(8);
      const telemetryRing = createSonareEngineTelemetryRingBuffer(8);
      const runtime = new SonareRtRealtimeEngineRuntime({
        module: rt,
        memory,
        sampleRate: 48000,
        blockSize,
        channelCount: 2,
        commandSharedBuffer: commandRing.sharedBuffer,
        telemetrySharedBuffer: telemetryRing.sharedBuffer,
      });
      try {
        const inputL = new Float32Array(blockSize);
        const inputR = new Float32Array(blockSize);
        inputL[0] = 0.25;
        inputL[blockSize - 1] = 0.5;
        inputR[0] = -0.25;
        inputR[blockSize - 1] = -0.5;
        const outL = new Float32Array(blockSize);
        const outR = new Float32Array(blockSize);

        expect(
          pushSonareEngineCommandRingBuffer(commandRing, {
            type: SonareEngineCommandType.TransportPlay,
            sampleTime: -1,
          }),
        ).toBe(true);
        expect(runtime.process([[inputL, inputR]], [[outL, outR]])).toBe(true);
        expect(outL[0]).toBeCloseTo(0.25, 6);
        expect(outL[blockSize - 1]).toBeCloseTo(0.5, 6);
        expect(outR[0]).toBeCloseTo(-0.25, 6);
        expect(outR[blockSize - 1]).toBeCloseTo(-0.5, 6);
        expect(commandRing.header[1]).toBe(1);

        const first = readSonareEngineTelemetryRingBuffer(telemetryRing);
        expect(first.telemetry.at(-1)).toMatchObject({
          type: SonareEngineTelemetryType.ProcessBlock,
          error: SonareEngineTelemetryError.None,
          timelineSample: blockSize,
        });

        expect(
          pushSonareEngineCommandRingBuffer(commandRing, {
            type: SonareEngineCommandType.TransportSeekSample,
            sampleTime: -1,
            argInt: 48000,
          }),
        ).toBe(true);
        expect(runtime.process([[inputL, inputR]], [[outL, outR]])).toBe(true);
        const second = readSonareEngineTelemetryRingBuffer(telemetryRing, first.nextReadIndex);
        expect(second.telemetry.at(-1)?.timelineSample).toBe(48000 + blockSize);
      } finally {
        runtime.destroy();
      }
    });

    it('processes non-128-frame quanta within the prepared max block size', async () => {
      const { module: rt, memory } = await createRtModule();
      const commandRing = createSonareEngineCommandRingBuffer(8);
      const telemetryRing = createSonareEngineTelemetryRingBuffer(8);
      const runtime = new SonareRtRealtimeEngineRuntime({
        module: rt,
        memory,
        sampleRate: 48000,
        blockSize: 256,
        channelCount: 2,
        commandSharedBuffer: commandRing.sharedBuffer,
        telemetrySharedBuffer: telemetryRing.sharedBuffer,
      });
      try {
        expect(
          pushSonareEngineCommandRingBuffer(commandRing, {
            type: SonareEngineCommandType.TransportPlay,
            sampleTime: -1,
          }),
        ).toBe(true);
        const frames = 64;
        const inL = new Float32Array(frames);
        const inR = new Float32Array(frames);
        inL[frames - 1] = 0.75;
        inR[frames - 1] = -0.75;
        const outL = new Float32Array(frames);
        const outR = new Float32Array(frames);
        expect(runtime.process([[inL, inR]], [[outL, outR]])).toBe(true);
        expect(outL[frames - 1]).toBeCloseTo(0.75, 6);
        expect(outR[frames - 1]).toBeCloseTo(-0.75, 6);
        const read = readSonareEngineTelemetryRingBuffer(telemetryRing);
        expect(read.telemetry.at(-1)).toMatchObject({
          type: SonareEngineTelemetryType.ProcessBlock,
          error: SonareEngineTelemetryError.None,
          timelineSample: frames,
          value: frames,
        });
      } finally {
        runtime.destroy();
      }
    });

    it('maps extended SAB commands onto the sonare-rt C ABI exports', async () => {
      const { module: rt, memory } = await createRtModule();
      const commandRing = createSonareEngineCommandRingBuffer(16);
      const telemetryRing = createSonareEngineTelemetryRingBuffer(16);
      const runtime = new SonareRtRealtimeEngineRuntime({
        module: rt,
        memory,
        sampleRate: 48000,
        blockSize: 128,
        channelCount: 2,
        commandSharedBuffer: commandRing.sharedBuffer,
        telemetrySharedBuffer: telemetryRing.sharedBuffer,
      });
      try {
        const commands = [
          { type: SonareEngineCommandType.SetTempoMap, argFloat: 90 },
          {
            type: SonareEngineCommandType.SetLoop,
            targetId: 1,
            argFloat: 0,
            argInt: 2_000_000,
          },
          { type: SonareEngineCommandType.TransportSeekPpq, argFloat: 0.5 },
          { type: SonareEngineCommandType.SetMetronome, argInt: 1 },
          { type: SonareEngineCommandType.ArmRecord, argInt: 1 },
          { type: SonareEngineCommandType.Punch, argInt: 0, argFloat: 1 },
        ];
        for (const command of commands) {
          expect(pushSonareEngineCommandRingBuffer(commandRing, command)).toBe(true);
        }
        expect(
          pushSonareEngineCommandRingBuffer(commandRing, {
            type: SonareEngineCommandType.TransportPlay,
            sampleTime: -1,
          }),
        ).toBe(true);
        const outL = new Float32Array(128);
        const outR = new Float32Array(128);
        expect(runtime.process([[]], [[outL, outR]])).toBe(true);
        expect(commandRing.header[1]).toBe(commands.length + 1);
        const first = readSonareEngineTelemetryRingBuffer(telemetryRing);
        expect(first.telemetry.at(-1)).toMatchObject({
          type: SonareEngineTelemetryType.ProcessBlock,
          error: SonareEngineTelemetryError.None,
        });
        expect(first.telemetry.at(-1)?.timelineSample).toBeGreaterThan(10000);
      } finally {
        runtime.destroy();
      }
    });
  });

  describe('SonareRealtimeEngineNode', () => {
    function fakeContext(): BaseAudioContext {
      return {
        sampleRate: 48000,
        audioWorklet: {
          added: [] as (string | URL)[],
          addModule(moduleUrl: string | URL): Promise<void> {
            this.added.push(moduleUrl);
            return Promise.resolve();
          },
        },
      } as unknown as BaseAudioContext;
    }

    it('creates a SAB-backed AudioWorkletNode facade and queues transport commands', async () => {
      let capturedOptions: AudioWorkletNodeOptions | undefined;
      const posted: unknown[] = [];
      const disconnected: boolean[] = [];
      const node = await SonareRealtimeEngineNode.create(fakeContext(), {
        moduleUrl: 'sonare-worklet.js',
        blockSize: 128,
        channelCount: 2,
        commandRingCapacity: 4,
        telemetryRingCapacity: 4,
        nodeFactory: (_context, processorName, options) => {
          expect(processorName).toBe('sonare-realtime-engine-processor');
          capturedOptions = options;
          return {
            port: {
              postMessage: (message: unknown) => posted.push(message),
              onmessage: undefined,
            },
            disconnect: () => disconnected.push(true),
          } as unknown as AudioWorkletNode;
        },
      });

      expect(node.capabilities.mode).toBe('sab');
      expect(node.capabilities.runtimeTarget).toBe('embind');
      expect(node.commandRing).toBeDefined();
      expect(node.telemetryRing).toBeDefined();
      expect(capturedOptions?.processorOptions).toMatchObject({
        sampleRate: 48000,
        blockSize: 128,
        channelCount: 2,
      });
      expect(node.play()).toBe(true);
      const commandRing = node.commandRing;
      const telemetryRing = node.telemetryRing;
      if (!commandRing || !telemetryRing) {
        throw new Error('expected command and telemetry rings');
      }
      expect(popSonareEngineCommandRingBuffer(commandRing)).toMatchObject({
        type: SonareEngineCommandType.TransportPlay,
      });

      writeSonareEngineTelemetryRingBuffer(telemetryRing, {
        type: SonareEngineTelemetryType.ProcessBlock,
        error: SonareEngineTelemetryError.None,
        renderFrame: 0,
        timelineSample: 128,
        audibleTimelineSample: 128,
        graphLatencySamplesQ8: 0,
        value: 128,
      });
      const seen: unknown[] = [];
      node.onTelemetry((telemetry) => seen.push(telemetry));
      expect(node.pollTelemetry()).toHaveLength(1);
      expect(seen[0]).toMatchObject({ timelineSample: 128 });
      node.destroy();
      expect(disconnected).toEqual([true]);
      expect(posted.at(-1)).toMatchObject({ type: SonareEngineCommandType.TransportStop });
    });

    it('falls back to postMessage commands when requested', async () => {
      const posted: unknown[] = [];
      const node = await SonareRealtimeEngineNode.create(fakeContext(), {
        mode: 'postMessage',
        nodeFactory: () =>
          ({
            port: {
              postMessage: (message: unknown) => posted.push(message),
              onmessage: undefined,
            },
            disconnect: () => undefined,
          }) as unknown as AudioWorkletNode,
      });
      expect(node.capabilities.mode).toBe('postMessage');
      expect(node.commandRing).toBeUndefined();
      expect(node.seekSample(48000)).toBe(true);
      expect(posted[0]).toMatchObject({
        type: SonareEngineCommandType.TransportSeekSample,
        argInt: 48000,
      });
      node.destroy();
    });

    it('automatically degrades to postMessage when SharedArrayBuffer is unavailable', async () => {
      const previous = globalThis.SharedArrayBuffer;
      try {
        Object.defineProperty(globalThis, 'SharedArrayBuffer', {
          configurable: true,
          writable: true,
          value: undefined,
        });
        const node = await SonareRealtimeEngineNode.create(fakeContext(), {
          nodeFactory: () =>
            ({
              port: {
                postMessage: () => undefined,
                onmessage: undefined,
              },
              disconnect: () => undefined,
            }) as unknown as AudioWorkletNode,
        });
        expect(node.capabilities.mode).toBe('postMessage');
        expect(node.capabilities.degradedReason).toMatch(/SharedArrayBuffer/);
        expect(node.commandRing).toBeUndefined();
        node.destroy();
      } finally {
        Object.defineProperty(globalThis, 'SharedArrayBuffer', {
          configurable: true,
          writable: true,
          value: previous,
        });
      }
    });

    it('can select the dedicated sonare-rt AudioWorklet target', async () => {
      let capturedName = '';
      let capturedOptions: AudioWorkletNodeOptions | undefined;
      const context = fakeContext();
      const node = await SonareRealtimeEngineNode.create(context, {
        runtimeTarget: 'sonare-rt',
        moduleUrl: 'sonare-engine-worklet.js',
        rtModuleUrl: 'sonare-rt.js',
        mode: 'postMessage',
        nodeFactory: (_context, processorName, options) => {
          capturedName = processorName;
          capturedOptions = options;
          return {
            port: {
              postMessage: () => undefined,
              onmessage: undefined,
            },
            disconnect: () => undefined,
          } as unknown as AudioWorkletNode;
        },
      });

      expect((context.audioWorklet as unknown as { added: string[] }).added).toEqual([
        'sonare-engine-worklet.js',
      ]);
      expect(capturedName).toBe('sonare-realtime-engine-processor');
      expect(capturedOptions?.processorOptions).toMatchObject({
        runtimeTarget: 'sonare-rt',
        rtModuleUrl: 'sonare-rt.js',
      });
      expect(node.capabilities.runtimeTarget).toBe('sonare-rt');
      node.destroy();
    });

    it('rejects an ABI mismatch before constructing an AudioWorkletNode', async () => {
      let constructed = false;
      await expect(
        SonareRealtimeEngineNode.create(fakeContext(), {
          engineAbiVersion: 1,
          expectedEngineAbiVersion: 2,
          nodeFactory: () => {
            constructed = true;
            return {
              port: { postMessage: () => undefined, onmessage: undefined },
              disconnect: () => undefined,
            } as unknown as AudioWorkletNode;
          },
        }),
      ).rejects.toThrow(/Engine ABI mismatch/);
      expect(constructed).toBe(false);
    });

    it('exposes the high-level SonareEngine facade for transport, timeline, and offline APIs', async () => {
      const posted: unknown[] = [];
      const offline = new (await import('../dist/index.js')).RealtimeEngine(48000, 128);
      offline.addParameter({
        id: 7,
        name: 'gain',
        unit: 'dB',
        minValue: -60,
        maxValue: 12,
        defaultValue: 0,
        rtSafe: true,
        defaultCurve: 0,
      });
      const engine = await SonareEngine.create(fakeContext(), {
        mode: 'postMessage',
        offlineEngine: offline,
        offlineChannelCount: 2,
        nodeFactory: () =>
          ({
            port: {
              postMessage: (message: unknown) => posted.push(message),
              onmessage: undefined,
            },
            disconnect: () => undefined,
          }) as unknown as AudioWorkletNode,
      });
      try {
        expect(engine.capabilities.mode).toBe('postMessage');
        expect(engine.listParameters()).toHaveLength(1);
        expect(engine.transport.play()).toBe(true);
        expect(engine.transport.seekSeconds(1)).toBe(true);
        engine.transport.setTempo(90);
        expect(engine.transport.setLoop(0, 1, true)).toBe(true);
        expect(engine.setParam('gain-node', 'gain', -6)).toBe(false);
        engine.scheduleParam('gain-node', 'gain', 0.5, -3);
        engine.addAutomationPoint(7, 1, 0);
        expect(engine.setSoloMute(3, true, false)).toBe(false);
        const clipId = engine.addClip(
          0,
          [new Float32Array(128).fill(0.25), new Float32Array(128).fill(-0.25)],
          0,
        );
        expect(clipId).toBeGreaterThan(0);
        engine.removeClip(clipId);
        expect(engine.armRecord(0, true)).toBe(true);
        expect(engine.punch(0, 1)).toBe(true);
        engine.setMetronome({ enabled: true, clickSamples: 16 });
        const markerId = engine.addMarker(0, 'start');
        expect(markerId).toBeGreaterThan(0);
        expect(engine.seekMarker(markerId)).toBe(false);
        const rendered = await engine.renderOffline(128);
        expect(rendered).toHaveLength(2);
        expect(rendered[0]).toHaveLength(128);

        expect(posted).toEqual(
          expect.arrayContaining([
            expect.objectContaining({ type: SonareEngineCommandType.TransportPlay }),
            expect.objectContaining({ type: SonareEngineCommandType.TransportSeekSample }),
            expect.objectContaining({ type: SonareEngineCommandType.SetTempoMap }),
            expect.objectContaining({ type: SonareEngineCommandType.SetLoop }),
            expect.objectContaining({ type: SonareEngineCommandType.ArmRecord }),
            expect.objectContaining({ type: SonareEngineCommandType.Punch }),
            expect.objectContaining({ type: SonareEngineCommandType.SetMetronome }),
          ]),
        );
      } finally {
        engine.destroy();
      }
    });

    it('runs suspend/resume/destroy lifecycle without accepting stale transport commands', async () => {
      const posted: unknown[] = [];
      const disconnected: boolean[] = [];
      const lifecycle: string[] = [];
      const context = {
        ...fakeContext(),
        suspend: () => {
          lifecycle.push('suspend');
          return Promise.resolve();
        },
        resume: () => {
          lifecycle.push('resume');
          return Promise.resolve();
        },
      } as BaseAudioContext & { suspend: () => Promise<void>; resume: () => Promise<void> };
      const engine = await SonareEngine.create(context, {
        mode: 'postMessage',
        nodeFactory: () =>
          ({
            port: {
              postMessage: (message: unknown) => posted.push(message),
              onmessage: undefined,
            },
            disconnect: () => disconnected.push(true),
          }) as unknown as AudioWorkletNode,
      });

      await engine.suspend();
      await engine.resume();
      expect(lifecycle).toEqual(['suspend', 'resume']);
      expect(engine.transport.play()).toBe(true);
      engine.destroy();
      expect(disconnected).toEqual([true]);
      expect(posted.at(-1)).toMatchObject({ type: SonareEngineCommandType.TransportStop });
      expect(engine.transport.play()).toBe(false);
      await engine.suspend();
      await engine.resume();
      expect(lifecycle).toEqual(['suspend', 'resume']);
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
