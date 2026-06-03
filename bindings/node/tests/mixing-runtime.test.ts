import { beforeEach, describe, expect, it } from 'vitest';
import { Mixer, mixingScenePresetJson } from '../src/index.js';

const BLOCK_SIZE = 8;
const SAMPLE_RATE = 48000;

// `commentaryDucking` has three input strips (host, guest, music-bed) that all
// route directly to the master bus, which keeps solo / meter behaviour easy to
// reason about deterministically.
const DIRECT_PRESET = 'commentaryDucking';

function buildMixer(): Mixer {
  return Mixer.fromSceneJson(mixingScenePresetJson(DIRECT_PRESET), SAMPLE_RATE, BLOCK_SIZE);
}

function constantStrips(amplitudes: number[]): {
  left: Float32Array[];
  right: Float32Array[];
} {
  const left = amplitudes.map((amp) => new Float32Array(BLOCK_SIZE).fill(amp));
  const right = amplitudes.map((amp) => new Float32Array(BLOCK_SIZE).fill(amp));
  return { left, right };
}

function peakAbs(values: Float32Array): number {
  let peak = 0;
  for (const value of values) {
    peak = Math.max(peak, Math.abs(value));
  }
  return peak;
}

describe('Mixer runtime methods', () => {
  let mixer: Mixer;

  beforeEach(() => {
    mixer = buildMixer();
  });

  it('resolves strip ids and reports null for unknown ids', () => {
    expect(mixer.stripCount()).toBe(3);
    expect(mixer.stripById('host')).toBe(0);
    expect(typeof mixer.stripById('guest')).toBe('number');
    expect(mixer.stripById('does-not-exist')).toBeNull();
  });

  it('smoke-calls each setter without throwing', () => {
    expect(() => mixer.setSoloed('host', true)).not.toThrow();
    expect(() => mixer.setSoloed(0, false)).not.toThrow();
    expect(() => mixer.setSoloSafe('guest', true)).not.toThrow();
    expect(() => mixer.setPolarityInvert('host', true, false)).not.toThrow();
    expect(() => mixer.setPanLaw('host', 'const6dB')).not.toThrow();
    expect(() => mixer.setChannelDelaySamples('host', 2)).not.toThrow();
    expect(() => mixer.setVcaOffsetDb('host', -3)).not.toThrow();
    expect(() => mixer.setDualPan('host', -0.5, 0.5)).not.toThrow();
  });

  it('returns a numeric index from addSend and accepts setSendDb', () => {
    const sendIndex = mixer.addSend('host', 'host-extra', 'master', -6, 'postFader');
    expect(typeof sendIndex).toBe('number');
    expect(sendIndex).toBe(0);
    expect(() => mixer.setSendDb('host', sendIndex, -12)).not.toThrow();

    const scene = JSON.parse(mixer.toSceneJson());
    const host = scene.strips.find((strip: { id: string }) => strip.id === 'host');
    expect(host.sends).toHaveLength(1);
    expect(host.sends[0].id).toBe('host-extra');
    expect(host.sends[0].sendDb).toBeCloseTo(-12, 5);
  });

  it('returns numeric meter snapshots from stripMeter and meterTap', () => {
    const { left, right } = constantStrips([0.5, 0.25, 0.1]);
    mixer.processStereo(left, right);

    const meter = mixer.stripMeter('host');
    for (const field of [
      'peakDbL',
      'peakDbR',
      'rmsDbL',
      'rmsDbR',
      'correlation',
      'momentaryLufs',
      'integratedLufs',
    ] as const) {
      expect(Number.isFinite(meter[field])).toBe(true);
    }
    expect(typeof meter.likelyMonoCompatible).toBe('boolean');
    expect(meter.peakDbL).toBeGreaterThan(-120);

    const preFader = mixer.meterTap('host', 'preFader');
    const postFader = mixer.meterTap('host', 'postFader');
    expect(Number.isFinite(preFader.peakDbL)).toBe(true);
    expect(Number.isFinite(postFader.peakDbL)).toBe(true);
  });

  it('stripMeter accepts an optional tap argument', () => {
    const { left, right } = constantStrips([0.5, 0.25, 0.1]);
    mixer.processStereo(left, right);

    // No tap -> post-fader (same as the dedicated meterTap('postFader')).
    const post = mixer.stripMeter('host');
    const postTap = mixer.stripMeter('host', 'postFader');
    const preTap = mixer.stripMeter('host', 'preFader');

    for (const m of [post, postTap, preTap]) {
      expect(Number.isFinite(m.peakDbL)).toBe(true);
      expect(m.peakDbL).toBeGreaterThan(-120);
    }
    // The no-tap default matches the explicit post-fader tap.
    expect(post.peakDbL).toBeCloseTo(postTap.peakDbL, 5);
    // Pre-fader on host (before its fader trim) should be >= post-fader.
    expect(preTap.peakDbL).toBeGreaterThanOrEqual(postTap.peakDbL - 1e-3);
  });

  it('setPan without panMode keeps the strip current pan mode', () => {
    // Put the strip into stereoPan mode explicitly first.
    mixer.setPan('host', 0.0, 'stereoPan');
    const beforeScene = JSON.parse(mixer.toSceneJson());
    const beforeHost = beforeScene.strips.find((s: { id: string }) => s.id === 'host');
    expect(beforeHost.panMode).toBe(1); // 1 = stereoPan

    // A plain pan nudge (no panMode) must NOT reset the mode back to Balance.
    mixer.setPan('host', 0.3);
    const afterScene = JSON.parse(mixer.toSceneJson());
    const afterHost = afterScene.strips.find((s: { id: string }) => s.id === 'host');
    expect(afterHost.panMode).toBe(1); // still stereoPan
    expect(afterHost.pan).toBeCloseTo(0.3, 5);

    // Passing an explicit mode still switches it.
    mixer.setPan('host', 0.3, 'balance');
    const finalScene = JSON.parse(mixer.toSceneJson());
    const finalHost = finalScene.strips.find((s: { id: string }) => s.id === 'host');
    expect(finalHost.panMode).toBe(0); // 0 = balance
  });

  it('returns an array of goniometer points', () => {
    const { left, right } = constantStrips([0.5, 0.25, 0.1]);
    mixer.processStereo(left, right);

    const points = mixer.readGoniometerLatest('host', 4);
    expect(Array.isArray(points)).toBe(true);
    expect(points.length).toBeGreaterThan(0);
    expect(points.length).toBeLessThanOrEqual(4);
    for (const point of points) {
      expect(typeof point.left).toBe('number');
      expect(typeof point.right).toBe('number');
    }
  });

  it('accepts scheduled automation events with string-union curves', () => {
    expect(() => mixer.scheduleFaderAutomation('host', 0, -6, 'linear')).not.toThrow();
    expect(() => mixer.schedulePanAutomation('host', 0, 0.5, 'exponential')).not.toThrow();
    expect(() => mixer.scheduleWidthAutomation('host', 0, 1.2, 'linear')).not.toThrow();

    const sendIndex = mixer.addSend('host', 'host-aux', 'master', -6, 'preFader');
    expect(() =>
      mixer.scheduleSendAutomation('host', sendIndex, 0, -3, 'exponential'),
    ).not.toThrow();
  });
});

