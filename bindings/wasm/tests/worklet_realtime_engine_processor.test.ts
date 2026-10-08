import { vi } from 'vitest';
import { RealtimeEngine } from '../dist/index.js';
// The accepted-type table itself, so the coverage below is generated from the
// same source of truth as the guard rather than restated.
import { ENGINE_SYNC_MESSAGE_TYPES, isEngineSyncMessage } from '../src/worklet/guards';
import type { SonareEngineSyncMessage } from '../src/worklet/messages';
import type { SonareEngineCommandRecord } from '../src/worklet/protocol';
import {
  createSonareClipPageRequestRingBuffer,
  createSonareEngineCommandRingBuffer,
  createSonareEngineTelemetryRingBuffer,
  createSonareExternalMidiRingBuffer,
  createSonareScopeRingBuffer,
  describe,
  expect,
  it,
  pushSonareEngineCommandRingBuffer,
  readSonareClipPageRequestRingBuffer,
  readSonareEngineTelemetryRingBuffer,
  readSonareExternalMidiRingBuffer,
  readSonareScopeRingBuffer,
  registerSonareRealtimeEngineWorkletProcessor,
  SonareEngine,
  SonareEngineCommandType,
  SonareEngineTelemetryError,
  SonareEngineTelemetryType,
  SonareRealtimeEngineWorkletProcessor,
  setupWorklet,
} from './_worklet_helpers';

