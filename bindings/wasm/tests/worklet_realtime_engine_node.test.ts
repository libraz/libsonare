import { readFileSync } from 'node:fs';
import { vi } from 'vitest';
import { ErrorCode } from '../src/errors';
import { ENGINE_SYNC_MESSAGE_TYPES } from '../src/worklet/guards';
import { SONARE_ENGINE_ROWS } from './_list_budget_rows';
import {
  describe,
  expect,
  it,
  popSonareEngineCommandRingBuffer,
  pushSonareClipPageRequestRingBuffer,
  pushSonareExternalMidiRingBuffer,
  SonareEngine,
  SonareEngineCommandType,
  SonareEngineTelemetryError,
  SonareEngineTelemetryType,
  SonareRealtimeEngineNode,
  SonareRealtimeEngineWorkletProcessor,
  setupWorklet,
  writeSonareEngineTelemetryRingBuffer,
} from './_worklet_helpers';

/** The first strip of a scene document, as `setStrip` takes it. */
const stripDocument = (json: string) => JSON.parse(json).strips[0];
/** The first bus of a scene document, as `setBusStrip` takes it. */
const busDocument = (json: string) => JSON.parse(json).buses[0];
/** The scene text the facade sends for one strip or bus taken from a document. */
const sceneOf = (key: 'strips' | 'buses', json: string): string =>
  JSON.stringify({ [key]: [JSON.parse(json)[key][0]] });

/** The offline mirror's read side the commit tests inspect. */
type RealtimeEngineLike = {
  transportState(): { loopStartPpq: number; loopEndPpq: number; samplePosition: number };
};

/** The engine `SonareEngine.create` accepts as its offline mirror. */
type OfflineEngineOption = NonNullable<
  NonNullable<Parameters<typeof SonareEngine.create>[1]>['offlineEngine']
>;

