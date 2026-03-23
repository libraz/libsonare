import { describe, expect, it } from 'vitest';
import { Audio, analyze, detectBpm, detectKey, version } from '../src/index.js';

describe('sonare native binding', () => {
  it('version returns a string', () => {
    const v = version();
    expect(typeof v).toBe('string');
    expect(v.length).toBeGreaterThan(0);
  });

  it('detectBpm with silence returns a number', () => {
    const silence = new Float32Array(22050); // 1 second of silence
    const bpm = detectBpm(silence, 22050);
    expect(typeof bpm).toBe('number');
    expect(bpm).toBeGreaterThanOrEqual(0);
  });

  it('detectKey with silence returns key object', () => {
    const silence = new Float32Array(22050);
    const key = detectKey(silence, 22050);
    expect(key).toHaveProperty('root');
    expect(key).toHaveProperty('mode');
    expect(key).toHaveProperty('confidence');
    expect(typeof key.root).toBe('string');
    expect(typeof key.mode).toBe('string');
    expect(typeof key.confidence).toBe('number');
  });

  it('analyze with silence returns full result', () => {
    const silence = new Float32Array(22050);
    const result = analyze(silence, 22050);
    expect(result).toHaveProperty('bpm');
    expect(result).toHaveProperty('bpmConfidence');
    expect(result).toHaveProperty('key');
    expect(result).toHaveProperty('timeSignature');
    expect(result).toHaveProperty('beatTimes');
    expect(typeof result.bpm).toBe('number');
    expect(result.key).toHaveProperty('root');
    expect(result.key).toHaveProperty('mode');
    expect(result.timeSignature).toHaveProperty('numerator');
    expect(result.timeSignature).toHaveProperty('denominator');
    expect(result.beatTimes).toBeInstanceOf(Float32Array);
  });

  it('Audio.fromBuffer creates audio object', () => {
    const samples = new Float32Array(22050);
    const audio = Audio.fromBuffer(samples, 22050);
    expect(audio.getLength()).toBe(22050);
    expect(audio.getSampleRate()).toBe(22050);
    expect(audio.getDuration()).toBeCloseTo(1.0, 2);

    const data = audio.getData();
    expect(data).toBeInstanceOf(Float32Array);
    expect(data.length).toBe(22050);

    audio.destroy();
  });
});
