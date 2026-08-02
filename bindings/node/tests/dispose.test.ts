import { describe, expect, it } from 'vitest';
import { Audio, Project } from '../src/index.js';
import { sine as sineWave } from './_helpers.js';

function sine(freq: number, sr: number, seconds: number): Float32Array {
  return sineWave(freq, seconds, { sampleRate: sr, amp: 0.5 });
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

  it('does not expose mutable cached PCM and rejects use after destroy', () => {
    const audio = Audio.fromBuffer(new Float32Array([0.5, -0.25]), 48000);
    const exposed = audio.getData();
    exposed[0] = 0;
    expect(audio.getData()[0]).toBe(0.5);

    audio.destroy();
    expect(() => audio.getData()).toThrow('Audio has been destroyed');
    expect(() => audio.getLength()).toThrow('Audio has been destroyed');
    expect(() => audio.detectBpm()).toThrow('Audio has been destroyed');
  });

  it('Project supports `using`', () => {
    expect(() => {
      using project = Project.create();
      expect(project).toBeDefined();
    }).not.toThrow();
  });
});
