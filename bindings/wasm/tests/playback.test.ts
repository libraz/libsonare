import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  capabilities,
  HrtfSet,
  init,
  PlaybackLoudnessMeter,
  PlaybackRenderer,
  type PlaybackRendererConfig,
  renderPlayback,
  SonareError,
} from '../dist/index.js';

const SR = 48000;
const packageRoot = new URL('../', import.meta.url);
const defaultShrf = new URL('dist/hrtf/default.shrf', packageRoot);

const SPEAKERS_5_1: PlaybackRendererConfig = {
  input: { layout: '5.1' },
  target: { kind: 'speakers', layout: '5.1' },
};

function hrtfBytes(): Uint8Array {
  return new Uint8Array(readFileSync(defaultShrf));
}

/** Deterministic uniform noise, one plane per channel. */
function noisePlanes(channels: number, frames: number, seed = 1): Float32Array[] {
  let state = seed >>> 0;
  return Array.from({ length: channels }, () => {
    const plane = new Float32Array(frames);
    for (let i = 0; i < frames; i++) {
      state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
      plane[i] = 0.1 * (state / 2 ** 31 - 1);
    }
    return plane;
  });
}

function interleave(planes: Float32Array[]): Float32Array {
  const frames = planes[0].length;
  const out = new Float32Array(frames * planes.length);
  for (let i = 0; i < frames; i++) {
    for (let ch = 0; ch < planes.length; ch++) {
      out[i * planes.length + ch] = planes[ch][i];
    }
  }
  return out;
}

/** First index whose magnitude reaches the peak's -40 dB. */
function onset(signal: Float32Array): number {
  let peak = 0;
  for (const v of signal) {
    peak = Math.max(peak, Math.abs(v));
  }
  const threshold = peak * 0.01;
  return signal.findIndex((v) => Math.abs(v) >= threshold);
}