describe('SonareRealtimeEngineNode', () => {
  setupWorklet();

  describe('SonareRealtimeEngineNode', () => {
    const midi1Word = (status: number, channel: number, data0: number, data1: number): number =>
      (0x2 << 28) | ((status & 0xf) << 20) | ((channel & 0xf) << 16) | (data0 << 8) | data1;

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

    function readyWorkletNode(port: {
      onmessage?: ((event: MessageEvent<unknown>) => void) | null;
      [member: string]: unknown;
    }): AudioWorkletNode {
      queueMicrotask(() => {
        port.onmessage?.({
          data: { type: 'ready', runtimeTarget: 'embind' },
        } as MessageEvent<unknown>);
      });
      return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
    }

    it('rejects a pending ready wait on destroy and ignores a late ready message', async () => {
      const port: { onmessage?: ((event: MessageEvent<unknown>) => void) | null } & Record<
        string,
        unknown
      > = { postMessage: () => undefined };
      let handler: ((event: MessageEvent<unknown>) => void) | null | undefined;
      const node = await SonareRealtimeEngineNode.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () => {
          queueMicrotask(() => {
            handler = port.onmessage;
          });
          return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
        },
      });
      await Promise.resolve();
      node.destroy();
      await expect(node.ready).rejects.toMatchObject({ code: 7, codeName: 'InvalidState' });
      await expect(node.ready).rejects.toThrow(
        /^SonareRealtimeEngineNode\.ready: SonareRealtimeEngineNode was destroyed before it became ready$/,
      );
      // A handler the host kept from before destroy cannot revive the node.
      handler?.({ data: { type: 'ready', runtimeTarget: 'embind' } } as MessageEvent<unknown>);
      await expect(node.ready).rejects.toMatchObject({ codeName: 'InvalidState' });
      expect(() => node.sendCommand({ type: SonareEngineCommandType.TransportPlay })).toThrow(
        expect.objectContaining({ code: ErrorCode.InvalidState }),
      );
      expect(() => node.sendCommand({ type: SonareEngineCommandType.TransportPlay })).toThrow(
        /^SonareRealtimeEngineNode\.sendCommand: SonareRealtimeEngineNode has been destroyed$/,
      );
    });

    it('keeps a node that became ready resolved when it is destroyed', async () => {
      const port: { onmessage?: ((event: MessageEvent<unknown>) => void) | null } & Record<
        string,
        unknown
      > = { postMessage: () => undefined };
      const node = await SonareRealtimeEngineNode.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () => readyWorkletNode(port),
      });
      await node.ready;
      node.destroy();
      await expect(node.ready).resolves.toBeUndefined();
    });

    it('leaves queued external MIDI to a manual pollMidiOut while only meters are subscribed', async () => {
      const port: { onmessage?: ((event: MessageEvent<unknown>) => void) | null } & Record<
        string,
        unknown
      > = { postMessage: () => undefined };
      const node = await SonareRealtimeEngineNode.create(fakeContext(), {
        numChannels: 2,
        mode: 'sab',
        nodeFactory: () => readyWorkletNode(port),
      });
      try {
        const ring = node.externalMidiRing;
        if (!ring) {
          throw new Error('expected an external-MIDI ring');
        }
        expect(pushSonareExternalMidiRingBuffer(ring, 3, 64, 0x00003c80, 3)).toBe(true);
        const unsubscribeMeter = node.onMeter(() => undefined);
        const unsubscribeTelemetry = node.onTelemetry(() => undefined);
        const unsubscribeScope = node.onScope(() => undefined);
        await new Promise((resolve) => setTimeout(resolve, 40));
        expect(node.pollMidiOut()).toEqual([
          { destinationId: 3, renderFrame: 64, bytes: Uint8Array.of(0x80, 0x3c, 0) },
        ]);
        expect(node.pollMidiOut()).toEqual([]);

        // A MIDI subscriber drains through the shared poll; after it leaves, the
        // remaining subscriptions stop consuming again.
        const delivered: number[] = [];
        const unsubscribeMidi = node.onMidiOut((events) => {
          delivered.push(...events.map((event) => event.renderFrame));
        });
        expect(pushSonareExternalMidiRingBuffer(ring, 3, 128, 0x00003c80, 3)).toBe(true);
        await new Promise((resolve) => setTimeout(resolve, 40));
        expect(delivered).toEqual([128]);
        unsubscribeMidi();
        expect(pushSonareExternalMidiRingBuffer(ring, 3, 192, 0x00003c80, 3)).toBe(true);
        await new Promise((resolve) => setTimeout(resolve, 40));
        expect(node.pollMidiOut().map((event) => event.renderFrame)).toEqual([192]);
        unsubscribeMeter();
        unsubscribeTelemetry();
        unsubscribeScope();
      } finally {
        node.destroy();
      }
    });

    it('drains every-port clock bytes from the ring without a destinationId', async () => {
      const port: { onmessage?: ((event: MessageEvent<unknown>) => void) | null } & Record<
        string,
        unknown
      > = { postMessage: () => undefined };
      const node = await SonareRealtimeEngineNode.create(fakeContext(), {
        numChannels: 2,
        mode: 'sab',
        nodeFactory: () => readyWorkletNode(port),
      });
      try {
        const ring = node.externalMidiRing;
        if (!ring) {
          throw new Error('expected an external-MIDI ring');
        }
        // The ring keeps the C ABI's every-port destination as its wire value.
        expect(pushSonareExternalMidiRingBuffer(ring, 0xffffffff, 32, 0x000000f8, 1)).toBe(true);
        expect(pushSonareExternalMidiRingBuffer(ring, 4, 48, 0x00003c80, 3)).toBe(true);
        const [clock, note] = node.pollMidiOut();
        expect(clock).toEqual({ renderFrame: 32, bytes: Uint8Array.of(0xf8) });
        expect(clock).not.toHaveProperty('destinationId');
        expect(note.destinationId).toBe(4);
      } finally {
        node.destroy();
      }
    });

    it('delivers an instrument change made during playback at once, ahead of the MIDI that follows it', async () => {
      const posted: unknown[] = [];
      const port: { onmessage?: ((event: MessageEvent<unknown>) => void) | null } & Record<
        string,
        unknown
      > = { postMessage: (message: unknown) => posted.push(message) };
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () => readyWorkletNode(port),
      });
      try {
        engine.setTrackLanes([{ trackId: 3 }]);
        engine.transport.play();
        posted.length = 0;
        const sf2 = new Uint8Array(
          readFileSync(new URL('../../../tests/fixtures/sf2/minimal_gs.sf2', import.meta.url)),
        );
        engine.loadSoundFont(sf2);
        engine.setBuiltinInstrument(3, { gain: 0.5 });
        engine.setSynthInstrument(3, { preset: 'sawLead' });
        engine.setSf2Instrument(3, { gain: 0.5 });
        // Program Change 5 on group 0, channel 0: it must reach the new instrument.
        engine.pushMidiUmp(3, [0x20c00500]);
        engine.setMidiFx(3, { transposeSemitones: 12 });
        engine.clearMidiFx(3);
        expect(posted.map((message) => (message as { type?: unknown }).type)).toEqual([
          'syncLoadSoundFont',
          'syncBuiltinInstrument',
          'syncSynthInstrument',
          'syncSf2Instrument',
          'syncMidiUmp',
          'syncMidiFx',
          'syncClearMidiFx',
        ]);

        posted.length = 0;
        engine.transport.stop();
        expect(
          posted.filter((message) =>
            /Instrument|SoundFont|MidiFx/.test(String((message as { type?: unknown }).type)),
          ),
        ).toEqual([]);
      } finally {
        engine.destroy();
      }
    });

    /** A SonareEngine whose port only records what it posted. */
    async function recordingEngine(options: Record<string, unknown> = {}) {
      const posted: unknown[] = [];
      const port: { onmessage?: ((event: MessageEvent<unknown>) => void) | null } & Record<
        string,
        unknown
      > = { postMessage: (message: unknown) => posted.push(message) };
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () => readyWorkletNode(port),
        ...options,
      });
      const offline = (
        engine as unknown as { offlineEngine: Record<string, (...a: never[]) => unknown> }
      ).offlineEngine;
      const postedTypes = () => posted.map((message) => (message as { type?: unknown }).type);
      return { engine, offline, posted, postedTypes };
    }

    it('changes one clip on the offline mirror without resending the rest', async () => {
      const { engine, offline } = await recordingEngine();
      try {
        engine.setTrackLanes([{ trackId: 3 }]);
        engine.addClip(3, { channels: [new Float32Array(64).fill(0.25)], startPpq: 0, id: 1 });
        const setClips = vi.spyOn(offline as never, 'setClips' as never);
        const upsertClip = vi.spyOn(offline as never, 'upsertClip' as never);
        const removeClip = vi.spyOn(offline as never, 'removeClip' as never);
        engine.addClip(3, { channels: [new Float32Array(64).fill(0.5)], startPpq: 0, id: 2 });
        engine.removeClip(2);
        engine.removeClip(99);
        expect(setClips).not.toHaveBeenCalled();
        expect(upsertClip).toHaveBeenCalledTimes(1);
        expect(removeClip).toHaveBeenCalledTimes(1);
        expect(offline.clipCount as unknown as number).toBe(1);
      } finally {
        engine.destroy();
      }
    });

    it('changes neither engine when the live command ring has no room', async () => {
      const { engine, offline } = await recordingEngine({ mode: 'sab', commandRingCapacity: 1 });
      try {
        engine.setTrackLanes([{ trackId: 3 }]);
        engine.addParameter({ id: 7, name: 'p' });
        engine.addMarker(0, 'a');
        engine.configureCapture({ capacityFrames: 128 });
        // Nothing drains the ring here, so one queued command fills it.
        expect(engine.transport.play()).toBeUndefined();
        const mutators = [
          'setParameter',
          'setTrackSoloMute',
          'setTrackMonitorMode',
          'setLoop',
          'seekMarker',
          'seekPpq',
          'seekSample',
          'armCapture',
          'setMetronome',
          'resetProcessorState',
          'resetMasterLoudnessMeter',
        ];
        const spies = mutators.map((name) => vi.spyOn(offline as never, name as never));
        const before = (offline.transportState as () => unknown)();
        expect(() => engine.setParam('', 7, 0.5)).toThrow(/command queue is full/);
        expect(() => engine.setTrackSoloMute(3, { solo: true, mute: true })).toThrow(
          /command queue is full/,
        );
        expect(() => engine.setTrackMonitorMode(3, 'pfl')).toThrow(/command queue is full/);
        expect(() => engine.setLoop({ startPpq: 0, endPpq: 4 })).toThrow(/command queue is full/);
        expect(() => engine.seekMarker(engine.markers()[0].id)).toThrow(/command queue is full/);
        expect(() => engine.transport.seekPpq(8)).toThrow(/command queue is full/);
        expect(() => engine.transport.seekSec(1)).toThrow(/command queue is full/);
        // A transport method is a field's member, named one level deeper than the class.
        expect(() => engine.transport.seekPpq(8)).toThrow(
          /^SonareEngine\.transport\.seekPpq: the engine command queue is full$/,
        );
        expect(() => engine.transport.play()).toThrow(
          /^SonareEngine\.transport\.play: the engine command queue is full$/,
        );
        expect(() => engine.armCapture({ armed: true })).toThrow(/command queue is full/);
        expect(() => engine.setMetronome({ enabled: true })).toThrow(/command queue is full/);
        expect(() => engine.resetProcessorState()).toThrow(/command queue is full/);
        expect(() => engine.resetMasterLoudnessMeter()).toThrow(/command queue is full/);
        for (const spy of spies) {
          expect(spy).not.toHaveBeenCalled();
        }
        expect((offline.transportState as () => unknown)()).toEqual(before);
        expect((offline.captureStatus as () => { armed: boolean })().armed).toBe(false);
      } finally {
        engine.destroy();
      }
    });

    it('leaves caches, mirror and worklet untouched when the offline engine refuses an edit', async () => {
      const { engine, offline, postedTypes } = await recordingEngine();
      try {
        engine.setTrackLanes([{ trackId: 3 }]);
        engine.setTempo(100);
        engine.setAutomationLane(7, [{ ppq: 0, value: 0.25 }]);
        const clipId = engine.addClip(3, {
          channels: [new Float32Array(64).fill(0.5)],
          startPpq: 0,
        });
        engine.setMidiClips([{ id: 50, destinationId: 3, startPpq: 0, events: [] }]);
        const state = engine as unknown as {
          tempoBpm: number;
          tempoSegments: unknown[];
          automationLanes: Map<number, unknown>;
          clips: Map<number, unknown>;
          midiClips: Map<number, unknown>;
        };
        const snapshot = () => ({
          bpm: state.tempoBpm,
          segments: structuredClone(state.tempoSegments),
          lanes: structuredClone([...state.automationLanes]),
          clips: [...state.clips.keys()],
          midiClips: [...state.midiClips.keys()],
        });
        const before = snapshot();
        const postedBefore = postedTypes().length;

        expect(() => engine.setTempo(Number.NaN)).toThrow();
        expect(() => engine.setAutomationLane(7, [{ ppq: 0, value: Number.NaN }])).toThrow();
        expect(() => engine.scheduleParam('', 7, { ppq: 1, value: Number.NaN })).toThrow();
        expect(() => engine.addClip(3, { channels: [new Float32Array(0)], startPpq: 0 })).toThrow();
        expect(() =>
          engine.setMidiClips([{ id: 51, destinationId: 3, startPpq: 0, gain: -1, events: [] }]),
        ).toThrow();
        expect(snapshot()).toEqual(before);
        expect(postedTypes().length).toBe(postedBefore);

        // Later valid edits carry none of the refused input.
        engine.setTimeSignature(3, 4);
        engine.removeClip(clipId);
        expect(state.clips.size).toBe(0);
        expect((offline.transportState as () => { bpm: number })().bpm).toBe(100);
      } finally {
        engine.destroy();
      }
    });

    it('keeps the recording when a capture configuration is refused', async () => {
      const { engine, offline, postedTypes } = await recordingEngine();
      try {
        engine.configureCapture({ capacityFrames: 256, numChannels: 1, source: 'input' });
        const capture = offline as unknown as {
          armCapture(state: { armed: boolean }): void;
          play(): void;
          process(inputs: Float32Array[]): Float32Array[];
          captureStatus(): { capturedFrames: number; source: string };
          capturedAudio(): Float32Array[];
        };
        capture.armCapture({ armed: true });
        capture.play();
        capture.process([new Float32Array(128).fill(0.5), new Float32Array(128).fill(0.5)]);
        const frames = capture.captureStatus().capturedFrames;
        expect(frames).toBeGreaterThan(0);
        const audio = capture.capturedAudio();
        const posted = postedTypes().length;
        for (const bad of [
          { capacityFrames: 512, inputMonitor: { enabled: true, gain: Number.NaN } },
          { capacityFrames: 512, inputMonitor: { enabled: true, gain: 1e39 } },
          { capacityFrames: 512, source: 'sideways' as never },
        ]) {
          expect(() => engine.configureCapture(bad)).toThrow(RangeError);
        }
        expect(capture.captureStatus().capturedFrames).toBe(frames);
        expect(capture.captureStatus().source).toBe('input');
        expect(capture.capturedAudio()).toEqual(audio);
        expect(postedTypes().length).toBe(posted);
        // The buffer shape cannot change while armed, so the accepted call keeps it.
        expect(() =>
          engine.configureCapture({ capacityFrames: 256, numChannels: 1 }),
        ).not.toThrow();
      } finally {
        engine.destroy();
      }
    });

    it('refuses a sampleRate that differs from the AudioContext before creating a node', async () => {
      const factory = vi.fn(() => ({ port: {}, disconnect: () => undefined }) as never);
      await expect(
        SonareRealtimeEngineNode.create(fakeContext(), {
          numChannels: 2,
          sampleRate: 44100,
          nodeFactory: factory,
        }),
      ).rejects.toThrow(RangeError);
      await expect(
        SonareEngine.create(fakeContext(), {
          numChannels: 2,
          sampleRate: 44100,
          nodeFactory: factory,
        }),
      ).rejects.toThrow(RangeError);
      expect(factory).not.toHaveBeenCalled();
      const port: Record<string, unknown> = { postMessage: () => undefined };
      const node = await SonareRealtimeEngineNode.create(fakeContext(), {
        numChannels: 2,
        sampleRate: 48000,
        mode: 'postMessage',
        nodeFactory: () => readyWorkletNode(port),
      });
      node.destroy();
    });

    it('pages a long clip whether warp-off is omitted, 0 or "off"', async () => {
      const posted: unknown[] = [];
      const port: { onmessage?: ((event: MessageEvent<unknown>) => void) | null } & Record<
        string,
        unknown
      > = { postMessage: (message: unknown) => posted.push(message) };
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () => readyWorkletNode(port),
      });
      try {
        engine.setTrackLanes([{ trackId: 3 }]);
        const spellings = [{}, { warpMode: 'off' as const }];
        for (const [index, spelling] of spellings.entries()) {
          posted.length = 0;
          const id = 900 + index;
          engine.addClip(3, { channels: [new Float32Array(20_000)], startPpq: 0, id, ...spelling });
          const types = posted
            .filter((message) => (message as { clipId?: unknown }).clipId === id)
            .map((message) => (message as { type: string }).type);
          expect(types[0]).toBe('syncClipPageProvider');
          expect(types.at(-1)).toBe('syncClipPageCommit');
        }
      } finally {
        engine.destroy();
      }
    });

    it('refuses a sample-bank synth patch before the offline engine or the worklet changes', async () => {
      const posted: unknown[] = [];
      const port: { onmessage?: ((event: MessageEvent<unknown>) => void) | null } & Record<
        string,
        unknown
      > = { postMessage: (message: unknown) => posted.push(message) };
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () => readyWorkletNode(port),
      });
      try {
        engine.setTrackLanes([{ trackId: 3 }]);
        const offline = (engine as unknown as { offlineEngine: { setSynthInstrument: unknown } })
          .offlineEngine;
        const offlineSet = vi.spyOn(
          offline as { setSynthInstrument: () => void },
          'setSynthInstrument',
        );
        for (const playing of [false, true]) {
          if (playing) {
            engine.transport.play();
          }
          posted.length = 0;
          expect(() =>
            engine.setSynthInstrument(3, { kind: 'sample', sampleBank: {} } as never),
          ).toThrow(expect.objectContaining({ codeName: 'NotSupported' }));
          expect(offlineSet).not.toHaveBeenCalled();
          expect(
            posted.filter(
              (message) => (message as { type?: unknown }).type === 'syncSynthInstrument',
            ),
          ).toEqual([]);
        }
        engine.transport.stop();
        expect(
          posted.filter(
            (message) => (message as { type?: unknown }).type === 'syncSynthInstrument',
          ),
        ).toEqual([]);
        engine.setSynthInstrument(3, { preset: 'sawLead' });
        expect(offlineSet).toHaveBeenCalledTimes(1);
      } finally {
        engine.destroy();
      }
    });

    it('creates a SAB-backed AudioWorkletNode facade and queues transport commands', async () => {
      let capturedOptions: AudioWorkletNodeOptions | undefined;
      const posted: unknown[] = [];
      const disconnected: boolean[] = [];
      const node = await SonareRealtimeEngineNode.create(fakeContext(), {
        moduleUrl: 'sonare-worklet.js',
        maxBlockSize: 128,
        numChannels: 2,
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
      expect(node.clipPageRequestRing).toBeDefined();
      expect(node.capabilities.clipPageRequestsRealtimeSafe).toBe(true);
      expect(capturedOptions?.processorOptions).toMatchObject({
        sampleRate: 48000,
        maxBlockSize: 128,
        numChannels: 2,
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
      expect(
        node.sendCommand({
          type: SonareEngineCommandType.SetTrackMonitorMode,
          targetId: 3,
          renderFrame: 256,
          argInt: 1,
        }),
      ).toBe(true);
      expect(popSonareEngineCommandRingBuffer(commandRing)).toEqual({
        type: SonareEngineCommandType.SetTrackMonitorMode,
        targetId: 3,
        renderFrame: 256,
        argFloat: 0,
        argInt: 1,
        syncSequence: 0,
      });
      // 18..25 are intentionally not part of the worklet command vocabulary.
      expect(node.sendCommand({ type: 18, renderFrame: -1 })).toBe(false);
      expect(
        node.sendCommand({
          type: SonareEngineCommandType.SetTrackMonitorMode,
          renderFrame: -1,
          argInt: 0.5,
        }),
      ).toBe(false);
      for (const targetId of [undefined, -1, 0x1_0000_0000]) {
        expect(
          node.sendCommand({
            type: SonareEngineCommandType.SetTrackMonitorMode,
            targetId,
            renderFrame: -1,
            argInt: 1,
          }),
        ).toBe(false);
      }
      // Every command carries its targetId through the same uint32 slot, so the
      // domain cannot be a property of the monitor-mode type alone. This path
      // answers false where the record writer throws.
      expect(
        node.sendCommand({ type: SonareEngineCommandType.TransportSeekSample, targetId: 5 }),
      ).toBe(true);
      expect(popSonareEngineCommandRingBuffer(commandRing)).toMatchObject({ targetId: 5 });
      for (const targetId of [-1, 0x1_0000_0000, 5.5]) {
        expect(
          node.sendCommand({ type: SonareEngineCommandType.TransportSeekSample, targetId }),
        ).toBe(false);
      }
      expect(popSonareEngineCommandRingBuffer(commandRing)).toBeNull();
      // A 64-bit lane the record writer would refuse by throwing answers false instead.
      expect(
        node.sendCommand({ type: SonareEngineCommandType.TransportSeekSample, argInt: 2n ** 60n }),
      ).toBe(false);
      expect(
        node.sendCommand({ type: SonareEngineCommandType.TransportSeekSample, argInt: 2n ** 40n }),
      ).toBe(true);
      expect(popSonareEngineCommandRingBuffer(commandRing)).toMatchObject({ argInt: 2 ** 40 });
      // The live engine refuses a negative position and a PPQ outside [0, 1e12].
      expect(node.seekSample(-1)).toBe(false);
      for (const ppq of [-1, 2e12, Number.NaN]) {
        expect(node.seekPpq(ppq)).toBe(false);
      }
      expect(node.seekPpq(1e12)).toBe(true);
      expect(popSonareEngineCommandRingBuffer(commandRing)).toMatchObject({ argFloat: 1e12 });
      // A punch window carries whole samples; a fractional or inverted one is refused.
      for (const [inSample, outSample] of [
        [0, 4.5],
        [-1, 4],
        [8, 4],
      ]) {
        expect(
          node.sendCommand({
            type: SonareEngineCommandType.Punch,
            renderFrame: -1,
            argInt: inSample,
            argFloat: outSample,
          }),
        ).toBe(false);
      }
      // A ring command names the last sync posted before it, which the worklet applies first.
      expect(node.play()).toBe(true);
      expect(popSonareEngineCommandRingBuffer(commandRing)).toMatchObject({ syncSequence: 0 });
      node.postSync({ type: 'syncMarkers', markers: [] });
      expect(posted.at(-1)).toMatchObject({ type: 'syncMarkers', syncSequence: 1 });
      expect(node.play()).toBe(true);
      expect(popSonareEngineCommandRingBuffer(commandRing)).toMatchObject({ syncSequence: 1 });
      expect(popSonareEngineCommandRingBuffer(commandRing)).toBeNull();

      writeSonareEngineTelemetryRingBuffer(telemetryRing, {
        type: SonareEngineTelemetryType.ProcessBlock,
        error: SonareEngineTelemetryError.None,
        renderFrame: 0,
        timelineSample: 128,
        audibleTimelineSample: 128,
        graphLatencySamples: 0,
        value: 128,
      });
      const seen: unknown[] = [];
      node.onTelemetry((telemetry) => seen.push(telemetry));
      // Listener registration immediately drains SAB records, without asking
      // the host to call pollTelemetry() itself.
      expect(node.pollTelemetry()).toHaveLength(0);
      expect(seen[0]).toMatchObject({ timelineSample: 128 });
      node.destroy();
      expect(disconnected).toEqual([true]);
      expect(posted.at(-1)).toMatchObject({ type: 'destroy' });
    });

    it('drains clip-page requests from the SAB ring and reports bounded drops', async () => {
      const node = await SonareRealtimeEngineNode.create(fakeContext(), {
        numChannels: 2,
        clipPageRequestRingCapacity: 1,
        nodeFactory: () => readyWorkletNode({ postMessage: () => undefined, onmessage: undefined }),
      });
      try {
        const ring = node.clipPageRequestRing;
        if (!ring) {
          throw new Error('expected clip-page request ring');
        }
        expect(pushSonareClipPageRequestRingBuffer(ring, 77, 2)).toBe(true);
        expect(pushSonareClipPageRequestRingBuffer(ring, 78, 3)).toBe(false);
        const seen: unknown[] = [];
        node.onClipPageRequests((message) => seen.push(message));
        expect(node.pollClipPageRequests()).toEqual({
          type: 'clipPageRequest',
          requests: [{ clipId: 77, pageIndex: 2 }],
          dropped: 1,
        });
        expect(seen).toHaveLength(1);
        expect(node.pollClipPageRequests()).toBeUndefined();
      } finally {
        node.destroy();
      }
    });

    it('waits for ready even when the host registered the worklet module', async () => {
      const port = {
        postMessage: () => undefined,
        onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
      };
      const enginePromise = SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () => ({ port, disconnect: () => undefined }) as unknown as AudioWorkletNode,
      });
      let settled = false;
      void enginePromise.then(() => {
        settled = true;
      });
      await Promise.resolve();
      expect(settled).toBe(false);
      port.onmessage?.({
        data: { type: 'ready', runtimeTarget: 'embind' },
      } as MessageEvent<unknown>);
      const engine = await enginePromise;
      engine.destroy();
    });

    it('rejects a block size smaller than the AudioWorklet render quantum', async () => {
      await expect(
        SonareRealtimeEngineNode.create(fakeContext(), {
          numChannels: 2,
          maxBlockSize: 64,
          nodeFactory: () =>
            readyWorkletNode({ postMessage: () => undefined, onmessage: undefined }),
        }),
      ).rejects.toThrow(/maxBlockSize.*128/);
    });

    it('surfaces worklet sync rejections after ready', async () => {
      const port = {
        postMessage: () => undefined,
        onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
      };
      const node = await SonareRealtimeEngineNode.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () => ({ port, disconnect: () => undefined }) as unknown as AudioWorkletNode,
      });
      const seen: unknown[] = [];
      node.onSyncError((message) => seen.push(message));
      port.onmessage?.({
        data: { type: 'syncError', syncType: 'syncTempo', message: 'invalid tempo' },
      } as MessageEvent<unknown>);
      expect(seen).toEqual([
        { type: 'syncError', syncType: 'syncTempo', message: 'invalid tempo' },
      ]);
    });

    it('names a worklet sync rejection with the entry whose call posted the sync', async () => {
      const posted: unknown[] = [];
      const livePosted: unknown[] = [];
      const port = {
        postMessage: (message: unknown) => posted.push(message),
        onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
      };
      const offline = new (await import('../dist/index.js')).RealtimeEngine({
        sampleRate: 48000,
        maxBlockSize: 128,
      }) as unknown as OfflineEngineOption;
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        offlineEngine: offline,
        offlineNumChannels: 2,
        nodeFactory: () => readyWorkletNode(port),
      });
      const live = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        maxBlockSize: 128,
        numChannels: 2,
        transport: { postMessage: (message) => livePosted.push(message) },
      });
      try {
        const seen: Array<{ message: string; failure?: Error }> = [];
        engine.onSyncError((message) => seen.push(message));
        engine.setTempo(120);
        const sync = posted.at(-1) as { type: string; entry?: string; bpm: number };
        expect(sync).toMatchObject({ type: 'syncTempo', entry: 'SonareEngine.setTempo' });
        // The live engine refuses what the offline mirror accepted.
        live.receiveMessage({
          ...sync,
          tempoSegments: undefined,
          bpm: Number.NaN,
        });
        const reply = livePosted.at(-1) as { type: string; entry?: string; message: string };
        expect(reply).toMatchObject({ type: 'syncError', entry: 'SonareEngine.setTempo' });
        port.onmessage?.({ data: reply } as MessageEvent<unknown>);
        expect(seen).toHaveLength(1);
        // One prefix, the consumer's entry, even though the audio thread raised through its own.
        expect(seen[0].message).toBe('SonareEngine.setTempo: bpm must be a finite number');
        expect(seen[0].message).not.toContain('RealtimeEngine.');
        expect(seen[0].failure?.message).toBe(seen[0].message);
      } finally {
        live.destroy();
        engine.destroy();
      }
    });

    it('stops delivering sync rejections after destroy', async () => {
      const port = {
        postMessage: () => undefined,
        onmessage: undefined as ((event: MessageEvent<unknown>) => void) | null | undefined,
      };
      const node = await SonareRealtimeEngineNode.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () => ({ port, disconnect: () => undefined }) as unknown as AudioWorkletNode,
      });
      const seen: unknown[] = [];
      node.onSyncError((message) => seen.push(message));
      // The worklet tears down asynchronously, so a syncError can still be in
      // flight when the host disposes the node. Keep a reference to the handler
      // to prove the guard holds even for a caller that kept its own.
      const retained = port.onmessage;
      node.destroy();
      expect(port.onmessage).toBeNull();
      retained?.({
        data: { type: 'syncError', syncType: 'syncTempo', message: 'invalid tempo' },
      } as MessageEvent<unknown>);
      expect(seen).toEqual([]);
    });

    it('propagates a pre-registered worklet initialization error', async () => {
      const port = {
        postMessage: () => undefined,
        onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
      };
      const enginePromise = SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () => ({ port, disconnect: () => undefined }) as unknown as AudioWorkletNode,
      });
      await Promise.resolve();
      port.onmessage?.({
        data: { type: 'error', message: 'WASM initialization failed' },
      } as MessageEvent<unknown>);
      await expect(enginePromise).rejects.toThrow('WASM initialization failed');
    });

    it('drains the offline mirror after more commands than its realtime ring capacity', async () => {
      // The index and worklet bundles each emit a self-contained .d.ts, so
      // RealtimeEngine is declared twice and its private field makes the two
      // copies nominally distinct. Same class at runtime.
      const offline = new (await import('../dist/index.js')).RealtimeEngine({
        sampleRate: 48000,
        maxBlockSize: 128,
      }) as unknown as OfflineEngineOption;
      offline.prepare({
        sampleRate: 48000,
        maxBlockSize: 128,
        commandCapacity: 4,
        telemetryCapacity: 4,
      });
      offline.addParameter({
        id: 7,
        name: 'gain',
        unit: 'dB',
        minValue: -60,
        maxValue: 12,
        defaultValue: 0,
        rtSafe: true,
        defaultCurve: 'hold',
      });
      const originalFlush = offline.applyCommandsDueNowPreservingFuture.bind(offline);
      let flushCount = 0;
      offline.applyCommandsDueNowPreservingFuture = () => {
        flushCount += 1;
        originalFlush();
      };
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        offlineEngine: offline,
        nodeFactory: () => readyWorkletNode({ postMessage: () => undefined, onmessage: undefined }),
      });
      try {
        // Engine creation synchronizes the pre-registered parameter set once;
        // this assertion measures only the subsequent command-side flushes.
        flushCount = 0;
        for (let index = 0; index < 32; index++) {
          expect(engine.setParam('gain-node', 'gain', index - 60)).toBeUndefined();
        }
        expect(flushCount).toBe(32);
      } finally {
        engine.destroy();
      }
    });

    it('syncs registered parameters and rejects unresolved parameter names', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      try {
        engine.addParameter({
          id: 7,
          name: 'gain',
          unit: 'dB',
          minValue: -60,
          maxValue: 12,
          defaultValue: 0,
          rtSafe: true,
          defaultCurve: 'hold',
        });
        expect(engine.setParam('gain-node', 'gain', -6)).toBeUndefined();
        expect(() => engine.setParam('gain-node', 'missing', -6)).toThrow(
          /Unknown engine parameter/,
        );
        engine.setMidiInputSource(3);
        engine.bindMidiCc({ channel: 0, ccNumber: 74, paramId: 7, minValue: -60, maxValue: 12 });
        engine.pushMidiInputNoteOn(0, 0, 60, 100, 128);
        engine.pushMidiInputCc(0, 0, 74, 96, 128);
        engine.clearMidiInputSource();
        engine.clearParameters();
        expect(engine.parameters()).toHaveLength(0);
        expect(posted).toEqual(
          expect.arrayContaining([
            expect.objectContaining({
              type: 'syncParameters',
              parameters: [expect.objectContaining({ id: 7, name: 'gain' })],
            }),
            expect.objectContaining({ type: 'syncParameters', parameters: [] }),
            expect.objectContaining({ type: 'syncMidiInputSource', destinationId: 3 }),
            expect.objectContaining({
              type: 'syncMidiCcBinding',
              binding: { channel: 0, ccNumber: 74, paramId: 7, minValue: -60, maxValue: 12 },
            }),
            expect.objectContaining({
              type: 'syncMidiInputNoteOn',
              data0: 60,
              data1: 100,
              offsetInBlockSamples: 128,
            }),
            expect.objectContaining({ type: 'syncMidiInputCc', data0: 74, data1: 96 }),
            expect.objectContaining({ type: 'syncClearMidiInputSource' }),
          ]),
        );
      } finally {
        engine.destroy();
      }
    });

    it('mirrors array and typed-array UMP plus raw input without aliasing caller buffers, and refuses a bare number', async () => {
      const posted: unknown[] = [];
      const offline = new (await import('../dist/index.js')).RealtimeEngine({
        sampleRate: 48000,
        maxBlockSize: 128,
      }) as unknown as OfflineEngineOption;
      const destinationUmp = vi.spyOn(offline, 'pushMidiUmp').mockImplementation(() => undefined);
      const inputUmp = vi.spyOn(offline, 'pushMidiInputUmp').mockImplementation(() => undefined);
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        offlineEngine: offline,
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      try {
        const words = new Uint32Array([0x41923c00, 0x12345678]);
        engine.pushMidiUmp(3, [0x20903c64], 128);
        engine.pushMidiUmp(3, words, 256);
        words[1] = 0;

        engine.setMidiInputSource(3);
        const inputWords = new Uint32Array([0x41923c00, 0x87654321]);
        engine.pushMidiInputUmp(inputWords, 512);
        inputWords[1] = 0;

        expect(destinationUmp).toHaveBeenCalledWith(3, [0x20903c64], 128);
        expect(destinationUmp).toHaveBeenCalledWith(
          3,
          Uint32Array.from([0x41923c00, 0x12345678]),
          256,
        );
        expect(inputUmp).toHaveBeenCalledWith(Uint32Array.from([0x41923c00, 0x87654321]), 512);
        expect(posted).toEqual(
          expect.arrayContaining([
            {
              type: 'syncMidiUmp',
              destinationId: 3,
              words: [0x20903c64],
              renderFrame: 128,
              entry: 'SonareEngine.pushMidiUmp',
            },
            {
              type: 'syncMidiUmp',
              destinationId: 3,
              words: Uint32Array.from([0x41923c00, 0x12345678]),
              renderFrame: 256,
              entry: 'SonareEngine.pushMidiUmp',
            },
            {
              type: 'syncMidiInputSource',
              destinationId: 3,
              entry: 'SonareEngine.setMidiInputSource',
            },
            {
              type: 'syncMidiInputUmp',
              words: Uint32Array.from([0x41923c00, 0x87654321]),
              offsetInBlockSamples: 512,
              entry: 'SonareEngine.pushMidiInputUmp',
            },
          ]),
        );

        destinationUmp.mockRestore();
        posted.length = 0;
        expect(() => engine.pushMidiUmp(3, 0x20903c64 as unknown as number[])).toThrow(TypeError);
        expect(posted).toEqual([]);
      } finally {
        engine.destroy();
      }
    });

    it('creates the scope ring only when scope telemetry is requested', async () => {
      const makeNode = (scopeIntervalFrames?: number) =>
        SonareRealtimeEngineNode.create(fakeContext(), {
          moduleUrl: 'sonare-worklet.js',
          maxBlockSize: 128,
          numChannels: 2,
          scopeIntervalFrames,
          scopeBands: 32,
          nodeFactory: (_context, _name, options) => {
            lastOptions = options;
            return {
              port: { postMessage: () => undefined, onmessage: undefined },
              disconnect: () => undefined,
            } as unknown as AudioWorkletNode;
          },
        });
      let lastOptions: AudioWorkletNodeOptions | undefined;

      const off = await makeNode();
      expect(off.scopeRing).toBeUndefined();
      expect(off.pollScope()).toEqual([]);
      expect(
        (lastOptions?.processorOptions as { scopeSharedBuffer?: SharedArrayBuffer })
          ?.scopeSharedBuffer,
      ).toBeUndefined();
      off.destroy();

      const on = await makeNode(128);
      expect(on.scopeRing).toBeDefined();
      expect(on.scopeRing?.bands).toBe(32);
      expect(
        (lastOptions?.processorOptions as { scopeSharedBuffer?: SharedArrayBuffer })
          ?.scopeSharedBuffer,
      ).toBeInstanceOf(SharedArrayBuffer);
      on.destroy();
    });

    it('refuses scopeBands past the 64-band ceiling instead of clamping it', async () => {
      const create = (scopeBands: number) =>
        SonareRealtimeEngineNode.create(fakeContext(), {
          numChannels: 2,
          moduleUrl: 'sonare-worklet.js',
          scopeIntervalFrames: 128,
          scopeBands,
          nodeFactory: () =>
            ({
              port: { postMessage: () => undefined, onmessage: undefined },
              disconnect: () => undefined,
            }) as unknown as AudioWorkletNode,
        });
      await expect(create(65)).rejects.toThrow(RangeError);
      await expect(create(0)).rejects.toThrow(RangeError);
      const atCeiling = await create(64);
      expect(atCeiling.scopeRing?.bands).toBe(64);
      atCeiling.destroy();
    });

    it('falls back to postMessage commands when requested', async () => {
      const posted: unknown[] = [];
      const node = await SonareRealtimeEngineNode.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      expect(node.capabilities.mode).toBe('postMessage');
      expect(node.capabilities.clipPageRequestsRealtimeSafe).toBe(false);
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
          numChannels: 2,
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

    it('rejects an ABI mismatch before constructing an AudioWorkletNode', async () => {
      let constructed = false;
      await expect(
        SonareRealtimeEngineNode.create(fakeContext(), {
          numChannels: 2,
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
      // The index and worklet bundles each emit a self-contained .d.ts, so
      // RealtimeEngine is declared twice and its private field makes the two
      // copies nominally distinct. Same class at runtime.
      const offline = new (await import('../dist/index.js')).RealtimeEngine({
        sampleRate: 48000,
        maxBlockSize: 128,
      }) as unknown as OfflineEngineOption;
      offline.addParameter({
        id: 7,
        name: 'gain',
        unit: 'dB',
        minValue: -60,
        maxValue: 12,
        defaultValue: 0,
        rtSafe: true,
        defaultCurve: 'hold', // canonical AutomationCurve::Hold (preserve original semantic)
      });
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        offlineEngine: offline,
        offlineNumChannels: 2,
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      try {
        expect(engine.capabilities.mode).toBe('postMessage');
        expect(engine.parameters()).toHaveLength(1);
        expect(posted).toEqual(
          expect.arrayContaining([
            expect.objectContaining({
              type: 'syncParameters',
              parameters: [expect.objectContaining({ id: 7, name: 'gain' })],
            }),
          ]),
        );
        expect(engine.transport.play()).toBeUndefined();
        expect(engine.transport.seekSec(1)).toBeUndefined();
        engine.transport.setTempo(90);
        engine.transport.setTempoSegments([
          { startPpq: 0, bpm: 90 },
          { startPpq: 4, bpm: 60 },
        ]);
        expect(engine.transport.setLoop({ startPpq: 0, endPpq: 1, enabled: true })).toBeUndefined();
        expect(engine.setParam('gain-node', 'gain', -6)).toBeUndefined();
        engine.scheduleParam('gain-node', 'gain', { ppq: 0.5, value: -3 });
        engine.addAutomationPoint(7, { ppq: 1, value: 0 });
        expect(engine.setTrackSoloMute(3, { solo: true, mute: false })).toBeUndefined();
        expect(engine.setStripFaderDb(3, -6)).toBeUndefined();
        expect(engine.setStripPan(3, 0.25)).toBeUndefined();
        const trackStripJson =
          '{"version":2,"strips":[{"id":"track-3","faderDb":-6,"panLaw":"linear0Db","inserts":[{"slot":"pre","processor":"eq.parametric","params":{"band0.type":1,"band0.frequencyHz":1000,"band0.gainDb":12,"band0.enabled":1}}]}],"buses":[],"connections":[]}';
        const masterStripJson =
          '{"version":2,"strips":[{"id":"master","faderDb":-3,"panLaw":"linear0Db","inserts":[{"slot":"pre","processor":"eq.parametric","params":{"band0.type":1,"band0.frequencyHz":1000,"band0.gainDb":12,"band0.enabled":1}}]}],"buses":[],"connections":[]}';
        engine.setStrip(3, stripDocument(trackStripJson));
        engine.setStrip('master', stripDocument(masterStripJson));
        expect(engine.setStripFaderDb('master', -3)).toBeUndefined();
        expect(engine.setStripPan('master', -0.25)).toBeUndefined();
        engine.setStripEqBand(3, 0, { type: 'peak', frequencyHz: 1000, gainDb: 6 });
        engine.setStripEqBand('master', 0, { type: 'peak', frequencyHz: 1000, gainDb: 3 });
        engine.setStripInsertBypassed(3, 0, { bypassed: true, resetOnBypass: true });
        engine.setStripInsertBypassed('master', 0, { bypassed: true, resetOnBypass: true });
        engine.setStripEqBand(3, 0, { type: 'peak', frequencyHz: 2000, gainDb: 2 });
        engine.setStripEqBand('master', 0, { type: 'peak', frequencyHz: 3000, gainDb: 1 });
        engine.setStripInsertBypassed(3, 0, { bypassed: false });
        engine.setStripInsertBypassed('master', 0, { bypassed: false });
        engine.setStrip(3, stripDocument(trackStripJson));
        engine.setStrip('master', stripDocument(masterStripJson));
        engine.setTrackBuses([{ busId: 100, gainDb: -3 }]);
        engine.setTrackSends(3, [{ busId: 100, levelDb: -6, enabled: true }]);
        expect(engine.setBusGainDb(100, -9)).toBeUndefined();
        // Realtime strip panner / channel-delay controls (R5).
        engine.setStripPan(3, -1);
        engine.setStripPanLaw(3, 'const6Db');
        engine.setStripPanMode(3, 'dualPan');
        engine.setStripDualPan(3, -1, 1);
        engine.setStripChannelDelaySamples(3, 32);
        const busStripJson =
          '{"version":2,"strips":[],"buses":[{"id":"100","inserts":[{"slot":"pre","processor":"eq.parametric","params":{"band0.type":1,"band0.frequencyHz":1000,"band0.gainDb":0,"band0.enabled":1}}]}],"connections":[]}';
        engine.setBusStrip(100, busDocument(busStripJson));
        engine.setBusInsertParam(100, 0, { name: 'band0.gainDb', value: 1 });
        engine.setBuiltinInstrument(3, { gain: 0.5 });
        engine.setSynthInstrument(3, { preset: 'sawLead' });
        engine.setSf2Instrument(3, { gain: 0.5 });
        // Live, non-destructive MIDI-FX insert (install then bypass).
        engine.setMidiFx(3, { transposeSemitones: 12 });
        engine.clearMidiFx(3);
        engine.setMidiClips([
          {
            id: 501,
            trackId: 3,
            destinationId: 3,
            lengthSamples: 8192,
            events: [
              { renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 },
              { renderFrame: 4096, word0: midi1Word(0x8, 0, 60, 0), wordCount: 1 },
            ],
          },
        ]);
        engine.pushMidiNoteOn(3, 0, 0, 64, 100);
        engine.pushMidiNoteOff(3, 0, 0, 64, 0);
        engine.pushMidiCc(3, 0, 0, 74, 100);
        engine.pushMidiSysex(3, new Uint8Array([0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7]));
        // Live GS insertion-effect (EFX) SysEx: select EFX TYPE = Overdrive
        // (MSB 0x01, LSB 0x10) on the SF2 instrument. This drives the control-
        // thread realize path (on_control_sysex -> insert factory) that
        // setSf2Instrument now wires; the injected factory must build the chain
        // without throwing. GS DT1: F0 41 10 42 12 <addr> <data> <checksum> F7.
        engine.pushMidiSysex(
          3,
          // addr 40 03 00 = EFX TYPE MSB = 0x01, checksum 0x3c
          new Uint8Array([0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x00, 0x01, 0x3c, 0xf7]),
        );
        engine.pushMidiSysex(
          3,
          // addr 40 03 01 = EFX TYPE LSB = 0x10, checksum 0x2c
          new Uint8Array([0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x01, 0x10, 0x2c, 0xf7]),
        );
        engine.pushMidiPanic();
        const clipId = engine.addClip(3, {
          channels: [new Float32Array(128).fill(0.25), new Float32Array(128).fill(-0.25)],
          startPpq: 0,
        });
        expect(clipId).toBeGreaterThan(0);
        engine.removeClip(clipId);
        expect(() => engine.armCapture({ armed: true })).toThrow(
          /Capture buffer is not configured/,
        );
        engine.configureCapture({
          capacityFrames: 4096,
          numChannels: 2,
          source: 'input',
          recordOffsetSamples: -32,
          inputMonitor: { enabled: true, gain: 0.5 },
        });
        engine.setTempo(60);
        expect(engine.countInEndSample(0, 2)).toBe(384000);
        expect(engine.armCapture({ armed: true })).toBeUndefined();
        engine.configureCapture({
          capacityFrames: 4096,
          numChannels: 2,
          source: 'input',
          recordOffsetSamples: -32,
          inputMonitor: { enabled: true, gain: 0.5 },
          punch: { startPpq: 1, endPpq: 1.5 },
        });
        engine.setMetronome({ enabled: true, clickSamples: 16 });
        const markerId = engine.addMarker(0, 'start');
        expect(markerId).toBeGreaterThan(0);
        // seekMarker now reaches the realtime engine (previously a no-op that
        // always returned false); it returns true like the sibling transport ops.
        expect(engine.seekMarker(markerId)).toBeUndefined();
        const rendered = await engine.renderOffline({ totalFrames: 128 });
        expect(rendered).toHaveLength(2);
        expect(rendered[0]).toHaveLength(128);
        expect(
          posted.some(
            (message) =>
              typeof message === 'object' &&
              message !== null &&
              (message as { type?: unknown }).type === 'syncBuiltinInstrument',
          ),
        ).toBe(true);
        expect(engine.transport.stop()).toBeUndefined();

        const track3Fader = engine.resolveStripAutomationId(3, 'faderDb');
        const track3Pan = engine.resolveStripAutomationId(3, 'pan');
        const bus100Fader = engine.resolveBusAutomationId(100, 'faderDb');
        expect(posted).toEqual(
          expect.arrayContaining([
            expect.objectContaining({ type: SonareEngineCommandType.TransportPlay }),
            expect.objectContaining({ type: SonareEngineCommandType.TransportSeekSample }),
            expect.objectContaining({ type: SonareEngineCommandType.SetLoop }),
            expect.objectContaining({ type: SonareEngineCommandType.ArmRecord }),
            expect.objectContaining({
              type: 'syncCapture',
              punch: { startSample: 48000, endSample: 72000 },
            }),
            expect.objectContaining({ type: SonareEngineCommandType.SetMetronome }),
            expect.objectContaining({ type: SonareEngineCommandType.TransportStop }),
            expect.objectContaining({
              type: SonareEngineCommandType.SetSoloMute,
              targetId: 0,
              argInt: 0x2,
            }),
            expect.objectContaining({
              type: SonareEngineCommandType.SetParamSmoothed,
              targetId: track3Fader,
              argFloat: -6,
            }),
            expect.objectContaining({
              type: SonareEngineCommandType.SetParamSmoothed,
              targetId: track3Pan,
              argFloat: 0.25,
            }),
            expect.objectContaining({
              type: SonareEngineCommandType.SetParamSmoothed,
              targetId: 0x4d58ff01,
              argFloat: -3,
            }),
            expect.objectContaining({
              type: SonareEngineCommandType.SetParamSmoothed,
              targetId: 0x4d58ff02,
              argFloat: -0.25,
            }),
            expect.objectContaining({
              type: SonareEngineCommandType.SetParamSmoothed,
              targetId: bus100Fader,
              argFloat: -9,
            }),
          ]),
        );
        expect(posted).toEqual(
          expect.arrayContaining([
            expect.objectContaining({ type: 'syncMixer', lanes: [{ trackId: 3 }] }),
            expect.objectContaining({
              type: 'syncMixer',
              buses: [{ busId: 100, gainDb: -3 }],
            }),
            expect.objectContaining({
              type: 'syncMixer',
              lanes: [{ trackId: 3, sends: [{ busId: 100, levelDb: -6, enabled: true }] }],
            }),
            expect.objectContaining({
              type: 'syncMixer',
              trackStrips: [{ trackId: 3, sceneJson: sceneOf('strips', trackStripJson) }],
              masterStripJson: sceneOf('strips', masterStripJson),
            }),
            expect.objectContaining({
              type: 'syncTrackStripEqBand',
              trackId: 3,
              bandIndex: 0,
              bandJson: expect.stringContaining('"frequencyHz":1000'),
            }),
            expect.objectContaining({
              type: 'syncMasterStripEqBand',
              bandIndex: 0,
              bandJson: expect.stringContaining('"frequencyHz":1000'),
            }),
            expect.objectContaining({
              type: 'syncTrackStripInsertBypassed',
              trackId: 3,
              insertIndex: 0,
              bypassed: true,
              resetOnBypass: true,
            }),
            expect.objectContaining({
              type: 'syncMasterStripInsertBypassed',
              insertIndex: 0,
              bypassed: true,
              resetOnBypass: true,
            }),
            expect.objectContaining({
              type: SonareEngineCommandType.SetParamSmoothed,
              targetId: track3Pan,
              argFloat: -1,
            }),
            expect.objectContaining({ type: 'syncTrackStripPanLaw', trackId: 3, panLaw: 2 }),
            expect.objectContaining({ type: 'syncTrackStripPanMode', trackId: 3, panMode: 2 }),
            expect.objectContaining({
              type: 'syncTrackStripDualPan',
              trackId: 3,
              leftPan: -1,
              rightPan: 1,
            }),
            expect.objectContaining({
              type: 'syncTrackStripChannelDelaySamples',
              trackId: 3,
              delaySamples: 32,
            }),
            expect.objectContaining({
              type: 'syncMixer',
              busStrips: [{ busId: 100, sceneJson: sceneOf('buses', busStripJson) }],
            }),
            expect.objectContaining({
              type: 'syncClipsDelta',
              upserts: [expect.objectContaining({ id: clipId, trackId: 3 })],
            }),
            expect.objectContaining({
              type: 'syncBuiltinInstrument',
              destinationId: 3,
              config: { gain: 0.5 },
            }),
            expect.objectContaining({
              type: 'syncSynthInstrument',
              destinationId: 3,
              patch: { preset: 'sawLead' },
            }),
            expect.objectContaining({
              type: 'syncSf2Instrument',
              destinationId: 3,
              config: { gain: 0.5 },
            }),
            expect.objectContaining({
              type: 'syncMidiFx',
              destinationId: 3,
              config: { transposeSemitones: 12 },
            }),
            expect.objectContaining({ type: 'syncClearMidiFx', destinationId: 3 }),
            expect.objectContaining({
              type: 'syncMidiClips',
              clips: [expect.objectContaining({ id: 501, destinationId: 3 })],
            }),
            expect.objectContaining({
              type: 'syncCapture',
              bufferFrames: 4096,
              channels: 2,
              source: 'input',
              recordOffsetSamples: -32,
              inputMonitor: { enabled: true, gain: 0.5 },
            }),
            expect.objectContaining({ type: 'syncMidiNoteOn', destinationId: 3, note: 64 }),
            expect.objectContaining({ type: 'syncMidiNoteOff', destinationId: 3, note: 64 }),
            expect.objectContaining({ type: 'syncMidiCc', destinationId: 3, controller: 74 }),
            expect.objectContaining({ type: 'syncMidiPanic' }),
          ]),
        );
        expect(posted).not.toEqual(
          expect.arrayContaining([
            expect.objectContaining({ type: SonareEngineCommandType.SetTempoMap }),
          ]),
        );
        // scheduleParam/addAutomationPoint mirror the lane to the live engine via
        // an out-of-band 'syncAutomation' message (previously offline-only).
        expect(posted).toEqual(
          expect.arrayContaining([expect.objectContaining({ type: 'syncAutomation', paramId: 7 })]),
        );
      } finally {
        engine.destroy();
      }
    });

    it('pre-bakes tempo-synced clips before posting them to the AudioWorklet', async () => {
      const posted: unknown[] = [];
      // The index and worklet bundles each emit a self-contained .d.ts, so
      // RealtimeEngine is declared twice and its private field makes the two
      // copies nominally distinct. Same class at runtime.
      const offline = new (await import('../dist/index.js')).RealtimeEngine({
        sampleRate: 48000,
        maxBlockSize: 128,
      }) as unknown as OfflineEngineOption;
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        offlineEngine: offline,
        offlineNumChannels: 1,
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      try {
        engine.setTrackLanes([{ trackId: 1 }]);
        const source = new Float32Array(4096);
        for (let i = 0; i < source.length; i++) {
          source[i] = Math.sin(i * 0.02);
        }
        const clipId = engine.addClip(1, {
          channels: [source],
          startPpq: 0,
          lengthSamples: 8192,
          warpMode: 'tempoSync',
          warpAnchors: [
            { warpSample: 0, sourceSample: 0 },
            { warpSample: 2048, sourceSample: 1024 },
            { warpSample: 8192, sourceSample: 4096 },
          ],
        });
        const message = posted.find(
          (
            candidate,
          ): candidate is {
            type: 'syncClipsDelta';
            upserts: Array<Record<string, unknown>>;
          } =>
            typeof candidate === 'object' &&
            candidate !== null &&
            (candidate as { type?: unknown }).type === 'syncClipsDelta',
        );
        const clip = message?.upserts.find((candidate) => candidate.id === clipId);
        expect(clip).toMatchObject({
          id: clipId,
          warpMode: 'off',
          clipOffsetSamples: 0,
          lengthSamples: 8192,
          loop: false,
        });
        expect(clip?.warpAnchors).toBeUndefined();
        // The delta message is read back as Record<string, unknown>.
        const channels = clip?.channels as Float32Array[] | undefined;
        expect(channels).toHaveLength(1);
        expect(channels?.[0]).toBeInstanceOf(Float32Array);
        expect(channels?.[0]).toHaveLength(8192);
      } finally {
        engine.destroy();
      }
    });

    it('sends long pre-baked clips as bounded transferable PCM pages', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        offlineNumChannels: 1,
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      try {
        engine.setTrackLanes([{ trackId: 1 }]);
        const source = new Float32Array(8192).fill(0.25);
        const clipId = engine.addClip(1, {
          channels: [source],
          startPpq: 0,
          lengthSamples: 16_385,
          warpMode: 'tempoSync',
          warpAnchors: [
            { warpSample: 0, sourceSample: 0 },
            { warpSample: 16_385, sourceSample: 8192 },
          ],
        });
        const providerIndex = posted.findIndex(
          (message) =>
            typeof message === 'object' &&
            message !== null &&
            (message as { type?: unknown }).type === 'syncClipPageProvider',
        );
        const pages = posted.filter(
          (
            message,
          ): message is { type: 'syncClipPage'; clipId: number; channels: Float32Array[] } =>
            typeof message === 'object' &&
            message !== null &&
            (message as { type?: unknown }).type === 'syncClipPage',
        );
        const commitIndex = posted.findIndex(
          (message) =>
            typeof message === 'object' &&
            message !== null &&
            (message as { type?: unknown }).type === 'syncClipPageCommit',
        );
        expect(posted[providerIndex]).toMatchObject({
          clipId,
          numSamples: 16_385,
          pageFrames: 4096,
          clip: { warpMode: 'off', channels: undefined },
        });
        expect(pages).toHaveLength(5);
        expect(pages.every((page) => page.clipId === clipId)).toBe(true);
        expect(pages.every((page) => page.channels[0].length <= 4096)).toBe(true);
        expect(commitIndex).toBeGreaterThan(providerIndex);
      } finally {
        engine.destroy();
      }
    });

    it('declares mixer lanes in explicit order via setTrackLanes', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      try {
        engine.setTrackBuses([{ busId: 7 }]);
        engine.setTrackLanes([
          { trackId: 2 },
          { trackId: 5, sends: [{ busId: 7, levelDb: -6, enabled: true }] },
        ]);
        expect(posted).toEqual(
          expect.arrayContaining([
            expect.objectContaining({
              type: 'syncMixer',
              lanes: [
                { trackId: 2 },
                { trackId: 5, sends: [{ busId: 7, levelDb: -6, enabled: true }] },
              ],
            }),
          ]),
        );
        // Lane indices follow the declared order: track 5 occupies lane 1.
        expect(engine.setTrackSoloMute(5, { solo: false, mute: true })).toBeUndefined();
        expect(posted).toEqual(
          expect.arrayContaining([
            expect.objectContaining({
              type: SonareEngineCommandType.SetSoloMute,
              targetId: 1,
              argInt: 0x1,
            }),
          ]),
        );
        expect(engine.setTrackMonitorMode(5, 'pfl', 321)).toBeUndefined();
        expect(posted).toEqual(
          expect.arrayContaining([
            expect.objectContaining({
              type: SonareEngineCommandType.SetTrackMonitorMode,
              targetId: 1,
              renderFrame: 321,
              argInt: 1,
            }),
          ]),
        );
        for (const mode of [true, false, 0.5, -1, 3]) {
          expect(() => engine.setTrackMonitorMode(5, mode as never)).toThrow(TypeError);
        }
        for (const mode of ['PFL', 'post-fader']) {
          expect(() => engine.setTrackMonitorMode(5, mode as never)).toThrow(RangeError);
        }
        // Appending keeps existing lanes; entries without sends keep prior sends.
        engine.setTrackLanes([{ trackId: 2 }, { trackId: 5 }, { trackId: 9 }]);
        expect(posted).toEqual(
          expect.arrayContaining([
            expect.objectContaining({
              type: 'syncMixer',
              lanes: [
                { trackId: 2 },
                { trackId: 5, sends: [{ busId: 7, levelDb: -6, enabled: true }] },
                { trackId: 9 },
              ],
            }),
          ]),
        );
        expect(() =>
          engine.setTrackLanes([{ trackId: 5 }, { trackId: 2 }, { trackId: 9 }]),
        ).toThrow(/append-only/);
        expect(() => engine.setTrackLanes([{ trackId: 2 }, { trackId: 5 }])).toThrow(/append-only/);
        expect(() =>
          engine.setTrackLanes([{ trackId: 2 }, { trackId: 5 }, { trackId: 9 }, { trackId: 9 }]),
        ).toThrow(/Duplicate track id/);
        expect(() =>
          engine.setTrackLanes([{ trackId: 2 }, { trackId: 5 }, { trackId: 9 }, { trackId: 0 }]),
        ).toThrow(/Invalid track id/);
      } finally {
        engine.destroy();
      }
    });

    it('retains explicit source channel layouts across mixer syncs', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      const latestSync = (): { lanes: Array<Record<string, unknown>> } => {
        const syncs = posted.filter(
          (message) => (message as { type?: unknown }).type === 'syncMixer',
        ) as Array<{ lanes: Array<Record<string, unknown>> }>;
        return syncs.at(-1) as { lanes: Array<Record<string, unknown>> };
      };
      try {
        engine.setTrackBuses([{ busId: 100 }]);
        engine.setTrackLanes([{ trackId: 1, sourceChannelLayout: 'stereo' }]);
        expect(latestSync().lanes[0]).toMatchObject({ trackId: 1, sourceChannelLayout: 'stereo' });

        engine.setTrackSends(1, [{ busId: 100, levelDb: -6, enabled: true }]);
        expect(latestSync().lanes[0]).toMatchObject({
          trackId: 1,
          sourceChannelLayout: 'stereo',
          sends: [{ busId: 100, levelDb: -6, enabled: true }],
        });

        // An omitted layout on an existing lane keeps the previous explicit
        // value, while a newly appended lane remains on the native default.
        engine.setTrackLanes([{ trackId: 1 }, { trackId: 2 }]);
        expect(latestSync().lanes[0]).toMatchObject({ trackId: 1, sourceChannelLayout: 'stereo' });
        expect(latestSync().lanes[1]).toEqual({ trackId: 2 });
      } finally {
        engine.destroy();
      }
    });

    it('refuses a non-stereo lane layout before posting to the worklet', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      try {
        engine.setTrackLanes([{ trackId: 1 }]);
        const before = posted.length;
        for (const layout of ['mono', '5.1', '7.1'] as const) {
          expect(() =>
            engine.setTrackLanes([{ trackId: 1 }, { trackId: 2, sourceChannelLayout: layout }]),
          ).toThrow(
            expect.objectContaining({
              name: 'SonareError',
              code: ErrorCode.NotSupported,
              message:
                'SonareEngine.setTrackLanes: lanes[1].sourceChannelLayout other than stereo is not supported',
            }),
          );
        }
        expect(posted).toHaveLength(before);
        expect(() =>
          engine.setTrackLanes([{ trackId: 1, sourceChannelLayout: 'stereo' }]),
        ).not.toThrow();
      } finally {
        engine.destroy();
      }
    });

    it('does not cache a rejected output-bus route', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      const latestSync = (): { lanes: Array<Record<string, unknown>> } =>
        posted.filter((message) => (message as { type?: unknown }).type === 'syncMixer').at(-1) as {
          lanes: Array<Record<string, unknown>>;
        };
      try {
        engine.setTrackBuses([{ busId: 100 }]);
        engine.setTrackLanes([{ trackId: 1, outputBusId: 100, sourceChannelLayout: 'stereo' }]);
        const beforeRejectedRoute = posted.length;
        expect(() => engine.setTrackOutputBus(1, 999)).toThrow();
        expect(posted).toHaveLength(beforeRejectedRoute);

        // A valid no-op topology sync must still use the prior route.
        expect(() => engine.setTrackSends(1, [])).not.toThrow();
        expect(latestSync().lanes).toEqual([
          { trackId: 1, outputBusId: 100, sourceChannelLayout: 'stereo' },
        ]);
      } finally {
        engine.destroy();
      }
    });

    it('does not cache rejected sends', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      const latestSync = (): { lanes: Array<Record<string, unknown>> } =>
        posted.filter((message) => (message as { type?: unknown }).type === 'syncMixer').at(-1) as {
          lanes: Array<Record<string, unknown>>;
        };
      try {
        engine.setTrackBuses([{ busId: 100 }]);
        engine.setTrackLanes([{ trackId: 1, outputBusId: 100 }]);
        const beforeRejectedSend = posted.length;
        expect(() => engine.setTrackSends(1, [{ busId: 999, levelDb: -6 }])).toThrow();
        expect(posted).toHaveLength(beforeRejectedSend);

        expect(() => engine.setTrackLanes([{ trackId: 1 }])).not.toThrow();
        expect(latestSync().lanes).toEqual([{ trackId: 1, outputBusId: 100 }]);
      } finally {
        engine.destroy();
      }
    });

    it('does not cache a rejected explicit lane update', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      const latestSync = (): { lanes: Array<Record<string, unknown>> } =>
        posted.filter((message) => (message as { type?: unknown }).type === 'syncMixer').at(-1) as {
          lanes: Array<Record<string, unknown>>;
        };
      try {
        engine.setTrackBuses([{ busId: 100 }]);
        engine.setTrackLanes([{ trackId: 1, outputBusId: 100, sourceChannelLayout: 'stereo' }]);
        const beforeRejectedLanes = posted.length;
        expect(() =>
          engine.setTrackLanes([
            {
              trackId: 1,
              outputBusId: 999,
              sourceChannelLayout: 'stereo',
              sends: [{ busId: 999, levelDb: -6 }],
            },
          ]),
        ).toThrow();
        expect(posted).toHaveLength(beforeRejectedLanes);

        expect(() => engine.setTrackSends(1, [])).not.toThrow();
        expect(latestSync().lanes).toEqual([
          { trackId: 1, outputBusId: 100, sourceChannelLayout: 'stereo' },
        ]);
      } finally {
        engine.destroy();
      }
    });

    it('does not retain an implicitly added lane rejected at the native limit', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      const latestSync = (): { lanes: Array<Record<string, unknown>> } =>
        posted.filter((message) => (message as { type?: unknown }).type === 'syncMixer').at(-1) as {
          lanes: Array<Record<string, unknown>>;
        };
      try {
        const lanes = Array.from({ length: 32 }, (_, index) => index + 1);
        engine.setTrackLanes(lanes.map((trackId) => ({ trackId })));
        const beforeRejectedLane = posted.length;
        expect(() => engine.setTrackSends(33, [])).toThrow();
        expect(posted).toHaveLength(beforeRejectedLane);

        expect(() => engine.setTrackSends(1, [])).not.toThrow();
        expect(latestSync().lanes).toHaveLength(32);
        expect(latestSync().lanes.some((lane) => lane.trackId === 33)).toBe(false);
      } finally {
        engine.destroy();
      }
    });

    it('does not settle offline insert automation for a rejected route', async () => {
      const blockSize = 128;
      const frames = blockSize * 4;
      const scene = JSON.stringify({
        version: 2,
        strips: [
          {
            id: 'track-7',
            inserts: [{ slot: 'pre', processor: 'utility.gain', params: { levelDb: -12 } }],
          },
        ],
        buses: [],
        connections: [],
      });
      const clips = [
        {
          id: 1,
          trackId: 7,
          channels: [new Float32Array(frames).fill(0.25), new Float32Array(frames).fill(0.25)],
          startPpq: 0,
          lengthSamples: frames,
        },
      ];
      const createEngine = async (): Promise<{
        engine: SonareEngine;
        offline: OfflineEngineOption;
        automationId: number;
      }> => {
        const realtime = new (await import('../dist/index.js')).RealtimeEngine({
          sampleRate: 48000,
          maxBlockSize: blockSize,
        });
        // A zero smoothing time makes a smoothed set a step, so the insert's 5 ms guard is the
        // only glide and its rate bounds what one block must move.
        realtime.setParamSmoothingMs(0);
        const offline = realtime as unknown as OfflineEngineOption;
        const engine = await SonareEngine.create(fakeContext(), {
          numChannels: 2,
          mode: 'postMessage',
          offlineEngine: offline,
          offlineNumChannels: 2,
          nodeFactory: () =>
            readyWorkletNode({
              postMessage: () => undefined,
              onmessage: undefined,
            }),
        });
        engine.setTrackLanes([{ trackId: 7 }]);
        engine.setStrip(7, stripDocument(scene));
        const automationId = engine.resolveStripInsertAutomationId(7, 0, 'levelDb');
        expect(automationId).toBeGreaterThan(0);
        offline.setClips(clips);
        offline.play();
        return { engine, offline, automationId };
      };

      const rejected = await createEngine();
      const baseline = await createEngine();
      try {
        const zeroes = () => [new Float32Array(blockSize), new Float32Array(blockSize)];
        const rejectedFirst = rejected.offline.process(zeroes());
        const baselineFirst = baseline.offline.process(zeroes());
        rejected.offline.setParameter(rejected.automationId, -12, undefined, { smoothed: true });
        rejected.offline.flushControlCommands();
        baseline.offline.setParameter(baseline.automationId, -12, undefined, { smoothed: true });
        baseline.offline.flushControlCommands();
        const rejectedPriming = rejected.offline.process(zeroes());
        const baselinePriming = baseline.offline.process(zeroes());
        rejected.offline.setParameter(rejected.automationId, 12, undefined, { smoothed: true });
        rejected.offline.flushControlCommands();
        baseline.offline.setParameter(baseline.automationId, 12, undefined, { smoothed: true });
        baseline.offline.flushControlCommands();
        const expected = baseline.offline.process(zeroes());
        const expectedNext = baseline.offline.process(zeroes());
        expect(() => rejected.engine.setTrackOutputBus(7, 999)).toThrow();
        const actual = rejected.offline.process(zeroes());
        let maxDiff = 0;
        for (let channel = 0; channel < actual.length; channel += 1) {
          for (let sample = 0; sample < blockSize; sample += 1) {
            maxDiff = Math.max(
              maxDiff,
              Math.abs(actual[channel][sample] - expected[channel][sample]),
            );
          }
        }
        expect(rejectedFirst[0][0]).toBeGreaterThan(0);
        expect(baselineFirst[0][0]).toBeGreaterThan(0);
        expect(rejectedPriming[0][0]).toBeGreaterThan(0);
        expect(baselinePriming[0][0]).toBeGreaterThan(0);
        // Insert automation advances once per render block, so compare two successive blocks
        // rather than samples within one block. The guard covers 1 - e^(-T/5 ms) of what is left
        // per block, so the second block still moves e^(-T/5 ms) times that much of the step.
        const gainOf = (db: number) => 0.25 * 10 ** (db / 20);
        const perBlock = 1 - Math.exp(-blockSize / (48000 * 0.005));
        const minimumMove = (gainOf(12) - gainOf(-12)) * (1 - perBlock) * perBlock;
        expect(Math.abs(expectedNext[0][0] - expected[0][0])).toBeGreaterThan(minimumMove);
        expect(maxDiff).toBeLessThanOrEqual(1e-6);
      } finally {
        rejected.engine.destroy();
        baseline.engine.destroy();
      }
    });

    it('keeps a raw insert ramp running across an accepted route sync', async () => {
      const blockSize = 128;
      const frames = blockSize * 4;
      const scene = JSON.stringify({
        version: 2,
        strips: [
          {
            id: 'track-7',
            inserts: [{ slot: 'pre', processor: 'utility.gain', params: { levelDb: -12 } }],
          },
        ],
        buses: [],
        connections: [],
      });
      const clips = [
        {
          id: 1,
          trackId: 7,
          channels: [new Float32Array(frames).fill(0.25), new Float32Array(frames).fill(0.25)],
          startPpq: 0,
          lengthSamples: frames,
        },
      ];
      const createEngine = async (): Promise<{
        engine: SonareEngine;
        offline: OfflineEngineOption;
        automationId: number;
      }> => {
        const offline = new (await import('../dist/index.js')).RealtimeEngine({
          sampleRate: 48000,
          maxBlockSize: blockSize,
        }) as unknown as OfflineEngineOption;
        const engine = await SonareEngine.create(fakeContext(), {
          numChannels: 2,
          mode: 'postMessage',
          offlineEngine: offline,
          offlineNumChannels: 2,
          nodeFactory: () =>
            readyWorkletNode({
              postMessage: () => undefined,
              onmessage: undefined,
            }),
        });
        engine.setTrackLanes([{ trackId: 7 }]);
        engine.setStrip(7, stripDocument(scene));
        const automationId = engine.resolveStripInsertAutomationId(7, 0, 'levelDb');
        expect(automationId).toBeGreaterThan(0);
        offline.setClips(clips);
        offline.play();
        return { engine, offline, automationId };
      };

      const accepted = await createEngine();
      const untouchedBaseline = await createEngine();
      try {
        const zeroes = () => [new Float32Array(blockSize), new Float32Array(blockSize)];
        for (const { offline, automationId } of [accepted, untouchedBaseline]) {
          offline.process(zeroes());
          offline.setParameter(automationId, -12, undefined, { smoothed: true });
          offline.flushControlCommands();
          offline.process(zeroes());
          offline.setParameter(automationId, 12, undefined, { smoothed: true });
          offline.flushControlCommands();
        }
        // The accepted topology change keeps the in-flight ramp instead of snapping it.
        const expected = untouchedBaseline.offline.process(zeroes());
        expect(() => accepted.engine.setTrackLanes([{ trackId: 7 }])).not.toThrow();
        const actual = accepted.offline.process(zeroes());
        let maxDiff = 0;
        for (let channel = 0; channel < actual.length; channel += 1) {
          for (let sample = 0; sample < blockSize; sample += 1) {
            maxDiff = Math.max(
              maxDiff,
              Math.abs(actual[channel][sample] - expected[channel][sample]),
            );
          }
        }
        expect(expected[0][0]).toBeLessThan(0.9);
        expect(maxDiff).toBeLessThanOrEqual(1e-6);
      } finally {
        accepted.engine.destroy();
        untouchedBaseline.engine.destroy();
      }
    });

    it('bypasses a bus insert live, like a track or master one', async () => {
      // Insert control has to be the same set for every strip class the facade
      // exposes. A bus could have its insert parameters automated but not
      // bypassed, and the only workaround -- re-posting the bus scene JSON --
      // rebuilds the chain, so a reverb tail or compressor envelope disappears
      // and the result is audibly not a bypass.
      const posted: unknown[] = [];
      const offline = new (await import('../dist/index.js')).RealtimeEngine({
        sampleRate: 48000,
        maxBlockSize: 128,
      }) as unknown as OfflineEngineOption;
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        offlineEngine: offline,
        offlineNumChannels: 2,
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      try {
        engine.setBusStrip(
          100,
          busDocument(
            '{"version":2,"strips":[],"buses":[{"id":"100","inserts":[{"slot":"pre","processor":"eq.parametric","params":{"band0.type":1,"band0.frequencyHz":1000,"band0.gainDb":6,"band0.enabled":1}}]}],"connections":[]}',
          ),
        );
        engine.setBusInsertBypassed(100, 0, { bypassed: true, resetOnBypass: true });
        engine.setBusInsertBypassed(100, 0, { bypassed: false });
        // A fractional bus id used to be truncated before the check that would
        // have refused it, so 200.7 registered a bus 200 nobody asked for and
        // synced it -- a record left behind by a call that went on to throw.
        // An id no earlier line registered is what makes the leftover visible.
        const before = posted.length;
        expect(() => engine.setBusInsertBypassed(200.7, 0, { bypassed: true })).toThrow(RangeError);
        expect(posted.length).toBe(before);
        expect(posted).toEqual(
          expect.arrayContaining([
            expect.objectContaining({
              type: 'syncBusStripInsertBypassed',
              busId: 100,
              insertIndex: 0,
              bypassed: true,
              resetOnBypass: true,
            }),
            expect.objectContaining({
              type: 'syncBusStripInsertBypassed',
              busId: 100,
              insertIndex: 0,
              bypassed: false,
              resetOnBypass: false,
            }),
          ]),
        );
        // Every strip class the live facade addresses carries both live insert
        // operations; a class with only one of them is the gap this covers.
        for (const method of [
          'setStripInsertParam',
          'setStripInsertBypassed',
          'setBusInsertParam',
          'setBusInsertBypassed',
        ]) {
          expect(typeof (engine as unknown as Record<string, unknown>)[method]).toBe('function');
        }
      } finally {
        engine.destroy();
      }
    });

    it('drops removed bus scenes before replaying a neutral master strip', async () => {
      const posted: unknown[] = [];
      const livePosted: unknown[] = [];
      const offline = new (await import('../dist/index.js')).RealtimeEngine({
        sampleRate: 48000,
        maxBlockSize: 128,
      }) as unknown as OfflineEngineOption;
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        offlineEngine: offline,
        offlineNumChannels: 2,
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      const live = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        maxBlockSize: 128,
        numChannels: 2,
        transport: { postMessage: (message) => livePosted.push(message) },
      });
      try {
        engine.setTrackBuses([{ busId: 100 }]);
        engine.setBusStrip(
          100,
          busDocument(
            '{"version":2,"strips":[],"buses":[{"id":"100","inserts":[{"slot":"pre","processor":"eq.parametric","params":{"band0.gainDb":0}}]}],"connections":[]}',
          ),
        );
        engine.setBusInsertParam(100, 0, { name: 'band0.gainDb', value: 5 });
        engine.setStrip(
          'master',
          stripDocument(
            '{"version":2,"strips":[{"id":"master","inputTrimDb":-30}],"buses":[],"connections":[]}',
          ),
        );
        engine.setTrackOutputBus(7, 100);
        const beforeRejectedRemoval = posted.length;
        expect(() => engine.setTrackBuses([])).toThrow();
        expect(posted).toHaveLength(beforeRejectedRemoval);
        engine.setTrackSends(7, []);
        const retained = posted.at(-1) as {
          type: string;
          buses: Array<{ busId: number }>;
          busStrips: Array<{ busId: number }>;
          insertParamOverrides?: Array<{ kind: string; busId?: number }>;
        };
        expect(retained.type).toBe('syncMixer');
        expect(retained.buses).toEqual([expect.objectContaining({ busId: 100 })]);
        expect(retained.busStrips).toEqual([expect.objectContaining({ busId: 100 })]);
        expect(retained.insertParamOverrides).toEqual([
          expect.objectContaining({ kind: 'bus', busId: 100 }),
        ]);
        engine.setTrackOutputBus(7, null);
        engine.setTrackBuses([]);
        engine.setStrip(
          'master',
          stripDocument('{"version":2,"strips":[{"id":"master"}],"buses":[],"connections":[]}'),
        );

        const syncs = posted.filter(
          (message) => (message as { type?: unknown }).type === 'syncMixer',
        ) as Array<{
          buses: unknown[];
          busStrips: unknown[];
          masterStripJson: string;
          insertParamOverrides?: unknown[];
        }>;
        expect(syncs.at(-1)).toEqual(expect.objectContaining({ buses: [], busStrips: [] }));
        expect(syncs.at(-1)?.insertParamOverrides ?? []).toEqual([]);
        expect(JSON.parse(syncs.at(-1)?.masterStripJson ?? '{}').strips[0]).not.toHaveProperty(
          'inputTrimDb',
        );
        for (const message of posted) {
          const type = (message as { type?: unknown }).type;
          if (typeof type === 'string' && type in ENGINE_SYNC_MESSAGE_TYPES) {
            live.receiveMessage(message);
          }
        }
        expect(
          livePosted.filter((message) => (message as { type?: string }).type === 'syncError'),
        ).toEqual([]);

        engine.setTrackBuses([{ busId: 100 }]);
        const restoredSync = posted.at(-1) as { type: string; busStrips: unknown[] };
        expect(restoredSync.type).toBe('syncMixer');
        expect(restoredSync.busStrips).toEqual([]);
      } finally {
        live.destroy();
        engine.destroy();
      }
    });

    it('keeps a pending bus insert command on its bus when buses are reordered', async () => {
      const blockSize = 128;
      const posted: unknown[] = [];
      const offline = new (await import('../dist/index.js')).RealtimeEngine({
        sampleRate: 48000,
        maxBlockSize: blockSize,
      }) as unknown as OfflineEngineOption;
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        offlineEngine: offline,
        offlineNumChannels: 2,
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      const live = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        maxBlockSize: blockSize,
        numChannels: 2,
        transport: { postMessage: () => undefined },
      });
      try {
        engine.setTrackBuses([{ busId: 100 }, { busId: 200 }]);
        engine.setTrackOutputBus(7, 100);
        engine.setTrackOutputBus(8, 200);
        for (const busId of [100, 200]) {
          engine.setBusStrip(
            busId,
            busDocument(
              JSON.stringify({
                version: 2,
                strips: [],
                buses: [
                  {
                    id: String(busId),
                    inserts: [
                      {
                        slot: 'pre',
                        processor: 'eq.parametric',
                        params: {
                          'band0.type': 1,
                          'band0.frequencyHz': busId === 100 ? 800 : 3000,
                          'band0.gainDb': 0,
                          'band0.enabled': 1,
                        },
                      },
                    ],
                  },
                ],
                connections: [],
              }),
            ),
          );
        }
        const automationId = offline.resolveBusInsertAutomationId(100, 0, 'band0.gainDb');
        expect(automationId).toBeGreaterThan(0);
        offline.setParameter(automationId, 12, 64);
        live.receiveMessage({
          type: SonareEngineCommandType.SetParam,
          targetId: automationId,
          renderFrame: 64,
          argFloat: 12,
        });
        // No process block between the scheduled command and topology edit.
        engine.setTrackBuses([{ busId: 200 }, { busId: 100, gainDb: -96 }]);
        for (const message of posted) {
          const type = (message as { type?: unknown }).type;
          if (typeof type === 'string' && type in ENGINE_SYNC_MESSAGE_TYPES) {
            live.receiveMessage(message);
          }
        }
        const frames = blockSize * 40;
        const a = new Float32Array(frames);
        const b = new Float32Array(frames);
        for (let i = 0; i < frames; i += 1) {
          a[i] = 0.2 * Math.sin((2 * Math.PI * 800 * i) / 48000);
          b[i] = 0.2 * Math.sin((2 * Math.PI * 3000 * i) / 48000);
        }
        const clips = [
          { id: 1, trackId: 7, channels: [a, a], startPpq: 0, lengthSamples: frames },
          { id: 2, trackId: 8, channels: [b, b], startPpq: 0, lengthSamples: frames },
        ];
        offline.setClips(clips);
        live.receiveMessage({ type: 'syncClips', clips });
        offline.play();
        live.receiveMessage({ type: SonareEngineCommandType.TransportPlay, renderFrame: -1 });
        let maxDiff = 0;
        let maxAbs = 0;
        // Bus 100's republished -96 dB gain ramps over 5 ms (two blocks) before it is silent.
        const settledBlock = 3;
        for (let block = 0; block < 30; block += 1) {
          const expected = offline.process([
            new Float32Array(blockSize),
            new Float32Array(blockSize),
          ]);
          const actual = [new Float32Array(blockSize), new Float32Array(blockSize)];
          expect(live.process([[]], [actual])).toBe(true);
          for (let channel = 0; channel < 2; channel += 1) {
            for (let i = 0; i < blockSize; i += 1) {
              if (block >= settledBlock) {
                maxAbs = Math.max(maxAbs, Math.abs(expected[channel][i]));
              }
              maxDiff = Math.max(maxDiff, Math.abs(expected[channel][i] - actual[channel][i]));
            }
          }
        }
        expect(maxAbs).toBeGreaterThan(0.05);
        // Only bus 200 remains audible. If the pending bus-100 command is
        // misrouted to its new slot, bus 200's 3 kHz band becomes much louder.
        expect(maxAbs).toBeLessThan(0.35);
        expect(maxDiff).toBeLessThanOrEqual(1e-6 * Math.max(1, maxAbs));
      } finally {
        live.destroy();
        engine.destroy();
      }
    });

    it('keeps queued bus insert automation IDs stable across reorder and retires removed IDs', async () => {
      const realtime = new (await import('../dist/index.js')).RealtimeEngine({
        sampleRate: 48000,
        maxBlockSize: 128,
      });
      const scene = (busId: number) =>
        JSON.stringify({
          version: 2,
          strips: [],
          buses: [
            {
              id: String(busId),
              inserts: [
                {
                  slot: 'pre',
                  processor: 'eq.parametric',
                  params: {
                    'band0.type': 1,
                    'band0.frequencyHz': 1000,
                    'band0.gainDb': 0,
                    'band0.enabled': 1,
                  },
                },
              ],
            },
          ],
          connections: [],
        });
      try {
        realtime.setTrackBuses([{ busId: 100 }, { busId: 200 }]);
        realtime.setBusStrip(100, busDocument(scene(100)));
        realtime.setBusStrip(200, busDocument(scene(200)));
        const oldId = realtime.resolveBusInsertAutomationId(100, 0, 'band0.gainDb');
        expect(oldId).toBeGreaterThan(0);
        realtime.setParameter(oldId, 12, 64);
        realtime.setTrackBuses([{ busId: 200 }, { busId: 100 }]);
        expect(realtime.resolveBusInsertAutomationId(100, 0, 'band0.gainDb')).toBe(oldId);
        const otherId = realtime.resolveBusInsertAutomationId(200, 0, 'band0.gainDb');
        expect(otherId).toBeGreaterThan(0);
        expect(otherId).not.toBe(oldId);
        realtime.setTrackBuses([{ busId: 200 }]);
        realtime.setTrackBuses([{ busId: 200 }, { busId: 100 }]);
        realtime.setBusStrip(100, busDocument(scene(100)));
        const replacementId = realtime.resolveBusInsertAutomationId(100, 0, 'band0.gainDb');
        expect(replacementId).toBeGreaterThan(0);
        expect(replacementId).not.toBe(oldId);
        expect(realtime.resolveBusInsertAutomationId(200, 0, 'band0.gainDb')).toBe(otherId);
      } finally {
        realtime.destroy();
      }
    });

    it('keeps a burst of more master insert values than the smoother slot capacity', async () => {
      const blockSize = 128;
      const posted: unknown[] = [];
      const livePosted: unknown[] = [];
      const offline = new (await import('../dist/index.js')).RealtimeEngine({
        sampleRate: 48000,
        maxBlockSize: blockSize,
      }) as unknown as OfflineEngineOption;
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        offlineEngine: offline,
        offlineNumChannels: 2,
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      const live = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        maxBlockSize: blockSize,
        numChannels: 2,
        transport: { postMessage: (message) => livePosted.push(message) },
      });
      const replay = (messages: unknown[]): void => {
        for (const message of messages) {
          const type = (message as { type?: unknown }).type;
          if (typeof type === 'string' && type in ENGINE_SYNC_MESSAGE_TYPES) {
            live.receiveMessage(message);
          }
        }
      };
      try {
        engine.setTrackLanes([{ trackId: 7 }]);
        engine.setStrip(
          'master',
          stripDocument(
            '{"version":2,"strips":[{"id":"master","inserts":[{"slot":"pre","processor":"eq.parametric","params":{}}]}],"buses":[],"connections":[]}',
          ),
        );
        replay(posted);
        const liveNative = (
          live as unknown as { native: Record<string, (...args: unknown[]) => unknown> }
        ).native;
        const exactFallback = vi.spyOn(liveNative, 'restoreMasterStripInsertParamByName');
        for (let band = 0; band < 17; band += 1) {
          const before = posted.length;
          engine.setStripInsertParam('master', 0, {
            name: `band${band}.gainDb`,
            value: 1 + band / 10,
          });
          replay(posted.slice(before));
        }
        expect(exactFallback).toHaveBeenCalledTimes(1);
        expect(exactFallback).toHaveBeenCalledWith(0, 'band16.gainDb', 2.6);
        const beforeRouting = posted.length;
        engine.setTrackSends(7, []);
        const sync = posted.at(-1) as { insertParamOverrides?: unknown[] };
        expect(sync.insertParamOverrides).toHaveLength(17);
        replay(posted.slice(beforeRouting));
        offline.process([new Float32Array(blockSize), new Float32Array(blockSize)]);
        expect(
          live.process([[]], [[new Float32Array(blockSize), new Float32Array(blockSize)]]),
        ).toBe(true);
        expect(
          offline.drainTelemetry().filter((item) => item.error === 'insertAutomationOverflow'),
        ).toEqual([]);
        expect(
          livePosted.filter((item) => (item as { type?: unknown }).type === 'syncError'),
        ).toEqual([]);
      } finally {
        live.destroy();
        engine.destroy();
      }
    });

    it('returns an automated insert to the replacement scene value', async () => {
      const blockSize = 128;
      const posted: unknown[] = [];
      const offline = new (await import('../dist/index.js')).RealtimeEngine({
        sampleRate: 48000,
        maxBlockSize: blockSize,
      }) as unknown as OfflineEngineOption;
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        offlineEngine: offline,
        offlineNumChannels: 2,
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      const live = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        maxBlockSize: blockSize,
        numChannels: 2,
      });
      const replay = (messages: unknown[]): void => {
        for (const message of messages) {
          const type = (message as { type?: unknown }).type;
          if (typeof type === 'string' && type in ENGINE_SYNC_MESSAGE_TYPES) {
            live.receiveMessage(message);
          } else if (typeof type === 'number') {
            live.receiveMessage(message);
          }
        }
      };
      const scene = (levelDb: number) =>
        JSON.stringify({
          version: 2,
          strips: [
            {
              id: 'track-7',
              inserts: [{ slot: 'pre', processor: 'utility.gain', params: { levelDb } }],
            },
          ],
          buses: [],
          connections: [],
        });
      try {
        engine.setTrackLanes([{ trackId: 7 }]);
        engine.setStrip(7, stripDocument(scene(0)));
        engine.setStripInsertParam(7, 0, { name: 'levelDb', value: 6 });
        engine.setStrip(7, stripDocument(scene(-12)));
        replay(posted);
        const frames = blockSize * 180;
        const tone = new Float32Array(frames);
        for (let i = 0; i < frames; i += 1) {
          tone[i] = 0.25 * Math.sin((2 * Math.PI * 440 * i) / 48000);
        }
        const clips = [
          { id: 1, trackId: 7, channels: [tone, tone], startPpq: 0, lengthSamples: frames },
        ];
        offline.setClips(clips);
        live.receiveMessage({ type: 'syncClips', clips });
        offline.play();
        live.receiveMessage({ type: SonareEngineCommandType.TransportPlay, renderFrame: -1 });
        const power = (): number => {
          let sum = 0;
          let liveSum = 0;
          for (let block = 0; block < 24; block += 1) {
            const out = offline.process([new Float32Array(blockSize), new Float32Array(blockSize)]);
            const liveOut = [new Float32Array(blockSize), new Float32Array(blockSize)];
            expect(live.process([[]], [liveOut])).toBe(true);
            if (block < 8) {
              continue;
            }
            for (let i = 0; i < blockSize; i += 1) {
              sum += out[0][i] ** 2;
              liveSum += liveOut[0][i] ** 2;
            }
          }
          expect(liveSum).toBeCloseTo(sum, 3);
          return sum;
        };
        const scenePower = power();
        expect(scenePower).toBeGreaterThan(0.01);
        const id = engine.resolveStripInsertAutomationId(7, 0, 'levelDb');
        const beforeAutomation = posted.length;
        engine.setAutomationLane(id, [{ ppq: 0, value: 0 }]);
        replay(posted.slice(beforeAutomation));
        const automatedPower = power();
        expect(automatedPower / scenePower).toBeGreaterThan(8);
        const beforeRelease = posted.length;
        engine.setAutomationLane(id, []);
        replay(posted.slice(beforeRelease));
        const releasedPower = power();
        expect(releasedPower / scenePower).toBeGreaterThan(0.9);
        expect(releasedPower / scenePower).toBeLessThan(1.1);
      } finally {
        live.destroy();
        engine.destroy();
      }
    });

    it('keeps bus, master and track strip setters across a syncMixer re-post', async () => {
      // syncMixer re-posts every cached strip scene JSON to the worklet. A
      // setter the cache did not absorb is reverted there while the offline
      // mirror keeps it, so the live output and an offline render disagree.
      const blockSize = 128;
      const posted: unknown[] = [];
      const offline = new (await import('../dist/index.js')).RealtimeEngine({
        sampleRate: 48000,
        maxBlockSize: blockSize,
      }) as unknown as OfflineEngineOption;
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        offlineEngine: offline,
        offlineNumChannels: 2,
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      const livePosted: unknown[] = [];
      const live = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        maxBlockSize: blockSize,
        numChannels: 2,
        transport: { postMessage: (message) => livePosted.push(message) },
      });
      try {
        engine.setTrackBuses([
          { busId: 100, gainDb: 0 },
          { busId: 200, gainDb: 0 },
        ]);
        engine.setTrackOutputBus(7, 100);
        engine.setTrackSends(7, [{ busId: 200, levelDb: -3, enabled: true }]);
        const eqParams = (gainDb: number) => ({
          'band0.type': 1,
          'band0.frequencyHz': 800,
          'band0.gainDb': gainDb,
          'band0.enabled': 1,
        });
        const busStripJson = JSON.stringify({
          version: 2,
          strips: [],
          buses: [
            {
              id: '100',
              inserts: [{ slot: 'pre', processor: 'eq.parametric', params: eqParams(0) }],
            },
          ],
          connections: [],
        });
        engine.setBusStrip(100, busDocument(busStripJson));
        engine.setStrip(
          'master',
          stripDocument(
            JSON.stringify({
              version: 2,
              strips: [
                {
                  id: 'master',
                  inserts: [{ slot: 'pre', processor: 'eq.parametric', params: eqParams(0) }],
                },
              ],
              buses: [],
              connections: [],
            }),
          ),
        );
        engine.setStrip(
          7,
          stripDocument(
            JSON.stringify({
              version: 2,
              strips: [
                {
                  id: 'track-7',
                  // Current scene files use an object-valued params bag.
                  inserts: [{ slot: 'pre', processor: 'eq.parametric', params: eqParams(0) }],
                },
              ],
              buses: [],
              connections: [],
            }),
          ),
        );
        engine.setBusPanMode(100, 'stereoPan');
        engine.setBusPan(100, 0.6);
        engine.setBusPanLaw(100, 'const6Db');
        engine.setBusEqBand(100, 2, {
          type: 'peak',
          frequencyHz: 800,
          gainDb: 9,
          q: 1,
          enabled: true,
        });
        engine.setStripEqBand('master', 1, {
          type: 'peak',
          frequencyHz: 3000,
          gainDb: -6,
          q: 2,
          enabled: true,
        });
        engine.setStripEqBand(7, 0, { type: 'peak', frequencyHz: 1500, gainDb: 6, enabled: true });
        const replay = (messages: unknown[]): void => {
          for (const message of messages) {
            const type = (message as { type?: unknown }).type;
            if (typeof type === 'string' && type in ENGINE_SYNC_MESSAGE_TYPES) {
              live.receiveMessage(message);
            } else if (typeof type === 'number') {
              live.receiveMessage(message);
            }
          }
        };
        // Bring the worklet to the same pre-insert state. First claim the
        // smoother slots at values far from the intended targets.
        const beforeInsert = posted.length;
        replay(posted);
        engine.setStripInsertParam(7, 0, { name: 'band0.gainDb', value: -6 });
        engine.setStripInsertParam('master', 0, { name: 'band0.gainDb', value: 3 });
        engine.setBusInsertParam(100, 0, { name: 'band0.gainDb', value: -12 });
        replay(posted.slice(beforeInsert));
        offline.process([new Float32Array(blockSize), new Float32Array(blockSize)]);
        expect(
          live.process([[]], [[new Float32Array(blockSize), new Float32Array(blockSize)]]),
        ).toBe(true);
        // Move existing slots toward new targets for only one block. A later
        // bus topology update drops its active slot while the processor still
        // holds an intermediate value; replay must restore the final target.
        const beforeTarget = posted.length;
        engine.setStripInsertParam(7, 0, { name: 'band0.gainDb', value: 6 });
        engine.setStripInsertParam('master', 0, { name: 'band0.gainDb', value: -3 });
        engine.setBusInsertParam(100, 0, { name: 'band0.gainDb', value: 12 });
        replay(posted.slice(beforeTarget));
        offline.process([new Float32Array(blockSize), new Float32Array(blockSize)]);
        expect(
          live.process([[]], [[new Float32Array(blockSize), new Float32Array(blockSize)]]),
        ).toBe(true);
        // Exercise the real user path where routing is changed in a later
        // control turn, after the insert knob edit has already been flushed.
        await Promise.resolve();
        const beforeRouting = posted.length;
        // Bus 200 never received scene JSON, so its cache entry is synthesized.
        engine.setBusDualPan(200, -0.7, -0.2);
        engine.setBusPanMode(200, 'dualPan');
        // Any routing change re-posts every cached strip.
        engine.setTrackSends(7, [{ busId: 200, levelDb: -6, enabled: true }]);

        expect(posted).toEqual(
          expect.arrayContaining([
            expect.objectContaining({ type: 'syncBusStripPan', busId: 100, pan: 0.6 }),
            expect.objectContaining({ type: 'syncBusStripPanLaw', busId: 100, panLaw: 2 }),
            expect.objectContaining({ type: 'syncBusStripPanMode', busId: 100, panMode: 1 }),
            expect.objectContaining({
              type: 'syncBusStripDualPan',
              busId: 200,
              leftPan: -0.7,
              rightPan: -0.2,
            }),
            expect.objectContaining({
              type: 'syncBusStripEqBand',
              busId: 100,
              bandIndex: 2,
              bandJson: expect.stringContaining('"frequencyHz":800'),
            }),
            expect.objectContaining({
              type: 'syncTrackStripInsertParamByName',
              trackId: 7,
              insertIndex: 0,
              paramName: 'band0.gainDb',
              value: 6,
            }),
            expect.objectContaining({
              type: 'syncMasterStripInsertParamByName',
              insertIndex: 0,
              paramName: 'band0.gainDb',
              value: -3,
            }),
            expect.objectContaining({
              type: 'syncBusStripInsertParamByName',
              busId: 100,
              insertIndex: 0,
              paramName: 'band0.gainDb',
              value: 12,
            }),
          ]),
        );
        // Replay only the structural messages generated after the drain block.
        // This models a later-turn routing edit and makes the post-sync replay
        // boundary explicit.
        replay(posted.slice(beforeRouting));
        expect(
          livePosted.filter((message) => (message as { type?: string }).type === 'syncError'),
        ).toEqual([]);
        const frames = blockSize * 320;
        const left = new Float32Array(frames);
        const right = new Float32Array(frames);
        for (let i = 0; i < frames; i += 1) {
          left[i] = 0.3 * Math.sin((2 * Math.PI * 800 * i) / 48000);
          right[i] = 0.2 * Math.sin((2 * Math.PI * 3000 * i) / 48000);
        }
        const clips = [
          { id: 1, trackId: 7, channels: [left, right], startPpq: 0, lengthSamples: frames },
        ];
        offline.setClips(clips);
        live.receiveMessage({ type: 'syncClips', clips });
        offline.process([new Float32Array(blockSize), new Float32Array(blockSize)]);
        expect(
          live.process([[]], [[new Float32Array(blockSize), new Float32Array(blockSize)]]),
        ).toBe(true);
        offline.play();
        live.receiveMessage({ type: SonareEngineCommandType.TransportPlay, renderFrame: -1 });
        let maxDiff = 0;
        let maxAbs = 0;
        let imbalance = 0;
        let beforeRepeatEnergy = 0;
        for (let block = 0; block < 40; block += 1) {
          const expected = offline.process([
            new Float32Array(blockSize),
            new Float32Array(blockSize),
          ]);
          const liveOut = [new Float32Array(blockSize), new Float32Array(blockSize)];
          expect(live.process([[]], [liveOut])).toBe(true);
          for (let channel = 0; channel < 2; channel += 1) {
            for (let i = 0; i < blockSize; i += 1) {
              maxAbs = Math.max(maxAbs, Math.abs(expected[channel][i]));
              maxDiff = Math.max(maxDiff, Math.abs(expected[channel][i] - liveOut[channel][i]));
              if (block >= 20) {
                beforeRepeatEnergy += expected[channel][i] ** 2;
              }
            }
          }
          for (let i = 0; i < blockSize; i += 1) {
            imbalance = Math.max(imbalance, Math.abs(expected[0][i] - expected[1][i]));
          }
        }
        expect(maxAbs).toBeGreaterThan(0.05);
        expect(imbalance).toBeGreaterThan(0.01);
        expect(maxDiff).toBeLessThanOrEqual(1e-6 * Math.max(1, maxAbs));
        // Repeating the same bus target after the routing edit must not alter
        // settled audio. Without structural replay, the cleared bus slot
        // freezes at its intermediate value and this second set changes it.
        const beforeRepeat = posted.length;
        engine.setBusInsertParam(100, 0, { name: 'band0.gainDb', value: 12 });
        replay(posted.slice(beforeRepeat));
        let afterRepeatEnergy = 0;
        for (let block = 0; block < 40; block += 1) {
          const expected = offline.process([
            new Float32Array(blockSize),
            new Float32Array(blockSize),
          ]);
          const liveOut = [new Float32Array(blockSize), new Float32Array(blockSize)];
          expect(live.process([[]], [liveOut])).toBe(true);
          if (block < 20) {
            continue;
          }
          for (let channel = 0; channel < 2; channel += 1) {
            for (let i = 0; i < blockSize; i += 1) {
              afterRepeatEnergy += expected[channel][i] ** 2;
            }
          }
        }
        expect(beforeRepeatEnergy).toBeGreaterThan(0.01);
        expect(afterRepeatEnergy / beforeRepeatEnergy).toBeCloseTo(1, 1);
        // The construction scene stays at its original insert params. The
        // separate overrides restore live values without rebuilding inserts.
        const mixerSyncs = posted.filter(
          (message) => (message as { type?: unknown }).type === 'syncMixer',
        );
        const lastSync = mixerSyncs[mixerSyncs.length - 1] as {
          busStrips: Array<{ busId: number; sceneJson: string }>;
          trackStrips: Array<{ trackId: number; sceneJson: string }>;
          masterStripJson: string;
          insertParamOverrides?: Array<{
            kind: string;
            trackId?: number;
            busId?: number;
            insertIndex: number;
            paramName: string;
            value: number;
          }>;
        };
        const busScene = (busId: number) =>
          JSON.parse(lastSync.busStrips.find((strip) => strip.busId === busId)?.sceneJson ?? '{}');
        expect(busScene(100).buses[0]).toEqual(
          expect.objectContaining({
            id: '100',
            pan: 0.6,
            panLaw: 'const6Db',
            panMode: 'stereoPan',
            eq: { enabled: true, bands: [{}, {}, expect.objectContaining({ frequencyHz: 800 })] },
          }),
        );
        expect(busScene(200)).toEqual({
          version: 2,
          strips: [],
          buses: [{ id: 'bus-200', dualPanLeft: -0.7, dualPanRight: -0.2, panMode: 'dualPan' }],
          connections: [],
        });
        expect(JSON.parse(lastSync.masterStripJson).strips[0].eq).toEqual({
          enabled: true,
          bands: [{}, expect.objectContaining({ frequencyHz: 3000, gainDb: -6 })],
        });
        const trackScene = JSON.parse(
          lastSync.trackStrips.find((strip) => strip.trackId === 7)?.sceneJson ?? '{}',
        );
        expect(trackScene.strips[0].eq).toEqual({
          enabled: true,
          bands: [expect.objectContaining({ frequencyHz: 1500, gainDb: 6 })],
        });
        expect(trackScene.strips[0].inserts[0].params).toEqual(
          expect.objectContaining({ 'band0.gainDb': 0 }),
        );
        expect(JSON.parse(lastSync.masterStripJson).strips[0].inserts[0].params).toEqual(
          expect.objectContaining({ 'band0.gainDb': 0 }),
        );
        expect(busScene(100).buses[0].inserts[0].params).toEqual(
          expect.objectContaining({ 'band0.gainDb': 0 }),
        );
        expect(lastSync.insertParamOverrides).toHaveLength(3);
        expect(lastSync.insertParamOverrides).toEqual(
          expect.arrayContaining([
            expect.objectContaining({
              kind: 'track',
              trackId: 7,
              insertIndex: 0,
              paramName: 'band0.gainDb',
              value: 6,
            }),
            expect.objectContaining({
              kind: 'master',
              insertIndex: 0,
              paramName: 'band0.gainDb',
              value: -3,
            }),
            expect.objectContaining({
              kind: 'bus',
              busId: 100,
              insertIndex: 0,
              paramName: 'band0.gainDb',
              value: 12,
            }),
          ]),
        );

        // Replacing a full strip scene is the new source of truth and clears
        // only that target's retained by-name values.
        const beforeReplacement = posted.length;
        engine.setStrip(
          7,
          stripDocument(
            '{"version":2,"strips":[{"id":"track-7","inserts":[{"slot":"pre","processor":"eq.parametric","params":{}}]}],"buses":[],"connections":[]}',
          ),
        );
        engine.setStrip(
          'master',
          stripDocument(
            '{"version":2,"strips":[{"id":"master","inserts":[{"slot":"pre","processor":"eq.parametric","params":{"band0.gainDb":0}}]}],"buses":[],"connections":[]}',
          ),
        );
        engine.setBusStrip(
          100,
          busDocument(
            '{"version":2,"strips":[],"buses":[{"id":"100","inserts":[{"slot":"pre","processor":"eq.parametric","params":{"band0.gainDb":0}}]}],"connections":[]}',
          ),
        );
        const clearedSync = posted.at(-1) as {
          type: string;
          insertParamOverrides?: unknown[];
        };
        expect(clearedSync.type).toBe('syncMixer');
        expect(clearedSync.insertParamOverrides ?? []).toEqual([]);
        const replacements = posted
          .slice(beforeReplacement)
          .filter((message) => (message as { type?: unknown }).type === 'syncMixer') as Array<{
          insertBaseResets?: Array<{ kind: string }>;
        }>;
        expect(replacements.map((message) => message.insertBaseResets?.[0]?.kind)).toEqual([
          'track',
          'master',
          'bus',
        ]);
        replay(posted.slice(beforeReplacement));
        expect(
          livePosted.filter((message) => (message as { type?: string }).type === 'syncError'),
        ).toEqual([]);
        let afterResetEnergy = 0;
        let resetMaxDiff = 0;
        for (let block = 0; block < 40; block += 1) {
          const expected = offline.process([
            new Float32Array(blockSize),
            new Float32Array(blockSize),
          ]);
          const liveOut = [new Float32Array(blockSize), new Float32Array(blockSize)];
          expect(live.process([[]], [liveOut])).toBe(true);
          if (block < 20) {
            continue;
          }
          for (let channel = 0; channel < 2; channel += 1) {
            for (let i = 0; i < blockSize; i += 1) {
              afterResetEnergy += expected[channel][i] ** 2;
              resetMaxDiff = Math.max(
                resetMaxDiff,
                Math.abs(expected[channel][i] - liveOut[channel][i]),
              );
            }
          }
        }
        expect(afterResetEnergy / beforeRepeatEnergy).toBeLessThan(0.8);
        expect(resetMaxDiff).toBeLessThanOrEqual(1e-6 * Math.max(1, maxAbs));
        const insertAutomationId = engine.resolveStripInsertAutomationId(7, 0, 'band0.gainDb');
        expect(insertAutomationId).toBeGreaterThan(0);
        let beforeAutomation = posted.length;
        engine.setAutomationLane(insertAutomationId, [{ ppq: 0, value: -12 }]);
        replay(posted.slice(beforeAutomation));
        for (let block = 0; block < 16; block += 1) {
          offline.process([new Float32Array(blockSize), new Float32Array(blockSize)]);
          expect(
            live.process([[]], [[new Float32Array(blockSize), new Float32Array(blockSize)]]),
          ).toBe(true);
        }
        beforeAutomation = posted.length;
        engine.setAutomationLane(insertAutomationId, []);
        replay(posted.slice(beforeAutomation));
        let releasedEnergy = 0;
        for (let block = 0; block < 32; block += 1) {
          const output = offline.process([
            new Float32Array(blockSize),
            new Float32Array(blockSize),
          ]);
          expect(
            live.process([[]], [[new Float32Array(blockSize), new Float32Array(blockSize)]]),
          ).toBe(true);
          if (block < 12) {
            continue;
          }
          for (const channel of output) {
            for (const sample of channel) {
              releasedEnergy += sample ** 2;
            }
          }
        }
        expect(releasedEnergy / afterResetEnergy).toBeGreaterThan(0.9);
        expect(releasedEnergy / afterResetEnergy).toBeLessThan(1.1);
        const beforeOneShotCheck = posted.length;
        engine.setTrackSends(7, []);
        expect(
          (posted.at(-1) as { insertParamOverrides?: unknown[] }).insertParamOverrides ?? [],
        ).toEqual([]);
        replay(posted.slice(beforeOneShotCheck));

        // A later full replacement may remove a sidechained insert entirely.
        // Its cached binding must be pruned before offline replay and posting.
        let beforeChange = posted.length;
        engine.setStripInsertParam(7, 0, { name: 'band0.gainDb', value: -6 });
        replay(posted.slice(beforeChange));
        offline.process([new Float32Array(blockSize), new Float32Array(blockSize)]);
        expect(
          live.process([[]], [[new Float32Array(blockSize), new Float32Array(blockSize)]]),
        ).toBe(true);
        beforeChange = posted.length;
        engine.setStripInsertParam(7, 0, { name: 'band0.gainDb', value: 6 });
        replay(posted.slice(beforeChange));
        // These messages are sidechain deltas while the track insert is
        // gliding. They must not rebuild lanes and freeze that glide live.
        beforeChange = posted.length;
        engine.setBusSidechain(100, 0, { kind: 'track', id: 7 });
        engine.setStripSidechain('master', 0, { kind: 'bus', id: 100 });
        replay(posted.slice(beforeChange));
        let sidechainMaxDiff = 0;
        let sidechainMaxAbs = 0;
        // The offline mirror stores the target immediately; live playback
        // glides to it. Compare their audible settled output after that glide.
        for (let block = 0; block < 64; block += 1) {
          const expected = offline.process([
            new Float32Array(blockSize),
            new Float32Array(blockSize),
          ]);
          const liveOut = [new Float32Array(blockSize), new Float32Array(blockSize)];
          expect(live.process([[]], [liveOut])).toBe(true);
          if (block < 56) {
            continue;
          }
          for (let channel = 0; channel < 2; channel += 1) {
            for (let i = 0; i < blockSize; i += 1) {
              sidechainMaxAbs = Math.max(sidechainMaxAbs, Math.abs(expected[channel][i]));
              sidechainMaxDiff = Math.max(
                sidechainMaxDiff,
                Math.abs(expected[channel][i] - liveOut[channel][i]),
              );
            }
          }
        }
        expect(sidechainMaxAbs).toBeGreaterThan(0.01);
        expect(sidechainMaxDiff).toBeLessThanOrEqual(1e-6 * Math.max(1, maxAbs));
        beforeChange = posted.length;
        engine.setBusInsertParam(100, 0, { name: 'band0.gainDb', value: 12 });
        engine.setStripInsertParam('master', 0, { name: 'band0.gainDb', value: -3 });
        replay(posted.slice(beforeChange));
        offline.process([new Float32Array(blockSize), new Float32Array(blockSize)]);
        expect(
          live.process([[]], [[new Float32Array(blockSize), new Float32Array(blockSize)]]),
        ).toBe(true);
        beforeChange = posted.length;
        engine.setBusStrip(
          100,
          busDocument('{"version":2,"strips":[],"buses":[{"id":"100"}],"connections":[]}'),
        );
        engine.setStrip(
          'master',
          stripDocument('{"version":2,"strips":[{"id":"master"}],"buses":[],"connections":[]}'),
        );
        replay(posted.slice(beforeChange));
        const noInserts = posted.at(-1) as {
          busSidechains: unknown[];
          masterSidechains: unknown[];
          insertParamOverrides?: unknown[];
        };
        expect(noInserts.busSidechains).toEqual([]);
        expect(noInserts.masterSidechains).toEqual([]);
        expect(noInserts.insertParamOverrides ?? []).toEqual([
          expect.objectContaining({ kind: 'track', trackId: 7, value: 6 }),
        ]);
        expect(
          livePosted.filter((message) => (message as { type?: string }).type === 'syncError'),
        ).toEqual([]);
      } finally {
        live.destroy();
        engine.destroy();
      }
    });

    it('keeps track and master strip scalar setters across a syncMixer re-post', async () => {
      // The in-place strip path re-applies every scalar in the re-posted JSON
      // (pan, pan law/mode, dual pan, channel delay, and the master fader/pan),
      // so a setter the cache did not absorb is reverted on the worklet only.
      const blockSize = 128;
      const posted: unknown[] = [];
      const offline = new (await import('../dist/index.js')).RealtimeEngine({
        sampleRate: 48000,
        maxBlockSize: blockSize,
      }) as unknown as OfflineEngineOption;
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        offlineEngine: offline,
        offlineNumChannels: 2,
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      const livePosted: unknown[] = [];
      const live = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 48000,
        maxBlockSize: blockSize,
        numChannels: 2,
        transport: { postMessage: (message) => livePosted.push(message) },
      });
      try {
        engine.setTrackLanes([{ trackId: 7 }, { trackId: 8 }]);
        engine.setStrip(
          7,
          stripDocument('{"version":2,"strips":[{"id":"track-7"}],"buses":[],"connections":[]}'),
        );
        engine.setStrip(
          8,
          stripDocument('{"version":2,"strips":[{"id":"track-8"}],"buses":[],"connections":[]}'),
        );
        engine.setStrip(
          'master',
          stripDocument('{"version":2,"strips":[{"id":"master"}],"buses":[],"connections":[]}'),
        );
        engine.setStripPanMode(7, 'stereoPan');
        engine.setStripPan(7, -0.5);
        engine.setStripPanLaw(7, 'const6Db');
        engine.setStripPanMode(8, 'dualPan');
        engine.setStripDualPan(8, 0.2, 0.9);
        engine.setStripChannelDelaySamples(8, 16);
        expect(engine.setStripFaderDb('master', -4)).toBeUndefined();
        expect(engine.setStripPan('master', 0.3)).toBeUndefined();
        const replay = (messages: unknown[]): void => {
          for (const message of messages) {
            const type = (message as { type?: unknown }).type;
            if (typeof type === 'string' && type in ENGINE_SYNC_MESSAGE_TYPES) {
              live.receiveMessage(message);
            } else if (typeof type === 'number') {
              live.receiveMessage(message);
            }
          }
        };
        replay(posted);
        // A running worklet drains the queued parameter commands before any
        // later re-post arrives, so render a (stopped) block on both first.
        offline.process([new Float32Array(blockSize), new Float32Array(blockSize)]);
        expect(
          live.process([[]], [[new Float32Array(blockSize), new Float32Array(blockSize)]]),
        ).toBe(true);
        const beforeRepost = posted.length;
        // Any routing change re-posts every cached strip.
        engine.setTrackSends(7, []);
        replay(posted.slice(beforeRepost));
        expect(
          livePosted.filter((message) => (message as { type?: string }).type === 'syncError'),
        ).toEqual([]);
        const frames = blockSize * 48;
        const left = new Float32Array(frames);
        const right = new Float32Array(frames);
        for (let i = 0; i < frames; i += 1) {
          left[i] = 0.3 * Math.sin((2 * Math.PI * 700 * i) / 48000);
          right[i] = 0.2 * Math.sin((2 * Math.PI * 2100 * i) / 48000);
        }
        const clips = [
          { id: 1, trackId: 7, channels: [left, right], startPpq: 0, lengthSamples: frames },
          { id: 2, trackId: 8, channels: [right, left], startPpq: 0, lengthSamples: frames },
        ];
        offline.setClips(clips);
        live.receiveMessage({ type: 'syncClips', clips });
        offline.play();
        live.receiveMessage({ type: SonareEngineCommandType.TransportPlay, renderFrame: -1 });
        let maxDiff = 0;
        let maxAbs = 0;
        for (let block = 0; block < 40; block += 1) {
          const expected = offline.process([
            new Float32Array(blockSize),
            new Float32Array(blockSize),
          ]);
          const liveOut = [new Float32Array(blockSize), new Float32Array(blockSize)];
          expect(live.process([[]], [liveOut])).toBe(true);
          // Past the parameter glides, which the two engines start a block apart.
          if (block < 20) {
            continue;
          }
          for (let channel = 0; channel < 2; channel += 1) {
            for (let i = 0; i < blockSize; i += 1) {
              maxAbs = Math.max(maxAbs, Math.abs(expected[channel][i]));
              maxDiff = Math.max(maxDiff, Math.abs(expected[channel][i] - liveOut[channel][i]));
            }
          }
        }
        expect(maxAbs).toBeGreaterThan(0.05);
        expect(maxDiff).toBeLessThanOrEqual(1e-6 * Math.max(1, maxAbs));
      } finally {
        live.destroy();
        engine.destroy();
      }
    });

    it('requests capture status, audio, and reset over the worklet port', async () => {
      const posted: unknown[] = [];
      const port = {
        onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
        postMessage(message: unknown) {
          posted.push(message);
          if (
            typeof message === 'object' &&
            message !== null &&
            (message as { type?: unknown }).type === 'captureRequest'
          ) {
            const request = message as { requestId: number; op: string };
            const response =
              request.op === 'status'
                ? {
                    type: 'captureResponse',
                    requestId: request.requestId,
                    ok: true,
                    status: {
                      capturedFrames: 128,
                      overflowCount: 0,
                      armed: true,
                      punchEnabled: false,
                      source: 'input',
                      recordOffsetSamples: -12,
                    },
                  }
                : request.op === 'read'
                  ? {
                      type: 'captureResponse',
                      requestId: request.requestId,
                      ok: true,
                      channels: [new Float32Array([0.5, 0.25])],
                    }
                  : { type: 'captureResponse', requestId: request.requestId, ok: true };
            port.onmessage?.({ data: response } as MessageEvent<unknown>);
          }
        },
      };
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () => readyWorkletNode(port),
      });

      await expect(engine.captureStatus()).resolves.toMatchObject({
        capturedFrames: 128,
        source: 'input',
        recordOffsetSamples: -12,
      });
      const audio = await engine.capturedAudio();
      expect(audio[0][0]).toBeCloseTo(0.5, 4);
      await expect(engine.resetCapture()).resolves.toBeUndefined();
      expect(posted).toEqual(
        expect.arrayContaining([
          expect.objectContaining({ type: 'captureRequest', op: 'status' }),
          expect.objectContaining({ type: 'captureRequest', op: 'read' }),
          expect.objectContaining({ type: 'captureRequest', op: 'reset' }),
        ]),
      );
      engine.destroy();
    });

    it('returns capture typed arrays directly and rejects malformed channel payloads', async () => {
      const posted: unknown[] = [];
      const port = {
        onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
        postMessage(message: unknown) {
          posted.push(message);
        },
      };
      const node = await SonareRealtimeEngineNode.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () => readyWorkletNode(port),
      });
      await node.ready;
      try {
        const channel = new Float32Array([0.5, 0.25]);
        const pending = node.requestCapturedAudio();
        const request = posted.at(-1) as { requestId: number };
        port.onmessage?.({
          data: {
            type: 'captureResponse',
            requestId: request.requestId,
            ok: true,
            channels: [channel],
          },
        } as MessageEvent<unknown>);
        const audio = await pending;
        expect(audio[0]).toBe(channel);

        const malformedChannels: unknown[][] = [
          [[0.5, 0.25]],
          [new Float32Array([0.5]), [0.25]],
          [new Float32Array(new SharedArrayBuffer(Float32Array.BYTES_PER_ELEMENT))],
        ];
        for (const channels of malformedChannels) {
          const malformed = node.requestCapturedAudio();
          const malformedRequest = posted.at(-1) as { requestId: number };
          port.onmessage?.({
            data: {
              type: 'captureResponse',
              requestId: malformedRequest.requestId,
              ok: true,
              channels,
            },
          } as MessageEvent<unknown>);
          await expect(malformed).rejects.toThrow('Malformed capture response.');
        }
      } finally {
        node.destroy();
      }
    });

    it('syncs time signatures and requests transport state over the worklet port', async () => {
      const posted: unknown[] = [];
      const port = {
        onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
        postMessage(message: unknown) {
          posted.push(message);
          if (
            typeof message === 'object' &&
            message !== null &&
            (message as { type?: unknown }).type === 'transportRequest'
          ) {
            const request = message as { requestId: number };
            port.onmessage?.({
              data: {
                type: 'transportResponse',
                requestId: request.requestId,
                ok: true,
                state: {
                  playing: true,
                  looping: true,
                  renderFrame: 128,
                  samplePosition: 48000,
                  ppq: 1,
                  bpm: 90,
                  barStartPpq: 0,
                  barIndex: 1,
                  timeSignature: { numerator: 7, denominator: 8, confidence: 1 },
                  loopStartPpq: 1,
                  loopEndPpq: 3,
                  sampleRate: 48000,
                },
              },
            } as MessageEvent<unknown>);
          }
        },
      };
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () => readyWorkletNode(port),
      });

      engine.setTempo(90);
      engine.setTimeSignature(7, 8);
      engine.setTempoSegments([
        { startPpq: 0, bpm: 90 },
        { startPpq: 4, bpm: 60 },
      ]);
      engine.setTimeSignatureSegments([
        { startPpq: 0, numerator: 7, denominator: 8 },
        { startPpq: 8, numerator: 3, denominator: 4 },
      ]);
      const firstMarker = engine.addMarker(1, 'in');
      const secondMarker = engine.addMarker(3, 'out');
      expect(engine.markers().length).toBe(2);
      expect(engine.markers()[0]).toMatchObject({ id: firstMarker, ppq: 1, name: 'in' });
      expect(engine.marker(secondMarker)).toMatchObject({ id: secondMarker, ppq: 3 });
      expect(engine.setLoopFromMarkers(firstMarker, secondMarker)).toBeUndefined();
      await expect(engine.transportState()).resolves.toMatchObject({
        playing: true,
        bpm: 90,
        timeSignature: { numerator: 7, denominator: 8 },
      });
      expect(engine.cachedTransportState()).toMatchObject({ samplePosition: 48000 });
      expect(posted).toEqual(
        expect.arrayContaining([
          expect.objectContaining({
            type: 'syncTempo',
            bpm: 90,
            timeSignature: { numerator: 7, denominator: 8 },
            tempoSegments: [
              { startPpq: 0, bpm: 90 },
              { startPpq: 4, bpm: 60 },
            ],
            timeSignatureSegments: [
              { startPpq: 0, numerator: 7, denominator: 8 },
              { startPpq: 8, numerator: 3, denominator: 4 },
            ],
          }),
          expect.objectContaining({
            type: SonareEngineCommandType.SetLoop,
            argFloat: 1,
            argFloat2: 3,
          }),
          expect.objectContaining({ type: 'transportRequest', op: 'state' }),
        ]),
      );
      engine.destroy();
    });

    it('replaces the whole marker set via setMarkers', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            addEventListener: () => undefined,
            removeEventListener: () => undefined,
            start: () => undefined,
          }),
      });

      const stale = engine.addMarker(9, 'stale');
      const resolved = engine.setMarkers([
        { ppq: 1, name: 'verse' },
        { ppq: 5, name: 'chorus' },
      ]);
      expect(resolved).toHaveLength(2);
      expect(resolved[0].id).not.toBe(stale);
      expect(engine.markers().length).toBe(2);
      expect(engine.markers()[0]).toMatchObject({ ppq: 1, name: 'verse' });
      expect(engine.marker(resolved[1].id)).toMatchObject({ ppq: 5, name: 'chorus' });
      expect(() => engine.marker(stale)).toThrow();

      // Explicit ids are kept; fresh ids never collide with them afterwards.
      const explicit = engine.setMarkers([{ ppq: 2, name: 'mark', id: 41 }]);
      expect(explicit[0].id).toBe(41);
      expect(engine.addMarker(3, 'after')).toBeGreaterThan(41);

      expect(() => engine.setMarkers([{ ppq: Number.NaN }])).toThrow(/Invalid marker ppq/);
      expect(() => engine.setMarkers([{ ppq: 0, id: 0 }])).toThrow(/Invalid marker id/);
      expect(() =>
        engine.setMarkers([
          { ppq: 0, id: 7 },
          { ppq: 1, id: 7 },
        ]),
      ).toThrow(/Duplicate marker id/);

      // A list past the native budget is refused before any entry is read.
      let reads = 0;
      const oversized = new Proxy([] as unknown[], {
        get(target, key, receiver) {
          if (key === 'length') {
            return 64 * 1024 * 1024 + 1;
          }
          if (typeof key === 'string' && /^\d+$/.test(key)) {
            reads++;
          }
          return Reflect.get(target, key, receiver);
        },
      }) as [];
      expect(() => engine.setMarkers(oversized)).toThrow(
        expect.objectContaining({
          name: 'RangeError',
          message: 'SonareEngine.setMarkers: markers exceeds the WASM Float32 input budget',
        }),
      );
      expect(reads).toBe(0);

      // Clearing posts an empty replace-all sync to the worklet.
      engine.setMarkers([]);
      expect(engine.markers().length).toBe(0);
      expect(posted).toEqual(
        expect.arrayContaining([expect.objectContaining({ type: 'syncMarkers', markers: [] })]),
      );
      engine.destroy();
    });

    it('refuses every list-taking setter past the native budget without reading an entry', async () => {
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: () => undefined,
            addEventListener: () => undefined,
            removeEventListener: () => undefined,
            start: () => undefined,
          }),
      });
      let reads = 0;
      const oversized = new Proxy([] as unknown[], {
        get(target, key, receiver) {
          if (key === 'length') {
            return 64 * 1024 * 1024 + 1;
          }
          if (typeof key === 'string' && /^\d+$/.test(key)) {
            reads++;
            throw new Error('entry read');
          }
          return Reflect.get(target, key, receiver);
        },
      }) as never;
      for (const row of SONARE_ENGINE_ROWS) {
        reads = 0;
        expect(() => row.call(engine, oversized), `${row.method} ${row.text}`).toThrow(
          expect.objectContaining({
            name: 'RangeError',
            message: `SonareEngine.${row.method}: ${row.text}`,
          }),
        );
        expect(reads, `${row.method} ${row.text} reads`).toBe(0);
      }
      engine.destroy();
    });

    it('replaces and clears automation lanes via setAutomationLane', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            addEventListener: () => undefined,
            removeEventListener: () => undefined,
            start: () => undefined,
          }),
      });

      // Reserved mixer namespace encoding: master = lane 0xff; kind 1 = faderDb,
      // kind 2 = pan. Track and bus ids are resolved by track or bus and stay put
      // when another lane or bus is declared.
      const masterFader = engine.resolveStripAutomationId('master', 'faderDb');
      expect(masterFader).toBe(0x4d58ff01);
      expect(engine.resolveStripAutomationId('master', 'pan')).toBe(0x4d58ff02);
      const track10Fader = engine.resolveStripAutomationId(10, 'faderDb');
      const track10Pan = engine.resolveStripAutomationId(10, 'pan');
      expect(track10Fader).toBeGreaterThan(0);
      expect(track10Pan).not.toBe(track10Fader);
      expect(engine.resolveStripAutomationId(20, 'faderDb')).not.toBe(track10Fader);
      expect(engine.resolveStripAutomationId(10, 'faderDb')).toBe(track10Fader);
      const bus1Fader = engine.resolveBusAutomationId(1, 'faderDb');
      expect(bus1Fader).toBeGreaterThan(0);
      expect(engine.resolveBusAutomationId(2, 'faderDb')).not.toBe(bus1Fader);
      expect(engine.resolveBusAutomationId(1, 'faderDb')).toBe(bus1Fader);

      // Replace-all installs the sorted lane on the offline engine and mirrors
      // it to the live worklet via syncAutomation.
      engine.setAutomationLane(masterFader, [
        { ppq: 4, value: -12 },
        { ppq: 0, value: 0 },
      ]);
      expect(engine.automationLaneCount).toBe(1);
      expect(posted).toEqual(
        expect.arrayContaining([
          expect.objectContaining({
            type: 'syncAutomation',
            paramId: masterFader,
            points: [
              { ppq: 0, value: 0 },
              { ppq: 4, value: -12 },
            ],
          }),
        ]),
      );

      // A second replace overwrites rather than appends.
      engine.setAutomationLane(masterFader, [{ ppq: 1, value: -6 }]);
      expect(engine.automationLaneCount).toBe(1);

      // Clearing posts an empty replace-all sync to the worklet.
      engine.setAutomationLane(masterFader, []);
      expect(posted).toEqual(
        expect.arrayContaining([
          expect.objectContaining({ type: 'syncAutomation', paramId: masterFader, points: [] }),
        ]),
      );
      engine.destroy();
    });

    it('clears insert automation on full strip replacement without clearing other strips', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      const scene = (kind: 'track' | 'bus' | 'master', withInsert: boolean) =>
        JSON.stringify({
          version: 2,
          strips:
            kind === 'bus'
              ? []
              : [
                  {
                    id: kind === 'track' ? 'track-7' : 'master',
                    inserts: withInsert
                      ? [{ slot: 'pre', processor: 'eq.parametric', params: { 'band0.gainDb': 0 } }]
                      : [],
                  },
                ],
          buses:
            kind === 'bus'
              ? [
                  {
                    id: '100',
                    inserts: withInsert
                      ? [{ slot: 'pre', processor: 'eq.parametric', params: { 'band0.gainDb': 0 } }]
                      : [],
                  },
                ]
              : [],
          connections: [],
        });
      try {
        engine.setTrackLanes([{ trackId: 7 }]);
        engine.setStrip(7, stripDocument(scene('track', true)));
        engine.setBusStrip(100, busDocument(scene('bus', true)));
        engine.setStrip('master', stripDocument(scene('master', true)));
        const ids = [
          engine.resolveStripInsertAutomationId(7, 0, 'band0.gainDb'),
          engine.resolveBusInsertAutomationId(100, 0, 'band0.gainDb'),
          engine.resolveStripInsertAutomationId('master', 0, 'band0.gainDb'),
        ];
        expect(ids.every((id) => id >= 0)).toBe(true);
        expect(new Set(ids).size).toBe(3);
        for (const id of ids) {
          engine.setAutomationLane(id, [{ ppq: 0, value: 6 }]);
        }
        expect(engine.automationLaneCount).toBe(3);

        const replace = [
          () => engine.setStrip(7, stripDocument(scene('track', false))),
          () => engine.setBusStrip(100, busDocument(scene('bus', false))),
          () => engine.setStrip('master', stripDocument(scene('master', false))),
        ];
        for (let index = 0; index < replace.length; index += 1) {
          const before = posted.length;
          replace[index]();
          expect(engine.automationLaneCount).toBe(2 - index);
          expect(posted.slice(before)).toEqual(
            expect.arrayContaining([
              expect.objectContaining({ type: 'syncAutomation', paramId: ids[index], points: [] }),
            ]),
          );
          expect(
            posted
              .slice(before)
              .filter((message) =>
                ids.some(
                  (id) =>
                    id !== ids[index] &&
                    (message as { type?: string; paramId?: number }).type === 'syncAutomation' &&
                    (message as { paramId?: number }).paramId === id,
                ),
              ),
          ).toEqual([]);
        }

        // A UI may cache the resolved id while repeatedly editing the same
        // slot. Reinstalling a lane through that id must remain associated
        // with the strip for the next full replacement.
        engine.setStrip(7, stripDocument(scene('track', true)));
        engine.setAutomationLane(ids[0], [{ ppq: 0, value: 3 }]);
        expect(engine.automationLaneCount).toBe(1);
        engine.setStrip(7, stripDocument(scene('track', false)));
        expect(engine.automationLaneCount).toBe(0);
      } finally {
        engine.destroy();
      }
    });

    it('removes a deleted bus automation lane before its identity is reused', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      const scene = JSON.stringify({
        version: 2,
        strips: [],
        buses: [
          {
            id: '100',
            inserts: [{ slot: 'pre', processor: 'eq.parametric', params: { 'band0.gainDb': 0 } }],
          },
        ],
        connections: [],
      });
      try {
        engine.setBusStrip(100, busDocument(scene));
        const oldId = engine.resolveBusInsertAutomationId(100, 0, 'band0.gainDb');
        expect(oldId).toBeGreaterThanOrEqual(0);
        engine.setAutomationLane(oldId, [{ ppq: 0, value: 5 }]);
        expect(engine.automationLaneCount).toBe(1);

        const beforeRemoval = posted.length;
        engine.setTrackBuses([]);
        expect(engine.automationLaneCount).toBe(0);
        expect(posted.slice(beforeRemoval)).toEqual(
          expect.arrayContaining([
            expect.objectContaining({ type: 'syncAutomation', paramId: oldId, points: [] }),
          ]),
        );

        engine.setBusStrip(100, busDocument(scene));
        const newId = engine.resolveBusInsertAutomationId(100, 0, 'band0.gainDb');
        expect(newId).toBeGreaterThanOrEqual(0);
        expect(newId).not.toBe(oldId);
        expect(engine.automationLaneCount).toBe(0);
      } finally {
        engine.destroy();
      }
    });

    it('sets and reads the warp voice capacity, mirroring it to the worklet', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            addEventListener: () => undefined,
            removeEventListener: () => undefined,
            start: () => undefined,
          }),
      });

      expect(engine.warpVoiceCapacity).toBe(8);

      engine.setWarpVoiceCapacity(12);
      expect(engine.warpVoiceCapacity).toBe(12);
      expect(posted).toEqual(
        expect.arrayContaining([
          { type: 'syncWarpVoiceCapacity', voices: 12, entry: 'SonareEngine.setWarpVoiceCapacity' },
        ]),
      );

      // A rejected value leaves the previously accepted capacity in place on
      // both the offline mirror and the worklet (no further sync is posted).
      posted.length = 0;
      expect(() => engine.setWarpVoiceCapacity(65)).toThrow();
      expect(engine.warpVoiceCapacity).toBe(12);
      expect(posted).toEqual([]);

      engine.destroy();
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
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () => {
          const node = readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          });
          node.disconnect = () => disconnected.push(true);
          return node;
        },
      });

      await engine.suspend();
      await engine.resume();
      expect(lifecycle).toEqual(['suspend', 'resume']);
      expect(engine.transport.play()).toBeUndefined();
      engine.destroy();
      expect(disconnected).toEqual([true]);
      expect(posted.at(-1)).toMatchObject({ type: 'destroy' });
      expect(() => engine.transport.play()).toThrow(
        expect.objectContaining({ code: ErrorCode.InvalidState }),
      );
      await engine.suspend();
      await engine.resume();
      expect(lifecycle).toEqual(['suspend', 'resume']);
    });

    it('reports a destroyed node as InvalidState on every command and request family', async () => {
      const posted: unknown[] = [];
      const node = await SonareRealtimeEngineNode.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      const invalidState = expect.objectContaining({
        name: 'SonareError',
        code: ErrorCode.InvalidState,
      });
      // Requests still pending when destroy runs.
      const pending = [
        node.requestCaptureStatus(),
        node.requestTransportState(),
        node.pollInsertGainReduction(0),
      ];
      node.destroy();
      for (const request of pending) {
        await expect(request).rejects.toEqual(invalidState);
      }
      for (const call of [
        () => node.play(),
        () => node.stop(),
        () => node.seekSample(0),
        () => node.seekPpq(0),
        () => node.seekPpq(-1),
        () => node.resetProcessorState(),
        () => node.sendCommand({ type: SonareEngineCommandType.TransportPlay }),
        () => node.canSendCommand({ type: SonareEngineCommandType.TransportPlay }),
        () => node.postSync({ type: 'syncMarkers', markers: [] }),
      ]) {
        expect(call).toThrow(invalidState);
      }
      for (const request of [
        () => node.requestCaptureStatus(),
        () => node.requestCapturedAudio(),
        () => node.requestCaptureReset(),
        () => node.requestTransportState(),
        () => node.pollInsertGainReduction(0),
      ]) {
        await expect(request()).rejects.toEqual(invalidState);
      }
    });

    it('commits a control edit to the offline mirror only when the live engine takes it', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      const offline = (engine as unknown as { offlineEngine: RealtimeEngineLike }).offlineEngine;
      try {
        // Both bounds reach the live command as the exact doubles the mirror receives.
        expect(engine.setLoop({ startPpq: 0, endPpq: 4 / 3 })).toBeUndefined();
        expect(posted.at(-1)).toMatchObject({ argFloat: 0, argFloat2: 4 / 3 });
        expect(offline.transportState().loopEndPpq).toBeCloseTo(4 / 3, 12);
        expect(engine.setLoop({ startPpq: 0, endPpq: 1e10 })).toBeUndefined();
        expect(posted.at(-1)).toMatchObject({ argFloat2: 1e10 });
        // A loop end the live engine refuses changes neither engine.
        posted.length = 0;
        expect(() => engine.setLoop({ startPpq: 0, endPpq: 2e12 })).toThrow(RangeError);
        expect(posted).toEqual([]);
        expect(offline.transportState().loopEndPpq).toBeCloseTo(1e10, 0);

        // A refused seek in seconds changes neither engine.
        engine.transport.seekSec(1);
        const position = offline.transportState().samplePosition;
        expect(position).toBe(48000);
        for (const seconds of [-1, Number.NaN, Number.POSITIVE_INFINITY]) {
          expect(() => engine.transport.seekSec(seconds)).toThrow(
            new RangeError('SonareEngine.transport.seekSec: timeSec must be finite and at least 0'),
          );
        }
        expect(offline.transportState().samplePosition).toBe(position);

        // A refused bus gain leaves the cached bus, so a later mixer sync does not replay it.
        expect(engine.setBusGainDb(1, -6)).toBeUndefined();
        expect(() => engine.setBusGainDb(1, Number.NaN)).toThrow(RangeError);
        posted.length = 0;
        engine.setTrackLanes([{ trackId: 7 }]);
        const mixer = posted
          .filter((message) => (message as { type?: unknown }).type === 'syncMixer')
          .at(-1) as { buses?: Array<{ busId: number; gainDb?: number }> } | undefined;
        expect(mixer?.buses?.find((bus) => bus.busId === 1)?.gainDb).toBe(-6);
      } finally {
        engine.destroy();
      }
    });

    it('keeps the offline recording when the live capture reset is refused', async () => {
      const engine = await SonareEngine.create(fakeContext(), {
        numChannels: 2,
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => {
              if ((message as { type?: unknown }).type === 'captureRequest') {
                throw new Error('port closed');
              }
            },
            onmessage: undefined,
          }),
      });
      const offline = (engine as unknown as { offlineEngine: { resetCapture: () => void } })
        .offlineEngine;
      const reset = vi.spyOn(offline, 'resetCapture');
      try {
        await expect(engine.resetCapture()).rejects.toThrow('port closed');
        expect(reset).not.toHaveBeenCalled();
      } finally {
        engine.destroy();
      }
    });
  });
});
