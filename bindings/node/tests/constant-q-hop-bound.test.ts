import { describe, expect, it } from 'vitest';
import { chromaCens, chromaCqt, cqt, cqtToAudio, vqt, vqtToAudio } from '../dist/index.js';

const sampleRate = 22050;
const samples = new Float32Array(sampleRate).fill(0.1);

// Every constant-Q entry, forward and inverse, takes a hop in [1, 65536], the transform's own maximum.
describe('a constant-Q hop is bounded the same way on every entry', () => {
  const magnitude = () => cqt({ samples, sampleRate, nBins: 12 }).magnitude;
  const CALLS: ReadonlyArray<[string, (hopLength: number) => unknown]> = [
    ['cqt', (hopLength) => cqt({ samples, sampleRate, hopLength })],
    ['vqt', (hopLength) => vqt({ samples, sampleRate, hopLength })],
    ['chromaCqt', (hopLength) => chromaCqt({ samples, sampleRate, hopLength })],
    ['chromaCens', (hopLength) => chromaCens({ samples, sampleRate, hopLength })],
    ['cqtToAudio', (hopLength) => cqtToAudio({ magnitude: magnitude(), sampleRate, hopLength })],
    ['vqtToAudio', (hopLength) => vqtToAudio({ magnitude: magnitude(), sampleRate, hopLength })],
  ];
  for (const [name, call] of CALLS) {
    it.each([0, 65537])(`${name} refuses hopLength %i`, (hopLength) => {
      expect(() => call(hopLength)).toThrow(
        new RangeError(`${name}: hopLength must be an integer in [1, 65536]`),
      );
    });
  }
});
