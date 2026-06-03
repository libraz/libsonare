import {
  createSonareEngineCommandRingBuffer,
  createSonareEngineTelemetryRingBuffer,
  createSonareMeterRingBuffer,
  createSonareSpectrumRingBuffer,
  describe,
  expect,
  it,
  Mixer,
  mixingScenePresetJson,
  popSonareEngineCommandRingBuffer,
  pushSonareEngineCommandRingBuffer,
  readSonareEngineTelemetryRingBuffer,
  readSonareMeterRingBuffer,
  readSonareSpectrumRingBuffer,
  registerSonareRealtimeEngineWorkletProcessor,
  registerSonareRealtimeVoiceChangerWorkletProcessor,
  registerSonareWorkletProcessor,
  setupWorklet,
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
} from './_worklet_helpers';

describe('SonareRtRealtimeEngineRuntime', () => {
  setupWorklet();

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
});
