import { vi } from 'vitest';
import {
  createSonareClipPageRequestRingBuffer,
  createSonareEngineCommandRingBuffer,
  createSonareEngineTelemetryRingBuffer,
  createSonareScopeRingBuffer,
  describe,
  expect,
  it,
  pushSonareEngineCommandRingBuffer,
  readSonareClipPageRequestRingBuffer,
  readSonareEngineTelemetryRingBuffer,
  readSonareScopeRingBuffer,
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
    const midi1Word = (status: number, channel: number, data0: number, data1: number): number =>
      (0x2 << 28) | ((status & 0xf) << 20) | ((channel & 0xf) << 16) | (data0 << 8) | data1;

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

        expect(processor.process([[]], [[new Float32Array(blockSize)]])).toBe(true);
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
        processor.receiveCommand({
          type: SonareEngineCommandType.SetParam,
          targetId: 0x4d580001,
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

        const status = processor.engine.captureStatus();
        expect(status.capturedFrames).toBe(blockSize);
        expect(status.source).toBe('input');
        expect(status.recordOffsetSamples).toBe(-12);
        const captured = processor.engine.capturedAudio();
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

    it('applies tempo and time-signature segment sync to metronome accents', () => {
      const blockSize = 9600;
      const output = new Float32Array(blockSize);
      const processor = new SonareRealtimeEngineWorkletProcessor({
        sampleRate: 4800,
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
          config: { enabled: true, beatGain: 0.1, accentGain: 0.8, clickSamples: 8 },
        });
        processor.receiveCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime: -1 });
        expect(processor.process([[]], [[output]])).toBe(true);
        expect(output[2400]).toBeGreaterThan(0);
        expect(output[4800]).toBeGreaterThan(0);
        expect(output[0]).toBeGreaterThan(output[2400] * 2);
        expect(output[7200]).toBeGreaterThan(output[2400] * 2);
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
        const meter = meters[0] as { targetId: number; peakDbL: number; peakDbR: number };
        expect(meters[0]).toMatchObject({
          type: 'meter',
          targetId: 0,
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

    it('routes every producer sync message through the guarded port handler', async () => {
      // The producers post these over node.port; the worklet's onMessage handler
      // gates them with isEngineSyncMessage before reaching receiveSync. A message
      // type missing from the guard is silently dropped, so the live engine never
      // sees it even though the offline mirror does. Drive each one through the
      // real registered-processor onMessage path (not receiveSync directly) so a
      // guard omission fails here instead of shipping a dead feature.
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
        // Configure a bus with an insert so the bus-insert-param sync resolves a
        // real target instead of throwing; this also drives syncMixer (already in
        // the guard) through the same guarded path.
        port.onmessage?.({
          data: {
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
          },
        });
        receiveSyncSpy.mockClear();
        const syncMessages = [
          {
            type: 'syncBusStripInsertParamByName',
            busId: 200,
            insertIndex: 0,
            paramName: 'band0.gainDb',
            value: 3,
          },
          { type: 'syncMidiDestinationExternal', destinationId: 7, external: true },
          { type: 'syncExternalMidiClock', enabled: true },
        ];
        for (const message of syncMessages) {
          port.onmessage?.({ data: message });
        }
        // A string-typed message the guard does not know must stay dropped.
        port.onmessage?.({ data: { type: 'totallyUnknownSync' } });
        expect(receiveSyncSpy.mock.calls.map((call) => (call[0] as { type: string }).type)).toEqual(
          ['syncBusStripInsertParamByName', 'syncMidiDestinationExternal', 'syncExternalMidiClock'],
        );
      } finally {
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
  });
});