describe('Mixer solo behaviour', () => {
  it('silences non-soloed strips while keeping the soloed strip audible', () => {
    const baseline = buildMixer();
    const equalAmplitudes = [0.5, 0.5, 0.5];

    const baselineResult = baseline.processStereo(
      ...Object.values(constantStrips(equalAmplitudes)),
    );
    const baselinePeak = peakAbs(baselineResult.left);

    const soloed = buildMixer();
    soloed.setSoloed('host', true);
    const soloInput = constantStrips(equalAmplitudes);
    const soloResult = soloed.processStereo(soloInput.left, soloInput.right);
    const soloPeak = peakAbs(soloResult.left);

    // Soloing one of three equal strips drops the master peak well below the
    // full mix, confirming the other two strips are implied-muted.
    expect(soloPeak).toBeGreaterThan(0);
    expect(soloPeak).toBeLessThan(baselinePeak * 0.6);

    expect(soloed.stripMeter('host').peakDbL).toBeGreaterThan(-120);
    expect(soloed.stripMeter('guest').peakDbL).toBeCloseTo(-120, 1);
    expect(soloed.stripMeter('music-bed').peakDbL).toBeCloseTo(-120, 1);
  });

  it('keeps a solo-safe strip audible when another strip is soloed', () => {
    const mixer = buildMixer();
    mixer.setSoloSafe('guest', true);
    mixer.setSoloed('host', true);

    const { left, right } = constantStrips([0.5, 0.5, 0.5]);
    mixer.processStereo(left, right);

    // Both the soloed strip and the solo-safe strip pass; the unmarked,
    // non-soloed strip is silenced.
    expect(mixer.stripMeter('host').peakDbL).toBeGreaterThan(-120);
    expect(mixer.stripMeter('guest').peakDbL).toBeGreaterThan(-120);
    expect(mixer.stripMeter('music-bed').peakDbL).toBeCloseTo(-120, 1);
  });
});

