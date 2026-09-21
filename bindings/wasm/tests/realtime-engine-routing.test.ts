/**
 * How the realtime engine routes what it is given: audio clips onto lanes and
 * ports, the cue bus, monitoring, and MIDI clips onto instruments or the output
 * queue.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, isSonareError, RealtimeEngine } from '../dist/index.js';

describe('Sonare WASM Module', () => {
  const rms = (data: Float32Array): number => {
    let sum = 0;
    for (const value of data) {
      sum += value * value;
    }
    return Math.sqrt(sum / data.length);
  };

  const midi1Word = (status: number, channel: number, data0: number, data1: number): number =>
    (0x2 << 28) | ((status & 0xf) << 20) | ((channel & 0xf) << 16) | (data0 << 8) | data1;

  beforeAll(async () => {
    await init();
  });

  describe('clip, lane and MIDI routing', () => {
    it('processes realtime engine clips, capture, and telemetry', () => {
      const engine = new RealtimeEngine(48000, 128);
      engine.setTempo(60);
      engine.setTimeSignature(3, 4);
      engine.setMarkers([
        { id: 11, ppq: 1, name: 'intro' },
        { id: 12, ppq: 2, name: 'out' },
      ]);
      expect(engine.markerCount()).toBe(2);
      expect(engine.markerByIndex(0).name).toBe('intro');
      expect(engine.marker(12).ppq).toBe(2);
      engine.setLoopFromMarkers(11, 12);
      engine.setMetronome({ enabled: true, beatGain: 0.25, accentGain: 0.75, clickSamples: 16 });
      expect(engine.metronome().enabled).toBe(true);
      engine.setMetronome({ enabled: true });
      expect(engine.metronome().clickSamples).toBe(0);
      expect(engine.countInEndSample(0, 2)).toBe(288000);
      expect(engine.sampleAtPpq(1.5)).toBe(72000);
      engine.setTempoSegments([
        { startPpq: 0, bpm: 120 },
        { startPpq: 4, bpm: 60 },
      ]);
      expect(engine.sampleAtPpq(4)).toBe(96000);
      expect(engine.sampleAtPpq(5)).toBe(144000);
      engine.setTimeSignatureSegments([
        { startPpq: 0, numerator: 4, denominator: 4 },
        { startPpq: 4, numerator: 3, denominator: 4 },
      ]);
      expect(engine.countInEndSample(engine.sampleAtPpq(4), 1)).toBe(engine.sampleAtPpq(7));
      engine.setTempo(60);
      engine.setTimeSignature(3, 4);
      let badPpqError: unknown;
      try {
        engine.sampleAtPpq(Number.NaN);
      } catch (error) {
        badPpqError = error;
      }
      expect(isSonareError(badPpqError)).toBe(true);
      if (!isSonareError(badPpqError)) {
        throw new Error('expected SonareError');
      }
      expect(badPpqError.code).toBe(ErrorCode.InvalidParameter);
      expect(() => engine.sampleAtPpq(1e300)).toThrow();
      expect(() => engine.seekPpq(1e300)).toThrow();
      expect(() => engine.setLoop(0, 1e300, true)).toThrow();
      engine.setMetronome({ enabled: false });
      engine.addParameter({
        id: 7,
        name: 'gain',
        unit: 'dB',
        minValue: -60,
        maxValue: 12,
        defaultValue: 0,
        rtSafe: true,
        defaultCurve: 0, // canonical AutomationCurve::Linear
      });
      expect(engine.parameterCount()).toBe(1);
      expect(engine.parameterInfo(7).name).toBe('gain');
      expect(engine.parameterInfoByIndex(0).unit).toBe('dB');
      engine.setAutomationLane(7, [
        { ppq: 0, value: 0 },
        { ppq: 1, value: 6.0205999, curveToNext: 0 }, // Linear
      ]);
      expect(engine.automationLaneCount()).toBe(1);
      engine.setGraph({
        nodes: [
          { id: 'in', numPorts: 2 },
          { id: 'gain', type: 1, gainDb: 0, numPorts: 2 },
          { id: 'out', numPorts: 2 },
        ],
        connections: [
          { sourceNode: 'in', sourcePort: 0, destNode: 'gain', destPort: 0 },
          { sourceNode: 'in', sourcePort: 1, destNode: 'gain', destPort: 1 },
          { sourceNode: 'gain', sourcePort: 0, destNode: 'out', destPort: 0 },
          { sourceNode: 'gain', sourcePort: 1, destNode: 'out', destPort: 1 },
        ],
        inputNode: 'in',
        outputNode: 'out',
        numChannels: 2,
        parameterBindings: [{ paramId: 7, nodeId: 'gain' }],
      });
      expect(engine.graphNodeCount()).toBe(3);
      expect(engine.graphConnectionCount()).toBe(4);
      engine.setClips([
        {
          id: 101,
          channels: [new Float32Array(128).fill(0.125), new Float32Array(128).fill(-0.125)],
          startPpq: 1,
          lengthSamples: 128,
        },
      ]);
      expect(engine.clipCount()).toBe(1);
      engine.setCaptureBuffer(2, 128);
      engine.setCapturePunch(48000, 48128);
      expect(() => engine.setCapturePunch(-1, 128)).toThrow();
      expect(() => engine.setCapturePunch(128, 127)).toThrow();
      engine.seekMarker(11);
      engine.play();

      const processed = engine.process([
        new Float32Array(128).fill(0.25),
        new Float32Array(128).fill(-0.25),
      ]);
      expect(processed[0][0]).toBeCloseTo(0.75, 4);
      expect(processed[1][0]).toBeCloseTo(-0.75, 4);

      const warpEngine = new RealtimeEngine(48000, 4);
      warpEngine.setClips([
        {
          id: 303,
          channels: [new Float32Array([0, 10, 20, 30])],
          startPpq: 0,
          lengthSamples: 4,
          warpMode: 'repitch',
          warpAnchors: [
            { warpSample: 0, sourceSample: 0 },
            { warpSample: 3, sourceSample: 1.5 },
          ],
        },
      ]);
      warpEngine.play();
      const warped = warpEngine.process([new Float32Array(4)]);
      expect(warped[0][0]).toBeCloseTo(0, 4);
      expect(warped[0][1]).toBeCloseTo(5, 4);
      expect(warped[0][2]).toBeCloseTo(10, 4);
      expect(warped[0][3]).toBeCloseTo(15, 4);
      warpEngine.destroy();

      const sr = 48000;
      const stretchSource = 12000;
      const stretchOutput = 24000;
      const tone = new Float32Array(stretchSource);
      for (let i = 0; i < stretchSource; i++) {
        tone[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / sr);
      }
      const stretchEngine = new RealtimeEngine(sr, stretchOutput);
      stretchEngine.setClips([
        {
          id: 305,
          channels: [tone],
          startPpq: 0,
          lengthSamples: stretchOutput,
          warpMode: 'time-stretch',
          warpAnchors: [
            { warpSample: 0, sourceSample: 0 },
            { warpSample: stretchOutput, sourceSample: stretchSource },
          ],
        },
      ]);
      stretchEngine.play();
      const stretched = stretchEngine.process([new Float32Array(stretchOutput)])[0];
      // Goertzel: a resampling warp at half rate would move the tone to 220 Hz.
      const stretchPower = (hz: number): number => {
        const w = (2 * Math.PI * hz) / sr;
        const coeff = 2 * Math.cos(w);
        let s1 = 0;
        let s2 = 0;
        for (let i = 4096; i < stretchOutput - 4096; i++) {
          const s0 = stretched[i] + coeff * s1 - s2;
          s2 = s1;
          s1 = s0;
        }
        const real = s1 - s2 * Math.cos(w);
        const imag = s2 * Math.sin(w);
        return real * real + imag * imag;
      };
      expect(stretchPower(440)).toBeGreaterThan(100 * stretchPower(220));
      stretchEngine.destroy();

      const fadeEngine = new RealtimeEngine(48000, 4);
      fadeEngine.setClips([
        {
          id: 302,
          channels: [new Float32Array([1, 1, 1, 1])],
          startPpq: 0,
          lengthSamples: 4,
          fadeInSamples: 4,
          fadeOutSamples: 2,
        },
      ]);
      fadeEngine.play();
      const faded = fadeEngine.process([new Float32Array(4)])[0];
      expect(faded[0]).toBeCloseTo(0, 4);
      expect(faded[1]).toBeGreaterThan(faded[0]);
      expect(faded[2]).toBeGreaterThan(faded[3]);
      fadeEngine.destroy();

      const loopLengthEngine = new RealtimeEngine(48000, 6);
      loopLengthEngine.setClips([
        {
          id: 3021,
          channels: [new Float32Array([1, 2, 3, 4])],
          startPpq: 0,
          lengthSamples: 6,
          loop: true,
          // @ts-expect-error loopLengthSamples belongs to setMidiClips; audio clips ignore it
          loopLengthSamples: 2,
        },
      ]);
      loopLengthEngine.play();
      const looped = loopLengthEngine.process([new Float32Array(6)])[0];
      expect(Array.from(looped)).toEqual([1, 2, 3, 4, 1, 2]);
      loopLengthEngine.destroy();

      const badWarpModeEngine = new RealtimeEngine(48000, 4);
      expect(() =>
        badWarpModeEngine.setClips([
          {
            id: 3030,
            channels: [new Float32Array([0, 10, 20, 30])],
            startPpq: 0,
            lengthSamples: 4,
            warpMode: 'typo' as 'repitch',
          },
        ]),
      ).toThrow();
      expect(() =>
        badWarpModeEngine.setClips([
          {
            id: 30301,
            channels: [new Float32Array([0, 10, 20, 30])],
            startPpq: 0,
            lengthSamples: 4,
            warpMode: 99 as 1,
          },
        ]),
      ).toThrow();
      badWarpModeEngine.destroy();

      const badLoopWarpEngine = new RealtimeEngine(48000, 4);
      expect(() =>
        badLoopWarpEngine.setClips([
          {
            id: 3031,
            channels: [new Float32Array([0, 10, 20, 30])],
            startPpq: 0,
            lengthSamples: 8,
            loop: true,
            warpMode: 'repitch',
            warpAnchors: [
              { warpSample: 0, sourceSample: 0 },
              { warpSample: 3, sourceSample: 1.5 },
            ],
          },
        ]),
      ).toThrow();
      badLoopWarpEngine.destroy();

      const failedSetClipsEngine = new RealtimeEngine(48000, 4);
      failedSetClipsEngine.setClips([
        {
          id: 3032,
          channels: [new Float32Array([0.25, 0.5, 0.75, 1.0])],
          startPpq: 0,
          lengthSamples: 4,
        },
      ]);
      failedSetClipsEngine.play();
      expect(() =>
        failedSetClipsEngine.setClips([
          {
            id: 3033,
            channels: [new Float32Array([0, 10, 20, 30])],
            startPpq: 0,
            lengthSamples: 8,
            loop: true,
            warpMode: 'repitch',
            warpAnchors: [
              { warpSample: 0, sourceSample: 0 },
              { warpSample: 3, sourceSample: 1.5 },
            ],
          },
        ]),
      ).toThrow();
      expect(failedSetClipsEngine.clipCount()).toBe(1);
      failedSetClipsEngine.seekSample(0);
      const afterFailedSet = failedSetClipsEngine.process([new Float32Array(4)]);
      expect(Array.from(afterFailedSet[0])).toEqual([0.25, 0.5, 0.75, 1.0]);
      failedSetClipsEngine.destroy();

      const badOffsetEngine = new RealtimeEngine(48000, 4);
      expect(() =>
        badOffsetEngine.setClips([
          {
            id: 3034,
            channels: [new Float32Array([0, 10, 20, 30])],
            startPpq: 0,
            clipOffsetSamples: 4,
          },
        ]),
      ).toThrow();
      badOffsetEngine.destroy();

      const tempoEngine = new RealtimeEngine(48000, 8192);
      const tempoSource = new Float32Array(4096);
      for (let i = 0; i < tempoSource.length; i++) {
        tempoSource[i] = Math.sin(i * 0.02);
      }
      tempoEngine.setClips([
        {
          id: 304,
          channels: [tempoSource],
          startPpq: 0,
          lengthSamples: 8192,
          warpMode: 'tempo-sync',
          warpAnchors: [
            { warpSample: 0, sourceSample: 0 },
            { warpSample: 2048, sourceSample: 1024 },
            { warpSample: 8192, sourceSample: 4096 },
          ],
        },
      ]);
      tempoEngine.play();
      const tempoSynced = tempoEngine.process([new Float32Array(8192)]);
      expect(Array.from(tempoSynced[0]).some((v) => Math.abs(v) > 0.1)).toBe(true);
      tempoEngine.destroy();

      const shortTempoEngine = new RealtimeEngine(48000, 8192);
      shortTempoEngine.setClips([
        {
          id: 3041,
          channels: [tempoSource],
          startPpq: 0,
          lengthSamples: 4096,
          warpMode: 'tempo-sync',
          warpAnchors: [
            { warpSample: 0, sourceSample: 0 },
            { warpSample: 2048, sourceSample: 1024 },
            { warpSample: 8192, sourceSample: 4096 },
          ],
        },
      ]);
      shortTempoEngine.play();
      const shortTempoSynced = shortTempoEngine.process([new Float32Array(8192)])[0];
      expect(Array.from(shortTempoSynced.slice(0, 4096)).some((v) => Math.abs(v) > 0.1)).toBe(true);
      expect(Array.from(shortTempoSynced.slice(4096)).every((v) => Math.abs(v) < 0.0001)).toBe(
        true,
      );
      shortTempoEngine.destroy();

      const pagedEngine = new RealtimeEngine(48000, 8);
      expect(() => pagedEngine.createClipPageProvider(1, 1_000_000_000_000, 1)).toThrow();
      expect(() => pagedEngine.createClipPageProvider(65, 8, 4)).toThrow();
      const provider = pagedEngine.createClipPageProvider(1, 8, 4);
      expect(() => provider.supply(0, [new Float32Array([1, 2])])).toThrow();
      expect(() =>
        pagedEngine.setClips([
          {
            id: 3050,
            pageProvider: provider,
            startPpq: 0,
            clipOffsetSamples: 8,
          },
        ]),
      ).toThrow();
      expect(() =>
        pagedEngine.setClips([
          {
            id: 3051,
            pageProvider: provider,
            startPpq: 0,
            warpMode: 'tempo-sync',
          },
        ]),
      ).toThrow();
      provider.supply(0, [new Float32Array([1, 2, 3, 4])]);
      pagedEngine.setClips([
        {
          id: 305,
          pageProvider: provider,
          startPpq: 0,
        },
      ]);
      pagedEngine.play();
      const firstPaged = pagedEngine.process([new Float32Array(8)]);
      expect(Array.from(firstPaged[0])).toEqual([1, 2, 3, 4, 0, 0, 0, 0]);
      expect(pagedEngine.popClipPageRequest()).toEqual({ clipId: 305, channel: 0, sample: 4 });
      expect(
        pagedEngine.drainTelemetry().some((record) => record.type === 1 && record.value === 305),
      ).toBe(true);
      provider.supply(1, [new Float32Array([5, 6, 7, 8])]);
      pagedEngine.seekSample(0);
      const secondPaged = pagedEngine.process([new Float32Array(8)]);
      expect(Array.from(secondPaged[0])).toEqual([1, 2, 3, 4, 5, 6, 7, 8]);
      expect(() => pagedEngine.clearClipPage(999, 0)).toThrow();
      provider.destroy();
      expect(() => pagedEngine.clearClipPage(provider.id, 0)).toThrow();
      const recycledProvider = pagedEngine.createClipPageProvider(1, 8, 4);
      expect(recycledProvider.id).toBe(provider.id);
      recycledProvider.destroy();
      pagedEngine.destroy();

      engine.armCapture();
      engine.seekMarker(11);
      const capturedBlock = engine.process([
        new Float32Array(128).fill(0.25),
        new Float32Array(128).fill(-0.25),
      ]);
      expect(capturedBlock[0][0]).toBeCloseTo(0.75, 4);
      const captureStatus = engine.captureStatus();
      expect(captureStatus.capturedFrames).toBe(128);
      expect(captureStatus.overflowCount).toBe(0);
      expect(captureStatus.source).toBe('output');
      expect(captureStatus.recordOffsetSamples).toBe(0);
      expect(engine.capturedAudio()[0][0]).toBeCloseTo(0.75, 4);
      engine.resetCapture();
      expect(engine.captureStatus().capturedFrames).toBe(0);

      const telemetry = engine.drainTelemetry();
      expect(telemetry.length).toBeGreaterThan(0);
      expect(telemetry.at(-1)?.timelineSample).toBe(48000 + 128);

      engine.setCaptureSource('input');
      engine.setRecordOffsetSamples(-37);
      engine.armCapture();
      engine.seekMarker(11);
      engine.process([new Float32Array(128).fill(0.25), new Float32Array(128).fill(-0.25)]);
      const inputCaptureStatus = engine.captureStatus();
      expect(inputCaptureStatus.source).toBe('input');
      expect(inputCaptureStatus.recordOffsetSamples).toBe(-37);
      expect(engine.capturedAudio()[0][0]).toBeCloseTo(0.25, 4);
      expect(engine.drainMeterTelemetry().some((record) => record.targetId === 0xffff)).toBe(true);

      engine.setCaptureSource(0);
      expect(engine.captureStatus().source).toBe('output');
      engine.setCaptureSource(1);
      expect(engine.captureStatus().source).toBe('input');
      expect(() => engine.setCaptureSource(2)).toThrow(/capture source/);

      engine.setInputMonitor(false);
      engine.resetCapture();
      engine.armCapture();
      engine.seekMarker(11);
      let monitored = engine.process([
        new Float32Array(128).fill(0.25),
        new Float32Array(128).fill(-0.25),
      ]);
      expect(monitored[0][0]).toBeCloseTo(0.25, 4);
      expect(monitored[1][0]).toBeCloseTo(-0.25, 4);
      expect(engine.capturedAudio()[0][0]).toBeCloseTo(0.25, 4);

      engine.setInputMonitor(true, 0.5);
      engine.seekMarker(11);
      monitored = engine.process([
        new Float32Array(128).fill(0.25),
        new Float32Array(128).fill(-0.25),
      ]);
      expect(monitored[0][0]).toBeCloseTo(0.5, 4);
      expect(monitored[1][0]).toBeCloseTo(-0.5, 4);
      expect(() => engine.setInputMonitor(true, Number.NaN)).toThrow();
      expect(() => engine.setInputMonitor(true, Number.POSITIVE_INFINITY)).toThrow();

      const meters = engine.drainMeterTelemetry();
      expect(meters.length).toBeGreaterThan(0);
      expect(meters.at(-1)).toMatchObject({ targetId: 0 });
      expect(meters.at(-1)?.peakDbL).toBeGreaterThan(-20);
      const bounced = engine.bounceOffline({
        totalFrames: 256,
        blockSize: 128,
        numChannels: 2,
        sourceSampleRate: 48000,
        targetSampleRate: 24000,
      });
      expect(bounced.frames).toBe(128);
      expect(bounced.numChannels).toBe(2);
      expect(bounced.sampleRate).toBe(24000);
      expect(bounced.interleaved.length).toBe(256);
      expect(Number.isFinite(bounced.integratedLufs) || !Number.isNaN(bounced.integratedLufs)).toBe(
        true,
      );
      for (const targetLufs of [0, Number.NaN]) {
        const normalized = engine.bounceOffline({
          totalFrames: 256,
          blockSize: 128,
          numChannels: 2,
          sourceSampleRate: 48000,
          targetSampleRate: 48000,
          normalizeLufs: true,
          targetLufs,
        });
        expect(Array.from(normalized.interleaved).every(Number.isFinite)).toBe(true);
      }
      engine.setClips([
        {
          id: 202,
          channels: [new Float32Array(128).fill(0.125), new Float32Array(128).fill(-0.25)],
          startPpq: 0,
          lengthSamples: 128,
        },
      ]);
      engine.seekSample(0);
      const frozen = engine.freezeOffline({
        totalFrames: 128,
        blockSize: 128,
        numChannels: 2,
        clipId: 77,
      });
      expect(frozen.clipId).toBe(77);
      expect(frozen.frames).toBe(128);
      expect(frozen.numChannels).toBe(2);
      engine.seekSample(0);
      const frozenRendered = engine.renderOffline([new Float32Array(128), new Float32Array(128)]);
      expect(frozenRendered[0][0]).toBeCloseTo(0.125, 4);
      expect(frozenRendered[1][0]).toBeCloseTo(-0.25, 4);
      engine.setClips([
        {
          id: 303,
          channels: [new Float32Array(128).fill(0.5), new Float32Array(128).fill(-0.5)],
          startPpq: 0,
          lengthSamples: 128,
        },
      ]);
      engine.seekSample(0);
      engine.freezeOffline({
        totalFrames: 128,
        blockSize: 128,
        numChannels: 2,
        clipId: 78,
        gain: 0,
      });
      engine.seekSample(0);
      const zeroGainFrozen = engine.renderOffline([new Float32Array(128), new Float32Array(128)]);
      expect(zeroGainFrozen[0][0]).toBeCloseTo(0, 4);
      expect(zeroGainFrozen[1][0]).toBeCloseTo(0, 4);
      // Unsupported bounce channel counts (3/4/5/7 have no speaker layout) must
      // be rejected, matching the C-ABI oracle round-trip, instead of silently
      // writing garbage planes.
      expect(() =>
        engine.bounceOffline({
          totalFrames: 256,
          blockSize: 128,
          numChannels: 3,
          sourceSampleRate: 48000,
          targetSampleRate: 48000,
        }),
      ).toThrow();
      // freezeOffline must reject a non-finite/negative gain or startPpq (gain:0
      // stays valid -- that is the tested zero-gain freeze above).
      for (const bad of [
        { gain: Number.NaN },
        { gain: -1 },
        { startPpq: Number.NaN },
        { startPpq: -1 },
      ]) {
        expect(() =>
          engine.freezeOffline({
            totalFrames: 128,
            blockSize: 128,
            numChannels: 2,
            clipId: 79,
            ...bad,
          }),
        ).toThrow();
      }
      // wasm32 narrows allocation sizes to 32-bit size_t. Reject before
      // allocation instead of wrapping the buffer and rendering the original
      // 64-bit frame count past its end.
      for (const totalFrames of [2 ** 32 + 1, Number.MAX_SAFE_INTEGER]) {
        expect(() =>
          engine.bounceOffline({
            totalFrames,
            blockSize: 128,
            numChannels: 2,
            sourceSampleRate: 48000,
            targetSampleRate: 48000,
          }),
        ).toThrow();
        expect(() =>
          engine.freezeOffline({
            totalFrames,
            blockSize: 128,
            numChannels: 2,
            clipId: 80,
          }),
        ).toThrow();
      }
      engine.destroy();
    });

    it('treats an explicit numPorts of 0 as a fallback to numChannels like the C ABI', () => {
      const engine = new RealtimeEngine(48000, 128);
      // The C ABI resolves a non-positive num_ports to num_channels; a plain-JS
      // caller who writes numPorts:0 must get the same fallback, not a throw.
      expect(() =>
        engine.setGraph({
          nodes: [
            { id: 'in', numPorts: 0 },
            { id: 'gain', type: 1, gainDb: 0, numPorts: 0 },
            { id: 'out', numPorts: 0 },
          ],
          connections: [
            { sourceNode: 'in', sourcePort: 0, destNode: 'gain', destPort: 0 },
            { sourceNode: 'gain', sourcePort: 0, destNode: 'out', destPort: 0 },
          ],
          inputNode: 'in',
          outputNode: 'out',
          numChannels: 2,
        }),
      ).not.toThrow();
      expect(engine.graphNodeCount()).toBe(3);
      engine.destroy();
    });

    it('routes track clips through lanes and lane commands', () => {
      const engine = new RealtimeEngine(48000, 256);
      const frames = 256 * 10;
      engine.setClips([
        {
          id: 1,
          trackId: 10,
          channels: [new Float32Array(frames).fill(1), new Float32Array(frames).fill(1)],
          startPpq: 0,
          lengthSamples: frames,
        },
        {
          id: 2,
          trackId: 20,
          channels: [new Float32Array(frames).fill(1), new Float32Array(frames).fill(1)],
          startPpq: 0,
          lengthSamples: frames,
        },
      ]);
      engine.setTrackLanes([10, { trackId: 20 }]);
      let duplicateLaneError: unknown;
      try {
        engine.setTrackLanes([{ trackId: 10 }, { trackId: 10 }]);
      } catch (error) {
        duplicateLaneError = error;
      }
      expect(isSonareError(duplicateLaneError)).toBe(true);
      if (!isSonareError(duplicateLaneError)) {
        throw new Error('expected SonareError');
      }
      expect(duplicateLaneError.code).toBe(ErrorCode.InvalidParameter);
      engine.setTrackLanes([10, { trackId: 20 }]);

      engine.play();
      let processed = engine.process([new Float32Array(256), new Float32Array(256)]);
      expect(processed[0].at(-1)).toBeCloseTo(2, 4);
      expect(processed[1].at(-1)).toBeCloseTo(2, 4);

      engine.setTrackMonitorMode(0, 'pfl');
      const withPfl = engine.processWithMonitor([new Float32Array(256), new Float32Array(256)]);
      expect(withPfl.output[0].at(-1)).toBeCloseTo(2, 4);
      expect(withPfl.monitor[0].at(-1)).toBeCloseTo(1, 4);
      expect(withPfl.monitor[1].at(-1)).toBeCloseTo(1, 4);

      engine.setTrackMonitorMode(0, 'off');
      engine.setSoloMute(0, true, false);
      for (let block = 0; block < 4; block += 1) {
        processed = engine.process([new Float32Array(256), new Float32Array(256)]);
      }
      expect(processed[0].at(-1)).toBeGreaterThan(0.75);
      expect(processed[0].at(-1)).toBeLessThan(1.25);

      engine.setParameterSmoothed(0x4d580001, -12, -1);
      for (let block = 0; block < 6; block += 1) {
        processed = engine.process([new Float32Array(256), new Float32Array(256)]);
      }
      expect(processed[0].at(-1)).toBeLessThan(0.45);
      expect(processed[1].at(-1)).toBeLessThan(0.45);
      engine.destroy();
    });

    it('keeps the cue bus out of the program output on the prepared path', () => {
      const engine = new RealtimeEngine(48000, 256);
      const frames = 256 * 10;
      const blockSize = 256;
      engine.setClips([
        {
          id: 1,
          trackId: 10,
          channels: [new Float32Array(frames).fill(1), new Float32Array(frames).fill(1)],
          startPpq: 0,
          lengthSamples: frames,
        },
        {
          id: 2,
          trackId: 20,
          channels: [new Float32Array(frames).fill(1), new Float32Array(frames).fill(1)],
          startPpq: 0,
          lengthSamples: frames,
        },
      ]);
      engine.setTrackLanes([10, { trackId: 20 }]);
      engine.prepareChannels(2, blockSize);
      engine.prepareMonitorChannels(2, blockSize);
      engine.setTrackMonitorMode(0, 'pfl');
      engine.play();

      for (let ch = 0; ch < 2; ch += 1) {
        engine.getChannelBuffer(ch, blockSize).fill(0);
        engine.getMonitorChannelBuffer(ch, blockSize).fill(0);
      }
      engine.processPreparedWithMonitor(blockSize);

      // Same split processWithMonitor produces: both lanes in the program
      // output, only the PFL-tapped lane in the cue.
      expect(engine.getChannelBuffer(0, blockSize).at(-1)).toBeCloseTo(2, 4);
      expect(engine.getChannelBuffer(1, blockSize).at(-1)).toBeCloseTo(2, 4);
      expect(engine.getMonitorChannelBuffer(0, blockSize).at(-1)).toBeCloseTo(1, 4);
      expect(engine.getMonitorChannelBuffer(1, blockSize).at(-1)).toBeCloseTo(1, 4);

      engine.destroy();
    });

    it('rejects a prepared monitor call that was never prepared or is too narrow', () => {
      const engine = new RealtimeEngine(48000, 256);
      engine.prepareChannels(2, 256);
      expect(() => engine.processPreparedWithMonitor(256)).toThrow();
      expect(() => engine.getMonitorChannelBuffer(0, 256)).toThrow();
      engine.prepareMonitorChannels(1, 256);
      // One cue plane cannot hold the two the engine writes.
      expect(() => engine.processPreparedWithMonitor(256)).toThrow();
      engine.prepareMonitorChannels(2, 256);
      expect(() => engine.processPreparedWithMonitor(512)).toThrow();
      expect(() => engine.processPreparedWithMonitor(256)).not.toThrow();
      engine.destroy();
    });

    it('routes asymmetric left-only sources through dual-pan and reverses them', () => {
      const engine = new RealtimeEngine(48000, 256);
      const frames = 256 * 12;
      engine.setClips([
        {
          id: 401,
          trackId: 10,
          channels: [new Float32Array(frames).fill(1), new Float32Array(frames)],
          startPpq: 0,
          lengthSamples: frames,
        },
      ]);
      engine.setTrackLanes([10]);
      engine.setTrackStripJson(
        10,
        '{"version":1,"strips":[{"id":"track-10"}],"buses":[],"connections":[]}',
      );
      engine.setTrackStripPanMode(10, 'dualPan');
      engine.setTrackStripDualPan(10, 1, -1);
      engine.settleParameters();
      engine.play();

      let routedRight: Float32Array[] = [new Float32Array(256), new Float32Array(256)];
      for (let block = 0; block < 4; block += 1) {
        routedRight = engine.process([new Float32Array(256), new Float32Array(256)]);
      }
      const rightEnergy = routedRight[1].reduce((sum, sample) => sum + sample * sample, 0);
      const leftEnergy = routedRight[0].reduce((sum, sample) => sum + sample * sample, 0);
      expect(rightEnergy).toBeGreaterThan(1);
      expect(leftEnergy).toBeLessThan(rightEnergy * 1e-6);

      engine.setTrackStripDualPan(10, -1, 1);
      engine.seekSample(0);
      engine.settleParameters();
      let routedLeft: Float32Array[] = [new Float32Array(256), new Float32Array(256)];
      for (let block = 0; block < 4; block += 1) {
        routedLeft = engine.process([new Float32Array(256), new Float32Array(256)]);
      }
      const reversedLeftEnergy = routedLeft[0].reduce((sum, sample) => sum + sample * sample, 0);
      const reversedRightEnergy = routedLeft[1].reduce((sum, sample) => sum + sample * sample, 0);
      expect(reversedLeftEnergy).toBeGreaterThan(1);
      expect(reversedRightEnergy).toBeLessThan(reversedLeftEnergy * 1e-6);
      engine.destroy();
    });

    it('renders scheduled MIDI clips through built-in instruments', () => {
      const engine = new RealtimeEngine(48000, 128);
      engine.setBuiltinInstrument({ gain: 0.5 }, 5);
      engine.setMidiClips([
        {
          id: 1,
          trackId: 5,
          destinationId: 5,
          lengthSamples: 8192,
          events: [
            { renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 },
            { renderFrame: 4096, word0: midi1Word(0x8, 0, 60, 0), wordCount: 1 },
          ],
        },
      ]);
      engine.play();
      const out = engine.process([new Float32Array(128), new Float32Array(128)]);
      expect(Math.max(rms(out[0]), rms(out[1]))).toBeGreaterThan(0);

      let badGroupError: unknown;
      try {
        engine.setMidiClips([
          {
            id: 2,
            trackId: 5,
            destinationId: 5,
            events: [
              { renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1, group: 16 },
            ],
          },
        ]);
      } catch (error) {
        badGroupError = error;
      }
      expect(isSonareError(badGroupError)).toBe(true);
      if (!isSonareError(badGroupError)) {
        throw new Error('expected SonareError');
      }
      expect(badGroupError.code).toBe(ErrorCode.InvalidParameter);

      let badChannelError: unknown;
      try {
        engine.pushMidiNoteOn(5, 0, 16, 60, 100);
      } catch (error) {
        badChannelError = error;
      }
      expect(isSonareError(badChannelError)).toBe(true);
      if (!isSonareError(badChannelError)) {
        throw new Error('expected SonareError');
      }
      expect(badChannelError.code).toBe(ErrorCode.InvalidParameter);

      let badSoundFontError: unknown;
      try {
        engine.loadSoundFont(new Uint8Array([0x6e, 0x6f, 0x74, 0x20, 0x73, 0x66, 0x32]));
      } catch (error) {
        badSoundFontError = error;
      }
      expect(isSonareError(badSoundFontError)).toBe(true);
      if (!isSonareError(badSoundFontError)) {
        throw new Error('expected SonareError');
      }
      expect(badSoundFontError.code).toBe(ErrorCode.InvalidFormat);

      engine.setMidiClips([]);
      engine.destroy();
    });

    it('routes a destination marked external to the MIDI output queue, bypassing its instrument', () => {
      const engine = new RealtimeEngine(48000, 128);
      engine.setTempo(120);
      // Bind an instrument to destination 5, then mark it external: the
      // instrument must be bypassed (silence) and its MIDI must queue instead,
      // already lowered to MIDI 1.0 bytes.
      engine.setBuiltinInstrument({ gain: 0.5 }, 5);
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
      const out = engine.process([new Float32Array(128), new Float32Array(128)]);
      expect(Math.max(rms(out[0]), rms(out[1]))).toBe(0);

      const events = engine.drainExternalMidi(256);
      expect(events.length).toBe(2);
      expect(events[0].destinationId).toBe(5);
      expect(events[0].renderFrame).toBe(0);
      expect(events[0].bytes).toEqual([0x90, 60, 100]);
      expect(events[1].bytes).toEqual([0x80, 60, 0]);
      expect(engine.drainExternalMidi(256).length).toBe(0);
      engine.destroy();
    });

    it('forwards transport and clock bytes to the MIDI output queue when enabled', () => {
      const engine = new RealtimeEngine(48000, 2400);
      engine.setTempo(120);
      engine.setExternalMidiClockEnabled(true);
      engine.seekSample(0);
      engine.play();
      engine.process([new Float32Array(2400), new Float32Array(2400)]);
      const events = engine.drainExternalMidi(256);
      expect(events.length).toBeGreaterThan(0);
      // All transport/clock bytes are tagged with the transport sentinel.
      for (const event of events) {
        expect(event.destinationId).toBe(0xffffffff);
      }
      // Start (0xFA) first, then at least one clock tick (0xF8).
      expect(events[0].bytes).toEqual([0xfa]);
      expect(events.some((event) => event.bytes[0] === 0xf8)).toBe(true);
      engine.destroy();
    });
  });
});
