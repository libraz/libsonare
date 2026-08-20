import {
  describe,
  expect,
  it,
  popSonareEngineCommandRingBuffer,
  pushSonareClipPageRequestRingBuffer,
  SonareEngine,
  SonareEngineCommandType,
  SonareEngineTelemetryError,
  SonareEngineTelemetryType,
  SonareRealtimeEngineNode,
  setupWorklet,
  writeSonareEngineTelemetryRingBuffer,
} from './_worklet_helpers';

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
      expect(node.clipPageRequestRing).toBeDefined();
      expect(node.capabilities.clipPageRequestsRealtimeSafe).toBe(true);
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
      expect(
        node.sendCommand({
          type: SonareEngineCommandType.SetTrackMonitorMode,
          targetId: 3,
          sampleTime: 256,
          argInt: 1,
        }),
      ).toBe(true);
      expect(popSonareEngineCommandRingBuffer(commandRing)).toEqual({
        type: SonareEngineCommandType.SetTrackMonitorMode,
        targetId: 3,
        sampleTime: 256,
        argFloat: 0,
        argInt: 1,
      });
      // 18..25 are intentionally not part of the worklet command vocabulary.
      expect(node.sendCommand({ type: 18, sampleTime: -1 })).toBe(false);
      expect(
        node.sendCommand({
          type: SonareEngineCommandType.SetTrackMonitorMode,
          sampleTime: -1,
          argInt: 0.5,
        }),
      ).toBe(false);
      for (const targetId of [undefined, -1, 0x1_0000_0000]) {
        expect(
          node.sendCommand({
            type: SonareEngineCommandType.SetTrackMonitorMode,
            targetId,
            sampleTime: -1,
            argInt: 1,
          }),
        ).toBe(false);
      }

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
          blockSize: 64,
          nodeFactory: () =>
            readyWorkletNode({ postMessage: () => undefined, onmessage: undefined }),
        }),
      ).rejects.toThrow(/blockSize.*128/);
    });

    it('surfaces worklet sync rejections after ready', async () => {
      const port = {
        postMessage: () => undefined,
        onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
      };
      const node = await SonareRealtimeEngineNode.create(fakeContext(), {
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

    it('propagates a pre-registered worklet initialization error', async () => {
      const port = {
        postMessage: () => undefined,
        onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
      };
      const enginePromise = SonareEngine.create(fakeContext(), {
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
      const offline = new (await import('../dist/index.js')).RealtimeEngine(
        48000,
        128,
      ) as unknown as OfflineEngineOption;
      offline.prepare(48000, 128, 4, 4);
      offline.addParameter({
        id: 7,
        name: 'gain',
        unit: 'dB',
        minValue: -60,
        maxValue: 12,
        defaultValue: 0,
        rtSafe: true,
        defaultCurve: 2,
      });
      const originalFlush = offline.flushControlCommands.bind(offline);
      let flushCount = 0;
      offline.flushControlCommands = () => {
        flushCount += 1;
        originalFlush();
      };
      const engine = await SonareEngine.create(fakeContext(), {
        mode: 'postMessage',
        offlineEngine: offline,
        nodeFactory: () => readyWorkletNode({ postMessage: () => undefined, onmessage: undefined }),
      });
      try {
        // Engine creation synchronizes the pre-registered parameter set once;
        // this assertion measures only the subsequent command-side flushes.
        flushCount = 0;
        for (let index = 0; index < 32; index++) {
          expect(engine.setParam('gain-node', 'gain', index - 60)).toBe(true);
        }
        expect(flushCount).toBe(32);
      } finally {
        engine.destroy();
      }
    });

    it('syncs registered parameters and rejects unresolved parameter names', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
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
          defaultCurve: 2,
        });
        expect(engine.setParam('gain-node', 'gain', -6)).toBe(true);
        expect(() => engine.setParam('gain-node', 'missing', -6)).toThrow(
          /Unknown engine parameter/,
        );
        engine.setMidiInputSource(3);
        engine.bindMidiCc(0, 74, 7, { minValue: -60, maxValue: 12 });
        engine.pushMidiInputNoteOn(0, 0, 60, 100, 128);
        engine.pushMidiInputCc(0, 0, 74, 96, 128);
        engine.clearMidiInputSource();
        engine.clearParameters();
        expect(engine.listParameters()).toHaveLength(0);
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
              channel: 0,
              controller: 74,
              paramId: 7,
              minValue: -60,
              maxValue: 12,
            }),
            expect.objectContaining({
              type: 'syncMidiInputNoteOn',
              data0: 60,
              data1: 100,
              portTimeSamples: 128,
            }),
            expect.objectContaining({ type: 'syncMidiInputCc', data0: 74, data1: 96 }),
            expect.objectContaining({ type: 'syncClearMidiInputSource' }),
          ]),
        );
      } finally {
        engine.destroy();
      }
    });

    it('creates the scope ring only when scope telemetry is requested', async () => {
      const makeNode = (scopeIntervalFrames?: number) =>
        SonareRealtimeEngineNode.create(fakeContext(), {
          moduleUrl: 'sonare-worklet.js',
          blockSize: 128,
          channelCount: 2,
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

    it('falls back to postMessage commands when requested', async () => {
      const posted: unknown[] = [];
      const node = await SonareRealtimeEngineNode.create(fakeContext(), {
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
      const offline = new (await import('../dist/index.js')).RealtimeEngine(
        48000,
        128,
      ) as unknown as OfflineEngineOption;
      offline.addParameter({
        id: 7,
        name: 'gain',
        unit: 'dB',
        minValue: -60,
        maxValue: 12,
        defaultValue: 0,
        rtSafe: true,
        defaultCurve: 2, // canonical AutomationCurve::Hold (preserve original semantic)
      });
      const engine = await SonareEngine.create(fakeContext(), {
        mode: 'postMessage',
        offlineEngine: offline,
        offlineChannelCount: 2,
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      try {
        expect(engine.capabilities.mode).toBe('postMessage');
        expect(engine.listParameters()).toHaveLength(1);
        expect(posted).toEqual(
          expect.arrayContaining([
            expect.objectContaining({
              type: 'syncParameters',
              parameters: [expect.objectContaining({ id: 7, name: 'gain' })],
            }),
          ]),
        );
        expect(engine.transport.play()).toBe(true);
        expect(engine.transport.seekSeconds(1)).toBe(true);
        engine.transport.setTempo(90);
        engine.transport.setTempoSegments([
          { startPpq: 0, bpm: 90 },
          { startPpq: 4, bpm: 60 },
        ]);
        expect(engine.transport.setLoop(0, 1, true)).toBe(true);
        expect(engine.setParam('gain-node', 'gain', -6)).toBe(true);
        engine.scheduleParam('gain-node', 'gain', 0.5, -3);
        engine.addAutomationPoint(7, 1, 0);
        expect(engine.setSoloMute(3, true, false)).toBe(true);
        expect(engine.setStripGain(3, -6)).toBe(true);
        expect(engine.setStripPan(3, 0.25)).toBe(true);
        const trackStripJson =
          '{"version":1,"strips":[{"id":"track-3","faderDb":-6,"panLaw":3,"inserts":[{"slot":"pre","processor":"eq.parametric","params":"{\\"band0.type\\":1,\\"band0.frequencyHz\\":1000,\\"band0.gainDb\\":12,\\"band0.enabled\\":1}"}]}],"buses":[],"connections":[]}';
        const masterStripJson =
          '{"version":1,"strips":[{"id":"master","faderDb":-3,"panLaw":3,"inserts":[{"slot":"pre","processor":"eq.parametric","params":"{\\"band0.type\\":1,\\"band0.frequencyHz\\":1000,\\"band0.gainDb\\":12,\\"band0.enabled\\":1}"}]}],"buses":[],"connections":[]}';
        engine.setTrackStripJson(3, trackStripJson);
        engine.setMasterStripJson(masterStripJson);
        expect(engine.setStripGain('master', -3)).toBe(true);
        expect(engine.setStripPan('master', -0.25)).toBe(true);
        engine.setTrackStripEqBand(3, 0, { type: 'Peak', frequencyHz: 1000, gainDb: 6 });
        engine.setMasterStripEqBand(0, { type: 'Peak', frequencyHz: 1000, gainDb: 3 });
        engine.setTrackStripInsertBypassed(3, 0, true, true);
        engine.setMasterStripInsertBypassed(0, true, true);
        engine.setStripEq(3, 0, { type: 'Peak', frequencyHz: 2000, gainDb: 2 });
        engine.setStripEq('master', 0, { type: 'Peak', frequencyHz: 3000, gainDb: 1 });
        engine.setStripInsertBypassed(3, 0, false);
        engine.setStripInsertBypassed('master', 0, false);
        engine.setStripInserts(3, trackStripJson);
        engine.setMasterChain(masterStripJson);
        engine.setTrackBuses([{ busId: 100, gainDb: -3 }]);
        engine.setSends(3, [{ busId: 100, levelDb: -6, enabled: true }]);
        expect(engine.setBusGain(100, -9)).toBe(true);
        // Realtime strip panner / channel-delay controls (R5).
        engine.setTrackStripPan(3, -1);
        engine.setTrackStripPanLaw(3, 'const6dB');
        engine.setTrackStripPanMode(3, 'dualPan');
        engine.setTrackStripDualPan(3, -1, 1);
        engine.setTrackStripChannelDelaySamples(3, 32);
        const busStripJson =
          '{"version":1,"strips":[],"buses":[{"id":"100","inserts":[{"slot":"pre","processor":"eq.parametric","params":"{\\"band0.type\\":1,\\"band0.frequencyHz\\":1000,\\"band0.gainDb\\":0,\\"band0.enabled\\":1}"}]}],"connections":[]}';
        engine.setBusStripJson(100, busStripJson);
        engine.setBusStripInsertParamByName(100, 0, 'band0.gainDb', 1);
        engine.setBuiltinInstrument(3, { gain: 0.5 });
        engine.setSynthInstrument(3, 'saw-lead');
        engine.setSf2Instrument(3, { gain: 0.5 });
        // Live, non-destructive MIDI-FX insert (install then bypass).
        engine.setMidiFx(3, '{"transpose_semitones":12}');
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
        const clipId = engine.addClip(
          3,
          [new Float32Array(128).fill(0.25), new Float32Array(128).fill(-0.25)],
          0,
        );
        expect(clipId).toBeGreaterThan(0);
        engine.removeClip(clipId);
        expect(() => engine.armRecord(0, true)).toThrow(/Capture buffer is not configured/);
        engine.configureCapture({
          bufferFrames: 4096,
          channels: 2,
          source: 'input',
          recordOffsetSamples: -32,
          inputMonitor: { enabled: true, gain: 0.5 },
        });
        engine.setTempo(60);
        expect(engine.countInEndSample(0, 2)).toBe(384000);
        expect(() => engine.armRecord(3, true)).toThrow(/Capture is global/);
        expect(engine.armRecord(0, true)).toBe(true);
        expect(engine.punch(1, 1.5)).toBe(true);
        engine.setMetronome({ enabled: true, clickSamples: 16 });
        const markerId = engine.addMarker(0, 'start');
        expect(markerId).toBeGreaterThan(0);
        // seekMarker now reaches the realtime engine (previously a no-op that
        // always returned false); it returns true like the sibling transport ops.
        expect(engine.seekMarker(markerId)).toBe(true);
        const rendered = await engine.renderOffline(128);
        expect(rendered).toHaveLength(2);
        expect(rendered[0]).toHaveLength(128);
        expect(
          posted.some(
            (message) =>
              typeof message === 'object' &&
              message !== null &&
              (message as { type?: unknown }).type === 'syncBuiltinInstrument',
          ),
        ).toBe(false);
        expect(engine.transport.stop()).toBe(true);

        expect(posted).toEqual(
          expect.arrayContaining([
            expect.objectContaining({ type: SonareEngineCommandType.TransportPlay }),
            expect.objectContaining({ type: SonareEngineCommandType.TransportSeekSample }),
            expect.objectContaining({ type: SonareEngineCommandType.SetLoop }),
            expect.objectContaining({ type: SonareEngineCommandType.ArmRecord }),
            expect.objectContaining({
              type: SonareEngineCommandType.Punch,
              argInt: 48000,
              argFloat: 72000,
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
              targetId: 0x4d580001,
              argFloat: -6,
            }),
            expect.objectContaining({
              type: SonareEngineCommandType.SetParamSmoothed,
              targetId: 0x4d580002,
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
              targetId: 0x4d58fe01,
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
              trackStrips: [{ trackId: 3, sceneJson: trackStripJson }],
              masterStripJson,
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
            expect.objectContaining({ type: 'syncTrackStripPan', trackId: 3, pan: -1 }),
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
              busStrips: [{ busId: 100, sceneJson: busStripJson }],
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
              patch: 'saw-lead',
            }),
            expect.objectContaining({
              type: 'syncSf2Instrument',
              destinationId: 3,
              config: { gain: 0.5 },
            }),
            expect.objectContaining({
              type: 'syncMidiFx',
              destinationId: 3,
              configJson: '{"transpose_semitones":12}',
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
      const offline = new (await import('../dist/index.js')).RealtimeEngine(
        48000,
        128,
      ) as unknown as OfflineEngineOption;
      const engine = await SonareEngine.create(fakeContext(), {
        mode: 'postMessage',
        offlineEngine: offline,
        offlineChannelCount: 1,
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      try {
        engine.setTrackLanes([1]);
        const source = new Float32Array(4096);
        for (let i = 0; i < source.length; i++) {
          source[i] = Math.sin(i * 0.02);
        }
        const clipId = engine.addClip(1, [source], 0, {
          lengthSamples: 8192,
          warpMode: 'tempo-sync',
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
        mode: 'postMessage',
        offlineChannelCount: 1,
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            onmessage: undefined,
          }),
      });
      try {
        engine.setTrackLanes([1]);
        const source = new Float32Array(8192).fill(0.25);
        const clipId = engine.addClip(1, [source], 0, {
          lengthSamples: 16_385,
          warpMode: 'tempo-sync',
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
          2,
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
        expect(engine.setSoloMute(5, false, true)).toBe(true);
        expect(posted).toEqual(
          expect.arrayContaining([
            expect.objectContaining({
              type: SonareEngineCommandType.SetSoloMute,
              targetId: 1,
              argInt: 0x1,
            }),
          ]),
        );
        expect(engine.setTrackMonitorMode(5, 'pfl', 321)).toBe(true);
        expect(posted).toEqual(
          expect.arrayContaining([
            expect.objectContaining({
              type: SonareEngineCommandType.SetTrackMonitorMode,
              targetId: 1,
              sampleTime: 321,
              argInt: 1,
            }),
          ]),
        );
        for (const mode of [true, false, 0.5, -1, 3, 'PFL', 'post-fader']) {
          expect(() => engine.setTrackMonitorMode(5, mode as never)).toThrow(RangeError);
        }
        // Appending keeps existing lanes; entries without sends keep prior sends.
        engine.setTrackLanes([2, 5, 9]);
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
        expect(() => engine.setTrackLanes([5, 2, 9])).toThrow(/append-only/);
        expect(() => engine.setTrackLanes([2, 5])).toThrow(/append-only/);
        expect(() => engine.setTrackLanes([2, 5, 9, 9])).toThrow(/Duplicate track id/);
        expect(() => engine.setTrackLanes([2, 5, 9, 0])).toThrow(/Invalid track id/);
      } finally {
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
                  barCount: 1,
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
      expect(engine.markerCount()).toBe(2);
      expect(engine.markerByIndex(0)).toMatchObject({ id: firstMarker, ppq: 1, name: 'in' });
      expect(engine.marker(secondMarker)).toMatchObject({ id: secondMarker, ppq: 3 });
      expect(engine.setLoopFromMarkers(firstMarker, secondMarker)).toBe(true);
      await expect(engine.getTransportState()).resolves.toMatchObject({
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
            argInt: 3_000_000,
          }),
          expect.objectContaining({ type: 'transportRequest', op: 'state' }),
        ]),
      );
      engine.destroy();
    });

    it('replaces the whole marker set via setMarkers', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
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
      expect(engine.markerCount()).toBe(2);
      expect(engine.markerByIndex(0)).toMatchObject({ ppq: 1, name: 'verse' });
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

      // Clearing posts an empty replace-all sync to the worklet.
      engine.setMarkers([]);
      expect(engine.markerCount()).toBe(0);
      expect(posted).toEqual(
        expect.arrayContaining([expect.objectContaining({ type: 'syncMarkers', markers: [] })]),
      );
      engine.destroy();
    });

    it('replaces and clears automation lanes via setAutomationLane', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
        mode: 'postMessage',
        nodeFactory: () =>
          readyWorkletNode({
            postMessage: (message: unknown) => posted.push(message),
            addEventListener: () => undefined,
            removeEventListener: () => undefined,
            start: () => undefined,
          }),
      });

      // Reserved mixer namespace encodings: master = lane 0xff, first track
      // lane = index 0, first bus = index 0 (lane byte 0xfe); kind 1 = faderDb,
      // kind 2 = pan.
      const masterFader = engine.automationParamId('master', 'faderDb');
      expect(masterFader).toBe(0x4d58ff01);
      expect(engine.automationParamId('master', 'pan')).toBe(0x4d58ff02);
      expect(engine.automationParamId(10, 'faderDb')).toBe(0x4d580001);
      expect(engine.automationParamId(10, 'pan')).toBe(0x4d580002);
      expect(engine.busAutomationParamId(1)).toBe(0x4d58fe01);

      // Replace-all installs the sorted lane on the offline engine and mirrors
      // it to the live worklet via syncAutomation.
      engine.setAutomationLane(masterFader, [
        { ppq: 4, value: -12 },
        { ppq: 0, value: 0 },
      ]);
      expect(engine.automationLaneCount()).toBe(1);
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
      expect(engine.automationLaneCount()).toBe(1);

      // Clearing posts an empty replace-all sync to the worklet.
      engine.setAutomationLane(masterFader, []);
      expect(posted).toEqual(
        expect.arrayContaining([
          expect.objectContaining({ type: 'syncAutomation', paramId: masterFader, points: [] }),
        ]),
      );
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
      expect(engine.transport.play()).toBe(true);
      engine.destroy();
      expect(disconnected).toEqual([true]);
      expect(posted.at(-1)).toMatchObject({ type: 'destroy' });
      expect(engine.transport.play()).toBe(false);
      await engine.suspend();
      await engine.resume();
      expect(lifecycle).toEqual(['suspend', 'resume']);
    });
  });
});
