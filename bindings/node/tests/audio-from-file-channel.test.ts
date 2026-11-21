import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import { describe, expect, it } from 'vitest';
import { Audio } from '../src/index.js';
import { sine, writePcm16Wav } from './_helpers.js';

const SR = 22050;

/** Largest absolute difference between two equal-length planes. */
function maxAbsDiff(a: Float32Array, b: Float32Array): number {
  let worst = 0;
  for (let i = 0; i < a.length; i++) {
    worst = Math.max(worst, Math.abs(a[i] - b[i]));
  }
  return worst;
}

function rms(samples: Float32Array): number {
  let sum = 0;
  for (let i = 0; i < samples.length; i++) {
    sum += samples[i] * samples[i];
  }
  return Math.sqrt(sum / samples.length);
}

// 16-bit quantization is the only difference a decoded plane may show against
// the samples written for it: one step is 1/32768, and the tolerance also
// absorbs the decoder's choice between a 32767 and a 32768 divisor.
const QUANTIZATION_TOLERANCE = 1e-4;

function withStereoFixture(run: (paths: { stereo: string; mono: string }) => void): void {
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'sonare-from-file-channel-'));
  try {
    run({ stereo: path.join(tmpDir, 'stereo.wav'), mono: path.join(tmpDir, 'mono.wav') });
  } finally {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

describe('Audio.fromFile channelIndex', () => {
  const left = sine(440, 0.1, { sampleRate: SR, amp: 0.6 });
  const right = sine(110, 0.1, { sampleRate: SR, amp: 0.3 });

  it('loads each plane of a stereo file unfolded', () => {
    withStereoFixture(({ stereo }) => {
      writePcm16Wav(stereo, [left, right], SR);
      expect(Audio.fileNumChannels(stereo)).toBe(2);

      const planes = [0, 1].map((index) => Audio.fromFile(stereo, index));
      const downmixed = Audio.fromFile(stereo);
      try {
        const decoded = planes.map((audio) => audio.data);
        const mixed = downmixed.data;
        expect(decoded[0]).toHaveLength(left.length);
        expect(decoded[1]).toHaveLength(right.length);
        expect(mixed).toHaveLength(left.length);

        // Each plane is the channel it was asked for, sample for sample.
        expect(maxAbsDiff(decoded[0], left)).toBeLessThan(QUANTIZATION_TOLERANCE);
        expect(maxAbsDiff(decoded[1], right)).toBeLessThan(QUANTIZATION_TOLERANCE);

        // The two planes differ from each other and from the fold, by margins
        // far above the quantization step: the amplitudes alone are 0.6 and 0.3.
        expect(maxAbsDiff(decoded[0], decoded[1])).toBeGreaterThan(0.3);
        expect(maxAbsDiff(decoded[0], mixed)).toBeGreaterThan(0.1);
        expect(maxAbsDiff(decoded[1], mixed)).toBeGreaterThan(0.1);
        expect(rms(decoded[0])).toBeGreaterThan(rms(decoded[1]) * 1.5);
      } finally {
        for (const audio of planes) {
          audio.destroy();
        }
        downmixed.destroy();
      }
    });
  });

  it('fromFile takes an optional channelIndex', () => {
    withStereoFixture(({ stereo }) => {
      writePcm16Wav(stereo, [left, right], SR);
      const downmixed = Audio.fromFile(stereo);
      const explicitAbsent = Audio.fromFile(stereo, undefined);
      const rightPlane = Audio.fromFile(stereo, 1);
      try {
        // Absent: the downmix, the same as before the parameter existed.
        expect(Array.from(explicitAbsent.data)).toEqual(Array.from(downmixed.data));
        expect(maxAbsDiff(downmixed.data, rightPlane.data)).toBeGreaterThan(0.1);
        // Given: that one channel.
        expect(maxAbsDiff(rightPlane.data, right)).toBeLessThan(QUANTIZATION_TOLERANCE);
      } finally {
        downmixed.destroy();
        explicitAbsent.destroy();
        rightPlane.destroy();
      }
      expect(() => Audio.fromFile(stereo, 2)).toThrow(RangeError);
    });
  });

  it('refuses a channel index the file has no channel for', () => {
    withStereoFixture(({ stereo, mono }) => {
      writePcm16Wav(stereo, [left, right], SR);
      writePcm16Wav(mono, [left], SR);

      // The decoder's refusal, bounded by the channels the file holds.
      expect(() => Audio.fromFile(stereo, 2)).toThrow(RangeError);
      expect(() => Audio.fromFile(mono, 1)).toThrow(RangeError);
      // A negative index is refused before the file is read.
      expect(() => Audio.fromFile(stereo, -1)).toThrow(RangeError);

      // The refusals above are the file's channel count talking, not a blanket
      // rejection: the channels the file does have still load.
      const monoPlane = Audio.fromFile(mono, 0);
      try {
        expect(maxAbsDiff(monoPlane.data, left)).toBeLessThan(QUANTIZATION_TOLERANCE);
      } finally {
        monoPlane.destroy();
      }
    });
  });

  it('refuses a wrong-typed path and channel index', () => {
    withStereoFixture(({ stereo }) => {
      writePcm16Wav(stereo, [left, right], SR);
      expect(() => Audio.fromFile(123 as unknown as string, 0)).toThrow(TypeError);
      expect(() => Audio.fromFile(stereo, '0' as unknown as number)).toThrow(TypeError);
      expect(() => Audio.fromFile(stereo, 0.5)).toThrow(RangeError);
    });
  });
});
