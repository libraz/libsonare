import { beforeEach, describe, expect, it } from 'vitest';
import { Mixer, mixingScenePreset } from '../src/index.js';
import { expectRefusal, present, unknownKeyError } from './_helpers.js';

const BLOCK_SIZE = 8;
const SAMPLE_RATE = 48000;

// `commentaryDucking` has three input strips (host, guest, music-bed) that all
// route directly to the master bus, which keeps solo / meter behaviour easy to
// reason about deterministically.
const DIRECT_PRESET = 'commentaryDucking';

function buildMixer(): Mixer {
  return Mixer.fromJson(JSON.stringify(mixingScenePreset(DIRECT_PRESET)), {
    sampleRate: SAMPLE_RATE,
    maxBlockSize: BLOCK_SIZE,
  });
}

function serialTailScene(): string {
  const delay = (ms: number) => ({
    slot: 'post',
    processor: 'effects.delay.stereo',
    params: {
      delayTimeLMs: ms,
      delayTimeRMs: ms,
      feedback: 0,
      dryWet: 1,
    },
  });
  return JSON.stringify({
    version: 2,
    strips: [
      {
        id: 'source',
        inserts: [delay(10)],
        sends: [{ id: 'to-aux', destinationBusId: 'aux', sendDb: 0, timing: 'postFader' }],
      },
    ],
    buses: [
      { id: 'aux', role: 'aux', inserts: [delay(20)] },
      { id: 'master', role: 'master', inserts: [delay(30)] },
    ],
    connections: [{ source: 'aux', destination: 'master' }],
  });
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
    expect(mixer.stripCount).toBe(3);
    expect(mixer.stripById('host')).toBe(0);
    expect(typeof mixer.stripById('guest')).toBe('number');
    expect(() => mixer.stripById('does-not-exist')).toThrow(RangeError);
  });

  it('smoke-calls each setter without throwing', () => {
    expect(() => mixer.setSoloed('host', true)).not.toThrow();
    expect(() => mixer.setSoloed(0, false)).not.toThrow();
    expect(() => mixer.setSoloSafe('guest', true)).not.toThrow();
    expect(() => mixer.setPolarityInvert('host', { left: true, right: false })).not.toThrow();
    expect(() => mixer.setPanLaw('host', 'const6Db')).not.toThrow();
    expect(() => mixer.setChannelDelaySamples('host', 2)).not.toThrow();
    expect(() => mixer.setVcaOffsetDb('host', -3)).not.toThrow();
    expect(() => mixer.setDualPan('host', -0.5, 0.5)).not.toThrow();
  });

  it('forwards asymmetric polarity and dual-pan values in scene JSON', () => {
    mixer.setPolarityInvert('host', { left: true, right: false });
    mixer.setDualPan('host', 0.75, -0.25);

    const scene = JSON.parse(mixer.toJson());
    const host = scene.strips.find((strip: { id: string }) => strip.id === 'host');
    expect(host.polarityInvertLeft).toBe(true);
    expect(host.polarityInvertRight).toBe(false);
    expect(host.dualPanLeft).toBe(0.75);
    expect(host.dualPanRight).toBe(-0.25);
  });

  it('stores surround pan on the strip scene', () => {
    expect(() =>
      mixer.setSurroundPan('host', { azimuth: -45, divergence: 0.25, lfe: 0.5 }),
    ).not.toThrow();
    const scene = JSON.parse(mixer.toJson());
    const host = scene.strips.find((strip: { id: string }) => strip.id === 'host');
    expect(host.surroundPan.azimuth).toBeCloseTo(-45, 5);
    expect(host.surroundPan.divergence).toBeCloseTo(0.25, 5);
    expect(host.surroundPan.lfe).toBeCloseTo(0.5, 5);
  });

  it('refuses an out-of-range surround pan instead of clamping it', () => {
    for (const pan of [{ azimuth: 181 }, { azimuth: -181 }, { divergence: 1.5 }, { lfe: -0.1 }]) {
      expectRefusal(() => mixer.setSurroundPan('host', pan), RangeError);
    }
  });

  it('refuses elevation and distance as unknown surround pan keys', () => {
    for (const key of ['elevation', 'distance']) {
      expect(() => mixer.setSurroundPan('host', { [key]: 0 } as never)).toThrow(
        unknownKeyError(`Mixer.setSurroundPan: unknown key ${key}`),
      );
    }
  });

  it('returns a numeric index from addSend and accepts setSendDb', () => {
    const sendIndex = mixer.addSend('host', {
      id: 'host-extra',
      destinationBusId: 'master',
      sendDb: -6,
      timing: 'postFader',
    });
    expect(typeof sendIndex).toBe('number');
    expect(sendIndex).toBe(0);
    expect(() => mixer.setSendDb('host', sendIndex, -12)).not.toThrow();

    const scene = JSON.parse(mixer.toJson());
    const host = scene.strips.find((strip: { id: string }) => strip.id === 'host');
    expect(host.sends).toHaveLength(1);
    expect(host.sends[0].id).toBe('host-extra');
    expect(host.sends[0].sendDb).toBeCloseTo(-12, 5);
  });

  it('removeSend drops a previously added send', () => {
    const sendIndex = mixer.addSend('host', {
      id: 'host-extra',
      destinationBusId: 'master',
      sendDb: -6,
      timing: 'postFader',
    });
    expect(sendIndex).toBe(0);

    const before = JSON.parse(mixer.toJson());
    const hostBefore = before.strips.find((strip: { id: string }) => strip.id === 'host');
    expect(hostBefore.sends).toHaveLength(1);

    expect(() => mixer.removeSend('host', sendIndex)).not.toThrow();

    const after = JSON.parse(mixer.toJson());
    const hostAfter = after.strips.find((strip: { id: string }) => strip.id === 'host');
    expect(hostAfter.sends ?? []).toHaveLength(0);
  });

  it('returns numeric meter snapshots from stripMeter', () => {
    const { left, right } = constantStrips([0.5, 0.25, 0.1]);
    mixer.processStereo(left, right);

    const meter = mixer.stripMeter('host');
    for (const field of ['peakDbL', 'peakDbR', 'rmsDbL', 'rmsDbR', 'correlation'] as const) {
      expect(Number.isFinite(meter[field])).toBe(true);
    }
    // One 8-sample block fills no loudness window and passes no gating block.
    expect('momentaryLufs' in meter).toBe(false);
    expect('integratedLufs' in meter).toBe(false);
    expect(typeof meter.likelyMonoCompatible).toBe('boolean');
    expect(meter.peakDbL).toBeGreaterThan(-120);

    const preFader = mixer.stripMeter('host', 'preFader');
    const postFader = mixer.stripMeter('host', 'postFader');
    expect(Number.isFinite(preFader.peakDbL)).toBe(true);
    expect(Number.isFinite(postFader.peakDbL)).toBe(true);
    const master = mixer.busMeter('master');
    expect(master.numChannels).toBe(2);
    expect(Number.isFinite(master.peakDb[0])).toBe(true);
  });

  it('stripMeter accepts an optional tap argument', () => {
    const { left, right } = constantStrips([0.5, 0.25, 0.1]);
    mixer.processStereo(left, right);

    // No tap -> post-fader (same as the explicit 'postFader' tap).
    const post = mixer.stripMeter('host');
    const postTap = mixer.stripMeter('host', 'postFader');
    const preTap = mixer.stripMeter('host', 'preFader');

    for (const m of [post, postTap, preTap]) {
      expect(Number.isFinite(m.peakDbL)).toBe(true);
      expect(m.peakDbL).toBeGreaterThan(-120);
    }
    // The no-tap default matches the explicit post-fader tap.
    const postTapPeak = present(postTap.peakDbL, 'post-fader tap peakDbL');
    expect(post.peakDbL).toBeCloseTo(postTapPeak, 5);
    // Pre-fader on host (before its fader trim) should be >= post-fader.
    expect(preTap.peakDbL).toBeGreaterThanOrEqual(postTapPeak - 1e-3);
  });

  it('refuses an out-of-domain pan or width with a RangeError', () => {
    expect(() => mixer.setPan('host', 1.5)).toThrow(RangeError);
    expect(() => mixer.setPan('host', -1.01)).toThrow(RangeError);
    expect(() => mixer.setDualPan('host', -1.5, 0)).toThrow(RangeError);
    expect(() => mixer.setWidth('host', 3)).toThrow(RangeError);
    expect(() => mixer.setWidth('host', -0.1)).toThrow(RangeError);
    expect(() => mixer.schedulePanAutomation('host', { samplePos: 0, pan: 1.5 })).toThrow(
      RangeError,
    );
    expect(() => mixer.scheduleWidthAutomation('host', { samplePos: 0, width: 2.5 })).toThrow(
      RangeError,
    );
  });

  it('setPan without panMode keeps the strip current pan mode', () => {
    // Put the strip into stereoPan mode explicitly first.
    mixer.setPan('host', 0.0, 'stereoPan');
    const beforeScene = JSON.parse(mixer.toJson());
    const beforeHost = beforeScene.strips.find((s: { id: string }) => s.id === 'host');
    expect(beforeHost.panMode).toBe('stereoPan');

    // A plain pan nudge (no panMode) must NOT reset the mode back to Balance.
    mixer.setPan('host', 0.3);
    const afterScene = JSON.parse(mixer.toJson());
    const afterHost = afterScene.strips.find((s: { id: string }) => s.id === 'host');
    expect(afterHost.panMode).toBe('stereoPan'); // still stereoPan
    expect(afterHost.pan).toBeCloseTo(0.3, 5);

    // Passing an explicit mode still switches it.
    mixer.setPan('host', 0.3, 'balance');
    const finalScene = JSON.parse(mixer.toJson());
    const finalHost = finalScene.strips.find((s: { id: string }) => s.id === 'host');
    expect(finalHost.panMode).toBe('balance');
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

  it('bounds the goniometer working buffer independently of maxPoints', () => {
    const { left, right } = constantStrips([0.5, 0.25, 0.1]);
    mixer.processStereo(left, right);

    // maxPoints is a request, not an allocation size. A huge value used to size
    // a std::vector directly, so a metering UI deriving it from a window size
    // could take the whole process down with an uncatchable std::bad_alloc.
    const huge = mixer.readGoniometerLatest('host', Number.MAX_SAFE_INTEGER);
    expect(Array.isArray(huge)).toBe(true);
    expect(huge.length).toBeGreaterThan(0);
    expect(huge.length).toBeLessThanOrEqual(4096);
    expect(huge.every((point) => Number.isFinite(point.left))).toBe(true);

    // Anything that is not a finite non-negative integer is a catchable error,
    // never a silent clamp to zero.
    for (const invalid of [-1, -0.5, 1.5, Number.NaN, Number.POSITIVE_INFINITY, Number.MAX_VALUE]) {
      expect(() => mixer.readGoniometerLatest('host', invalid)).toThrow(RangeError);
    }
    expect(() => mixer.readGoniometerLatest('host', '4' as unknown as number)).toThrow(TypeError);

    // Zero stays a legal empty read.
    expect(mixer.readGoniometerLatest('host', 0)).toEqual([]);
  });

  it('accepts scheduled automation events with string-union curves', () => {
    expect(() =>
      mixer.scheduleFaderAutomation('host', { samplePos: 0, faderDb: -6, curve: 'linear' }),
    ).not.toThrow();
    expect(() =>
      mixer.schedulePanAutomation('host', { samplePos: 0, pan: 0.5, curve: 'exponential' }),
    ).not.toThrow();
    expect(() =>
      mixer.scheduleWidthAutomation('host', { samplePos: 0, width: 1.2, curve: 'linear' }),
    ).not.toThrow();

    const sendIndex = mixer.addSend('host', {
      id: 'host-aux',
      destinationBusId: 'master',
      sendDb: -6,
      timing: 'preFader',
    });
    expect(() =>
      mixer.scheduleSendAutomation('host', sendIndex, {
        samplePos: 0,
        sendDb: -3,
        curve: 'exponential',
      }),
    ).not.toThrow();
  });
});

