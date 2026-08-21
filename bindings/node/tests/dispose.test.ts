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

  // Facade methods now read the cached snapshot directly instead of taking a
  // full copy per call: a five-minute mono file is 57.6 MB, and every
  // `analyzeBpm()` / `masterAudio()` / `mfcc()` allocated and threw away that
  // much. That is only safe while every native call treats samples as
  // read-only, so assert it over the whole surface rather than per method: the
  // method list is derived from the prototype, so one that starts mutating its
  // input is caught without anyone remembering to extend a list.
  it('no facade method mutates the shared decoded snapshot', () => {
    const source = sine(440, 22050, 0.25);
    const audio = Audio.fromBuffer(source, 22050);
    try {
      const before = Array.from(audio.getData());

      const skip = new Set(['constructor', 'destroy', 'getData']);
      const prototype = Object.getPrototypeOf(audio) as object;
      const called: string[] = [];
      for (const name of Object.getOwnPropertyNames(prototype)) {
        if (skip.has(name)) {
          continue;
        }
        const member = (audio as unknown as Record<string, unknown>)[name];
        if (typeof member !== 'function' || member.length > 0) {
          continue;
        }
        try {
          (member as () => unknown).call(audio);
          called.push(name);
        } catch {
          // A method that rejects this short buffer still cannot have mutated
          // it before rejecting, so the assertion below covers it either way.
        }
      }
      // Guard the derivation itself: a reflection change that finds nothing
      // would make this pass by exercising nothing.
      expect(called.length).toBeGreaterThan(10);

      expect(Array.from(audio.getData())).toEqual(before);
      // The public accessor still hands out a private copy, not the snapshot.
      expect(audio.getData()).not.toBe(audio.getData());
    } finally {
      audio.destroy();
    }
  });

  it('Project supports `using`', () => {
    expect(() => {
      using project = Project.create();
      expect(project).toBeDefined();
    }).not.toThrow();
  });
});
