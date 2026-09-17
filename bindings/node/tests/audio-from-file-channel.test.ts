import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import { describe, expect, it } from 'vitest';
import { Audio } from '../src/index.js';
import { sine } from './_helpers.js';

const SR = 22050;

/**
 * Write the given planes as one interleaved 16-bit PCM WAV.
 *
 * The fixture is written by the test rather than committed, so the expected
 * plane is the array this was handed: a decoded channel is compared against its
 * own source samples, not against the other channel.
 */
function writePcm16Wav(filePath: string, planes: Float32Array[], sampleRate: number): void {
  const channels = planes.length;
  const frameCount = planes[0].length;
  const bytesPerSample = 2;
  const dataSize = frameCount * channels * bytesPerSample;
  const wav = Buffer.alloc(44 + dataSize);
  wav.write('RIFF', 0, 4, 'ascii');
  wav.writeUInt32LE(36 + dataSize, 4);
  wav.write('WAVE', 8, 4, 'ascii');
  wav.write('fmt ', 12, 4, 'ascii');
  wav.writeUInt32LE(16, 16);
  wav.writeUInt16LE(1, 20);
  wav.writeUInt16LE(channels, 22);
  wav.writeUInt32LE(sampleRate, 24);
  wav.writeUInt32LE(sampleRate * channels * bytesPerSample, 28);
  wav.writeUInt16LE(channels * bytesPerSample, 32);
  wav.writeUInt16LE(16, 34);
  wav.write('data', 36, 4, 'ascii');
  wav.writeUInt32LE(dataSize, 40);
  for (let frame = 0; frame < frameCount; frame++) {
    for (let channel = 0; channel < channels; channel++) {
      const offset = 44 + (frame * channels + channel) * bytesPerSample;
      const quantized = Math.max(
        -32768,
        Math.min(32767, Math.round(planes[channel][frame] * 32767)),
      );
      wav.writeInt16LE(quantized, offset);
    }
  }
  fs.writeFileSync(filePath, wav);
}

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

describe('Audio.fromFileChannel', () => {
  const left = sine(440, 0.1, { sampleRate: SR, amp: 0.6 });
  const right = sine(110, 0.1, { sampleRate: SR, amp: 0.3 });

  it('loads each plane of a stereo file unfolded', () => {
    withStereoFixture(({ stereo }) => {
      writePcm16Wav(stereo, [left, right], SR);
      expect(Audio.fileChannelCount(stereo)).toBe(2);

      const planes = [0, 1].map((index) => Audio.fromFileChannel(stereo, index));
      const downmixed = Audio.fromFile(stereo);
      try {
        const decoded = planes.map((audio) => audio.getData());
        const mixed = downmixed.getData();
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

  it('refuses a channel index the file has no channel for', () => {
    withStereoFixture(({ stereo, mono }) => {
      writePcm16Wav(stereo, [left, right], SR);
      writePcm16Wav(mono, [left], SR);

      const outOfRange = expect.objectContaining({
        name: 'SonareError',
        codeName: 'InvalidParameter',
      });
      expect(() => Audio.fromFileChannel(stereo, 2)).toThrow(outOfRange);
      expect(() => Audio.fromFileChannel(stereo, -1)).toThrow(outOfRange);
      expect(() => Audio.fromFileChannel(mono, 1)).toThrow(outOfRange);

      // The refusals above are the file's channel count talking, not a blanket
      // rejection: the channels the file does have still load.
      const monoPlane = Audio.fromFileChannel(mono, 0);
      try {
        expect(maxAbsDiff(monoPlane.getData(), left)).toBeLessThan(QUANTIZATION_TOLERANCE);
      } finally {
        monoPlane.destroy();
      }
    });
  });

  it('refuses a wrong-typed path and channel index', () => {
    withStereoFixture(({ stereo }) => {
      writePcm16Wav(stereo, [left, right], SR);
      expect(() => Audio.fromFileChannel(123 as unknown as string, 0)).toThrow(TypeError);
      expect(() => Audio.fromFileChannel(stereo, '0' as unknown as number)).toThrow(TypeError);
      expect(() => Audio.fromFileChannel(stereo, undefined as unknown as number)).toThrow(
        TypeError,
      );
      expect(() => Audio.fromFileChannel(stereo, 0.5)).toThrow(RangeError);
    });
  });
});
