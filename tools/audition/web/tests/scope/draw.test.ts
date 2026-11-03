import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Item } from '../../src/api/schemas';
import { bufferOf } from '../../src/player/engine';
import { clearScopeCache } from '../../src/scope/compute';
import { drawSpec, drawWave, Layer, roleRgb, secondsAt } from '../../src/scope/draw';
import { getState, setState } from '../../src/state/store';
import { loadTake, pcm, setRoleColours, sine, sizeCanvas, stubCanvas } from './fakes';

vi.mock('../../src/player/engine');

const PLAYHEAD = 'rgba(248,246,242,0.92)';
const SHADE = 'rgba(16,18,22,0.58)';

let recorder: ReturnType<typeof stubCanvas>;
let wave: HTMLCanvasElement;

beforeEach(() => {
  clearScopeCache();
  recorder = stubCanvas();
  setRoleColours();
  wave = document.createElement('canvas');
  sizeCanvas(wave, 100, 40);
  vi.mocked(bufferOf).mockImplementation(
    (key: string) =>
      pcm(
        [sine(key === 'm' ? 50 : 120, 1, 8000, key === 'm' ? 0.8 : 0.3)],
        8000,
      ) as unknown as AudioBuffer,
  );
});

afterEach(() => {
  document.documentElement.removeAttribute('style');
});

describe('roleRgb', () => {
  it('reads the stylesheet custom property', () => {
    expect(roleRgb('model')).toEqual([94, 199, 168]);
    expect(roleRgb('reference')).toEqual([226, 163, 63]);
  });

  it('falls back for a role the stylesheet lacks, and for none', () => {
    expect(roleRgb('nothing')).toEqual([128, 136, 150]);
    expect(roleRgb('')).toEqual([133, 141, 155]);
  });
});

describe('secondsAt', () => {
  it('maps a pointer to the take and clamps at both ends', () => {
    const cv = document.createElement('canvas');
    sizeCanvas(cv, 200, 40, 50);
    expect(secondsAt(cv, 150, 10)).toBe(5);
    expect(secondsAt(cv, 0, 10)).toBe(0);
    expect(secondsAt(cv, 9999, 10)).toBe(10);
  });
});

