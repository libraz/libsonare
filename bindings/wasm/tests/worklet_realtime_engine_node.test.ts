import {
  describe,
  expect,
  it,
  popSonareEngineCommandRingBuffer,
  SonareEngine,
  SonareEngineCommandType,
  SonareEngineTelemetryError,
  SonareEngineTelemetryType,
  SonareRealtimeEngineNode,
  setupWorklet,
  writeSonareEngineTelemetryRingBuffer,
} from './_worklet_helpers';

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
        defaultCurve: 2, // canonical AutomationCurve::Hold (preserve original semantic)
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
          '{"version":1,"strips":[],"buses":[{"id":"100","inserts":[]}],"connections":[]}';
        engine.setBusStripJson(100, busStripJson);
        engine.setBuiltinInstrument(3, { gain: 0.5 });
        engine.setSynthInstrument(3, 'saw-lead');
        engine.setSf2Instrument(3, { gain: 0.5 });
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
            expect.objectContaining({ type: SonareEngineCommandType.SetTempoMap }),
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
        // scheduleParam/addAutomationPoint mirror the lane to the live engine via
        // an out-of-band 'syncAutomation' message (previously offline-only).
        expect(posted).toEqual(
          expect.arrayContaining([expect.objectContaining({ type: 'syncAutomation', paramId: 7 })]),
        );
      } finally {
        engine.destroy();
      }
    });

    it('declares mixer lanes in explicit order via setTrackLanes', async () => {
      const posted: unknown[] = [];
      const engine = await SonareEngine.create(fakeContext(), {
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
        nodeFactory: () =>
          ({
            port,
            disconnect: () => undefined,
          }) as unknown as AudioWorkletNode,
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
        nodeFactory: () =>
          ({
            port,
            disconnect: () => undefined,
          }) as unknown as AudioWorkletNode,
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
          ({
            port: {
              postMessage: (message: unknown) => posted.push(message),
              addEventListener: () => undefined,
              removeEventListener: () => undefined,
              start: () => undefined,
            },
            disconnect: () => undefined,
          }) as unknown as AudioWorkletNode,
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
          ({
            port: {
              postMessage: (message: unknown) => posted.push(message),
              addEventListener: () => undefined,
              removeEventListener: () => undefined,
              start: () => undefined,
            },
            disconnect: () => undefined,
          }) as unknown as AudioWorkletNode,
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
});
