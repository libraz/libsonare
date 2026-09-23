/**
 * `decomposeStemsLinked` builds one NMF model from the channels' averaged
 * magnitude and applies the resulting soft mask unchanged to every channel,
 * mirroring `decomposeStems` for the single-channel case.
 */

import { describe, expect, it } from 'vitest';
import { decomposeStems, decomposeStemsLinked } from '../src/index.js';

const SR = 22050;

function toneAt(n: number, freq: number, amp: number, startFrame: number, endFrame: number) {
  const x = new Float32Array(n);
  for (let i = startFrame; i < endFrame; i++) {
    x[i] = amp * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return x;
}

describe('decomposeStemsLinked', () => {
  it('returns k components x c channels of the input length, and components sum back to each channel', () => {
    const n = 8192;
    const left = toneAt(n, 220, 0.5, 0, n);
    const right = toneAt(n, 880, 0.4, 0, n);
    for (let i = 0; i < n; i++) {
      left[i] += 0.5 * Math.sin((2 * Math.PI * 880 * i) / SR);
      right[i] += 0.4 * Math.sin((2 * Math.PI * 220 * i) / SR);
    }
    const result = decomposeStemsLinked({
      channels: [left, right],
      sampleRate: SR,
      nComponents: 2,
      nFft: 1024,
      hopLength: 256,
      nIter: 30,
    });
    expect(result.components.length).toBe(2);
    for (const component of result.components) {
      expect(component.length).toBe(2);
      for (const channel of component) {
        expect(channel.length).toBe(n);
      }
    }
    expect(result.sampleRate).toBe(SR);

    const channels = [left, right];
    for (let c = 0; c < channels.length; c++) {
      let maxAbsErr = 0;
      for (let i = 1024; i < n - 1024; i++) {
        let sum = 0;
        for (const component of result.components) {
          sum += component[c][i];
        }
        maxAbsErr = Math.max(maxAbsErr, Math.abs(sum - channels[c][i]));
      }
      expect(maxAbsErr).toBeLessThan(1e-3);
    }
  });

  it('separates channel-exclusive, time-disjoint tones by 60 dB', () => {
    // Mirrors tests/util/decompose_test.cpp's "separates non-overlapping
    // per-channel tones by 60 dB" fixture: L carries 300 Hz in the first half,
    // R carries 2000 Hz in the second half (disjoint STFT bins at nFft=1024,
    // ~21.5 Hz/bin), gated rather than simultaneous so the averaged-magnitude
    // plane has a real temporal cue to converge on. Energy is measured on the
    // interior of each channel's own active half, away from the STFT edge
    // (nFft samples) and the on/off gate at the midpoint, where the analysis
    // window straddles both halves and shows crossover that is a windowing
    // artefact, not a claim about the mask.
    const n = 8192;
    const half = n / 2;
    const nFft = 1024;
    const left = new Float32Array(n);
    const right = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      const t = i / SR;
      if (i < half) {
        left[i] = 0.5 * Math.sin(2 * Math.PI * 300 * t);
      } else {
        right[i] = 0.5 * Math.sin(2 * Math.PI * 2000 * t);
      }
    }
    const result = decomposeStemsLinked({
      channels: [left, right],
      sampleRate: SR,
      nComponents: 2,
      nFft,
      hopLength: 256,
      nIter: 150,
      init: 'nndsvd',
      maskPower: 2,
    });

    function energyRange(signal: Float32Array, start: number, end: number): number {
      let sum = 0;
      for (let i = start; i < end; i++) {
        sum += signal[i] * signal[i];
      }
      return sum;
    }

    const guard = nFft;
    const lStart = guard;
    const lEnd = half - guard;
    const rStart = half + guard;
    const rEnd = n - guard;

    const lEnergy0 = energyRange(result.components[0][0], lStart, lEnd);
    const lEnergy1 = energyRange(result.components[1][0], lStart, lEnd);
    const lLeader = lEnergy0 >= lEnergy1 ? 0 : 1;
    const rLeader = 1 - lLeader;

    const leaderL = energyRange(result.components[lLeader][0], lStart, lEnd);
    const leaderR = energyRange(result.components[lLeader][1], rStart, rEnd);
    expect(leaderL).toBeGreaterThan(0);
    expect(10 * Math.log10(leaderL / Math.max(leaderR, 1e-30))).toBeGreaterThanOrEqual(60);

    const otherR = energyRange(result.components[rLeader][1], rStart, rEnd);
    const otherL = energyRange(result.components[rLeader][0], lStart, lEnd);
    expect(otherR).toBeGreaterThan(0);
    expect(10 * Math.log10(otherR / Math.max(otherL, 1e-30))).toBeGreaterThanOrEqual(60);
  });

  it('reproduces decomposeStems bit for bit on a single channel', () => {
    const n = 4096;
    const x = toneAt(n, 440, 0.3, 0, n);
    const mono = decomposeStems({ samples: x, sampleRate: SR, nComponents: 2, nIter: 20 });
    const linked = decomposeStemsLinked({
      channels: [x],
      sampleRate: SR,
      nComponents: 2,
      nIter: 20,
    });
    expect(linked.components.length).toBe(mono.components.length);
    for (let k = 0; k < mono.components.length; k++) {
      expect(linked.components[k][0]).toEqual(mono.components[k]);
    }
    expect(linked.w).toEqual(mono.w);
    expect(linked.h).toEqual(mono.h);
  });

  it('rejects an empty channels array', () => {
    expect(() => decomposeStemsLinked({ channels: [], sampleRate: SR })).toThrow();
  });

  it('rejects channels of mismatched length', () => {
    expect(() =>
      decomposeStemsLinked({
        channels: [new Float32Array(4096), new Float32Array(2048)],
        sampleRate: SR,
      }),
    ).toThrow();
  });
});