describe('Mixer solo behaviour', () => {
  it('silences non-soloed strips while keeping the soloed strip audible', () => {
    const baseline = buildMixer();
    const equalAmplitudes = [0.5, 0.5, 0.5];

    const baselineInput = constantStrips(equalAmplitudes);
    const baselineResult = baseline.processStereo(baselineInput.left, baselineInput.right);
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
    expect(soloed.stripMeter('guest').peakDbL).toBe(Number.NEGATIVE_INFINITY);
    expect(soloed.stripMeter('music-bed').peakDbL).toBe(Number.NEGATIVE_INFINITY);
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
    expect(mixer.stripMeter('music-bed').peakDbL).toBe(Number.NEGATIVE_INFINITY);
  });
});

describe('Mixer argument validation', () => {
  let mixer: Mixer;

  beforeEach(() => {
    mixer = buildMixer();
  });

  it('throws on invalid string-union arguments', () => {
    // @ts-expect-error invalid pan law string is rejected at runtime
    expect(() => mixer.setPanLaw('host', 'bogusLaw')).toThrow(/panLaw must be one of /);
    // @ts-expect-error invalid meter tap string is rejected at runtime
    expect(() => mixer.stripMeter('host', 'bogusTap')).toThrow(/tap must be one of /);
    expect(() =>
      mixer.scheduleFaderAutomation('host', {
        samplePos: 0,
        faderDb: -6,
        // @ts-expect-error invalid automation curve string is rejected at runtime
        curve: 'bogusCurve',
      }),
    ).toThrow(/curve must be one of /);
  });

  it('throws on out-of-range strip indices', () => {
    expect(() => mixer.setSoloed(99, true)).toThrow(/out of range/i);
    expect(() => mixer.stripMeter(99)).toThrow(/out of range/i);
  });

  it('throws on unknown strip id references', () => {
    expect(() => mixer.setSoloed('not-a-strip', true)).toThrow(/not found/i);
  });

  it('rejects non-finite live values and accepts later finite values', () => {
    expect(() => mixer.setFaderDb('host', Number.NaN)).toThrow();
    expect(() => mixer.setPan('host', Number.POSITIVE_INFINITY)).toThrow();
    expect(() =>
      mixer.scheduleFaderAutomation('host', { samplePos: 0, faderDb: Number.NaN, curve: 'linear' }),
    ).toThrow();
    expect(() => mixer.setFaderDb('host', -3)).not.toThrow();
    expect(() => mixer.setPan('host', 0.25)).not.toThrow();
  });

  it('maps string unions to native ints end-to-end', () => {
    expect(() => mixer.setPanLaw('host', 'const3Db')).not.toThrow();
    expect(() => mixer.setPanLaw('host', 'const4.5Db')).not.toThrow();
    expect(() => mixer.setPanLaw('host', 'const6Db')).not.toThrow();
    expect(() => mixer.setPanLaw('host', 'linear0Db')).not.toThrow();
    expect(() => mixer.stripMeter('host', 'preFader')).not.toThrow();
    expect(() => mixer.stripMeter('host', 'postFader')).not.toThrow();
    const sendIndex = mixer.addSend('host', {
      id: 'host-send',
      destinationBusId: 'master',
      sendDb: 0,
      timing: 'postFader',
    });
    expect(() =>
      mixer.scheduleSendAutomation('host', sendIndex, {
        samplePos: 0,
        sendDb: -3,
        curve: 'exponential',
      }),
    ).not.toThrow();
  });

  it('adds strips imperatively', () => {
    const before = mixer.stripCount;
    mixer.addStrip('late-arrival');
    expect(mixer.stripCount).toBe(before + 1);
    expect(mixer.stripById('late-arrival')).toBe(before);
    expect(() => mixer.compile()).not.toThrow();
    // The C call answers a failure with a NULL strip rather than an error code,
    // so a facade that skipped the null check would report both of these as a
    // successful add.
    expect(() => mixer.addStrip('late-arrival')).toThrow(
      /: id must not be the id of a strip already in this mixer$/,
    );
    expect(() => mixer.addStrip('over-sampled', { truePeakOversample: 32 })).toThrow(
      /: truePeakOversample must be one of 4, 8$/,
    );
    // A wrong-typed metering field is refused by name rather than substituted.
    expect(() => mixer.addStrip('wrong-type', { lufs: 'yes' as unknown as boolean })).toThrow(
      /lufs/,
    );
    expect(mixer.stripCount).toBe(before + 1);
  });

  it('refuses a metering argument that is not a plain object', () => {
    const before = mixer.stripCount;
    // A wrong-typed BAG would otherwise read as an empty one and put the caller
    // on the full default metering, the field readers' failure mode one level up.
    // @ts-expect-error a non-object metering bag is rejected at runtime
    expect(() => mixer.addStrip('number-bag', 5)).toThrow(TypeError);
    // @ts-expect-error an array is not a plain object, though `typeof []` is 'object'
    expect(() => mixer.addStrip('array-bag', [])).toThrow(TypeError);
    expect(mixer.stripCount).toBe(before);

    // Absent and explicit undefined are the omitted default, which the scene
    // document spells by carrying no metering object at all.
    mixer.addStrip('omitted');
    mixer.addStrip('explicit-undefined', undefined);
    expect(mixer.stripCount).toBe(before + 2);
    const strips = JSON.parse(mixer.toJson()).strips as Array<{
      id: string;
      metering?: Record<string, unknown>;
    }>;
    expect(strips.find((entry) => entry.id === 'omitted')?.metering).toBeUndefined();
    expect(strips.find((entry) => entry.id === 'explicit-undefined')?.metering).toBeUndefined();
  });

  it('round-trips an added strip metering configuration through the scene', () => {
    mixer.addStrip('metered', { lufs: false, truePeakOversample: 8 });
    // The document omits the object at the full default, so a strip added with
    // no metering argument carries none and only the opted-out one shows.
    mixer.addStrip('plain');
    const strips = JSON.parse(mixer.toJson()).strips as Array<{
      id: string;
      metering?: Record<string, unknown>;
    }>;
    expect(strips.find((entry) => entry.id === 'metered')?.metering).toEqual({
      enabled: true,
      lufs: false,
      truePeak: true,
      truePeakOversample: 8,
    });
    expect(strips.find((entry) => entry.id === 'plain')?.metering).toBeUndefined();
  });

  it('manages buses imperatively', () => {
    const before = mixer.busCount;
    expect(before).toBeGreaterThanOrEqual(0);
    mixer.addBus('extra-aux', 'aux');
    expect(mixer.busCount).toBe(before + 1);
    mixer.removeBus('extra-aux');
    expect(mixer.busCount).toBe(before);
  });

  it('manages VCA groups imperatively', () => {
    const before = mixer.vcaGroupCount;
    mixer.addVcaGroup('test-vca-group', -2, ['host', 'guest']);
    expect(mixer.vcaGroupCount).toBe(before + 1);
    mixer.setVcaGroupGainDb('test-vca-group', -7);
    mixer.setVcaGroupMembers('test-vca-group', ['guest']);
    const scene = JSON.parse(mixer.toJson());
    const group = scene.vcaGroups.find((entry: { id: string }) => entry.id === 'test-vca-group');
    expect(group.gainDb).toBe(-7);
    expect(group.members).toEqual(['guest']);
    mixer.removeVcaGroup('test-vca-group');
    expect(mixer.vcaGroupCount).toBe(before);
    // members default to an empty group.
    expect(() => mixer.addVcaGroup('test-vca-empty', 0)).not.toThrow();
    mixer.removeVcaGroup('test-vca-empty');
  });

  it('reports a numeric tail length', () => {
    mixer.compile();
    const tail = mixer.tailSamples;
    expect(typeof tail).toBe('number');
    expect(tail).toBeGreaterThanOrEqual(0);
  });

  it('reports the longest serial send tail', () => {
    const routed = Mixer.fromJson(serialTailScene(), {
      sampleRate: SAMPLE_RATE,
      maxBlockSize: BLOCK_SIZE,
    });
    try {
      expect(routed.tailSamples).toBeGreaterThan(1440);
    } finally {
      routed.destroy();
    }
  });

  it('reports a numeric latency', () => {
    mixer.compile();
    const latency = mixer.latencySamples;
    expect(typeof latency).toBe('number');
    expect(latency).toBeGreaterThanOrEqual(0);
  });

  it('drains a stereo tail block of the requested length', () => {
    mixer.compile();
    const result = mixer.drainTailStereo(BLOCK_SIZE);
    expect(result.left).toHaveLength(BLOCK_SIZE);
    expect(result.right).toHaveLength(BLOCK_SIZE);
    expect(result.sampleRate).toBe(SAMPLE_RATE);
  });

  it('enforces the [1, blockSize] integer rule shared with WASM and Python', () => {
    mixer.compile();
    // 0 is rejected (the tail must advance at least one frame).
    expect(() => mixer.drainTailStereo(0)).toThrow(/\[1, /);
    // Fractional counts are rejected rather than truncated toward zero.
    expect(() => mixer.drainTailStereo(2.5)).toThrow(/integer/);
    // Above the configured block size is rejected.
    expect(() => mixer.drainTailStereo(BLOCK_SIZE + 1)).toThrow(/\[1, /);
    // The endpoints of the range are accepted.
    expect(() => mixer.drainTailStereo(1)).not.toThrow();
    expect(() => mixer.drainTailStereo(BLOCK_SIZE)).not.toThrow();
  });
});

describe('Mixer.setOutputBus', () => {
  const tone = (): Float32Array[] => [new Float32Array(BLOCK_SIZE).fill(0.5)];
  const connections = (m: Mixer): unknown =>
    (JSON.parse(m.toJson()) as { connections?: unknown }).connections;

  function emptyMixer(): Mixer {
    return Mixer.fromJson(JSON.stringify({ version: 2 }), {
      sampleRate: SAMPLE_RATE,
      maxBlockSize: BLOCK_SIZE,
    });
  }

  function refusal(call: () => void): Error {
    let error: unknown;
    try {
      call();
    } catch (thrown) {
      error = thrown;
    }
    expect(error).toBeInstanceOf(RangeError);
    return error as Error;
  }

  it('sends a strip into a submix, then on to the master', () => {
    const mixer = emptyMixer();
    mixer.addStrip('a');
    mixer.addBus('sub', 'submix');
    mixer.setOutputBus('a', 'sub');
    mixer.compile();
    let out = mixer.processStereo(tone(), tone());
    expect(peakAbs(out.left)).toBe(0);
    expect(mixer.busMeter('sub').peakDb[0]).toBeGreaterThan(-120);

    mixer.setOutputBus('sub', 'master');
    mixer.compile();
    out = mixer.processStereo(tone(), tone());
    expect(peakAbs(out.left)).toBeGreaterThan(0.01);
    expect(connections(mixer)).toEqual([
      { source: 'a', destination: 'sub' },
      { source: 'sub', destination: 'master' },
    ]);
    mixer.destroy();
  });

  it('refuses bad edits with the ids in the message and leaves the scene unchanged', () => {
    const mixer = emptyMixer();
    mixer.addStrip('a');
    mixer.addBus('sub', 'submix');
    mixer.addBus('sub2', 'submix');
    mixer.setOutputBus('sub', 'sub2');
    const before = mixer.toJson();
    const cases: Array<[string, string, string]> = [
      ['ghost', 'sub', 'sourceId must be the id of a strip or a bus in this mixer'],
      ['a', 'nowhere', 'busId must be the id of a bus in this mixer'],
      ['a', 'a', 'busId must be the id of a bus, not of a strip'],
      ['master', 'sub', 'sourceId must not be the id of the master bus'],
      ['sub', 'sub', 'busId must not be the same id as sourceId'],
      ['sub2', 'sub', 'busId must not name a bus whose routing leads back to sourceId'],
    ];
    for (const [source, bus, text] of cases) {
      expect(refusal(() => mixer.setOutputBus(source, bus)).message).toBe(
        `Mixer.setOutputBus: ${text}`,
      );
    }
    expect(mixer.toJson()).toBe(before);
    mixer.destroy();
  });

  it('runs no compile in the call, accepted or refused', () => {
    const mixer = emptyMixer();
    mixer.addBus('sub');
    refusal(() => mixer.setOutputBus('sub', 'sub'));
    expect(() => mixer.busNonFiniteDiscardCount('sub')).toThrow();
    mixer.destroy();
  });

  it('refuses non-string arguments with a TypeError', () => {
    const mixer = emptyMixer();
    const call = mixer.setOutputBus as unknown as (...args: unknown[]) => void;
    expect(() => call.call(mixer, 'a', 1)).toThrow(TypeError);
    mixer.destroy();
  });
});