describe('SonareRealtimeEngineWorkletProcessor', () => {
  setupWorklet();

  describe('SonareRealtimeEngineWorkletProcessor', () => {
    const midi1Word = (status: number, channel: number, data0: number, data1: number): number =>
      (0x2 << 28) | ((status & 0xf) << 20) | ((channel & 0xf) << 16) | (data0 << 8) | data1;

    it('drains external MIDI into the SAB ring without postMessage', () => {
      const blockSize = 128;
      const externalMidiRing = createSonareExternalMidiRingBuffer(8);
      const posted: unknown[] = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        {
          sampleRate: 48000,
          blockSize,
          channelCount: 2,
          externalMidiSharedBuffer: externalMidiRing.sharedBuffer,
          externalMidiRingCapacity: externalMidiRing.capacity,
        },
        { postMessage: (message) => posted.push(message) },
      );
      try {
        const engine = (
          processor as unknown as {
            engine: {
              setMidiDestinationExternal: (destinationId: number, external: boolean) => void;
              setMidiClips: (clips: unknown[]) => void;
              play: (sampleTime?: number) => void;
            };
          }
        ).engine;
        engine.setMidiDestinationExternal(5, true);
        engine.setMidiClips([
          {
            id: 1,
            trackId: 5,
            destinationId: 5,
            lengthSamples: 8192,
            events: [
              { renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 },
              { renderFrame: 64, word0: midi1Word(0x8, 0, 60, 0), wordCount: 1 },
            ],
          },
        ]);
        engine.play();
        expect(
          processor.process([[]], [[new Float32Array(blockSize), new Float32Array(blockSize)]]),
        ).toBe(true);

        expect(readSonareExternalMidiRingBuffer(externalMidiRing)).toEqual({
          events: [
            { destinationId: 5, renderFrame: 0, byteWord: 0x00643c90, byteCount: 3 },
            { destinationId: 5, renderFrame: 64, byteWord: 0x00003c80, byteCount: 3 },
          ],
          dropped: 0,
        });
        expect(posted).not.toContainEqual(expect.objectContaining({ type: 'externalMidi' }));
      } finally {
        processor.destroy();
      }
    });

    it('keeps an external-MIDI event the full ring refused for a later quantum', () => {
      const blockSize = 128;
      const externalMidiRing = createSonareExternalMidiRingBuffer(1);
      const processor = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        blockSize,
        channelCount: 2,
        externalMidiSharedBuffer: externalMidiRing.sharedBuffer,
        externalMidiRingCapacity: externalMidiRing.capacity,
      });
      try {
        const engine = (
          processor as unknown as {
            engine: {
              setMidiDestinationExternal: (destinationId: number, external: boolean) => void;
              setMidiClips: (clips: unknown[]) => void;
              play: (sampleTime?: number) => void;
            };
          }
        ).engine;
        engine.setMidiDestinationExternal(5, true);
        engine.setMidiClips([
          {
            id: 1,
            trackId: 5,
            destinationId: 5,
            lengthSamples: 8192,
            events: [
              { renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 },
              { renderFrame: 64, word0: midi1Word(0x8, 0, 60, 0), wordCount: 1 },
            ],
          },
        ]);
        engine.play();
        const received: number[] = [];
        for (let quantum = 0; quantum < 3; quantum++) {
          processor.process([[]], [[new Float32Array(blockSize), new Float32Array(blockSize)]]);
          const read = readSonareExternalMidiRingBuffer(externalMidiRing);
          expect(read.dropped).toBe(0);
          received.push(...read.events.map((event) => event.byteWord));
        }
        expect(received).toEqual([0x00643c90, 0x00003c80]);
      } finally {
        processor.destroy();
      }
    });

    it('refuses a channelCount instead of rounding it into range', () => {
      const build = (channelCount: number): SonareRealtimeEngineWorkletProcessor =>
        new SonareRealtimeEngineWorkletProcessor({
          sampleRate: 48000,
          blockSize: 128,
          channelCount,
        });

      const control = build(2);
      try {
        expect(control.process([[]], [[new Float32Array(128), new Float32Array(128)]])).toBe(true);
      } finally {
        control.destroy();
      }

      for (const channelCount of [2.7, 0, -1, Number.NaN]) {
        expect(() => build(channelCount)).toThrow(/channelCount must be an integer of at least 1/);
      }
      // The ceiling is the engine's, so a count past it is refused there.
      expect(() => build(1e9)).toThrow(/max_channels must be within 1\.\./);
    });

    it('refuses a meterIntervalFrames instead of rounding it into range', () => {
      const blockSize = 128;
      const meterCount = (meterIntervalFrames: number): number => {
        const meters: unknown[] = [];
        const processor = new SonareRealtimeEngineWorkletProcessor(
          { sampleRate: 48000, blockSize, channelCount: 2, meterIntervalFrames },
          { onMeter: (meter) => meters.push(meter), postMessage: () => undefined },
        );
        try {
          for (let block = 0; block < 8; block++) {
            expect(
              processor.process([[]], [[new Float32Array(blockSize), new Float32Array(blockSize)]]),
            ).toBe(true);
          }
        } finally {
          processor.destroy();
        }
        return meters.length;
      };

      // The field is load-bearing: a longer interval publishes strictly fewer
      // snapshots over the same block count, and 0 turns publication off.
      const perBlock = meterCount(blockSize);
      expect(perBlock).toBeGreaterThan(0);
      expect(meterCount(blockSize * 4)).toBeLessThan(perBlock);
      expect(meterCount(0)).toBe(0);

      for (const meterIntervalFrames of [2048.5, -1, Number.NaN, Number.POSITIVE_INFINITY]) {
        expect(() => meterCount(meterIntervalFrames)).toThrow(
          /meterIntervalFrames must be an integer of at least 0/,
        );
      }
    });

    it('refuses a scopeIntervalFrames instead of rounding it into range', () => {
      const blockSize = 256;
      const scopeCount = (scopeIntervalFrames: number): number => {
        const scopeRing = createSonareScopeRingBuffer(64, 32);
        const processor = new SonareRealtimeEngineWorkletProcessor({
          sampleRate: 48000,
          blockSize,
          channelCount: 2,
          scopeSharedBuffer: scopeRing.sharedBuffer,
          scopeRingCapacity: scopeRing.capacity,
          scopeBands: scopeRing.bands,
          scopeIntervalFrames,
        });
        try {
          processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });
          for (let block = 0; block < 12; block++) {
            const input = new Float32Array(blockSize).fill(0.25);
            expect(
              processor.process(
                [[input, input]],
                [[new Float32Array(blockSize), new Float32Array(blockSize)]],
              ),
            ).toBe(true);
          }
        } finally {
          processor.destroy();
        }
        return readSonareScopeRingBuffer(scopeRing).scopes.length;
      };

      const perBlock = scopeCount(blockSize);
      expect(perBlock).toBeGreaterThan(0);
      expect(scopeCount(blockSize * 4)).toBeLessThan(perBlock);

      for (const scopeIntervalFrames of [256.5, -1, Number.NaN]) {
        expect(() => scopeCount(scopeIntervalFrames)).toThrow(
          /scopeIntervalFrames must be an integer of at least 0/,
        );
      }
    });

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

    it('drains a large SAB command snapshot before a structural strip sync', () => {
      const commandRing = createSonareEngineCommandRingBuffer(2048);
      const posted: unknown[] = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        {
          sampleRate: 48000,
          blockSize: 128,
          channelCount: 2,
          commandSharedBuffer: commandRing.sharedBuffer,
        },
        { postMessage: (message) => posted.push(message) },
      );
      try {
        for (let index = 0; index < 1080; index += 1) {
          expect(
            pushSonareEngineCommandRingBuffer(commandRing, {
              type: SonareEngineCommandType.TransportPlay,
              sampleTime: 48000,
            }),
          ).toBe(true);
        }
        processor.receiveSync({
          type: 'syncMixer',
          lanes: [{ trackId: 7 }],
          insertBaseResets: [{ kind: 'track', trackId: 7 }],
        });
        expect(Atomics.load(commandRing.header, 1)).toBe(1080);
        expect(posted).not.toEqual(
          expect.arrayContaining([
            expect.objectContaining({
              type: SonareEngineTelemetryType.Error,
              error: SonareEngineTelemetryError.InvalidCommand,
            }),
          ]),
        );
      } finally {
        processor.destroy();
      }
    });

    it('clears requested insert bases after the strip replay without settling ramps', () => {
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize: 128, channelCount: 2 },
        { postMessage: () => undefined },
      );
      try {
        const engine = (processor as unknown as { engine: RealtimeEngine }).engine;
        const calls: string[] = [];
        const record = <K extends keyof RealtimeEngine>(name: K) => {
          const original = (engine[name] as (...args: unknown[]) => unknown).bind(engine);
          vi.spyOn(engine, name).mockImplementation(((...args: unknown[]) => {
            calls.push(String(name));
            return original(...args);
          }) as never);
        };
        record('settleInsertParameters');
        record('setTrackStripJson');
        record('clearTrackInsertParameterBases');
        record('restoreTrackStripInsertParamByName');
        processor.receiveSync({
          type: 'syncMixer',
          lanes: [{ trackId: 7 }],
          trackStrips: [
            {
              trackId: 7,
              sceneJson:
                '{"version":1,"strips":[{"id":"track-7","inserts":[{"slot":"pre","processor":"eq.parametric","params":{}}]}],"buses":[],"connections":[]}',
            },
          ],
          insertBaseResets: [{ kind: 'track', trackId: 7 }],
          insertParamOverrides: [
            { kind: 'track', trackId: 7, insertIndex: 0, paramName: 'band0.gainDb', value: 0 },
          ],
        });
        expect(calls).toEqual([
          'setTrackStripJson',
          'clearTrackInsertParameterBases',
          'restoreTrackStripInsertParamByName',
        ]);
      } finally {
        processor.destroy();
      }
    });

    it('routes the cue bus to a second output only when asked', () => {
      const blockSize = 128;
      const clipFrames = blockSize * 4;
      const seed = (processor: SonareRealtimeEngineWorkletProcessor) => {
        const engine = (
          processor as unknown as {
            engine: {
              setClips: (clips: unknown[]) => void;
              setTrackLanes: (lanes: unknown[]) => void;
              setTrackMonitorMode: (target: number, mode: string) => void;
              play: (sampleTime?: number) => void;
            };
          }
        ).engine;
        engine.setClips([
          {
            id: 1,
            trackId: 10,
            channels: [new Float32Array(clipFrames).fill(1), new Float32Array(clipFrames).fill(1)],
            startPpq: 0,
            lengthSamples: clipFrames,
          },
          {
            id: 2,
            trackId: 20,
            channels: [new Float32Array(clipFrames).fill(1), new Float32Array(clipFrames).fill(1)],
            startPpq: 0,
            lengthSamples: clipFrames,
          },
        ]);
        engine.setTrackLanes([10, { trackId: 20 }]);
        engine.setTrackMonitorMode(0, 'pfl');
        engine.play();
      };

      // Default: one output, cue folded into the program mix, exactly as before.
      const folded = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        blockSize,
        channelCount: 2,
      });
      const foldedOut = [new Float32Array(blockSize), new Float32Array(blockSize)];
      try {
        seed(folded);
        expect(folded.process([[]], [foldedOut])).toBe(true);
      } finally {
        folded.destroy();
      }

      const split = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        blockSize,
        channelCount: 2,
        cueOutput: true,
      });
      const program = [new Float32Array(blockSize), new Float32Array(blockSize)];
      const cue = [new Float32Array(blockSize), new Float32Array(blockSize)];
      try {
        seed(split);
        expect(split.process([[]], [program, cue])).toBe(true);
        // Both lanes reach the program output; only the PFL-tapped lane reaches
        // the cue. The folded run mixes the two together, so it sits higher.
        expect(program[0].at(-1)).toBeCloseTo(2, 4);
        expect(cue[0].at(-1)).toBeCloseTo(1, 4);
        expect(cue[1].at(-1)).toBeCloseTo(1, 4);
        expect(foldedOut[0].at(-1)).toBeGreaterThan(program[0].at(-1) ?? 0);
      } finally {
        split.destroy();
      }
    });

    it('runs with the cue enabled even when the host supplies one output', () => {
      const blockSize = 128;
      const processor = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        blockSize,
        channelCount: 2,
        cueOutput: true,
      });
      try {
        const out = [new Float32Array(blockSize), new Float32Array(blockSize)];
        expect(processor.process([[]], [out])).toBe(true);
      } finally {
        processor.destroy();
      }
    });

    it('fans a mono engine plane out to every host output channel', () => {
      const blockSize = 128;
      const clipFrames = blockSize * 4;
      const processor = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        blockSize,
        channelCount: 1,
      });
      try {
        const engine = (
          processor as unknown as {
            engine: {
              setClips: (clips: unknown[]) => void;
              setTrackLanes: (lanes: unknown[]) => void;
              play: (sampleTime?: number) => void;
            };
          }
        ).engine;
        engine.setClips([
          {
            id: 1,
            trackId: 10,
            channels: [new Float32Array(clipFrames).fill(1)],
            startPpq: 0,
            lengthSamples: clipFrames,
          },
        ]);
        engine.setTrackLanes([10]);
        engine.play();
        const out = [new Float32Array(blockSize), new Float32Array(blockSize)];
        expect(processor.process([[]], [out])).toBe(true);

        // A single plane must reach both host channels: sending it to output 0
        // only would play the mono program hard-panned left.
        expect(out[0].at(-1)).not.toBe(0);
        expect(Array.from(out[1])).toEqual(Array.from(out[0]));
      } finally {
        processor.destroy();
      }
    });

    it('silences a host output channel past the last engine plane', () => {
      const blockSize = 128;
      const clipFrames = blockSize * 4;
      const processor = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        blockSize,
        channelCount: 2,
      });
      try {
        const engine = (
          processor as unknown as {
            engine: {
              setClips: (clips: unknown[]) => void;
              setTrackLanes: (lanes: unknown[]) => void;
              play: (sampleTime?: number) => void;
            };
          }
        ).engine;
        engine.setClips([
          {
            id: 1,
            trackId: 10,
            channels: [new Float32Array(clipFrames).fill(1), new Float32Array(clipFrames).fill(1)],
            startPpq: 0,
            lengthSamples: clipFrames,
          },
        ]);
        engine.setTrackLanes([10]);
        engine.play();
        // The stale sentinel proves the third channel is actively zeroed rather
        // than left untouched.
        const out = [
          new Float32Array(blockSize),
          new Float32Array(blockSize),
          new Float32Array(blockSize).fill(0.5),
        ];
        expect(processor.process([[]], [out])).toBe(true);

        // Both engine planes carry audio, so a plane-0 fallback into the
        // uncovered channel would be audible rather than silent.
        expect(out[0].at(-1)).not.toBe(0);
        expect(out[1].at(-1)).not.toBe(0);
        expect(out[2].every((sample) => sample === 0)).toBe(true);
      } finally {
        processor.destroy();
      }
    });

    it('zeroes the output tail a short engine plane does not fill', () => {
      const blockSize = 128;
      const shortFrames = 64;
      const clipFrames = blockSize * 4;
      const processor = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        blockSize,
        channelCount: 2,
      });
      try {
        const internals = processor as unknown as {
          engine: {
            setClips: (clips: unknown[]) => void;
            setTrackLanes: (lanes: unknown[]) => void;
            play: (sampleTime?: number) => void;
          };
          channelBuffers: Float32Array[];
        };
        internals.engine.setClips([
          {
            id: 1,
            trackId: 10,
            channels: [new Float32Array(clipFrames).fill(1), new Float32Array(clipFrames).fill(1)],
            startPpq: 0,
            lengthSamples: clipFrames,
          },
        ]);
        internals.engine.setTrackLanes([10]);
        internals.engine.play();
        // Narrow one heap view so the plane covers less than the render quantum.
        internals.channelBuffers[1] = internals.channelBuffers[1].subarray(0, shortFrames);
        const out = [new Float32Array(blockSize), new Float32Array(blockSize).fill(0.5)];
        expect(processor.process([[]], [out])).toBe(true);

        // The narrowed view must still be the one that was rendered through; a
        // re-acquire would restore the full plane and make this vacuous.
        expect(internals.channelBuffers[1].length).toBe(shortFrames);
        expect(out[1][shortFrames - 1]).not.toBe(0);
        expect(out[1].subarray(shortFrames).every((sample) => sample === 0)).toBe(true);
      } finally {
        processor.destroy();
      }
    });

    it('replaces custom parameters from sync messages', () => {
      const processor = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        blockSize: 128,
        channelCount: 1,
      });
      try {
        processor.receiveSync({
          type: 'syncParameters',
          parameters: [
            {
              id: 7,
              name: 'gain',
              unit: 'dB',
              minValue: -60,
              maxValue: 12,
              defaultValue: 0,
              rtSafe: true,
              defaultCurve: 2,
            },
          ],
        });
        const engine = (processor as unknown as { engine: { parameterCount: () => number } })
          .engine;
        expect(engine.parameterCount()).toBe(1);

        processor.receiveSync({ type: 'syncParameters', parameters: [] });
        expect(engine.parameterCount()).toBe(0);
      } finally {
        processor.destroy();
      }
    });

    it('keeps live automation lanes when another parameter is registered', () => {
      const processor = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        blockSize: 128,
        channelCount: 1,
      });
      const parameter = (id: number) => ({
        id,
        name: `p${id}`,
        unit: '',
        minValue: 0,
        maxValue: 1,
        defaultValue: 0,
        rtSafe: true,
        defaultCurve: 1,
      });
      try {
        processor.receiveSync({ type: 'syncParameters', parameters: [parameter(7)] });
        processor.receiveSync({
          type: 'syncAutomation',
          paramId: 7,
          points: [{ ppq: 0, value: 0.25 }],
        });
        const engine = (
          processor as unknown as {
            engine: { parameterCount: () => number; automationLaneCount: () => number };
          }
        ).engine;
        const lanes = engine.automationLaneCount();
        expect(lanes).toBeGreaterThan(0);
        processor.receiveSync({
          type: 'syncParameters',
          parameters: [parameter(7), parameter(8)],
        });
        expect(engine.parameterCount()).toBe(2);
        expect(engine.automationLaneCount()).toBe(lanes);
        processor.receiveSync({ type: 'syncParameters', parameters: [] });
        expect(engine.parameterCount()).toBe(0);
      } finally {
        processor.destroy();
      }
    });

    it('applies live MIDI input sync messages', () => {
      const processor = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        blockSize: 128,
        channelCount: 1,
      });
      try {
        processor.receiveSync({ type: 'syncMidiInputSource', destinationId: 3 });
        processor.receiveSync({
          type: 'syncMidiInputNoteOn',
          group: 0,
          channel: 0,
          data0: 60,
          data1: 100,
          portTimeSamples: 128,
        });
        const engine = (processor as unknown as { engine: { midiInputPendingCount: () => number } })
          .engine;
        expect(engine.midiInputPendingCount()).toBe(1);
        processor.receiveSync({ type: 'syncClearMidiInputSource' });
      } finally {
        processor.destroy();
      }
    });

    it('applies raw MIDI input UMP sync with every word and its port timestamp', () => {
      const processor = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        blockSize: 128,
        channelCount: 1,
      });
      try {
        const engine = (
          processor as unknown as {
            engine: { pushMidiInputUmp: (words: Uint32Array, portTimeSamples: number) => void };
          }
        ).engine;
        const rawInput = vi.spyOn(engine, 'pushMidiInputUmp').mockImplementation(() => undefined);
        const words = new Uint32Array([0x41923c00, 0x12345678]);
        processor.receiveSync({ type: 'syncMidiInputSource', destinationId: 9 });
        processor.receiveSync({ type: 'syncMidiInputUmp', words, portTimeSamples: 768 });

        expect(rawInput).toHaveBeenCalledWith(Uint32Array.from([0x41923c00, 0x12345678]), 768);
        expect(rawInput.mock.calls[0]?.[0]).toEqual(Uint32Array.from([0x41923c00, 0x12345678]));
      } finally {
        processor.destroy();
      }
    });

    it('applies multiword destination UMP and preserves legacy one-word sync', () => {
      const processor = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        blockSize: 128,
        channelCount: 1,
      });
      try {
        const engine = (
          processor as unknown as {
            engine: {
              pushMidiUmp: (
                destinationId: number,
                words: Uint32Array | number[],
                renderFrame: number,
              ) => void;
            };
          }
        ).engine;
        const destinationUmp = vi.spyOn(engine, 'pushMidiUmp').mockImplementation(() => undefined);
        processor.receiveSync({
          type: 'syncMidiUmp',
          destinationId: 4,
          words: Uint32Array.from([0x41923c00, 0x12345678]),
          renderFrame: 256,
        });
        processor.receiveSync({
          type: 'syncMidiUmp',
          destinationId: 4,
          word0: 0x20903c64,
          renderFrame: 512,
        });

        expect(destinationUmp).toHaveBeenNthCalledWith(
          1,
          4,
          Uint32Array.from([0x41923c00, 0x12345678]),
          256,
        );
        expect(destinationUmp).toHaveBeenNthCalledWith(2, 4, [0x20903c64], 512);
      } finally {
        processor.destroy();
      }
    });

    it('contains an invalid SAB command and continues rendering later blocks', () => {
      const blockSize = 128;
      const commandRing = createSonareEngineCommandRingBuffer(8);
      const telemetryRing = createSonareEngineTelemetryRingBuffer(8);
      const processor = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        blockSize,
        channelCount: 1,
        commandSharedBuffer: commandRing.sharedBuffer,
        telemetrySharedBuffer: telemetryRing.sharedBuffer,
      });
      try {
        // Bypass the main-thread producer validation to emulate a malformed
        // SAB record from an older or hostile producer.
        expect(
          pushSonareEngineCommandRingBuffer(commandRing, {
            type: SonareEngineCommandType.TransportSeekPpq,
            sampleTime: -1,
            argFloat: Number.NaN,
          }),
        ).toBe(true);
        const first = new Float32Array(blockSize);
        expect(processor.process([[]], [[first]])).toBe(true);
        expect(Array.from(first).every(Number.isFinite)).toBe(true);

        const telemetry = readSonareEngineTelemetryRingBuffer(telemetryRing);
        expect(telemetry.telemetry).toContainEqual(
          expect.objectContaining({
            type: SonareEngineTelemetryType.Error,
            error: SonareEngineTelemetryError.InvalidCommand,
            value: SonareEngineCommandType.TransportSeekPpq,
          }),
        );

        const later = new Float32Array(blockSize);
        expect(processor.process([[]], [[later]])).toBe(true);
        expect(Array.from(later).every(Number.isFinite)).toBe(true);
      } finally {
        processor.destroy();
      }
    });

    it('dispatches the lane monitor-mode command with its ABI fields', () => {
      const processor = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        blockSize: 128,
        channelCount: 1,
      });
      try {
        const engine = (
          processor as unknown as {
            engine: {
              setTrackMonitorMode: (laneIndex: number, mode: number, frame: number) => void;
            };
          }
        ).engine;
        const setTrackMonitorMode = vi.spyOn(engine, 'setTrackMonitorMode');
        processor.receiveCommand({
          type: SonareEngineCommandType.SetTrackMonitorMode,
          targetId: 2,
          sampleTime: 96,
          argInt: 2,
        });
        expect(setTrackMonitorMode).toHaveBeenCalledWith(2, 2, 96);
        for (const targetId of [undefined, -1, 0x1_0000_0000]) {
          processor.receiveCommand({
            type: SonareEngineCommandType.SetTrackMonitorMode,
            targetId,
            sampleTime: 96,
            argInt: 1,
          });
        }
        expect(setTrackMonitorMode).toHaveBeenCalledTimes(1);
      } finally {
        processor.destroy();
      }
    });

    it('reports a rejected sync without stopping subsequent processing', () => {
      const posted: unknown[] = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize: 128, channelCount: 1 },
        { postMessage: (message) => posted.push(message) },
      );
      try {
        expect(() =>
          processor.receiveSync({
            type: 'syncTempo',
            bpm: Number.NaN,
            timeSignature: { numerator: 4, denominator: 4 },
          }),
        ).not.toThrow();
        expect(posted.at(-1)).toMatchObject({ type: 'syncError', syncType: 'syncTempo' });
        expect(processor.process([[]], [[new Float32Array(128)]])).toBe(true);
      } finally {
        processor.destroy();
      }
    });

    it('releases its engine when it receives a destroy sync message', () => {
      const processor = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        blockSize: 128,
        channelCount: 1,
      });
      processor.receiveSync({ type: 'destroy' });
      expect(processor.process([[]], [[new Float32Array(128)]])).toBe(false);
      // Idempotent after the sync has already released native state.
      processor.destroy();
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

    it('reports native clip-page request queue overflow once without a request batch', () => {
      const posted: unknown[] = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize: 128, channelCount: 1 },
        { postMessage: (message) => posted.push(message) },
      );
      try {
        const engine = (
          processor as unknown as {
            engine: { clipPageRequestOverflowCount: () => number };
          }
        ).engine;
        engine.clipPageRequestOverflowCount = () => 3;

        expect(processor.process([[]], [[new Float32Array(128)]])).toBe(true);
        expect(posted).toContainEqual({ type: 'clipPageRequest', requests: [], dropped: 3 });

        posted.length = 0;
        expect(processor.process([[]], [[new Float32Array(128)]])).toBe(true);
        expect(posted).not.toContainEqual({ type: 'clipPageRequest', requests: [], dropped: 3 });
      } finally {
        processor.destroy();
      }
    });

    it('keeps the clip bus silent while the transport is stopped', () => {
      const blockSize = 128;
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 2 },
        { postMessage: () => undefined },
      );
      try {
        const left = new Float32Array(blockSize).fill(0.5);
        const right = new Float32Array(blockSize).fill(-0.5);
        processor.receiveSync({
          type: 'syncClips',
          clips: [{ id: 1, channels: [left, right], startPpq: 0 }],
        });

        const outL = new Float32Array(blockSize);
        const outR = new Float32Array(blockSize);
        // Stopped: the playhead is frozen, so the clip must not be rendered —
        // replaying the frozen window every block would emit a sustained buzz.
        expect(processor.process([[]], [[outL, outR]])).toBe(true);
        expect(Math.max(...outL.map(Math.abs), ...outR.map(Math.abs))).toBe(0);

        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });
        expect(processor.process([[]], [[outL, outR]])).toBe(true);
        expect(Math.max(...outL.map(Math.abs))).toBeCloseTo(0.5, 5);
      } finally {
        processor.destroy();
      }
    });

    it('renders clip delta sync equivalently to full clip sync', () => {
      const blockSize = 128;
      const fullProcessor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 1 },
        { postMessage: () => undefined },
      );
      const deltaProcessor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 1 },
        { postMessage: () => undefined },
      );
      try {
        const source = new Float32Array(blockSize * 2).fill(0.5);
        const removed = new Float32Array(blockSize * 2).fill(1);
        const clip = { id: 1, trackId: 10, channels: [source], startPpq: 0 };
        fullProcessor.receiveSync({ type: 'syncMixer', lanes: [{ trackId: 10 }] });
        fullProcessor.receiveSync({ type: 'syncClips', clips: [clip] });
        fullProcessor.receiveCommand({
          type: SonareEngineCommandType.TransportPlay,
          sampleTime: -1,
        });

        deltaProcessor.receiveSync({ type: 'syncMixer', lanes: [{ trackId: 10 }] });
        deltaProcessor.receiveSync({
          type: 'syncClipsDelta',
          upserts: [{ id: 99, trackId: 10, channels: [removed], startPpq: 0 }],
          removeIds: [],
        });
        deltaProcessor.receiveSync({
          type: 'syncClipsDelta',
          upserts: [clip],
          removeIds: [99],
        });
        deltaProcessor.receiveCommand({
          type: SonareEngineCommandType.TransportPlay,
          sampleTime: -1,
        });

        const fullOut = new Float32Array(blockSize);
        const deltaOut = new Float32Array(blockSize);
        expect(fullProcessor.process([[]], [[fullOut]])).toBe(true);
        expect(deltaProcessor.process([[]], [[deltaOut]])).toBe(true);
        expect(Array.from(deltaOut)).toEqual(Array.from(fullOut));
        expect(deltaOut[0]).toBeCloseTo(0.5, 4);
      } finally {
        fullProcessor.destroy();
        deltaProcessor.destroy();
      }
    });

    it('adopts pre-baked PCM pages before scheduling a long clip', () => {
      const blockSize = 128;
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 1 },
        { postMessage: () => undefined },
      );
      try {
        processor.receiveSync({ type: 'syncMixer', lanes: [{ trackId: 10 }] });
        processor.receiveSync({
          type: 'syncClipPageProvider',
          clipId: 88,
          clip: { id: 88, trackId: 10, startPpq: 0, lengthSamples: 256, warpMode: 'off' },
          numChannels: 1,
          numSamples: 256,
          pageFrames: 128,
        });
        processor.receiveSync({
          type: 'syncClipPage',
          clipId: 88,
          pageIndex: 0,
          channels: [new Float32Array(128).fill(0.25)],
        });
        processor.receiveSync({
          type: 'syncClipPage',
          clipId: 88,
          pageIndex: 1,
          channels: [new Float32Array(128).fill(0.5)],
        });
        processor.receiveSync({ type: 'syncClipPageCommit', clipId: 88 });
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });
        const output = new Float32Array(blockSize);
        expect(processor.process([[]], [[output]])).toBe(true);
        expect(output[0]).toBeCloseTo(0.25, 4);
      } finally {
        processor.destroy();
      }
    });

    it('reports a missing worklet clip page as one bounded pull request', () => {
      const blockSize = 4;
      const posted: unknown[] = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 1 },
        { postMessage: (message) => posted.push(message) },
      );
      try {
        processor.receiveSync({ type: 'syncMixer', lanes: [{ trackId: 10 }] });
        // The OPFS path creates and primes the provider before it has the
        // finished clip schedule, then commits the schedule independently.
        processor.receiveSync({
          type: 'syncClipPageProvider',
          clipId: 89,
          numChannels: 1,
          numSamples: 8,
          pageFrames: 4,
        });
        processor.receiveSync({
          type: 'syncClipPage',
          clipId: 89,
          pageIndex: 0,
          channels: [new Float32Array(4).fill(0.25)],
        });
        processor.receiveSync({
          type: 'syncClipPageCommit',
          clipId: 89,
          clip: { id: 89, trackId: 10, startPpq: 0, lengthSamples: 8, warpMode: 'off' },
        });
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });

        const first = new Float32Array(blockSize);
        expect(processor.process([[]], [[first]])).toBe(true);
        expect(Array.from(first)).toEqual([0.25, 0.25, 0.25, 0.25]);

        const missing = new Float32Array(blockSize);
        expect(processor.process([[]], [[missing]])).toBe(true);
        expect(Array.from(missing)).toEqual([0, 0, 0, 0]);
        expect(posted).toContainEqual({
          type: 'clipPageRequest',
          requests: [{ clipId: 89, pageIndex: 1 }],
        });

        processor.receiveSync({
          type: 'syncClipPage',
          clipId: 89,
          pageIndex: 1,
          channels: [new Float32Array(4).fill(0.5)],
        });
        processor.receiveCommand({
          type: SonareEngineCommandType.TransportSeekSample,
          sampleTime: -1,
          argInt: 0,
        });
        const replayedFirst = new Float32Array(blockSize);
        const replayedSecond = new Float32Array(blockSize);
        expect(processor.process([[]], [[replayedFirst]])).toBe(true);
        expect(processor.process([[]], [[replayedSecond]])).toBe(true);
        expect(Array.from(replayedFirst)).toEqual([0.25, 0.25, 0.25, 0.25]);
        expect(Array.from(replayedSecond)).toEqual([0.5, 0.5, 0.5, 0.5]);
      } finally {
        processor.destroy();
      }
    });

    it('posts clip page requests only when the missing set changes, without the allocating pop', () => {
      const blockSize = 4;
      const posted: Array<{ type: string; requests: unknown[] }> = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 1 },
        {
          postMessage: (message) => {
            const clone = structuredClone(message) as { type: string; requests: unknown[] };
            if (clone.type === 'clipPageRequest') {
              posted.push(clone);
            }
          },
        },
      );
      try {
        const engine = (
          processor as unknown as {
            engine: {
              popClipPageRequest: () => unknown;
              popClipPageRequestToScratch: () => boolean;
            };
          }
        ).engine;
        const allocatingPop = vi.spyOn(engine, 'popClipPageRequest');
        const scratchPop = vi.spyOn(engine, 'popClipPageRequestToScratch');
        processor.receiveSync({ type: 'syncMixer', lanes: [{ trackId: 10 }] });
        processor.receiveSync({
          type: 'syncClipPageProvider',
          clipId: 91,
          numChannels: 1,
          numSamples: 8,
          pageFrames: 4,
        });
        processor.receiveSync({
          type: 'syncClipPage',
          clipId: 91,
          pageIndex: 0,
          channels: [new Float32Array(4).fill(0.25)],
        });
        processor.receiveSync({
          type: 'syncClipPageCommit',
          clipId: 91,
          clip: { id: 91, trackId: 10, startPpq: 0, lengthSamples: 8, warpMode: 'off' },
        });
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });

        // Page 1 stays missing: every quantum re-reports it natively, but only
        // the first report is posted.
        for (let i = 0; i < 6; ++i) {
          expect(processor.process([[]], [[new Float32Array(blockSize)]])).toBe(true);
        }
        expect(posted).toEqual([
          { type: 'clipPageRequest', requests: [{ clipId: 91, pageIndex: 1 }] },
        ]);
        expect(scratchPop.mock.calls.length).toBeGreaterThan(6);
        expect(allocatingPop).not.toHaveBeenCalled();

        // Once the page lands the set is empty again; evicting it and replaying
        // the same miss is a new change and is posted again.
        processor.receiveSync({
          type: 'syncClipPage',
          clipId: 91,
          pageIndex: 1,
          channels: [new Float32Array(4).fill(0.5)],
        });
        processor.receiveCommand({
          type: SonareEngineCommandType.TransportSeekSample,
          sampleTime: -1,
          argInt: 0,
        });
        for (let i = 0; i < 3; ++i) {
          processor.process([[]], [[new Float32Array(blockSize)]]);
        }
        expect(posted).toHaveLength(1);
        processor.receiveSync({ type: 'syncClipPageClear', clipId: 91, pageIndex: 1 });
        processor.receiveCommand({
          type: SonareEngineCommandType.TransportSeekSample,
          sampleTime: -1,
          argInt: 0,
        });
        for (let i = 0; i < 6; ++i) {
          processor.process([[]], [[new Float32Array(blockSize)]]);
        }
        expect(posted).toHaveLength(2);
        expect(posted[1]).toEqual({
          type: 'clipPageRequest',
          requests: [{ clipId: 91, pageIndex: 1 }],
        });
      } finally {
        processor.destroy();
      }
    });

    it('re-posts a stable missing set once per repost interval and posts a changed set immediately', () => {
      const blockSize = 128;
      const pageFrames = 1 << 20;
      const sampleRate = 48000;
      const intervalQuanta = Math.ceil((sampleRate * 0.25) / blockSize);
      let quantum = 0;
      const postedAt: number[] = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate, blockSize, channelCount: 1 },
        {
          postMessage: (message) => {
            if ((message as { type?: string }).type === 'clipPageRequest') {
              postedAt.push(quantum);
            }
          },
        },
      );
      const run = (quanta: number) => {
        for (let i = 0; i < quanta; ++i, ++quantum) {
          processor.process([[]], [[new Float32Array(blockSize)]]);
        }
      };
      try {
        processor.receiveSync({ type: 'syncMixer', lanes: [{ trackId: 10 }] });
        processor.receiveSync({
          type: 'syncClipPageProvider',
          clipId: 92,
          numChannels: 1,
          numSamples: pageFrames * 2,
          pageFrames,
        });
        processor.receiveSync({
          type: 'syncClipPageCommit',
          clipId: 92,
          clip: {
            id: 92,
            trackId: 10,
            startPpq: 0,
            lengthSamples: pageFrames * 2,
            warpMode: 'off',
          },
        });
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });

        run(intervalQuanta * 3 + 5);
        expect(postedAt).toEqual([0, intervalQuanta, intervalQuanta * 2, intervalQuanta * 3]);

        // Page lands, then is evicted and missed again: a changed set posts at once.
        processor.receiveSync({
          type: 'syncClipPage',
          clipId: 92,
          pageIndex: 0,
          channels: [new Float32Array(pageFrames).fill(0.5)],
        });
        processor.receiveCommand({
          type: SonareEngineCommandType.TransportSeekSample,
          sampleTime: -1,
          argInt: 0,
        });
        run(2);
        expect(postedAt).toHaveLength(4);
        processor.receiveSync({ type: 'syncClipPageClear', clipId: 92, pageIndex: 0 });
        processor.receiveCommand({
          type: SonareEngineCommandType.TransportSeekSample,
          sampleTime: -1,
          argInt: 0,
        });
        const before = quantum;
        run(3);
        expect(postedAt).toHaveLength(5);
        expect(postedAt[4]).toBeLessThanOrEqual(before + 2);
      } finally {
        processor.destroy();
      }
    });

    it('reuses one preallocated request payload across clip page posts', () => {
      const blockSize = 4;
      const messages: object[] = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 1 },
        { postMessage: (message) => messages.push(message as object) },
      );
      try {
        const engine = (
          processor as unknown as { engine: { clipPageRequestOverflowCount: () => number } }
        ).engine;
        let overflow = 0;
        engine.clipPageRequestOverflowCount = () => overflow;
        processor.process([[]], [[new Float32Array(blockSize)]]);
        overflow = 1;
        processor.process([[]], [[new Float32Array(blockSize)]]);
        overflow = 2;
        processor.process([[]], [[new Float32Array(blockSize)]]);
        const requestMessages = messages.filter(
          (m) => (m as { type?: string }).type === 'clipPageRequest',
        );
        expect(requestMessages).toHaveLength(2);
        expect(requestMessages[0]).toBe(requestMessages[1]);
        expect((requestMessages[0] as { requests: unknown[] }).requests).toBe(
          (requestMessages[1] as { requests: unknown[] }).requests,
        );
      } finally {
        processor.destroy();
      }
    });

    it('publishes a missing worklet clip page through the SAB ring without postMessage', () => {
      const blockSize = 4;
      const posted: unknown[] = [];
      const clipPageRequestRing = createSonareClipPageRequestRingBuffer(4);
      const processor = new SonareRealtimeEngineWorkletProcessor(
        {
          sampleRate: 48000,
          blockSize,
          channelCount: 1,
          clipPageRequestSharedBuffer: clipPageRequestRing.sharedBuffer,
        },
        { postMessage: (message) => posted.push(message) },
      );
      try {
        processor.receiveSync({ type: 'syncMixer', lanes: [{ trackId: 10 }] });
        processor.receiveSync({
          type: 'syncClipPageProvider',
          clipId: 90,
          numChannels: 1,
          numSamples: 8,
          pageFrames: 4,
        });
        processor.receiveSync({
          type: 'syncClipPage',
          clipId: 90,
          pageIndex: 0,
          channels: [new Float32Array(4).fill(0.25)],
        });
        processor.receiveSync({
          type: 'syncClipPageCommit',
          clipId: 90,
          clip: { id: 90, trackId: 10, startPpq: 0, lengthSamples: 8, warpMode: 'off' },
        });
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });

        // The first block only reads the resident page 0, but the engine looks
        // ahead of the playhead, so page 1 is requested before the block that
        // needs it rather than after that block has already read silence.
        expect(processor.process([[]], [[new Float32Array(blockSize)]])).toBe(true);
        expect(readSonareClipPageRequestRingBuffer(clipPageRequestRing)).toEqual({
          requests: [{ clipId: 90, pageIndex: 1 }],
          dropped: 0,
        });
        // The host has not delivered the page yet, so the block that actually
        // reads it misses and re-requests. A host that keeps the newest request
        // per clip coalesces the repeat.
        expect(processor.process([[]], [[new Float32Array(blockSize)]])).toBe(true);
        expect(readSonareClipPageRequestRingBuffer(clipPageRequestRing)).toEqual({
          requests: [{ clipId: 90, pageIndex: 1 }],
          dropped: 0,
        });
        expect(posted).not.toContainEqual({
          type: 'clipPageRequest',
          requests: [{ clipId: 90, pageIndex: 1 }],
        });
      } finally {
        processor.destroy();
      }
    });

    it('applies mixer lane sync and lane parameter commands', () => {
      const blockSize = 128;
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 1 },
        { postMessage: () => undefined },
      );
      try {
        const source = new Float32Array(blockSize * 4).fill(1);
        processor.receiveSync({ type: 'syncMixer', lanes: [{ trackId: 10 }] });
        processor.receiveSync({
          type: 'syncClips',
          clips: [{ id: 1, trackId: 10, channels: [source], startPpq: 0 }],
        });
        const liveEngine = (processor as unknown as { engine: RealtimeEngine }).engine;
        processor.receiveCommand({
          type: SonareEngineCommandType.SetParam,
          targetId: liveEngine.resolveTrackLaneAutomationId(10, 'faderDb'),
          argFloat: -12,
          sampleTime: -1,
        });
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });

        const out = new Float32Array(blockSize);
        expect(processor.process([[]], [[out]])).toBe(true);
        expect(out[blockSize - 1]).toBeGreaterThan(0.2);
        expect(out[blockSize - 1]).toBeLessThan(0.9);
      } finally {
        processor.destroy();
      }
    });

    it('applies scheduled MIDI clip resync to the live embind engine', () => {
      const blockSize = 128;
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 2 },
        { postMessage: () => undefined },
      );
      try {
        processor.receiveSync({
          type: 'syncBuiltinInstrument',
          destinationId: 4,
          config: { gain: 0.5 },
        });
        processor.receiveSync({ type: 'syncMidiClips', clips: [] });
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });

        const silentL = new Float32Array(blockSize);
        const silentR = new Float32Array(blockSize);
        expect(processor.process([[]], [[silentL, silentR]])).toBe(true);
        expect(Math.max(...silentL.map(Math.abs), ...silentR.map(Math.abs))).toBe(0);

        processor.receiveCommand({ type: SonareEngineCommandType.TransportStop, sampleTime: -1 });
        processor.receiveCommand({
          type: SonareEngineCommandType.TransportSeekSample,
          sampleTime: -1,
          argInt: 0,
        });
        processor.receiveSync({
          type: 'syncMidiClips',
          clips: [
            {
              id: 1,
              trackId: 4,
              destinationId: 4,
              lengthSamples: 8192,
              events: [
                { renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 },
                { renderFrame: 4096, word0: midi1Word(0x8, 0, 60, 0), wordCount: 1 },
              ],
            },
          ],
        });
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });

        const outL = new Float32Array(blockSize);
        const outR = new Float32Array(blockSize);
        expect(processor.process([[]], [[outL, outR]])).toBe(true);
        expect(Math.max(...outL.map(Math.abs), ...outR.map(Math.abs))).toBeGreaterThan(0);
      } finally {
        processor.destroy();
      }
    });

    it('renders live MIDI note sync on the next processed block', () => {
      const blockSize = 128;
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 2 },
        { postMessage: () => undefined },
      );
      try {
        processor.receiveSync({
          type: 'syncBuiltinInstrument',
          destinationId: 8,
          config: { gain: 0.5 },
        });
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });
        processor.receiveSync({
          type: 'syncMidiNoteOn',
          destinationId: 8,
          group: 0,
          channel: 0,
          note: 64,
          velocity: 100,
          renderFrame: -1,
        });

        const outL = new Float32Array(blockSize);
        const outR = new Float32Array(blockSize);
        expect(processor.process([[]], [[outL, outR]])).toBe(true);
        expect(Math.max(...outL.map(Math.abs), ...outR.map(Math.abs))).toBeGreaterThan(0);
      } finally {
        processor.destroy();
      }
    });

    it('applies mixer strip specs from syncMixer', () => {
      const blockSize = 128;
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 1 },
        { postMessage: () => undefined },
      );
      try {
        const source = new Float32Array(blockSize * 4).fill(1);
        processor.receiveSync({
          type: 'syncMixer',
          lanes: [{ trackId: 10 }],
          trackStrips: [
            {
              trackId: 10,
              sceneJson:
                '{"version":1,"strips":[{"id":"track-10","faderDb":-12,"panLaw":3}],"buses":[],"connections":[]}',
            },
          ],
          masterStripJson:
            '{"version":1,"strips":[{"id":"master","faderDb":-6,"panLaw":3}],"buses":[],"connections":[]}',
        });
        processor.receiveSync({
          type: 'syncTrackStripEqBand',
          trackId: 10,
          bandIndex: 0,
          bandJson: '{"type":"Peak","frequencyHz":1000,"gainDb":3}',
        });
        processor.receiveSync({
          type: 'syncMasterStripEqBand',
          bandIndex: 0,
          bandJson: '{"type":"Peak","frequencyHz":1000,"gainDb":1}',
        });
        processor.receiveSync({
          type: 'syncClips',
          clips: [{ id: 1, trackId: 10, channels: [source], startPpq: 0 }],
        });
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });

        const out = new Float32Array(blockSize);
        expect(processor.process([[]], [[out]])).toBe(true);
        expect(out[blockSize - 1]).toBeGreaterThan(0.05);
        expect(out[blockSize - 1]).toBeLessThan(0.75);
      } finally {
        processor.destroy();
      }
    });

    it('applies worklet strip insert bypass sync messages', () => {
      const blockSize = 128;
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 1 },
        { postMessage: () => undefined },
      );
      try {
        const source = new Float32Array(blockSize * 4).fill(1);
        const insertParams =
          '{\\"band0.type\\":1,\\"band0.frequencyHz\\":1000,\\"band0.gainDb\\":6,\\"band0.enabled\\":1}';
        processor.receiveSync({
          type: 'syncMixer',
          lanes: [{ trackId: 10 }],
          trackStrips: [
            {
              trackId: 10,
              sceneJson: `{"version":1,"strips":[{"id":"track-10","inserts":[{"slot":"pre","processor":"eq.parametric","params":"${insertParams}"}]}],"buses":[],"connections":[]}`,
            },
          ],
          masterStripJson: `{"version":1,"strips":[{"id":"master","inserts":[{"slot":"pre","processor":"eq.parametric","params":"${insertParams}"}]}],"buses":[],"connections":[]}`,
        });
        processor.receiveSync({
          type: 'syncTrackStripInsertBypassed',
          trackId: 10,
          insertIndex: 0,
          bypassed: true,
          resetOnBypass: true,
        });
        processor.receiveSync({
          type: 'syncMasterStripInsertBypassed',
          insertIndex: 0,
          bypassed: true,
          resetOnBypass: true,
        });
        processor.receiveSync({
          type: 'syncClips',
          clips: [{ id: 1, trackId: 10, channels: [source], startPpq: 0 }],
        });
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });

        const out = new Float32Array(blockSize);
        expect(processor.process([[]], [[out]])).toBe(true);
        expect(Number.isFinite(out[blockSize - 1])).toBe(true);
      } finally {
        processor.destroy();
      }
    });

    it('applies bus/send mixer sync and preserves same-frame meter targets', () => {
      const blockSize = 128;
      const meters: unknown[] = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 1, meterIntervalFrames: blockSize },
        { onMeter: (meter) => meters.push(meter), postMessage: () => undefined },
      );
      try {
        const source = new Float32Array(blockSize * 4).fill(1);
        processor.receiveSync({
          type: 'syncMixer',
          buses: [{ busId: 200, gainDb: 0 }],
          lanes: [{ trackId: 10, sends: [{ busId: 200, levelDb: 0, enabled: true }] }],
          busStrips: [
            {
              busId: 200,
              sceneJson:
                '{"version":1,"strips":[],"buses":[{"id":"200","inserts":[{"slot":"pre","processor":"eq.parametric","params":"{\\"band0.type\\":1,\\"band0.frequencyHz\\":1000,\\"band0.gainDb\\":0,\\"band0.enabled\\":1}"}]}],"connections":[]}',
            },
          ],
        });
        processor.receiveSync({
          type: 'syncBusStripInsertParamByName',
          busId: 200,
          insertIndex: 0,
          paramName: 'band0.gainDb',
          value: 3,
        });
        // A bus insert is bypassable live, exactly like a track or master one.
        // Re-posting the bus scene JSON is not a substitute: it rebuilds the
        // chain and drops the insert's internal state.
        processor.receiveSync({
          type: 'syncBusStripInsertBypassed',
          busId: 200,
          insertIndex: 0,
          bypassed: true,
          resetOnBypass: false,
        });
        processor.receiveSync({
          type: 'syncClips',
          clips: [{ id: 1, trackId: 10, channels: [source], startPpq: 0 }],
        });
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });

        const out = new Float32Array(blockSize);
        expect(processor.process([[]], [[out]])).toBe(true);
        const targetIds = meters
          .map((meter) => (meter as { targetId?: number }).targetId)
          .filter((targetId): targetId is number => typeof targetId === 'number');
        expect(targetIds).toEqual(expect.arrayContaining([0, 1, 33]));
        expect(new Set(targetIds).size).toBe(targetIds.length);
      } finally {
        processor.destroy();
      }
    });

    it('applies bus strip pan and EQ band sync messages', () => {
      const blockSize = 128;
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 2 },
        { postMessage: () => undefined },
      );
      const rms = (data: Float32Array): number =>
        Math.sqrt(data.reduce((sum, value) => sum + value * value, 0) / data.length);
      try {
        const frames = blockSize * 64;
        const source = new Float32Array(frames);
        for (let i = 0; i < frames; i += 1) {
          source[i] = 0.25 * Math.sin((2 * Math.PI * 1000 * i) / 48000);
        }
        processor.receiveSync({
          type: 'syncMixer',
          buses: [{ busId: 200, gainDb: 0 }],
          lanes: [{ trackId: 10, outputBusId: 200 }],
        });
        processor.receiveSync({ type: 'syncBusStripPanMode', busId: 200, panMode: 0 });
        processor.receiveSync({ type: 'syncBusStripPanLaw', busId: 200, panLaw: 2 });
        processor.receiveSync({ type: 'syncBusStripPan', busId: 200, pan: 1 });
        processor.receiveSync({
          type: 'syncBusStripEqBand',
          busId: 200,
          bandIndex: 0,
          bandJson: '{"type":"Peak","frequencyHz":1000,"gainDb":12,"q":1,"enabled":true}',
        });
        processor.receiveSync({
          type: 'syncClips',
          clips: [{ id: 1, trackId: 10, channels: [source, source], startPpq: 0 }],
        });
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });
        const out = [new Float32Array(blockSize), new Float32Array(blockSize)];
        for (let block = 0; block < 12; block += 1) {
          expect(processor.process([[]], [out])).toBe(true);
        }
        // Panned hard right, and boosted well above the ~0.177 dry RMS.
        expect(rms(out[0])).toBeLessThan(rms(out[1]) * 0.01);
        expect(rms(out[1])).toBeGreaterThan(0.4);

        processor.receiveSync({
          type: 'syncBusStripDualPan',
          busId: 200,
          leftPan: -1,
          rightPan: -1,
        });
        processor.receiveSync({ type: 'syncBusStripPanMode', busId: 200, panMode: 2 });
        for (let block = 0; block < 12; block += 1) {
          expect(processor.process([[]], [out])).toBe(true);
        }
        expect(rms(out[1])).toBeLessThan(rms(out[0]) * 0.01);
        expect(rms(out[0])).toBeGreaterThan(0.4);
      } finally {
        processor.destroy();
      }
    });

    it('applies capture sync to the live embind engine', () => {
      const blockSize = 128;
      const meters: unknown[] = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 2, meterIntervalFrames: blockSize },
        { onMeter: (meter) => meters.push(meter), postMessage: () => undefined },
      );
      try {
        processor.receiveSync({
          type: 'syncCapture',
          bufferFrames: blockSize,
          channels: 2,
          source: 'input',
          recordOffsetSamples: -12,
          inputMonitor: { enabled: true, gain: 0.5 },
        });
        processor.receiveCommand({
          type: SonareEngineCommandType.ArmRecord,
          sampleTime: -1,
          argInt: 1,
        });

        const inL = new Float32Array(blockSize).fill(0.25);
        const inR = new Float32Array(blockSize).fill(-0.25);
        const outL = new Float32Array(blockSize);
        const outR = new Float32Array(blockSize);
        expect(processor.process([[inL, inR]], [[outL, outR]])).toBe(true);
        expect(outL[0]).toBeCloseTo(0.125, 4);
        expect(outR[0]).toBeCloseTo(-0.125, 4);

        // The processor keeps its engine private and exposes no capture reader.
        const { engine } = processor as unknown as {
          engine: Pick<RealtimeEngine, 'captureStatus' | 'capturedAudio'>;
        };
        const status = engine.captureStatus();
        expect(status.capturedFrames).toBe(blockSize);
        expect(status.source).toBe('input');
        expect(status.recordOffsetSamples).toBe(-12);
        const captured = engine.capturedAudio();
        expect(captured[0][0]).toBeCloseTo(0.25, 4);
        expect(captured[1][0]).toBeCloseTo(-0.25, 4);
        expect(meters.some((meter) => (meter as { targetId?: number }).targetId === 0xffff)).toBe(
          true,
        );
      } finally {
        processor.destroy();
      }
    });

    it('responds to capture status, read, and reset requests', () => {
      const blockSize = 128;
      const posted: unknown[] = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 1 },
        { postMessage: (message) => posted.push(message) },
      );
      try {
        processor.receiveSync({
          type: 'syncCapture',
          bufferFrames: blockSize,
          channels: 1,
          source: 'input',
          recordOffsetSamples: 0,
          inputMonitor: { enabled: false, gain: 1 },
        });
        processor.receiveCommand({
          type: SonareEngineCommandType.ArmRecord,
          sampleTime: -1,
          argInt: 1,
        });
        expect(
          processor.process(
            [[new Float32Array(blockSize).fill(0.5)]],
            [[new Float32Array(blockSize)]],
          ),
        ).toBe(true);

        processor.receiveCaptureRequest({ type: 'captureRequest', requestId: 1, op: 'status' });
        expect(posted.at(-1)).toMatchObject({
          type: 'captureResponse',
          requestId: 1,
          ok: true,
          status: { capturedFrames: blockSize, source: 'input' },
        });

        processor.receiveCaptureRequest({ type: 'captureRequest', requestId: 2, op: 'read' });
        const read = posted.at(-1) as { channels?: Float32Array[] };
        expect(read.channels?.[0][0]).toBeCloseTo(0.5, 4);

        processor.receiveCaptureRequest({ type: 'captureRequest', requestId: 3, op: 'reset' });
        expect(posted.at(-1)).toMatchObject({ type: 'captureResponse', requestId: 3, ok: true });
        processor.receiveCaptureRequest({ type: 'captureRequest', requestId: 4, op: 'status' });
        expect(posted.at(-1)).toMatchObject({
          type: 'captureResponse',
          requestId: 4,
          ok: true,
          status: { capturedFrames: 0 },
        });
      } finally {
        processor.destroy();
      }
    });

    it('applies tempo sync and responds to transport state requests', () => {
      const blockSize = 128;
      const posted: unknown[] = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 1 },
        { postMessage: (message) => posted.push(message) },
      );
      try {
        processor.receiveSync({
          type: 'syncTempo',
          bpm: 90,
          timeSignature: { numerator: 7, denominator: 8 },
        });
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });
        expect(processor.process([[]], [[new Float32Array(blockSize)]])).toBe(true);

        processor.receiveTransportRequest({ type: 'transportRequest', requestId: 11, op: 'state' });
        expect(posted.at(-1)).toMatchObject({
          type: 'transportResponse',
          requestId: 11,
          ok: true,
          state: {
            playing: true,
            bpm: 90,
            timeSignature: { numerator: 7, denominator: 8 },
          },
        });
      } finally {
        processor.destroy();
      }
    });

    it('preserves a full block of future commands while restoring insert values', () => {
      const blockSize = 128;
      const posted: unknown[] = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 1 },
        { postMessage: (message) => posted.push(message) },
      );
      try {
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: 64 });
        for (let index = 1; index < 64; index++) {
          processor.receiveCommand({
            type: SonareEngineCommandType.TransportSeekSample,
            sampleTime: 64,
            argInt: 0,
          });
        }
        const insertParams =
          '{\\"band0.type\\":1,\\"band0.frequencyHz\\":1000,\\"band0.gainDb\\":0,\\"band0.enabled\\":1}';
        processor.receiveSync({
          type: 'syncMixer',
          lanes: [],
          masterStripJson: `{"version":1,"strips":[{"id":"master","inserts":[{"slot":"pre","processor":"eq.parametric","params":"${insertParams}"}]}],"buses":[],"connections":[]}`,
          insertParamOverrides: [
            { kind: 'master', insertIndex: 0, paramName: 'band0.gainDb', value: 6 },
          ],
        });
        expect(processor.process([[]], [[new Float32Array(blockSize)]])).toBe(true);
        processor.receiveTransportRequest({ type: 'transportRequest', requestId: 42, op: 'state' });
        expect(posted.at(-1)).toMatchObject({
          type: 'transportResponse',
          requestId: 42,
          ok: true,
          state: { playing: true },
        });
      } finally {
        processor.destroy();
      }
    });

    it('keeps an overflowed future command queued for the next audio block', () => {
      const blockSize = 128;
      const posted: unknown[] = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 1 },
        { postMessage: (message) => posted.push(message) },
      );
      try {
        for (let index = 0; index < 64; index++) {
          processor.receiveCommand({
            type: SonareEngineCommandType.TransportSeekSample,
            sampleTime: 64,
            argInt: 0,
          });
        }
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: 64 });
        processor.receiveSync({ type: 'syncMixer', lanes: [] });
        for (let index = 0; index < 2; index++) {
          expect(processor.process([[]], [[new Float32Array(blockSize)]])).toBe(true);
        }
        processor.receiveTransportRequest({ type: 'transportRequest', requestId: 43, op: 'state' });
        expect(posted.at(-1)).toMatchObject({
          type: 'transportResponse',
          requestId: 43,
          ok: true,
          state: { playing: true },
        });
      } finally {
        processor.destroy();
      }
    });

    it('applies tempo and time-signature segment sync to metronome accents', () => {
      const blockSize = 16000;
      const output = new Float32Array(blockSize);
      const processor = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 8000,
        blockSize,
        channelCount: 1,
      });
      try {
        processor.receiveSync({
          type: 'syncTempo',
          bpm: 120,
          timeSignature: { numerator: 3, denominator: 4 },
          tempoSegments: [{ startPpq: 0, bpm: 120 }],
          timeSignatureSegments: [{ startPpq: 0, numerator: 3, denominator: 4 }],
        });
        processor.receiveSync({
          type: 'syncMetronome',
          config: {
            enabled: true,
            beatGain: 0.1,
            accentGain: 0.8,
            clickSamples: 0,
            clickSeconds: 0.01,
          },
        });
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });
        expect(processor.process([[]], [[output]])).toBe(true);
        expect(output[4000]).toBeGreaterThan(0);
        expect(output[8000]).toBeGreaterThan(0);
        expect(output[0]).toBeGreaterThan(output[4000] * 2);
        // 10 ms at 8 kHz survives the command's enabled-state handoff; the
        // previous reconstruction dropped clickSeconds and ended after 16 samples.
        expect(output[70]).toBeGreaterThan(0);
        expect(output[12000]).toBeGreaterThan(output[4000] * 2);
      } finally {
        processor.destroy();
      }
    });

    it('publishes real output meters from the realtime engine exactly once per interval', () => {
      // Both sinks feed ONE recorder, the way the AudioWorklet registration
      // wires them (onMeter and postMessage both resolve to port.postMessage).
      // Separate arrays would count a duplicated record as one delivery each
      // and read as success.
      const delivered: unknown[] = [];
      const record = (message: unknown) => {
        if ((message as { type?: string }).type === 'meter') {
          delivered.push(message);
        }
      };
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize: 128, channelCount: 2, meterIntervalFrames: 128 },
        { onMeter: record, postMessage: record },
      );
      try {
        const inL = new Float32Array(128).fill(0.5);
        const inR = new Float32Array(128).fill(-0.5);
        const outL = new Float32Array(128);
        const outR = new Float32Array(128);
        expect(processor.process([[inL, inR]], [[outL, outR]])).toBe(true);
        expect(delivered).toHaveLength(1);
        const meter = delivered[0] as { targetId: number; peakDbL: number; peakDbR: number };
        expect(delivered[0]).toMatchObject({
          type: 'meter',
          targetId: 0,
          frame: 0,
        });
        expect(meter.peakDbL).toBeCloseTo(-6.0206, 2);
        expect(meter.peakDbR).toBeCloseTo(-6.0206, 2);
      } finally {
        processor.destroy();
      }
    });

    it('delivers meters over postMessage when the transport has no onMeter sink', () => {
      const posted: unknown[] = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize: 128, channelCount: 2, meterIntervalFrames: 128 },
        { postMessage: (message) => posted.push(message) },
      );
      try {
        const inL = new Float32Array(128).fill(0.5);
        const inR = new Float32Array(128).fill(-0.5);
        const outL = new Float32Array(128);
        const outR = new Float32Array(128);
        expect(processor.process([[inL, inR]], [[outL, outR]])).toBe(true);
        expect(
          posted.filter((message) => (message as { type?: string }).type === 'meter'),
        ).toHaveLength(1);
      } finally {
        processor.destroy();
      }
    });

    it('publishes scope telemetry (FFT spectrum + goniometer) into the SAB scope ring', () => {
      const blockSize = 256;
      const sampleRate = 48000;
      const toneHz = 1000;
      const scopeRing = createSonareScopeRingBuffer(64, 32);
      const processor = new SonareRealtimeEngineWorkletProcessor({
        sampleRate,
        blockSize,
        channelCount: 2,
        scopeSharedBuffer: scopeRing.sharedBuffer,
        scopeRingCapacity: scopeRing.capacity,
        scopeBands: scopeRing.bands,
        scopeIntervalFrames: blockSize,
      });
      try {
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });
        let phase = 0;
        for (let block = 0; block < 12; block++) {
          const inL = new Float32Array(blockSize);
          const inR = new Float32Array(blockSize);
          for (let i = 0; i < blockSize; i++) {
            const s = 0.5 * Math.sin((2 * Math.PI * toneHz * phase) / sampleRate);
            inL[i] = s;
            inR[i] = s;
            phase++;
          }
          expect(
            processor.process(
              [[inL, inR]],
              [[new Float32Array(blockSize), new Float32Array(blockSize)]],
            ),
          ).toBe(true);
        }

        const read = readSonareScopeRingBuffer(scopeRing);
        expect(read.scopes.length).toBeGreaterThan(0);
        const master = read.scopes.find((scope) => scope.targetId === 0);
        expect(master).toBeDefined();
        if (master) {
          expect(master.bands.length).toBe(32);
          let peak = 0;
          for (let b = 1; b < master.bands.length; b++) {
            if (master.bands[b] > master.bands[peak]) {
              peak = b;
            }
          }
          // 1 kHz over a 32-band [0, 24 kHz] split -> band 0/1.
          expect(peak).toBeLessThanOrEqual(2);
          expect(master.bands[peak]).toBeGreaterThan(master.bands[24] + 20);
          // A mono-correlated tone scatters along the goniometer diagonal.
          expect(master.points.length).toBeGreaterThan(0);
        }
      } finally {
        processor.destroy();
      }
    });

    // One sample per SonareEngineSyncMessage member, keyed by discriminant. The
    // Record annotation makes the union the source of truth for the coverage
    // below as well: a member without a sample here is a compile error, so this
    // table cannot quietly fall behind the one the guard is derived from.
    const syncMessageSamples: Record<SonareEngineSyncMessage['type'], SonareEngineSyncMessage> = {
      destroy: { type: 'destroy' },
      syncAutomation: { type: 'syncAutomation', paramId: 7, points: [{ ppq: 0, value: 0.5 }] },
      syncBuiltinInstrument: {
        type: 'syncBuiltinInstrument',
        destinationId: 4,
        config: { gain: 0.5 },
      },
      syncBusStripDualPan: {
        type: 'syncBusStripDualPan',
        busId: 200,
        leftPan: -1,
        rightPan: 1,
      },
      syncBusStripEqBand: {
        type: 'syncBusStripEqBand',
        busId: 200,
        bandIndex: 0,
        bandJson: '{"type":"Peak","frequencyHz":1000,"gainDb":3}',
      },
      syncBusStripInsertBypassed: {
        type: 'syncBusStripInsertBypassed',
        busId: 200,
        insertIndex: 0,
        bypassed: true,
        resetOnBypass: false,
      },
      syncBusStripInsertParamByName: {
        type: 'syncBusStripInsertParamByName',
        busId: 200,
        insertIndex: 0,
        paramName: 'band0.gainDb',
        value: 3,
      },
      syncBusStripPan: { type: 'syncBusStripPan', busId: 200, pan: 0 },
      syncBusStripPanLaw: { type: 'syncBusStripPanLaw', busId: 200, panLaw: 0 },
      syncBusStripPanMode: { type: 'syncBusStripPanMode', busId: 200, panMode: 0 },
      syncCapture: {
        type: 'syncCapture',
        bufferFrames: 128,
        channels: 2,
        source: 'input',
        recordOffsetSamples: -12,
        inputMonitor: { enabled: true, gain: 0.5 },
      },
      syncClearMidiFx: { type: 'syncClearMidiFx', destinationId: 4 },
      syncClearMidiInputSource: { type: 'syncClearMidiInputSource' },
      syncClipPage: {
        type: 'syncClipPage',
        clipId: 900,
        pageIndex: 0,
        channels: [new Float32Array(256)],
      },
      syncClipPageClear: { type: 'syncClipPageClear', clipId: 900, pageIndex: 0 },
      syncClipPageCommit: { type: 'syncClipPageCommit', clipId: 900 },
      syncClipPageDestroy: { type: 'syncClipPageDestroy', clipId: 900 },
      syncClipPagePrefetchFrames: { type: 'syncClipPagePrefetchFrames', frames: 4096 },
      syncClipPageProvider: {
        type: 'syncClipPageProvider',
        clipId: 900,
        numChannels: 1,
        numSamples: 1024,
        pageFrames: 256,
      },
      syncClips: { type: 'syncClips', clips: [] },
      syncClipsDelta: { type: 'syncClipsDelta', upserts: [], removeIds: [] },
      syncExternalMidiClock: { type: 'syncExternalMidiClock', enabled: true },
      syncLoadSoundFont: { type: 'syncLoadSoundFont', data: new Uint8Array(0) },
      syncMarkers: { type: 'syncMarkers', markers: [] },
      syncMasterStripEqBand: {
        type: 'syncMasterStripEqBand',
        bandIndex: 0,
        bandJson: '{"type":"Peak","frequencyHz":1000,"gainDb":3}',
      },
      syncMasterStripInsertBypassed: {
        type: 'syncMasterStripInsertBypassed',
        insertIndex: 0,
        bypassed: true,
        resetOnBypass: false,
      },
      syncMasterStripInsertParamByName: {
        type: 'syncMasterStripInsertParamByName',
        insertIndex: 0,
        paramName: 'band0.gainDb',
        value: 1,
      },
      syncMetronome: {
        type: 'syncMetronome',
        config: {
          enabled: true,
          beatGain: 0.35,
          accentGain: 0.7,
          clickSamples: 0,
          clickSeconds: 0,
        },
      },
      syncMidiCc: {
        type: 'syncMidiCc',
        destinationId: 4,
        group: 0,
        channel: 0,
        controller: 7,
        value: 100,
        renderFrame: 0,
      },
      syncMidiCcBinding: {
        type: 'syncMidiCcBinding',
        channel: 0,
        controller: 7,
        paramId: 7,
        minValue: 0,
        maxValue: 1,
      },
      syncMidiClips: { type: 'syncMidiClips', clips: [] },
      syncMidiDestinationExternal: {
        type: 'syncMidiDestinationExternal',
        destinationId: 7,
        external: true,
      },
      syncMidiFx: { type: 'syncMidiFx', destinationId: 4, configJson: '{}' },
      syncMidiChannelPressure: {
        type: 'syncMidiChannelPressure',
        destinationId: 4,
        group: 0,
        channel: 0,
        data0: 100,
        data1: 0,
        renderFrame: 0,
      },
      syncMidiInputCc: {
        type: 'syncMidiInputCc',
        group: 0,
        channel: 0,
        data0: 7,
        data1: 100,
        portTimeSamples: 0,
      },
      syncMidiInputChannelPressure: {
        type: 'syncMidiInputChannelPressure',
        group: 0,
        channel: 0,
        data0: 100,
        data1: 0,
        portTimeSamples: 0,
      },
      syncMidiInputNoteOff: {
        type: 'syncMidiInputNoteOff',
        group: 0,
        channel: 0,
        data0: 60,
        data1: 0,
        portTimeSamples: 0,
      },
      syncMidiInputNoteOn: {
        type: 'syncMidiInputNoteOn',
        group: 0,
        channel: 0,
        data0: 60,
        data1: 100,
        portTimeSamples: 0,
      },
      syncMidiInputPitchBend: {
        type: 'syncMidiInputPitchBend',
        group: 0,
        channel: 0,
        data0: 12288,
        data1: 0,
        portTimeSamples: 0,
      },
      syncMidiInputPolyPressure: {
        type: 'syncMidiInputPolyPressure',
        group: 0,
        channel: 0,
        data0: 60,
        data1: 100,
        portTimeSamples: 0,
      },
      syncMidiInputSource: { type: 'syncMidiInputSource', destinationId: 4 },
      syncMidiInputUmp: {
        type: 'syncMidiInputUmp',
        words: Uint32Array.from([0x41923c00, 0x12345678]),
        portTimeSamples: 768,
      },
      syncMidiNoteOff: {
        type: 'syncMidiNoteOff',
        destinationId: 4,
        group: 0,
        channel: 0,
        note: 60,
        velocity: 0,
        renderFrame: 0,
      },
      syncMidiNoteOn: {
        type: 'syncMidiNoteOn',
        destinationId: 4,
        group: 0,
        channel: 0,
        note: 60,
        velocity: 100,
        renderFrame: 0,
      },
      syncMidiPanic: { type: 'syncMidiPanic', renderFrame: 0 },
      syncMidiPitchBend: {
        type: 'syncMidiPitchBend',
        destinationId: 4,
        group: 0,
        channel: 0,
        data0: 12288,
        data1: 0,
        renderFrame: 0,
      },
      syncMidiPolyPressure: {
        type: 'syncMidiPolyPressure',
        destinationId: 4,
        group: 0,
        channel: 0,
        data0: 60,
        data1: 100,
        renderFrame: 0,
      },
      syncMidiSysex: {
        type: 'syncMidiSysex',
        destinationId: 4,
        data: new Uint8Array([0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7]),
        renderFrame: 0,
      },
      syncMidiUmp: {
        type: 'syncMidiUmp',
        destinationId: 4,
        words: Uint32Array.from([0x41923c00, 0x12345678]),
        renderFrame: 0,
      },
      syncMixer: {
        type: 'syncMixer',
        lanes: [{ trackId: 10, sends: [{ busId: 200, levelDb: 0, enabled: true }] }],
        buses: [{ busId: 200, gainDb: 0 }],
      },
      syncParameters: {
        type: 'syncParameters',
        parameters: [
          {
            id: 7,
            name: 'gain',
            unit: 'dB',
            minValue: 0,
            maxValue: 1,
            defaultValue: 0,
            rtSafe: true,
            defaultCurve: 2,
          },
        ],
      },
      syncSf2Instrument: { type: 'syncSf2Instrument', destinationId: 4, config: { gain: 0.5 } },
      syncSynthInstrument: { type: 'syncSynthInstrument', destinationId: 4, patch: 'saw-lead' },
      syncTempo: {
        type: 'syncTempo',
        bpm: 120,
        timeSignature: { numerator: 4, denominator: 4 },
      },
      syncTrackStripChannelDelaySamples: {
        type: 'syncTrackStripChannelDelaySamples',
        trackId: 10,
        delaySamples: 4,
      },
      syncTrackStripDualPan: {
        type: 'syncTrackStripDualPan',
        trackId: 10,
        leftPan: -1,
        rightPan: 1,
      },
      syncTrackStripEqBand: {
        type: 'syncTrackStripEqBand',
        trackId: 10,
        bandIndex: 0,
        bandJson: '{"type":"Peak","frequencyHz":1000,"gainDb":3}',
      },
      syncTrackStripInsertBypassed: {
        type: 'syncTrackStripInsertBypassed',
        trackId: 10,
        insertIndex: 0,
        bypassed: true,
        resetOnBypass: false,
      },
      syncTrackStripInsertParamByName: {
        type: 'syncTrackStripInsertParamByName',
        trackId: 10,
        insertIndex: 0,
        paramName: 'band0.gainDb',
        value: 1,
      },
      syncTrackStripPan: { type: 'syncTrackStripPan', trackId: 10, pan: 0 },
      syncTrackStripPanLaw: { type: 'syncTrackStripPanLaw', trackId: 10, panLaw: 0 },
      syncTrackStripPanMode: { type: 'syncTrackStripPanMode', trackId: 10, panMode: 0 },
      syncTrackStripSurroundPan: {
        type: 'syncTrackStripSurroundPan',
        trackId: 10,
        pan: { azimuth: -110, elevation: 0, divergence: 0, lfe: 0, distance: 1 },
      },
      syncWarpVoiceCapacity: { type: 'syncWarpVoiceCapacity', voices: 12 },
    };

    it('routes every producer sync message through the guarded port handler', async () => {
      // The producers post these over node.port; the worklet's onMessage handler
      // gates them with isEngineSyncMessage before reaching receiveSync. A message
      // type missing from the guard is silently dropped, so the live engine never
      // sees it even though the offline mirror does. Drive every union member
      // through the real registered-processor onMessage path (not receiveSync
      // directly) so a guard omission fails here instead of shipping a dead
      // feature, and take the case list from the guard's own table so the two
      // cannot disagree.
      const previousProcessor = (
        globalThis as typeof globalThis & { AudioWorkletProcessor?: unknown }
      ).AudioWorkletProcessor;
      const previousRegister = (globalThis as typeof globalThis & { registerProcessor?: unknown })
        .registerProcessor;
      type MockPort = {
        posted: unknown[];
        onmessage?: (event: { data: unknown }) => void;
        postMessage: (message: unknown) => void;
      };
      let registeredCtor:
        | (new (options?: {
            processorOptions?: unknown;
          }) => { port?: MockPort })
        | undefined;
      const receiveSyncSpy = vi.spyOn(
        SonareRealtimeEngineWorkletProcessor.prototype,
        'receiveSync',
      );
      // applySync is the handler that actually touches the live engine;
      // receiveSync only wraps it in the syncError reporter. Assert on the inner
      // one so "reached the engine" is what the case proves.
      const applySyncSpy = vi.spyOn(
        SonareRealtimeEngineWorkletProcessor.prototype as unknown as {
          applySync: (message: SonareEngineSyncMessage) => void;
        },
        'applySync',
      );
      try {
        Object.assign(globalThis, {
          AudioWorkletProcessor: class {
            port: MockPort = {
              posted: [],
              onmessage: undefined,
              postMessage: (message: unknown) => {
                this.port.posted.push(message);
              },
            };
          },
          registerProcessor: (_name: string, ctor: unknown) => {
            registeredCtor = ctor as typeof registeredCtor;
          },
        });
        registerSonareRealtimeEngineWorkletProcessor();
        expect(typeof registeredCtor).toBe('function');
        const Ctor = registeredCtor;
        if (!Ctor) {
          throw new Error('processor was not registered');
        }
        const instance = new Ctor({
          processorOptions: { sampleRate: 48000, blockSize: 128, channelCount: 2 },
        });
        const port = instance.port;
        if (!port) {
          throw new Error('registered processor has no port');
        }
        // The embind bridge is constructed asynchronously and posts 'ready'.
        const start = Date.now();
        while (!port.posted.some((m) => (m as { type?: string }).type === 'ready')) {
          if (Date.now() - start > 5000) {
            throw new Error('engine bridge did not become ready');
          }
          await new Promise((resolve) => setTimeout(resolve, 5));
        }
        const workletEngine = (
          instance as unknown as {
            bridge: {
              engine: {
                pushMidiUmp: (
                  destinationId: number,
                  words: Uint32Array | number[],
                  renderFrame: number,
                ) => void;
                pushMidiInputUmp: (words: Uint32Array, portTimeSamples: number) => void;
              };
            };
          }
        ).bridge.engine;
        const destinationUmp = vi
          .spyOn(workletEngine, 'pushMidiUmp')
          .mockImplementation(() => undefined);
        const rawInput = vi
          .spyOn(workletEngine, 'pushMidiInputUmp')
          .mockImplementation(() => undefined);
        // Give the track, bus and master strips a real insert so the strip
        // syncs resolve a live target instead of throwing; this also drives
        // syncMixer through the same guarded path.
        const insertParams =
          '{\\"band0.type\\":1,\\"band0.frequencyHz\\":1000,\\"band0.gainDb\\":0,\\"band0.enabled\\":1}';
        const insertJson = `{"slot":"pre","processor":"eq.parametric","params":"${insertParams}"}`;
        port.onmessage?.({
          data: {
            type: 'syncMixer',
            buses: [{ busId: 200, gainDb: 0 }],
            lanes: [{ trackId: 10, sends: [{ busId: 200, levelDb: 0, enabled: true }] }],
            trackStrips: [
              {
                trackId: 10,
                sceneJson: `{"version":1,"strips":[{"id":"track-10","inserts":[${insertJson}]}],"buses":[],"connections":[]}`,
              },
            ],
            busStrips: [
              {
                busId: 200,
                sceneJson: `{"version":1,"strips":[],"buses":[{"id":"200","inserts":[${insertJson}]}],"connections":[]}`,
              },
            ],
            masterStripJson: `{"version":1,"strips":[{"id":"master","inserts":[${insertJson}]}],"buses":[],"connections":[]}`,
          },
        });
        receiveSyncSpy.mockClear();
        applySyncSpy.mockClear();
        const syncMessageTypes = Object.keys(syncMessageSamples) as Array<
          SonareEngineSyncMessage['type']
        >;
        // Both tables are exhaustive over the union by their own annotation, so
        // they must hold the same keys. Cross-checking them makes a key deleted
        // from either one red here too, not only at compile time.
        expect([...syncMessageTypes].sort()).toEqual(Object.keys(ENGINE_SYNC_MESSAGE_TYPES).sort());
        // Every sample must key itself, or the table could cover a member by
        // name while driving a different message.
        for (const type of syncMessageTypes) {
          expect(syncMessageSamples[type].type).toBe(type);
        }
        // Ordering the engine imposes on the fixture: a MIDI input source has to
        // be selected before an input event is accepted, and both clearing it and
        // closing the bridge have to come after everything depending on them.
        // 'destroy' last in particular, since receiveSync returns early once
        // closed and nothing after it would reach applySync.
        const first: Array<SonareEngineSyncMessage['type']> = ['syncMidiInputSource'];
        const last: Array<SonareEngineSyncMessage['type']> = [
          'syncClearMidiInputSource',
          'destroy',
        ];
        const drivenTypes: Array<SonareEngineSyncMessage['type']> = [
          ...first,
          ...syncMessageTypes.filter((type) => !first.includes(type) && !last.includes(type)),
          ...last,
        ];
        for (const type of drivenTypes.slice(0, -1)) {
          port.onmessage?.({ data: syncMessageSamples[type] });
        }
        // Reaching applySync is the guard's contract, but a handler that then
        // rejects its own producer's payload is the same dead feature one layer
        // down. Only a SoundFont load is expected to fail here: unlike a strip or
        // a MIDI destination, no valid SF2 can be fabricated in the fixture.
        expect(
          port.posted
            .filter((message) => (message as { type?: string }).type === 'syncError')
            .map((message) => (message as { syncType: string }).syncType),
        ).toEqual(['syncLoadSoundFont']);
        expect(destinationUmp).toHaveBeenCalledWith(
          4,
          Uint32Array.from([0x41923c00, 0x12345678]),
          0,
        );
        expect(rawInput).toHaveBeenCalledWith(Uint32Array.from([0x41923c00, 0x12345678]), 768);
        // A string-typed message the guard does not know must stay dropped.
        port.onmessage?.({ data: { type: 'totallyUnknownSync' } });
        // A sync-prefixed one the guard does not know must not vanish: it is a
        // producer/worklet version skew, and the node surfaces it via onSyncError.
        port.posted.length = 0;
        port.onmessage?.({ data: { type: 'syncSomethingThisBuildDoesNotKnow' } });
        expect(port.posted).toEqual([
          {
            type: 'syncError',
            syncType: 'syncSomethingThisBuildDoesNotKnow',
            message: 'Unrecognized worklet sync message: syncSomethingThisBuildDoesNotKnow',
          },
        ]);
        port.onmessage?.({ data: syncMessageSamples.destroy });
        expect(applySyncSpy.mock.calls.map((call) => call[0].type)).toEqual(drivenTypes);
        expect(receiveSyncSpy.mock.calls.map((call) => (call[0] as { type: string }).type)).toEqual(
          drivenTypes,
        );
      } finally {
        applySyncSpy.mockRestore();
        receiveSyncSpy.mockRestore();
        Object.assign(globalThis, {
          AudioWorkletProcessor: previousProcessor,
          registerProcessor: previousRegister,
        });
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

    it('transfers capture buffers without invalidating native capture state', () => {
      const blockSize = 128;
      const posted: Array<{ message: unknown; transfer?: Transferable[] }> = [];
      const processor = new SonareRealtimeEngineWorkletProcessor(
        { sampleRate: 48000, blockSize, channelCount: 2 },
        {
          postMessage: (message, transfer) => {
            // Model MessagePort's structured-clone + transfer behavior. The
            // sender-side typed arrays are detached here; the delivered clone
            // is what a main-thread consumer would receive.
            const delivered =
              transfer && transfer.length > 0 ? structuredClone(message, { transfer }) : message;
            posted.push({ message: delivered, transfer });
          },
        },
      );
      try {
        processor.receiveSync({
          type: 'syncCapture',
          bufferFrames: blockSize,
          channels: 2,
          source: 'input',
          recordOffsetSamples: 0,
          inputMonitor: { enabled: false, gain: 1 },
        });
        processor.receiveCommand({
          type: SonareEngineCommandType.ArmRecord,
          sampleTime: -1,
          argInt: 1,
        });
        processor.process(
          [[new Float32Array(blockSize).fill(0.25), new Float32Array(blockSize).fill(-0.5)]],
          [[new Float32Array(blockSize), new Float32Array(blockSize)]],
        );

        processor.receiveCaptureRequest({ type: 'captureRequest', requestId: 10, op: 'read' });
        const read = posted.at(-1);
        const response = read?.message as { channels?: Float32Array[] };
        expect(response.channels).toHaveLength(2);
        expect(response.channels?.[0][0]).toBeCloseTo(0.25, 4);
        expect(response.channels?.[1][0]).toBeCloseTo(-0.5, 4);
        expect(read?.transfer).toHaveLength(2);
        expect(new Set(read?.transfer).size).toBe(2);
        expect(read?.transfer?.every((buffer) => buffer instanceof ArrayBuffer)).toBe(true);

        // The transfer detached only the temporary JS response. Native capture
        // storage remains available for subsequent control requests.
        processor.receiveCaptureRequest({ type: 'captureRequest', requestId: 11, op: 'status' });
        expect(posted.at(-1)?.message).toMatchObject({
          type: 'captureResponse',
          requestId: 11,
          ok: true,
          status: { capturedFrames: blockSize },
        });
        processor.receiveCaptureRequest({ type: 'captureRequest', requestId: 12, op: 'reset' });
        expect(posted.at(-1)?.message).toMatchObject({
          type: 'captureResponse',
          requestId: 12,
          ok: true,
        });
      } finally {
        processor.destroy();
      }
    });
  });
});

