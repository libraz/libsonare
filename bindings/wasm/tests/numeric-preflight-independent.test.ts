import { beforeAll, describe, expect, it } from 'vitest';
import {
  cyclicTempogram,
  deemphasis,
  fourierTempogram,
  frameSignal,
  init,
  plp,
  preemphasis,
  RealtimeEngine,
  splitSilence,
  tempogram,
  tempogramRatio,
  trimSilence,
  zeroCrossings,
} from '../src/index';

beforeAll(async () => {
  await init();
});

const sampleRate = 22050;
const samples = new Float32Array([0, 1, -1, 0]);
const geometry = { frameLength: 2, hopLength: 1 };

describe('sample-count operations keep sampleRate as a shape-only field', () => {
  it.each([
    ['preemphasis', preemphasis],
    ['deemphasis', deemphasis],
    ['frameSignal', frameSignal],
    ['trimSilence', trimSilence],
    ['splitSilence', splitSilence],
    ['zeroCrossings', zeroCrossings],
  ] as const)('%s accepts the same samples with a non-audio rate in both forms', (name, fn) => {
    const options =
      name === 'frameSignal' || name === 'trimSilence' || name === 'splitSilence' ? geometry : {};
    const call = fn as (...args: unknown[]) => unknown;
    const expected = call({ samples, sampleRate, ...options });
    expect(call({ samples, sampleRate: 7999, ...options })).toEqual(expected);
    expect(call(samples, 7999, options)).toEqual(expected);
    expect(() => call({ samples, sampleRate: 'bad', ...options })).toThrow(TypeError);
  });

  it.each([
    ['preemphasis', preemphasis, new Float32Array(0)],
    ['deemphasis', deemphasis, new Float32Array(0)],
    ['zeroCrossings', zeroCrossings, new Int32Array(0)],
  ] as const)('%s returns an empty typed array', (_name, fn, expected) => {
    expect(fn(new Float32Array(0), sampleRate)).toEqual(expected);
  });

  it('keeps the requested framing rows when the input yields zero frames', () => {
    const empty = new Float32Array(0);
    expect(frameSignal({ samples: empty, sampleRate, ...geometry })).toEqual({
      rows: 2,
      cols: 0,
      data: empty,
    });
  });
});

describe('rhythm sample rates', () => {
  const onsetEnvelope = new Float32Array([0, 1, 0, 0]);
  const request = { onsetEnvelope, winLength: 4, hopLength: 1 };

  it.each([
    ['tempogram', tempogram],
    ['fourierTempogram', fourierTempogram],
  ] as const)('%s ignores the rate value', (_name, fn) => {
    expect(fn({ ...request, sampleRate: 0 })).toEqual(fn({ ...request, sampleRate }));
    expect(() => fn({ ...request, sampleRate: 'bad' as never })).toThrow(TypeError);
  });

  it('ignores the rate value for tempogramRatio', () => {
    const tempogram = { data: new Float32Array([1, 0.5]), rows: 2, cols: 1 };
    const request = { tempogram, factors: [1], hopLength: 1 };
    expect(tempogramRatio({ ...request, sampleRate: 0 })).toEqual(
      tempogramRatio({ ...request, sampleRate }),
    );
  });

  it('accepts a positive below-audio rate for cyclicTempogram', () => {
    const result = cyclicTempogram({ ...request, sampleRate: 1000, nBins: 12 });
    expect(result.rows).toBe(12);
    expect(result.cols).toBe(onsetEnvelope.length);
    expect(() => cyclicTempogram({ ...request, sampleRate: 0 })).toThrow(RangeError);
  });

  it('accepts a positive below-audio rate for plp', () => {
    expect(plp({ ...request, sampleRate: 1000 })).toHaveLength(onsetEnvelope.length);
    expect(() => plp({ ...request, sampleRate: 0 })).toThrow(RangeError);
  });
});

describe('zero-size engine blocks', () => {
  it('processes an empty channel list without advancing the transport', () => {
    using engine = new RealtimeEngine({ sampleRate, maxBlockSize: 16 });
    const before = engine.transportState().renderFrame;
    expect(engine.process([])).toEqual([]);
    expect(engine.process([], { withMonitor: true })).toEqual({ output: [], monitor: [] });
    expect(engine.transportState().renderFrame).toBe(before);
  });

  it('processes equal zero-frame planes and still refuses unequal lengths', () => {
    using engine = new RealtimeEngine({ sampleRate, maxBlockSize: 16 });
    const planes = [new Float32Array(0)];
    const before = engine.transportState().renderFrame;
    expect(engine.process(planes)).toEqual(planes);
    expect(engine.process(planes, { withMonitor: true })).toEqual({
      output: planes,
      monitor: planes,
    });
    expect(engine.transportState().renderFrame).toBe(before);
    expect(() => engine.process([...planes, new Float32Array(1)])).toThrow(RangeError);
  });

  it('renders zero offline frames while still requiring an output channel', () => {
    using engine = new RealtimeEngine({ sampleRate, maxBlockSize: 16 });
    const channels = [new Float32Array(0)];
    expect(engine.renderOffline({ channels, finalize: false })).toEqual(channels);
    expect(() => engine.renderOffline({ channels: [], finalize: false })).toThrow(RangeError);
  });
});
