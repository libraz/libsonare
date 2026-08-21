import type { RealtimeVoiceChangerPodConfig } from '../src/public_types';
import {
  describe,
  expect,
  it,
  registerSonareRealtimeVoiceChangerWorkletProcessor,
  SonareRealtimeVoiceChangerWorkletProcessor,
  setupWorklet,
} from './_worklet_helpers';

function workletPod(): RealtimeVoiceChangerPodConfig {
  return {
    inputGainDb: 0,
    outputGainDb: 0,
    wetMix: 1,
    retuneSemitones: 0,
    retuneMix: 0,
    retuneGrainSize: 0,
    formantFactor: 1,
    formantAmount: 0,
    formantBody: 0,
    formantBrightness: 0,
    formantNasal: 0,
    eqHighpassHz: 80,
    eqBodyDb: 0,
    eqPresenceDb: 0,
    eqAirDb: 0,
    gateThresholdDb: -55,
    gateAttackMs: 2,
    gateReleaseMs: 100,
    gateRangeDb: 18,
    compressorThresholdDb: -22,
    compressorRatio: 2,
    compressorAttackMs: 6,
    compressorReleaseMs: 90,
    compressorMakeupGainDb: 0,
    deesserFrequencyHz: 7200,
    deesserThresholdDb: -28,
    deesserRatio: 4,
    deesserRangeDb: 8,
    reverbMix: 0,
    reverbTimeMs: 100,
    reverbDamping: 0.5,
    reverbSeed: 1,
    limiterCeilingDb: -1,
    limiterReleaseMs: 50,
    limiterEnableIspLimiter: true,
    limiterIspCeilingDbtp: -1,
  };
}

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

        processor.receiveMessage({
          type: 'setConfig',
          config: workletPod(),
        });
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
        // The old assertion stopped here and never looked at out2, which is
        // exactly where the defect lived.
        expect(out2.every((s) => Number.isFinite(s))).toBe(true);
      } finally {
        processor.destroy();
      }
    });

    // The processed voice used to reach only the left speaker: the mono branch
    // wrote output[0] and left every other output channel holding whatever the
    // previous block put there. The default channelCount here is 1 while
    // AudioWorkletNode defaults to `channelCountMode: 'max'`, so a plain stereo
    // graph hit it. The engine worklet already fanned a single plane out to
    // every channel; the two now share one implementation.
    it('fans a mono plane out to every output channel instead of hard-panning left', () => {
      const blockSize = 128;
      const processor = new SonareRealtimeVoiceChangerWorkletProcessor({
        preset: 'neutral-monitor',
        sampleRate: 48000,
        blockSize,
        channelCount: 1,
      });
      try {
        const input = new Float32Array(blockSize);
        for (let i = 0; i < blockSize; i++) {
          input[i] = 0.4 * Math.sin((2 * Math.PI * 220 * i) / 48000);
        }
        // Seed the outputs with a value the processor must overwrite, so an
        // untouched channel is distinguishable from a silent one.
        const out1 = new Float32Array(blockSize);
        const out2 = new Float32Array(blockSize);
        // The chain reports a fixed latency (the retune OLA delays by a whole
        // grain), so run past it before asserting the signal is audible.
        let audible = false;
        for (let block = 0; block < 40; block++) {
          out1.fill(-9);
          out2.fill(-9);
          expect(processor.process([[input]], [[out1, out2]])).toBe(true);
          // Both channels are written every block, latency or not.
          expect(out1.every((s) => s !== -9)).toBe(true);
          expect(Array.from(out2)).toEqual(Array.from(out1));
          audible = audible || out1.some((s) => s !== 0);
        }
        expect(audible).toBe(true);
      } finally {
        processor.destroy();
      }
    });

    it('zeroes output channels past the last processed plane', () => {
      // Two planes drive two channels; a third output channel is silence rather
      // than a duplicate of plane 0, which would put the left signal into a
      // rear/centre channel and add energy the chain never produced.
      const blockSize = 128;
      const processor = new SonareRealtimeVoiceChangerWorkletProcessor({
        preset: 'neutral-monitor',
        sampleRate: 48000,
        blockSize,
        channelCount: 2,
      });
      try {
        const left = new Float32Array(blockSize).fill(0.3);
        const right = new Float32Array(blockSize).fill(-0.3);
        const outputs = [
          new Float32Array(blockSize).fill(-9),
          new Float32Array(blockSize).fill(-9),
          new Float32Array(blockSize).fill(-9),
        ];
        expect(processor.process([[left, right]], [outputs])).toBe(true);
        for (const channel of outputs) {
          expect(channel.every((s) => s !== -9)).toBe(true);
        }
        expect(Array.from(outputs[2])).toEqual(new Array(blockSize).fill(0));
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
          changer: { setPodConfig: (config: unknown) => void };
        };
        const originalSetConfig = internal.changer.setPodConfig.bind(internal.changer);
        let setConfigCalls = 0;
        internal.changer.setPodConfig = (config: unknown) => {
          setConfigCalls++;
          originalSetConfig(config);
        };

        // (1) Synchronous application on message receipt.
        const beforeRecv = setConfigCalls;
        processor.receiveMessage({
          type: 'setConfig',
          config: workletPod(),
        });
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
        changer: { setPodConfig: (config: unknown) => void };
      };
      let setConfigCalls = 0;
      const originalSetConfig = internal.changer.setPodConfig.bind(internal.changer);
      internal.changer.setPodConfig = (config: unknown) => {
        setConfigCalls++;
        originalSetConfig(config);
      };
      processor.destroy();
      expect(() =>
        processor.receiveMessage({
          type: 'setConfig',
          config: workletPod(),
        }),
      ).not.toThrow();
      expect(() => processor.receiveMessage({ type: 'reset' })).not.toThrow();
      expect(setConfigCalls).toBe(0);
    });

    it('hammering setConfig via receiveMessage between blocks allocates no Float32Array storage (RT-safety)', () => {
      // Reproduces the real AudioWorklet hazard directly: dragging a UI
      // slider fires postMessage -> receiveMessage({type: 'setConfig', ...})
      // once per animation frame, and on WASM that handler runs on the SAME
      // single rendering thread as process() (there is no separate "control
      // thread" inside an AudioWorkletGlobalScope). Interleave setConfig with
      // process() the way a real drag would and assert no size-based
      // Float32Array allocation happens on this thread for either call.
      //
      // This only observes JS-side typed-array allocation; it cannot see
      // whether the underlying WASM call heap-allocates internally (e.g. via
      // malloc for a C++ shared_ptr). That is covered directly, with a real
      // allocation-counting harness over operator new/delete, by
      // "RealtimeVoiceChanger::set_config performs no heap allocation" in
      // tests/mixing/no_alloc_voice_reverb_test.cpp.
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
        const input = new original(blockSize);
        const output = new original(blockSize);
        for (let i = 0; i < blockSize; i++) {
          input[i] = Math.sin((2 * Math.PI * 220 * i) / 48000) * 0.1;
        }
        const pods = [
          workletPod(),
          { ...workletPod(), wetMix: 0.2 },
          { ...workletPod(), wetMix: 0.8 },
        ];
        storageAllocationCount = 0;
        for (let i = 0; i < 30; i++) {
          processor.receiveMessage({ type: 'setConfig', config: pods[i % pods.length] });
          expect(processor.process([[input]], [[output]])).toBe(true);
        }
        expect(storageAllocationCount).toBe(0);
        expect(output.every((sample) => Number.isFinite(sample))).toBe(true);
      } finally {
        (globalThis as { Float32Array: typeof Float32Array }).Float32Array = original;
        processor.destroy();
      }
    });

    it('rejects an unknown preset id at construction', async () => {
      // The contract is not implementation-dependent: the preset lookup in the
      // core throws SonareException(InvalidParameter) for an id it does not
      // recognize, the embind wrapper rethrows, and -fexceptions carries it to
      // JS as a catchable SonareError. Constructing with an unknown id must
      // therefore fail rather than fall back to a default voice, which would
      // send a performer's audio through the wrong processing chain silently.
      const { ErrorCode, isSonareError } = await import('../dist/index.js');
      let caught: unknown;
      try {
        new SonareRealtimeVoiceChangerWorkletProcessor({
          preset: 'definitely-not-a-preset',
        } as never);
      } catch (error) {
        caught = error;
      }
      expect(caught, 'an unknown preset id must not construct silently').toBeDefined();
      expect(isSonareError(caught)).toBe(true);
      expect((caught as { code: number }).code).toBe(ErrorCode.InvalidParameter);
      expect((caught as Error).message).toContain('unknown realtime voice changer preset');
    });
  });
});
