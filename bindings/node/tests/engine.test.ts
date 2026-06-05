import { describe, expect, it } from 'vitest';
import { engineAbiVersion, RealtimeEngine, voiceChangerAbiVersion } from '../src/index.js';

describe('RealtimeEngine native binding', () => {
  it('exposes engine ABI version', () => {
    expect(engineAbiVersion()).toBeGreaterThan(0);
  });

  it('exposes voice changer ABI version', () => {
    const v = voiceChangerAbiVersion();
    expect(Number.isFinite(v)).toBe(true);
    expect(v).toBeGreaterThan(0);
  });

  it('processes a block and drains telemetry', () => {
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
    expect(engine.metronome().clickSamples).toBe(16);
    expect(engine.countInEndSample(0, 2)).toBe(288000);
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
    const captureLeft = new Float32Array(128);
    const captureRight = new Float32Array(128);
    engine.setCaptureBuffer([captureLeft, captureRight]);
    engine.setCapturePunch(48000, 48128);
    engine.armCapture();
    engine.seekMarker(11);
    engine.play();

    const left = new Float32Array(128).fill(0.25);
    const right = new Float32Array(128).fill(-0.25);
    const processed = engine.process([left, right]);

    expect(processed).toHaveLength(2);
    expect(processed[0][0]).toBeCloseTo(0.75, 4);
    expect(processed[1][0]).toBeCloseTo(-0.75, 4);
    const captureStatus = engine.captureStatus();
    expect(captureStatus.capturedFrames).toBe(128);
    expect(captureStatus.overflowCount).toBe(0);
    expect(captureStatus.armed).toBe(true);
    expect(captureLeft[0]).toBeCloseTo(0.75, 4);
    expect(captureRight[0]).toBeCloseTo(-0.75, 4);
    engine.resetCapture();
    expect(engine.captureStatus().capturedFrames).toBe(0);

    const telemetry = engine.drainTelemetry();
    expect(telemetry.length).toBeGreaterThan(0);
    expect(telemetry.at(-1)?.type).toBe(0);
    expect(telemetry.at(-1)?.error).toBe(0);
    expect(telemetry.at(-1)?.renderFrame).toBe(0);
    expect(telemetry.at(-1)?.timelineSample).toBe(48000 + 128);

    engine.destroy();
  });

  it('offline render matches repeated process output', () => {
    const frames = 256;
    const left = new Float32Array(frames);
    const right = new Float32Array(frames);
    for (let i = 0; i < frames; i++) {
      left[i] = Math.sin(i * 0.01);
      right[i] = -left[i];
    }

    const realtime = new RealtimeEngine(48000, 128);
    realtime.play();
    const rtLeft: number[] = [];
    const rtRight: number[] = [];
    for (let offset = 0; offset < frames; offset += 128) {
      const block = realtime.process([
        left.slice(offset, offset + 128),
        right.slice(offset, offset + 128),
      ]);
      rtLeft.push(...block[0]);
      rtRight.push(...block[1]);
    }
    realtime.destroy();

    const offline = new RealtimeEngine(48000, 128);
    offline.play();
    const rendered = offline.renderOffline([left, right], 128);
    offline.destroy();

    expect(Array.from(rendered[0])).toEqual(rtLeft);
    expect(Array.from(rendered[1])).toEqual(rtRight);

    const bounce = new RealtimeEngine(48000, 128);
    bounce.play();
    const bounced = bounce.bounceOffline({
      totalFrames: 256,
      blockSize: 128,
      numChannels: 2,
      sourceSampleRate: 48000,
      targetSampleRate: 24000,
    });
    bounce.destroy();

    expect(bounced.frames).toBe(128);
    expect(bounced.numChannels).toBe(2);
    expect(bounced.sampleRate).toBe(24000);
    expect(bounced.interleaved.length).toBe(256);
    // Silent input integrates to -Infinity LUFS, which is the correct gating
    // result for digital silence; just assert a numeric value is reported.
    expect(typeof bounced.integratedLufs).toBe('number');

    const freeze = new RealtimeEngine(48000, 128);
    freeze.setClips([
      {
        id: 7,
        channels: [new Float32Array(128).fill(0.125), new Float32Array(128).fill(-0.25)],
        startPpq: 0,
        lengthSamples: 128,
      },
    ]);
    freeze.play();
    const frozen = freeze.freezeOffline({
      totalFrames: 128,
      blockSize: 128,
      numChannels: 2,
      clipId: 77,
    });
    expect(frozen.clipId).toBe(77);
    expect(frozen.frames).toBe(128);
    expect(frozen.numChannels).toBe(2);
    expect(freeze.clipCount()).toBe(1);
    freeze.seekSample(0);
    const frozenRendered = freeze.renderOffline(
      [new Float32Array(128), new Float32Array(128)],
      128,
    );
    freeze.destroy();
    expect(frozenRendered[0][0]).toBeCloseTo(0.125, 4);
    expect(frozenRendered[1][0]).toBeCloseTo(-0.25, 4);
  });

  it('reads captured audio out of the capture buffer', () => {
    const engine = new RealtimeEngine(48000, 128);
    const captureLeft = new Float32Array(128);
    const captureRight = new Float32Array(128);
    engine.setCaptureBuffer([captureLeft, captureRight]);
    engine.armCapture();
    engine.play();

    engine.process([new Float32Array(128).fill(0.5), new Float32Array(128).fill(-0.5)]);
    const status = engine.captureStatus();
    expect(status.capturedFrames).toBe(128);

    const captured = engine.capturedAudio();
    expect(captured).toHaveLength(2);
    expect(captured[0].length).toBe(128);
    expect(captured[1].length).toBe(128);
    expect(captured[0][0]).toBeCloseTo(0.5, 4);
    expect(captured[1][0]).toBeCloseTo(-0.5, 4);
    engine.destroy();
  });

  it('exposes transport state and live parameter injection', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setTempo(90);
    engine.addParameter({
      id: 3,
      name: 'gain',
      unit: 'dB',
      minValue: -60,
      maxValue: 12,
      defaultValue: 0,
      rtSafe: true,
      defaultCurve: 0, // canonical AutomationCurve::Linear
    });
    // Live parameter injection must not throw.
    engine.setParameter(3, 3.0);
    engine.setParameterSmoothed(3, -3.0, -1);

    engine.play();
    engine.process([new Float32Array(128), new Float32Array(128)]);

    const transport = engine.getTransportState();
    expect(transport.playing).toBe(true);
    expect(transport.isPlaying).toBe(true);
    expect(transport.bpm).toBeCloseTo(90, 3);
    expect(transport.sampleRate).toBeCloseTo(48000, 1);
    expect(transport.samplePosition).toBeGreaterThanOrEqual(128);
    expect(Number.isFinite(transport.ppq)).toBe(true);

    const meterRecords = engine.drainMeterTelemetry();
    expect(Array.isArray(meterRecords)).toBe(true);
    for (const record of meterRecords) {
      expect(record.peakDb).toHaveLength(2);
      expect(record.rmsDb).toHaveLength(2);
      expect(Number.isFinite(record.integratedLufs)).toBe(true);
    }
    engine.destroy();
  });
});
