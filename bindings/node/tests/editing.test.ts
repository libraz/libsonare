import { describe, expect, it } from 'vitest';
import { Audio, noteStretch, pitchCorrectToMidi, voiceChange } from '../src/index.js';

const SR = 22050;

function generateSine(freq: number, sr: number, duration: number): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] = Math.sin((2 * Math.PI * freq * i) / sr);
  }
  return samples;
}

describe('editing effects', () => {
  const tone = generateSine(440, SR, 0.5);

  it('pitchCorrectToMidi returns a non-empty Float32Array', () => {
    // 440 Hz is MIDI 69 (A4); correct toward MIDI 71 (B4).
    const result = pitchCorrectToMidi(tone, SR, 69, 71);
    expect(result).toBeInstanceOf(Float32Array);
    expect(result.length).toBeGreaterThan(0);
  });

  it('noteStretch returns a non-empty Float32Array', () => {
    const result = noteStretch(tone, SR, 0, tone.length, 1.5);
    expect(result).toBeInstanceOf(Float32Array);
    expect(result.length).toBeGreaterThan(0);
  });

  it('voiceChange returns a non-empty Float32Array', () => {
    const result = voiceChange(tone, SR, 2, 1.1);
    expect(result).toBeInstanceOf(Float32Array);
    expect(result.length).toBeGreaterThan(0);
  });

  it('exposes editing methods on the Audio class', () => {
    const audio = Audio.fromBuffer(tone, SR);
    expect(audio.pitchCorrectToMidi(69, 71)).toBeInstanceOf(Float32Array);
    expect(audio.noteStretch(0, audio.getLength(), 1.5)).toBeInstanceOf(Float32Array);
    expect(audio.voiceChange(2, 1.1)).toBeInstanceOf(Float32Array);
    audio.destroy();
  });
});