describe('drawWave', () => {
  it('clears and draws nothing without a take', () => {
    drawWave(wave, new Layer(), 0);
    expect(recorder(wave).of('clearRect')).toHaveLength(1);
    expect(recorder(wave).of('drawImage')).toHaveLength(0);
  });

  it('sizes the backing store to the layout', () => {
    loadTake();
    drawWave(wave, new Layer(), 0);
    expect([wave.width, wave.height]).toEqual([100, 40]);
  });

  it('draws each trace in its role colour, the sounding one solid', () => {
    loadTake({ versionIndex: 0 });
    const layer = new Layer();
    drawWave(wave, layer, 0);
    const fills = new Set(
      recorder(layer.cv as HTMLCanvasElement)
        .of('fillRect')
        .map((c) => c.fill),
    );
    expect(fills).toContain('rgba(94,199,168,0.95)');
    expect(fills).toContain('rgba(226,163,63,0.22)');
  });

  it('swaps solid and dim when the other version sounds', () => {
    loadTake({ versionIndex: 1 });
    const layer = new Layer();
    drawWave(wave, layer, 0);
    const fills = new Set(
      recorder(layer.cv as HTMLCanvasElement)
        .of('fillRect')
        .map((c) => c.fill),
    );
    expect(fills).toContain('rgba(226,163,63,0.95)');
    expect(fills).toContain('rgba(94,199,168,0.22)');
  });

  it('draws the sounding version last, over the others', () => {
    loadTake({ versionIndex: 0 });
    const layer = new Layer();
    drawWave(wave, layer, 0);
    const fills = recorder(layer.cv as HTMLCanvasElement)
      .of('fillRect')
      .map((c) => c.fill);
    expect(fills.lastIndexOf('rgba(226,163,63,0.22)')).toBeLessThan(
      fills.indexOf('rgba(94,199,168,0.95)'),
    );
  });

  it('uses no role colour when blind', () => {
    loadTake({ blind: true, blindOrder: [1, 0], versionIndex: 0 });
    const layer = new Layer();
    drawWave(wave, layer, 0);
    const fills = new Set(
      recorder(layer.cv as HTMLCanvasElement)
        .of('fillRect')
        .map((c) => c.fill),
    );
    expect(fills).toEqual(new Set(['rgba(133,141,155,0.95)', 'rgba(133,141,155,0.22)']));
  });

  it('puts the playhead where the time is', () => {
    loadTake();
    drawWave(wave, new Layer(), 2.5);
    const head = recorder(wave)
      .of('moveTo')
      .filter((c) => c.stroke === PLAYHEAD);
    expect(head[0]?.args).toEqual([25, 0]);
    expect(
      recorder(wave)
        .of('lineTo')
        .find((c) => c.stroke === PLAYHEAD)?.args,
    ).toEqual([25, 40]);
  });

  it('moves the playhead without repainting the picture', () => {
    loadTake();
    const layer = new Layer();
    drawWave(wave, layer, 1);
    const cv = layer.cv as HTMLCanvasElement;
    const painted = recorder(cv).calls.length;
    drawWave(wave, layer, 7);
    expect(recorder(cv).calls.length).toBe(painted);
    const x = recorder(wave)
      .of('moveTo')
      .filter((c) => c.stroke === PLAYHEAD)
      .map((c) => c.args[0]);
    expect(x).toContain(10);
    expect(x).toContain(70);
  });

  it('takes back everything outside the marked passage', () => {
    loadTake({ region: [2, 6] });
    drawWave(wave, new Layer(), 0);
    const shade = recorder(wave)
      .of('fillRect')
      .filter((c) => c.fill === SHADE);
    expect(shade.map((c) => c.args)).toEqual([
      [0, 0, 20, 40],
      [60, 0, 40, 40],
    ]);
  });

  it('shades nothing without a region', () => {
    loadTake();
    drawWave(wave, new Layer(), 0);
    expect(
      recorder(wave)
        .of('fillRect')
        .filter((c) => c.fill === SHADE),
    ).toHaveLength(0);
  });

  it('moves the passage without repainting the picture', () => {
    loadTake({ region: [2, 6] });
    const layer = new Layer();
    drawWave(wave, layer, 0);
    const painted = recorder(layer.cv as HTMLCanvasElement).calls.length;
    setState({ region: [1, 3] });
    drawWave(wave, layer, 0);
    expect(recorder(layer.cv as HTMLCanvasElement).calls.length).toBe(painted);
    expect(
      recorder(wave)
        .of('fillRect')
        .filter((c) => c.fill === SHADE)
        .at(-1)?.args,
    ).toEqual([30, 0, 70, 40]);
  });

  it('ticks every strike and labels those that clear the last', () => {
    const notes = [0, 0.01, 0.5, 5].map((start, i) => ({ start, note: 60 + i, velocity: 80 }));
    loadTake({ items: [{ id: 't', tracks: {}, meta: { notes } } as Item] });
    const layer = new Layer();
    drawWave(wave, layer, 0);
    const rec = recorder(layer.cv as HTMLCanvasElement);
    const ticks = rec.of('moveTo').filter((c) => c.stroke === 'rgba(232,226,212,0.30)');
    expect(ticks).toHaveLength(3);
    const labels = rec.of('fillText').map((c) => c.args[0]);
    expect(labels[0]).toBe('#1 60+61');
    expect(labels).not.toContain('#2 62');
    expect(labels).toContain('#3 63');
  });

  describe('cache', () => {
    const paints = (layer: Layer) => recorder(layer.cv as HTMLCanvasElement).of('clearRect').length;

    it('repaints on a new epoch, a new sounding version, blind and a new size', () => {
      loadTake({}, 1);
      const layer = new Layer();
      drawWave(wave, layer, 0);
      drawWave(wave, layer, 1);
      expect(paints(layer)).toBe(1);
      setState({
        take: {
          ...(getState().take as NonNullable<ReturnType<typeof getState>['take']>),
          epoch: 2,
        },
      });
      drawWave(wave, layer, 1);
      expect(paints(layer)).toBe(2);
      setState({ versionIndex: 1 });
      drawWave(wave, layer, 1);
      expect(paints(layer)).toBe(3);
      setState({ blind: true, blindOrder: [0, 1] });
      drawWave(wave, layer, 1);
      expect(paints(layer)).toBe(4);
    });

    it('recomputes peaks only for a new epoch', () => {
      const bufs = { m: pcm([sine(50, 1)]), r: pcm([sine(120, 1)]) };
      const reads = vi.fn();
      for (const b of Object.values(bufs)) {
        const get = b.getChannelData;
        b.getChannelData = (i: number) => {
          reads();
          return get(i);
        };
      }
      vi.mocked(bufferOf).mockImplementation(
        (k: string) => bufs[k as 'm' | 'r'] as unknown as AudioBuffer,
      );
      loadTake({}, 1);
      const layer = new Layer();
      drawWave(wave, layer, 0);
      const first = reads.mock.calls.length;
      expect(first).toBeGreaterThan(0);
      // Repainted for the other version, but both versions' peaks come from the cache.
      setState({ versionIndex: 1 });
      drawWave(wave, layer, 0);
      expect(reads.mock.calls.length).toBe(first);
      setState({
        take: {
          ...(getState().take as NonNullable<ReturnType<typeof getState>['take']>),
          epoch: 2,
        },
      });
      drawWave(wave, layer, 0);
      expect(reads.mock.calls.length).toBe(first * 2);
    });
  });
});

