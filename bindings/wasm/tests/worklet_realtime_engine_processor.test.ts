import {
  createSonareEngineCommandRingBuffer,
  createSonareEngineTelemetryRingBuffer,
  describe,
  expect,
  it,
  pushSonareEngineCommandRingBuffer,
  readSonareEngineTelemetryRingBuffer,
  registerSonareRealtimeEngineWorkletProcessor,
  SonareEngineCommandType,
  SonareEngineTelemetryError,
  SonareEngineTelemetryType,
  SonareRealtimeEngineWorkletProcessor,
  setupWorklet,
} from './_worklet_helpers';

describe('SonareRealtimeEngineWorkletProcessor', () => {
  setupWorklet();

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
});
