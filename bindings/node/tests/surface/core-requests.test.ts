import { describe, expect, it } from 'vitest';
import { type DetectKeyRequest, detectKey } from '../../dist/index.js';
import { requestOnly, sampleOneShot } from '../../dist/validation.js';

const sampleRate = 22050;

/** A C major triad re-struck every half second. */
function triad(): Float32Array {
  const out = new Float32Array(sampleRate * 2);
  for (const f of [261.63, 329.63, 392.0]) {
    for (let i = 0; i < out.length; i++) {
      out[i] += 0.2 * Math.sin((2 * Math.PI * f * i) / sampleRate);
    }
  }
  return out;
}

describe('request objects', () => {
  it('refuses an unknown request key by name', () => {
    const request = { samples: triad(), sampleRate, notAnOption: 1 } as unknown;
    let caught: unknown;
    try {
      detectKey(request as DetectKeyRequest);
    } catch (error) {
      caught = error;
    }
    expect(caught).toBeInstanceOf(TypeError);
    expect((caught as Error).message).toContain('notAnOption');
  });

  it('treats null for an optional boolean like omitting it', () => {
    const samples = triad();
    const omitted = detectKey({ samples, sampleRate });
    const withNull = detectKey({
      samples,
      sampleRate,
      useHpss: null,
    } as unknown as DetectKeyRequest);
    expect(withNull).toEqual(omitted);
  });

  it('treats null for an optional string like omitting it', () => {
    const samples = triad();
    const omitted = detectKey({ samples, sampleRate });
    const withNull = detectKey({
      samples,
      sampleRate,
      genreHint: null,
    } as unknown as DetectKeyRequest);
    expect(withNull).toEqual(omitted);
  });
});

describe('one-shot normaliser', () => {
  const samples = new Float32Array(2048);
  const call = (...args: unknown[]): unknown => sampleOneShot('detectKey', args);

  it('merges the positional form into one request', () => {
    const request = call(samples, sampleRate, { nFft: 1024 }) as Record<string, unknown>;
    expect(request).toEqual({ samples, sampleRate, nFft: 1024 });
  });

  it('returns the request form unchanged', () => {
    const request = { samples, sampleRate, hopLength: 256 };
    expect(call(request)).toEqual(request);
  });

  it.each([
    ['omitted', undefined, TypeError],
    ['zero', 0, RangeError],
    ['negative zero', -0, RangeError],
    ['negative', -44100, RangeError],
    ['NaN', Number.NaN, RangeError],
    ['Infinity', Number.POSITIVE_INFINITY, RangeError],
    ['fraction', 44100.5, RangeError],
    ['boolean', true, TypeError],
    ['string', '44100', TypeError],
    ['bigint', 44100n, TypeError],
    ['2^31', 2 ** 31, RangeError],
  ])('refuses a %s sample rate in the positional form', (_name, value, errorClass) => {
    expect(() => call(samples, value)).toThrow(errorClass);
  });

  it.each([
    ['omitted', undefined, TypeError],
    ['zero', 0, RangeError],
    ['fraction', 44100.5, RangeError],
    ['string', '44100', TypeError],
    ['2^31', 2 ** 31, RangeError],
  ])('refuses a %s sample rate in the request form', (_name, value, errorClass) => {
    expect(() => call({ samples, sampleRate: value })).toThrow(errorClass);
  });

  it('names sampleRate when it is missing', () => {
    expect(() => call(samples)).toThrow(/sampleRate/);
    expect(() => call({ samples })).toThrow(/sampleRate/);
  });

  it('refuses a request plus an extra positional argument', () => {
    expect(() => call({ samples, sampleRate }, 44100)).toThrow(TypeError);
    expect(() => call({ samples, sampleRate }, undefined)).not.toThrow();
  });

  it('refuses more than three positional arguments', () => {
    expect(() => call(samples, sampleRate, {}, 1)).toThrow(TypeError);
  });

  it('refuses samples or sampleRate inside the options', () => {
    expect(() => call(samples, sampleRate, { samples })).toThrow(/samples/);
    expect(() => call(samples, sampleRate, { sampleRate })).toThrow(/sampleRate/);
  });

  it('refuses an unknown option or request key by name', () => {
    expect(() => call(samples, sampleRate, { bogus: 1 })).toThrow(/bogus/);
    expect(() => call({ samples, sampleRate, bogus: 1 })).toThrow(/bogus/);
  });

  it('reports a wrong-typed first argument as a samples error', () => {
    expect(() => call(null, sampleRate)).toThrow(TypeError);
    expect(() => call([1, 2], sampleRate)).toThrow(TypeError);
    expect(() => call(new Float64Array(4), sampleRate)).toThrow(TypeError);
  });
});

describe('request-only normaliser', () => {
  const call = (...args: unknown[]): unknown => requestOnly('detectKey', args);

  it('returns the one request object', () => {
    const request = { nFft: 512 };
    expect(call(request)).toBe(request);
  });

  it('refuses anything but a plain object, an extra argument and an unknown key', () => {
    expect(() => call(new Float32Array(4))).toThrow(TypeError);
    expect(() => call(null)).toThrow(TypeError);
    expect(() => call({}, 1)).toThrow(TypeError);
    expect(() => call({ bogus: 1 })).toThrow(/bogus/);
  });
});