describe('playback renderer (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('reports the playback feature', () => {
    expect(capabilities().features.playback).toBe(true);
  });

  it('ships the default HRTF set and the config schema as package exports', () => {
    const pkg = JSON.parse(readFileSync(new URL('package.json', packageRoot), 'utf8'));
    expect(pkg.exports['./hrtf/default.shrf']).toBe('./dist/hrtf/default.shrf');
    expect(pkg.exports['./schemas/playback-renderer-config.schema.json']).toBe(
      './dist/schemas/playback-renderer-config.schema.json',
    );
    const source = readFileSync(new URL('../../src/playback/default.shrf', packageRoot));
    expect(Buffer.compare(readFileSync(defaultShrf), source)).toBe(0);
    const schema = new URL('dist/schemas/playback-renderer-config.schema.json', packageRoot);
    expect(JSON.parse(readFileSync(schema, 'utf8'))).toBeTypeOf('object');
  });

  it('builds an HRTF set from bytes and refuses malformed data', () => {
    const hrtf = HrtfSet.fromBytes(hrtfBytes());
    hrtf.delete();
    let caught: unknown;
    try {
      HrtfSet.fromBytes(new Uint8Array([1, 2, 3, 4]));
    } catch (error) {
      caught = error;
    }
    expect(caught).toBeInstanceOf(SonareError);
    expect((caught as SonareError).codeName).toBe('InvalidParameter');
  });

  it('refuses a headphones target without an HRTF set by name', () => {
    expect(() => new PlaybackRenderer({ config: {} })).toThrow(/hrtf required/);
    expect(() =>
      renderPlayback({ samples: new Float32Array(256), channels: 2, sampleRate: SR, config: {} }),
    ).toThrow(/hrtf required/);
  });

  it('reports the target latency and channel counts', () => {
    const hrtf = HrtfSet.fromBytes(hrtfBytes());
    const headphones = new PlaybackRenderer({ config: {}, hrtf, sampleRate: SR });
    hrtf.delete();
    try {
      expect(headphones.latencySamples()).toBe(1312);
      expect(headphones.outputChannels()).toBe(2);
      expect(headphones.inputChannels()).toBe(2);
    } finally {
      headphones.delete();
    }
    const cases: Array<['stereo' | '5.1' | '7.1', number, number]> = [
      ['stereo', 2, 288],
      ['5.1', 6, 1312],
      ['7.1', 8, 1312],
    ];
    for (const [layout, channels, latency] of cases) {
      const renderer = new PlaybackRenderer({
        config: { target: { kind: 'speakers', layout } },
        sampleRate: SR,
      });
      try {
        expect(renderer.outputChannels()).toBe(channels);
        expect(renderer.latencySamples()).toBe(latency);
      } finally {
        renderer.delete();
      }
    }
  });

  it('delays an impulse by exactly the reported latency', () => {
    const renderer = new PlaybackRenderer({
      config: SPEAKERS_5_1,
      sampleRate: SR,
      maxBlockSize: 1024,
    });
    try {
      const at = 100;
      const center: number[] = [];
      for (let block = 0; block < 3; block++) {
        const planes = Array.from({ length: 6 }, () => new Float32Array(1024));
        if (block === 0) {
          planes[2][at] = 0.5;
        }
        const out = renderer.processPlanar(planes);
        expect(out).toHaveLength(6);
        center.push(...out[2]);
      }
      expect(
        Math.abs(onset(Float32Array.from(center)) - (at + renderer.latencySamples())),
      ).toBeLessThanOrEqual(1);
    } finally {
      renderer.delete();
    }
  });

  it('renders interleaved and planar blocks identically', () => {
    const hrtf = HrtfSet.fromBytes(hrtfBytes());
    const planar = new PlaybackRenderer({ config: {}, hrtf, sampleRate: SR, maxBlockSize: 256 });
    const interleaved = new PlaybackRenderer({
      config: {},
      hrtf,
      sampleRate: SR,
      maxBlockSize: 256,
    });
    hrtf.delete();
    try {
      for (let block = 0; block < 12; block++) {
        const planes = noisePlanes(2, 256, block + 1);
        const a = planar.processPlanar(planes);
        const b = interleaved.processInterleaved(interleave(planes), 2);
        expect(b).toEqual(interleave(a));
        expect(a.every((plane) => plane.every(Number.isFinite))).toBe(true);
      }
    } finally {
      planar.delete();
      interleaved.delete();
    }
  });

  it('rejects a structurally invalid block while still counting non-finite samples it accepts', () => {
    const renderer = new PlaybackRenderer({
      config: SPEAKERS_5_1,
      sampleRate: SR,
      maxBlockSize: 128,
    });
    try {
      // A non-finite sample is the renderer's own content policy (replace and
      // count via the C ABI), not a structural error the WASM binding should
      // front-run: it must be accepted on both the planar and interleaved
      // paths, exactly like the C ABI, Node and Python.
      const bad = noisePlanes(6, 128);
      bad[1][5] = Number.NaN;
      const planarOut = renderer.processPlanar(bad);
      expect(planarOut.every((plane) => plane.every(Number.isFinite))).toBe(true);
      const afterPlanar = renderer.nonFiniteDiscardCount();
      expect(afterPlanar).toBeGreaterThan(0);

      expect(() => renderer.processPlanar(noisePlanes(2, 128))).toThrow(
        /rejected 2 input channels/,
      );
      expect(() => renderer.processPlanar(noisePlanes(6, 129))).toThrow(/maxBlockSize/);

      const samples = interleave(noisePlanes(6, 128));
      samples[7] = Number.POSITIVE_INFINITY;
      renderer.processInterleaved(samples, 6);
      expect(renderer.nonFiniteDiscardCount()).toBeGreaterThan(afterPlanar);
    } finally {
      renderer.delete();
    }
  });

  it('treats a 0-frame planar block as a no-op and refuses a 0-frame offline render', () => {
    const renderer = new PlaybackRenderer({
      config: SPEAKERS_5_1,
      sampleRate: SR,
      maxBlockSize: 128,
    });
    try {
      const empty = Array.from({ length: 6 }, () => new Float32Array(0));
      const out = renderer.processPlanar(empty);
      expect(out).toHaveLength(6);
      expect(out.every((plane) => plane.length === 0)).toBe(true);
    } finally {
      renderer.delete();
    }
    expect(() =>
      renderPlayback({
        samples: new Float32Array(0),
        channels: 6,
        sampleRate: SR,
        config: SPEAKERS_5_1,
      }),
    ).toThrow(/must not be empty/);
  });

  it('refuses non-finite samples in an offline render, unlike the block path', () => {
    const planes = noisePlanes(6, 512);
    planes[3][100] = Number.NaN;
    let caught: unknown;
    try {
      renderPlayback({
        samples: interleave(planes),
        channels: 6,
        sampleRate: SR,
        config: SPEAKERS_5_1,
      });
    } catch (error) {
      caught = error;
    }
    expect(caught).toBeInstanceOf(RangeError);
  });

  it('follows the input channel count under "auto" and reports it as plain data', () => {
    const renderer = new PlaybackRenderer({
      config: { target: { kind: 'speakers', layout: '5.1' } },
      sampleRate: SR,
      maxBlockSize: 128,
    });
    try {
      renderer.processPlanar(noisePlanes(2, 128));
      renderer.processPlanar(noisePlanes(6, 128));
      expect(renderer.inputChannels()).toBe(6);
      renderer.processPlanar(noisePlanes(8, 128));
      expect(renderer.inputChannels()).toBe(8);
      expect(() => renderer.processPlanar(noisePlanes(3, 128))).toThrow(SonareError);
      const diagnostics = renderer.diagnostics();
      expect(Object.getPrototypeOf(diagnostics)).toBe(Object.prototype);
      expect(diagnostics.layout_switches).toBe(2);
      expect(diagnostics.active_input_layout).toBe('7.1');
      expect(diagnostics.latency.samples).toBe(renderer.latencySamples());
      expect(structuredClone(diagnostics)).toEqual(diagnostics);
    } finally {
      renderer.delete();
    }
  });

  it('applies realtime keys and refuses a changed prepare key', () => {
    const renderer = new PlaybackRenderer({ config: SPEAKERS_5_1, sampleRate: SR });
    try {
      const config = renderer.config();
      expect(config.target?.kind).toBe('speakers');
      expect(config.target?.layout).toBe('5.1');
      renderer.setConfig({ ...config, night_mode: { amount: 1 } });
      expect(renderer.config().night_mode?.amount).toBe(1);
      renderer.setConfig(JSON.stringify({ ...config, dialogue_level_db: 3 }));
      expect(() =>
        renderer.setConfig({ ...config, target: { ...config.target, layout: '7.1' } }),
      ).toThrow(/requires a new renderer/);
    } finally {
      renderer.delete();
    }
  });

  it('turns the binaural image with the head orientation', () => {
    const hrtf = HrtfSet.fromBytes(hrtfBytes());
    const still = new PlaybackRenderer({ config: {}, hrtf, sampleRate: SR, maxBlockSize: 512 });
    const turned = new PlaybackRenderer({ config: {}, hrtf, sampleRate: SR, maxBlockSize: 512 });
    hrtf.delete();
    try {
      turned.setHeadOrientation(90);
      turned.setHeadOrientation(Number.NaN, 0, 0);
      let differs = false;
      for (let block = 0; block < 6; block++) {
        const planes = noisePlanes(2, 512, block + 7);
        const a = still.processPlanar(planes);
        const b = turned.processPlanar(planes);
        differs ||= a[0].some((v, i) => v !== b[0][i]);
      }
      expect(differs).toBe(true);
    } finally {
      still.delete();
      turned.delete();
    }
  });

  it('renders a whole buffer aligned with its input', () => {
    const frames = 4000;
    const planes = Array.from({ length: 6 }, () => new Float32Array(frames));
    const at = 1234;
    planes[2][at] = 0.5;
    const result = renderPlayback({
      samples: interleave(planes),
      channels: 6,
      sampleRate: SR,
      config: SPEAKERS_5_1,
    });
    expect(result.channels).toBe(6);
    expect(result.samples).toHaveLength(frames * 6);
    const center = new Float32Array(frames);
    for (let i = 0; i < frames; i++) {
      center[i] = result.samples[i * 6 + 2];
    }
    expect(Math.abs(onset(center) - at)).toBeLessThanOrEqual(1);

    const hrtf = HrtfSet.fromBytes(hrtfBytes());
    try {
      const binaural = renderPlayback({
        samples: interleave(noisePlanes(2, 2048)),
        channels: 2,
        sampleRate: SR,
        config: {},
        hrtf,
      });
      expect(binaural.channels).toBe(2);
      expect(binaural.samples).toHaveLength(2048 * 2);
      expect(binaural.samples.every(Number.isFinite)).toBe(true);
    } finally {
      hrtf.delete();
    }
  });

  it('measures integrated program loudness', () => {
    const meter = new PlaybackLoudnessMeter(2, SR);
    try {
      const planes = Array.from({ length: 2 }, () => {
        const plane = new Float32Array(SR);
        for (let i = 0; i < plane.length; i++) {
          plane[i] = 0.1 * Math.sin((2 * Math.PI * 1000 * i) / SR);
        }
        return plane;
      });
      meter.pushInterleaved(interleave(planes));
      const lufs = meter.integratedLufs();
      expect(Number.isFinite(lufs)).toBe(true);
      expect(lufs).toBeLessThan(0);
      expect(() => meter.pushInterleaved(new Float32Array(3))).toThrow(/whole number of frames/);
    } finally {
      meter.delete();
    }
    expect(() => new PlaybackLoudnessMeter(3, SR)).toThrow(SonareError);
  });

  it('releases every handle through delete() or destroy()', () => {
    for (const release of ['delete', 'destroy'] as const) {
      const handles = [
        new PlaybackRenderer({ config: SPEAKERS_5_1, sampleRate: SR }),
        new PlaybackLoudnessMeter(6, SR),
        HrtfSet.fromBytes(hrtfBytes()),
      ];
      for (const handle of handles) {
        expect('dispose' in handle).toBe(false);
        handle[release]();
      }
    }
  });
});