describe('processor state reset through the worklet facade', () => {
  setupWorklet();

  const SR = 48000;
  const BLOCK = 128;
  const TRACK = 10;
  const BUS = 1;
  const START_SECONDS = 0.5;
  const START = SR * START_SECONDS;
  const RENDER_BLOCKS = 75;
  const RENDER = BLOCK * RENDER_BLOCKS;
  const DIRTY_BLOCKS = 100;

  type Processor = 'plate' | 'gate' | 'tape' | 'delay';
  type Placement = 'lane' | 'bus' | 'master';
  type Path = 'facade_render_offline' | 'facade_reset_play';
  type Row = readonly [Processor, Placement, number, Path];

  // Pairwise rows over processor x placement x idle blocks x path.
  const ROWS: readonly Row[] = [
    ['plate', 'bus', 1, 'facade_render_offline'],
    ['tape', 'lane', 0, 'facade_render_offline'],
    ['delay', 'master', 0, 'facade_reset_play'],
    ['tape', 'master', 37, 'facade_render_offline'],
    ['delay', 'lane', 1, 'facade_render_offline'],
    ['gate', 'bus', 0, 'facade_reset_play'],
    ['tape', 'bus', 37, 'facade_reset_play'],
    ['tape', 'master', 1, 'facade_render_offline'],
    ['plate', 'lane', 37, 'facade_reset_play'],
    ['gate', 'master', 1, 'facade_render_offline'],
    ['tape', 'master', 1, 'facade_reset_play'],
    ['gate', 'lane', 37, 'facade_reset_play'],
    ['plate', 'master', 37, 'facade_render_offline'],
    ['delay', 'master', 37, 'facade_reset_play'],
    ['plate', 'master', 0, 'facade_render_offline'],
    ['delay', 'bus', 37, 'facade_reset_play'],
  ];

  const INSERTS: Record<Processor, string> = {
    plate:
      '{"slot":"pre","processor":"effects.reverb.plate","params":{"decaySec":2.0,"modRateHz":1.0,"modDepthSamples":16,"dryWet":0.5}}',
    gate: '{"slot":"pre","processor":"dynamics.gate","params":{"thresholdDb":-30,"attackMs":1,"holdMs":5,"releaseMs":80}}',
    tape: '{"slot":"pre","processor":"saturation.tape","params":{"bias":0.5,"driveDb":6}}',
    delay:
      '{"slot":"pre","processor":"effects.delay.stereo","params":{"feedback":0.6,"delayTimeLMs":37,"delayTimeRMs":53,"dryWet":0.5}}',
  };

  const stripJson = (placement: Placement, insert: string): string => {
    switch (placement) {
      case 'lane':
        return `{"version":1,"strips":[{"id":"s","inserts":[${insert}]}]}`;
      case 'bus':
        return `{"version":1,"strips":[],"buses":[{"id":"${BUS}","inserts":[${insert}]}]}`;
      case 'master':
        return `{"version":1,"strips":[{"id":"master","inserts":[${insert}]}]}`;
    }
  };

  // Tone bursts with silent gaps, so a gate opens and closes and every tail is excited.
  const burst = (): Float32Array[] => {
    const left = new Float32Array(96000);
    const right = new Float32Array(96000);
    for (let i = 0; i < left.length; i += 1) {
      const env = i % 4800 < 2400 ? 0.4 : 0;
      left[i] = env * Math.sin((2 * Math.PI * 330 * i) / SR);
      right[i] = env * Math.sin((2 * Math.PI * 495 * i) / SR);
    }
    return [left, right];
  };

  /** The mixer calls both the raw engine and the facade accept, in one shape. */
  interface Configurable {
    setTrackBuses(buses: Array<{ busId: number; gainDb: number }>): void;
    setTrackLanes(lanes: Array<{ trackId: number; outputBusId: number }>): void;
    setTrackStripJson(trackId: number, json: string): void;
    setBusStripJson(busId: number, json: string): void;
    setMasterStripJson(json: string): void;
  }

  const configure = (target: Configurable, processor: Processor, placement: Placement): void => {
    if (placement === 'bus') {
      target.setTrackBuses([{ busId: BUS, gainDb: 0 }]);
    }
    target.setTrackLanes([{ trackId: TRACK, outputBusId: placement === 'bus' ? BUS : 0 }]);
    const json = stripJson(placement, INSERTS[processor]);
    if (placement === 'lane') {
      target.setTrackStripJson(TRACK, json);
    }
    if (placement === 'bus') {
      target.setBusStripJson(BUS, json);
    }
    if (placement === 'master') {
      target.setMasterStripJson(json);
    }
  };

  const planes = (frames: number): Float32Array[] => [
    new Float32Array(frames),
    new Float32Array(frames),
  ];

  /** Baseline: a fresh raw engine, primed at the start position, then rendered. */
  const baseline = (processor: Processor, placement: Placement): Float32Array[] => {
    const engine = new RealtimeEngine(SR, BLOCK);
    engine.setClips([{ trackId: TRACK, channels: burst(), startPpq: 0, lengthSamples: 96000 }]);
    configure(engine, processor, placement);
    engine.seekSample(START);
    engine.primeOfflineParameters(2, BLOCK);
    const out = engine.renderOffline(planes(RENDER), BLOCK);
    engine.destroy();
    return out;
  };

  const compare = (actual: Float32Array[], expected: Float32Array[]): void => {
    let diff = 0;
    let peak = 0;
    for (let ch = 0; ch < expected.length; ch += 1) {
      expect(actual[ch].length).toBe(expected[ch].length);
      for (let i = 0; i < expected[ch].length; i += 1) {
        peak = Math.max(peak, Math.abs(expected[ch][i]));
        diff = Math.max(diff, Math.abs(actual[ch][i] - expected[ch][i]));
      }
    }
    expect(peak).toBeGreaterThan(0);
    expect(diff).toBeLessThanOrEqual(1e-6 * Math.max(1, peak));
  };

  const fakeContext = (): BaseAudioContext =>
    ({
      sampleRate: SR,
      audioWorklet: { addModule: () => Promise.resolve() },
    }) as unknown as BaseAudioContext;

  const readyNode = (posted: unknown[]): AudioWorkletNode => {
    const port = {
      onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
      postMessage: (message: unknown) => posted.push(message),
    };
    queueMicrotask(() => {
      port.onmessage?.({
        data: { type: 'ready', runtimeTarget: 'embind' },
      } as MessageEvent<unknown>);
    });
    return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
  };

  const createFacade = async (posted: unknown[], mirror: RealtimeEngine) =>
    SonareEngine.create(fakeContext(), {
      mode: 'postMessage',
      offlineEngine: mirror as unknown as NonNullable<
        NonNullable<Parameters<typeof SonareEngine.create>[1]>['offlineEngine']
      >,
      nodeFactory: () => readyNode(posted),
    });

  const processBlocks = (engine: RealtimeEngine, blocks: number): void => {
    for (let i = 0; i < blocks; i += 1) {
      engine.process(planes(BLOCK));
    }
  };

  /** Delivers what the facade posted to the processor, in order, as the node would. */
  const deliver = (processor: SonareRealtimeEngineWorkletProcessor, posted: unknown[]): void => {
    for (const message of posted.splice(0)) {
      if (isEngineSyncMessage(message)) {
        processor.receiveSync(message);
      } else if (typeof (message as { type?: unknown }).type === 'number') {
        processor.receiveCommand(message as SonareEngineCommandRecord);
      }
    }
  };

  const renderBlock = (processor: SonareRealtimeEngineWorkletProcessor): Float32Array[] => {
    const outputs = [planes(BLOCK)];
    expect(processor.process([[]], outputs)).toBe(true);
    return outputs[0];
  };

  const renderBlocks = (
    processor: SonareRealtimeEngineWorkletProcessor,
    blocks: number,
  ): Float32Array[] => {
    const out = planes(blocks * BLOCK);
    for (let b = 0; b < blocks; b += 1) {
      const chunk = renderBlock(processor);
      out[0].set(chunk[0], b * BLOCK);
      out[1].set(chunk[1], b * BLOCK);
    }
    return out;
  };

  const runRenderOffline = async (
    processor: Processor,
    placement: Placement,
    idle: number,
  ): Promise<Float32Array[]> => {
    const posted: unknown[] = [];
    const mirror = new RealtimeEngine(SR, BLOCK);
    const facade = await createFacade(posted, mirror);
    try {
      facade.addClip(TRACK, burst(), 0, { lengthSamples: 96000 });
      configure(facade, processor, placement);
      mirror.play();
      processBlocks(mirror, DIRTY_BLOCKS);
      mirror.stop();
      processBlocks(mirror, idle);
      mirror.seekSample(START);
      return await facade.renderOffline(RENDER);
    } finally {
      facade.destroy();
    }
  };

  const runResetPlay = async (
    processor: Processor,
    placement: Placement,
    idle: number,
  ): Promise<Float32Array[]> => {
    const posted: unknown[] = [];
    const mirror = new RealtimeEngine(SR, BLOCK);
    const facade = await createFacade(posted, mirror);
    const worklet = new SonareRealtimeEngineWorkletProcessor(
      { sampleRate: SR, blockSize: BLOCK, channelCount: 2 },
      { postMessage: () => undefined },
    );
    try {
      facade.addClip(TRACK, burst(), 0, { lengthSamples: 96000 });
      configure(facade, processor, placement);
      deliver(worklet, posted);
      facade.transport.play();
      deliver(worklet, posted);
      renderBlocks(worklet, DIRTY_BLOCKS);
      facade.transport.stop();
      deliver(worklet, posted);
      renderBlocks(worklet, idle);
      facade.transport.seekSeconds(START_SECONDS);
      expect(facade.resetProcessorState()).toBe(true);
      facade.transport.play();
      deliver(worklet, posted);
      return renderBlocks(worklet, RENDER_BLOCKS);
    } finally {
      worklet.destroy();
      facade.destroy();
    }
  };

  it.each(ROWS)(
    '%s on a %s, %i idle blocks, %s starts like a fresh primed engine',
    async (processor, placement, idle, path) => {
      const expected = baseline(processor, placement);
      const actual =
        path === 'facade_render_offline'
          ? await runRenderOffline(processor, placement, idle)
          : await runResetPlay(processor, placement, idle);
      compare(actual, expected);
    },
  );

  it('applies resetProcessorState to the mirror and queues command 29 for the processor', async () => {
    const posted: unknown[] = [];
    const mirror = new RealtimeEngine(SR, BLOCK);
    const facade = await createFacade(posted, mirror);
    try {
      expect(SonareEngineCommandType.ResetProcessorState).toBe(29);
      posted.length = 0;
      expect(facade.resetProcessorState(256)).toBe(true);
      expect(posted).toContainEqual({ type: 29, sampleTime: 256 });
    } finally {
      facade.destroy();
    }
  });

  it('answers the facade queries from the mirror', async () => {
    const posted: unknown[] = [];
    const mirror = new RealtimeEngine(SR, BLOCK);
    const facade = await createFacade(posted, mirror);
    try {
      expect(facade.tailSamples()).toBe(0);
      expect(facade.graphLatencySamplesQ8()).toBe(0);
      expect(facade.canSetLaneSidechain(0, 0, 11)).toEqual({ ok: false, reason: 'invalidTarget' });
      expect(facade.canSetBusSidechain(99, 0, 'bus', 1)).toEqual({
        ok: false,
        reason: 'invalidTarget',
      });
      expect(facade.canSetMasterSidechain(5, 'bus', 1)).toEqual({
        ok: false,
        reason: 'insertOutOfRange',
      });
      facade.setTrackLanes([TRACK]);
      facade.setTrackStripJson(TRACK, stripJson('lane', INSERTS.delay));
      expect(facade.tailSamples()).toBeGreaterThanOrEqual(Math.round(0.053 * SR));
      expect(facade.tailSamples()).toBe(mirror.tailSamples());
    } finally {
      facade.destroy();
    }
  });

  it('refuses a command record the node allowlist does not know and accepts 29', async () => {
    const posted: unknown[] = [];
    const context = { sampleRate: SR } as unknown as BaseAudioContext;
    const { SonareRealtimeEngineNode } = await import('../dist/worklet.js');
    const node = await SonareRealtimeEngineNode.create(context, {
      mode: 'postMessage',
      engineAbiVersion: 1,
      nodeFactory: () => readyNode(posted),
    });
    try {
      expect(node.resetProcessorState(512)).toBe(true);
      expect(posted).toContainEqual({ type: 29, sampleTime: 512 });
    } finally {
      node.destroy();
    }
  });

  it('keeps the mirror and the processor in agreement after a strip JSON edit', async () => {
    const posted: unknown[] = [];
    const mirror = new RealtimeEngine(SR, BLOCK);
    const facade = await createFacade(posted, mirror);
    const worklet = new SonareRealtimeEngineWorkletProcessor(
      { sampleRate: SR, blockSize: BLOCK, channelCount: 2 },
      { postMessage: () => undefined },
    );
    try {
      facade.addClip(TRACK, burst(), 0, { lengthSamples: 96000 });
      facade.setTrackLanes([TRACK]);
      facade.setTrackStripJson(TRACK, stripJson('lane', INSERTS.plate));
      // The edit replaces the insert after the first structural sync was posted.
      facade.setTrackStripJson(TRACK, stripJson('lane', INSERTS.delay));
      deliver(worklet, posted);
      expect(facade.resetProcessorState()).toBe(true);
      facade.transport.play();
      deliver(worklet, posted);
      const live = renderBlocks(worklet, RENDER_BLOCKS);

      mirror.primeOfflineParameters(2, BLOCK);
      const offline = mirror.renderOffline(planes(RENDER), BLOCK);
      compare(live, offline);
    } finally {
      worklet.destroy();
      facade.destroy();
    }
  });
});