describe('drawSpec', () => {
  const small = () => {
    const cv = document.createElement('canvas');
    sizeCanvas(cv, 24, 80);
    return cv;
  };
  const image = (layer: Layer) => {
    const call = recorder(layer.cv as HTMLCanvasElement).of('putImageData')[0];
    return call?.args[0] as { data: Uint8ClampedArray; width: number; height: number };
  };

  it('is the floor colour everywhere for silence', () => {
    vi.mocked(bufferOf).mockReturnValue(
      pcm([new Float32Array(8000)], 8000) as unknown as AudioBuffer,
    );
    loadTake();
    const cv = small();
    const layer = new Layer();
    drawSpec(cv, layer, 0);
    const { data } = image(layer);
    for (let i = 0; i < data.length; i += 4) {
      expect([data[i], data[i + 1], data[i + 2], data[i + 3]]).toEqual([18, 20, 24, 255]);
    }
  });

  it('lights the row of a sine on the log axis', () => {
    vi.mocked(bufferOf).mockReturnValue(
      pcm([sine(1000, 1, 8000, 1)], 8000) as unknown as AudioBuffer,
    );
    loadTake();
    const cv = small();
    const layer = new Layer();
    drawSpec(cv, layer, 0);
    const { data, width, height } = image(layer);
    const x = 5;
    let best = 0;
    let bestSum = -1;
    for (let y = 0; y < height; y++) {
      const o = (y * width + x) * 4;
      const sum = (data[o] as number) + (data[o + 1] as number) + (data[o + 2] as number);
      if (sum > bestSum) {
        bestSum = sum;
        best = y;
      }
    }
    const lmin = Math.log(40);
    const lmax = Math.log(4000);
    const expected = height - ((Math.log(1000) - lmin) / (lmax - lmin)) * height;
    expect(Math.abs(best - expected)).toBeLessThanOrEqual(2);
    expect(bestSum).toBeGreaterThan(600);
  });

  it('tints the ramp with the sounding version role', () => {
    const loud = pcm([sine(1000, 1, 8000, 0.05)], 8000) as unknown as AudioBuffer;
    vi.mocked(bufferOf).mockReturnValue(loud);
    const channel = (versionIndex: number) => {
      clearScopeCache();
      loadTake({ versionIndex });
      const layer = new Layer();
      drawSpec(small(), layer, 0);
      const { data, width, height } = image(layer);
      let best: [number, number, number] = [0, 0, 0];
      for (let y = 0; y < height; y++) {
        const o = (y * width + 5) * 4;
        const px: [number, number, number] = [data[o] ?? 0, data[o + 1] ?? 0, data[o + 2] ?? 0];
        if (px[0] + px[1] + px[2] > best[0] + best[1] + best[2]) {
          best = px;
        }
      }
      return best;
    };
    const model = channel(0);
    const reference = channel(1);
    // A quiet tone peaks on the role colour's own stop, where teal is green-led and amber red-led.
    expect(model[1]).toBeGreaterThan(model[0]);
    expect(reference[0]).toBeGreaterThan(reference[2]);
  });

  it('labels the decades below Nyquist', () => {
    loadTake();
    const layer = new Layer();
    drawSpec(small(), layer, 0);
    expect(
      recorder(layer.cv as HTMLCanvasElement)
        .of('fillText')
        .map((c) => c.args[0]),
    ).toEqual(['100', '1k']);
  });

  it('draws the playhead and the passage over the cached image', () => {
    loadTake({ region: [5, 10] });
    const cv = small();
    drawSpec(cv, new Layer(), 5);
    const rec = recorder(cv);
    expect(rec.of('drawImage')).toHaveLength(1);
    expect(
      rec
        .of('fillRect')
        .filter((c) => c.fill === SHADE)
        .map((c) => c.args),
    ).toEqual([
      [0, 0, 12, 80],
      [24, 0, 0, 80],
    ]);
    expect(rec.of('moveTo').find((c) => c.stroke === PLAYHEAD)?.args).toEqual([12, 0]);
  });

  it('repaints only when the sounding version, epoch or size changes', () => {
    loadTake({}, 1);
    const cv = small();
    const layer = new Layer();
    drawSpec(cv, layer, 0);
    drawSpec(cv, layer, 3);
    expect(recorder(layer.cv as HTMLCanvasElement).of('putImageData')).toHaveLength(1);
    setState({ versionIndex: 1 });
    drawSpec(cv, layer, 3);
    expect(recorder(layer.cv as HTMLCanvasElement).of('putImageData')).toHaveLength(2);
  });
});
