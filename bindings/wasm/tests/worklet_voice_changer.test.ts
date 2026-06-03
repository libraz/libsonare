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

describe('SonareRealtimeVoiceChangerWorkletProcessor', () => {
  setupWorklet();

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
});
