/**
 * decodeChannels and downmix: channel-preserving decode and the BS.775 fold.
 */

import { beforeAll, describe, expect, it, vi } from 'vitest';
import { Audio, decodeChannels, downmix, init } from '../dist/index.js';

const SAMPLE_RATE = 22050;
const FRAMES = 64;

/** The 16-bit value written for `channel` at `frame`. */
function pcmValue(frame: number, channel: number): number {
  return Math.round(2000 * (channel + 1) * Math.sin(0.1 * frame * (channel + 1)));
}

function pcm16Wav(channels: number): Uint8Array {
  const dataBytes = FRAMES * channels * 2;
  const buffer = new ArrayBuffer(44 + dataBytes);
  const view = new DataView(buffer);
  const tag = (offset: number, text: string) => {
    for (let i = 0; i < text.length; ++i) {
      view.setUint8(offset + i, text.charCodeAt(i));
    }
  };
  tag(0, 'RIFF');
  view.setUint32(4, 36 + dataBytes, true);
  tag(8, 'WAVEfmt ');
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, channels, true);
  view.setUint32(24, SAMPLE_RATE, true);
  view.setUint32(28, SAMPLE_RATE * channels * 2, true);
  view.setUint16(32, channels * 2, true);
  view.setUint16(34, 16, true);
  tag(36, 'data');
  view.setUint32(40, dataBytes, true);
  for (let frame = 0; frame < FRAMES; ++frame) {
    for (let channel = 0; channel < channels; ++channel) {
      view.setInt16(44 + (frame * channels + channel) * 2, pcmValue(frame, channel), true);
    }
  }
  return new Uint8Array(buffer);
}

function fakeAudioBuffer(channels: Float32Array[], sampleRate: number): AudioBuffer {
  return {
    length: channels[0]?.length ?? 0,
    numberOfChannels: channels.length,
    sampleRate,
    getChannelData: (channel: number) => channels[channel],
  } as AudioBuffer;
}

describe('decodeChannels', () => {
  beforeAll(async () => {
    await init();
  });

  for (const channelCount of [1, 2, 6]) {
    it(`keeps all ${channelCount} channels and their samples`, () => {
      const decoded = decodeChannels(pcm16Wav(channelCount));
      expect(decoded.sampleRate).toBe(SAMPLE_RATE);
      expect(decoded.channels).toHaveLength(channelCount);
      for (let channel = 0; channel < channelCount; ++channel) {
        const plane = decoded.channels[channel];
        expect(plane).toBeInstanceOf(Float32Array);
        expect(plane).toHaveLength(FRAMES);
        for (let frame = 0; frame < FRAMES; ++frame) {
          expect(plane[frame]).toBe(pcmValue(frame, channel) / 32768);
        }
      }
    });
  }

  it('folds to the samples Audio.fromMemory returns', () => {
    for (const channelCount of [1, 2, 6]) {
      const bytes = pcm16Wav(channelCount);
      const mono = downmix(decodeChannels(bytes).channels, 0);
      expect(mono).toHaveLength(1);
      expect(Array.from(mono[0])).toEqual(Array.from(Audio.fromMemory(bytes).data));
    }
  });

  it('refuses bytes that are not audio and empty input', () => {
    expect(() => decodeChannels(new Uint8Array([1, 2, 3, 4]))).toThrow();
    expect(() => decodeChannels(new Uint8Array(0))).toThrow();
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
    for (const v of downmix(surround, 0)[0]) {
      expect(v).toBeCloseTo(0.5 * (left + right), 6);
    }
  });

  it('copies on an identical layout and folds an unmodelled count by mean', () => {
    const stereo = [plane(0.25), plane(-0.5)];
    expect(downmix(stereo, 1).map((p) => Array.from(p))).toEqual(stereo.map((p) => Array.from(p)));
    for (const v of downmix([plane(0.3), plane(0.6), plane(0.9)], 0)[0]) {
      expect(v).toBeCloseTo(0.6, 6);
    }
  });

  it('refuses an upmix, an unknown layout, no channels and ragged channels', () => {
    expect(() => downmix([plane(0.1), plane(0.1)], 2)).toThrow();
    expect(() => downmix([plane(0.1), plane(0.1)], 4 as never)).toThrow();
    expect(() => downmix([], 0)).toThrow();
    expect(() => downmix([plane(0.1), new Float32Array(3)], 0)).toThrow();
  });

  it('refuses a layout that would wrap into a valid one rather than folding to it', () => {
    // 2^32 + 1 wraps to 1 (stereo) under embind's int conversion.
    expect(() => downmix([plane(0.1), plane(0.1)], (2 ** 32 + 1) as never)).toThrow(RangeError);
    expect(() => downmix([plane(0.1), plane(0.1)], 1.5 as never)).toThrow(RangeError);
    expect(() => downmix([plane(0.1), plane(0.1)], '1' as never)).toThrow(TypeError);
  });
});

describe('browser decoder fallback fold', () => {
  beforeAll(async () => {
    await init();
  });

  it('folds a surround AudioBuffer exactly as downmix does, not as a plain average', async () => {
    const planes = [0.1, 0.2, 0.3, 0.4, 0.05, 0.15].map((v) => new Float32Array(4).fill(v));
    const decodeAudioData = vi.fn(async () => fakeAudioBuffer(planes, 48000));
    const audio = await Audio.fromMemoryWithBrowserFallback(new Uint8Array([1, 2, 3, 4, 5]), {
      audioContext: { decodeAudioData, sampleRate: 48000 },
    });
    expect(decodeAudioData).toHaveBeenCalledTimes(1);
    expect(Array.from(audio.data)).toEqual(Array.from(downmix(planes, 0)[0]));
    const plainAverage = (0.1 + 0.2 + 0.3 + 0.4 + 0.05 + 0.15) / 6;
    expect(Math.abs(audio.data[0] - plainAverage)).toBeGreaterThan(0.01);
  });

  it('matches the native decoder for the same stereo samples', async () => {
    const bytes = pcm16Wav(2);
    const native = Audio.fromMemory(bytes).data;
    const { channels } = decodeChannels(bytes);
    const audio = await Audio.fromMemoryWithBrowserFallback(new Uint8Array([9, 9, 9]), {
      audioContext: {
        decodeAudioData: async () => fakeAudioBuffer(channels, SAMPLE_RATE),
        sampleRate: SAMPLE_RATE,
      },
    });
    expect(Array.from(audio.data)).toEqual(Array.from(native));
  });
});
