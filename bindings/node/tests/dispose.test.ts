import { describe, expect, it } from 'vitest';
import { Audio, Project } from '../src/index.js';

function sine(freq: number, sr: number, seconds: number): Float32Array {
  const n = Math.floor(sr * seconds);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = 0.5 * Math.sin((2 * Math.PI * freq * i) / sr);
  }
  return out;
}

describe('Symbol.dispose / using', () => {
  it('Audio frees its native handle via `using`', () => {
    let captured: Audio | undefined;
    {
      using audio = Audio.fromBuffer(sine(440, 22050, 0.1), 22050);
      captured = audio;
      expect(audio.getLength()).toBeGreaterThan(0);
    }
    // After the block, dispose ran. destroy() is idempotent, so an explicit
    // second call must not throw.
    expect(() => captured?.destroy()).not.toThrow();
  });

  it('destroy() is idempotent', () => {
    const audio = Audio.fromBuffer(sine(220, 22050, 0.05), 22050);
    audio.destroy();
    expect(() => audio.destroy()).not.toThrow();
  });

  it('Project supports `using`', () => {
    expect(() => {
      using project = Project.create();
      expect(project).toBeDefined();
    }).not.toThrow();
  });
});