describe('Mixer argument validation', () => {
  let mixer: Mixer;

  beforeEach(() => {
    mixer = buildMixer();
  });

  it('throws on invalid string-union arguments', () => {
    // @ts-expect-error invalid pan law string is rejected at runtime
    expect(() => mixer.setPanLaw('host', 'bogusLaw')).toThrow(/pan law/i);
    // @ts-expect-error invalid meter tap string is rejected at runtime
    expect(() => mixer.meterTap('host', 'bogusTap')).toThrow(/meter tap/i);
    // @ts-expect-error invalid automation curve string is rejected at runtime
    expect(() => mixer.scheduleFaderAutomation('host', 0, -6, 'bogusCurve')).toThrow(
      /automation curve/i,
    );
  });

  it('throws on out-of-range strip indices', () => {
    expect(() => mixer.setSoloed(99, true)).toThrow(/out of range/i);
    expect(() => mixer.stripMeter(99)).toThrow(/out of range/i);
  });

  it('throws on unknown strip id references', () => {
    expect(() => mixer.setSoloed('not-a-strip', true)).toThrow(/not found/i);
  });

  it('maps string unions to native ints end-to-end', () => {
    expect(() => mixer.setPanLaw('host', 'const3dB')).not.toThrow();
    expect(() => mixer.setPanLaw('host', 'const4.5dB')).not.toThrow();
    expect(() => mixer.setPanLaw('host', 'const6dB')).not.toThrow();
    expect(() => mixer.setPanLaw('host', 'linear0dB')).not.toThrow();
    expect(() => mixer.meterTap('host', 'preFader')).not.toThrow();
    expect(() => mixer.meterTap('host', 'postFader')).not.toThrow();
    const sendIndex = mixer.addSend('host', 'host-send', 'master', 0, 'postFader');
    expect(() =>
      mixer.scheduleSendAutomation('host', sendIndex, 0, -3, 'exponential'),
    ).not.toThrow();
  });

  it('manages buses imperatively', () => {
    const before = mixer.busCount();
    expect(before).toBeGreaterThanOrEqual(0);
    mixer.addBus('extra-aux', 'aux');
    expect(mixer.busCount()).toBe(before + 1);
    mixer.removeBus('extra-aux');
    expect(mixer.busCount()).toBe(before);
  });

  it('manages VCA groups imperatively', () => {
    const before = mixer.vcaGroupCount();
    mixer.addVcaGroup('test-vca-group', -2, ['host', 'guest']);
    expect(mixer.vcaGroupCount()).toBe(before + 1);
    mixer.removeVcaGroup('test-vca-group');
    expect(mixer.vcaGroupCount()).toBe(before);
    // members default to an empty group.
    expect(() => mixer.addVcaGroup('test-vca-empty', 0)).not.toThrow();
    mixer.removeVcaGroup('test-vca-empty');
  });
});
