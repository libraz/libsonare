import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import { describe, expect, it } from 'vitest';
import { Audio, decodeChannels, downmix } from '../src/index.js';

const SAMPLE_RATE = 22050;
const FRAMES = 64;

/** Frame-interleaved 16-bit PCM WAV whose channel `c` holds `values[c]` scaled per frame. */
function pcm16Wav(channels: number): Buffer {
  const dataSize = FRAMES * channels * 2;
  const wav = Buffer.alloc(44 + dataSize);
  wav.write('RIFF', 0, 4, 'ascii');
  wav.writeUInt32LE(36 + dataSize, 4);
  wav.write('WAVEfmt ', 8, 8, 'ascii');
  wav.writeUInt32LE(16, 16);
  wav.writeUInt16LE(1, 20);
  wav.writeUInt16LE(channels, 22);
  wav.writeUInt32LE(SAMPLE_RATE, 24);
  wav.writeUInt32LE(SAMPLE_RATE * channels * 2, 28);
  wav.writeUInt16LE(channels * 2, 32);
  wav.writeUInt16LE(16, 34);
  wav.write('data', 36, 4, 'ascii');
  wav.writeUInt32LE(dataSize, 40);
  for (let frame = 0; frame < FRAMES; frame++) {
    for (let channel = 0; channel < channels; channel++) {
      const value = Math.round(2000 * (channel + 1) * Math.sin(0.1 * frame * (channel + 1)));
      wav.writeInt16LE(value, 44 + (frame * channels + channel) * 2);
    }
  }
  return wav;
}

describe('decodeChannels', () => {
  for (const channelCount of [1, 2, 6]) {
    it(`keeps all ${channelCount} channels and matches the core decoder`, () => {
      const bytes = pcm16Wav(channelCount);
      const decoded = decodeChannels(bytes);
      expect(decoded.sampleRate).toBe(SAMPLE_RATE);
      expect(decoded.channels).toHaveLength(channelCount);

      const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'sonare-decode-channels-'));
      try {
        const file = path.join(dir, 'in.wav');
        fs.writeFileSync(file, bytes);
        expect(Audio.fileChannelCount(file)).toBe(channelCount);
        for (let channel = 0; channel < channelCount; channel++) {
          const plane = decoded.channels[channel];
          expect(plane).toBeInstanceOf(Float32Array);
          expect(plane).toHaveLength(FRAMES);
          expect(Array.from(plane)).toEqual(
            Array.from(Audio.fromFileChannel(file, channel).getData()),
          );
        }
      } finally {
        fs.rmSync(dir, { recursive: true, force: true });
      }
    });
  }

  it('folds to the samples Audio.fromMemory returns', () => {
    for (const channelCount of [2, 6]) {
      const bytes = pcm16Wav(channelCount);
      const mono = downmix(decodeChannels(bytes).channels, 0);
      expect(mono).toHaveLength(1);
      expect(Array.from(mono[0])).toEqual(Array.from(Audio.fromMemory(bytes).getData()));
    }
  });

  it('refuses bytes that are not audio and empty input', () => {
    expect(() => decodeChannels(Buffer.from('garbage'))).toThrow();
    expect(() => decodeChannels(new Uint8Array(0))).toThrow(
      expect.objectContaining({ codeName: 'InvalidParameter' }),
    );
    expect(() => decodeChannels('nope' as unknown as Uint8Array)).toThrow(TypeError);
  });
});

describe('downmix', () => {
  const plane = (value: number) => new Float32Array(8).fill(value);

  it('applies the BS.775 coefficients from 5.1 to stereo and mono', () => {
    const inv = Math.SQRT1_2;
    const surround = [0.1, 0.2, 0.3, 0.4, 0.05, 0.15].map(plane);
    const stereo = downmix(surround, 1);
    expect(stereo).toHaveLength(2);
    const left = 0.1 + inv * 0.3 + inv * 0.05;
    const right = 0.2 + inv * 0.3 + inv * 0.15;
    for (const v of stereo[0]) {
      expect(v).toBeCloseTo(left, 6);
    }
    for (const v of stereo[1]) {
      expect(v).toBeCloseTo(right, 6);
    }
    const mono = downmix(surround, 0);
    for (const v of mono[0]) {
      expect(v).toBeCloseTo(0.5 * (left + right), 6);
    }
  });

  it('copies on an identical layout and folds an unmodelled count by mean', () => {
    const stereo = [plane(0.25), plane(-0.5)];
    expect(downmix(stereo, 1).map((p) => Array.from(p))).toEqual(stereo.map((p) => Array.from(p)));
    const three = [plane(0.3), plane(0.6), plane(0.9)];
    for (const v of downmix(three, 0)[0]) {
      expect(v).toBeCloseTo(0.6, 6);
    }
  });

  it('refuses an upmix, an unknown layout, no channels and ragged channels', () => {
    expect(() => downmix([plane(0.1), plane(0.1)], 2)).toThrow();
    expect(() => downmix([plane(0.1), plane(0.1)], 4 as never)).toThrow();
    expect(() => downmix([], 0)).toThrow();
    expect(() => downmix([plane(0.1), new Float32Array(3)], 0)).toThrow(RangeError);
    expect(() => downmix([[0.1, 0.2]] as unknown as Float32Array[], 0)).toThrow(TypeError);
  });
});
