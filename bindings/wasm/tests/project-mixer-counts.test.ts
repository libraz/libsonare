/**
 * Coverage for the WASM Project/Mixer methods newly surfaced from the C ABI:
 * project counts, tempo/time-signature maps, markers, sample-rate / overlap
 * policy accessors, mixer scene JSON, last-bounce diagnostics, and mixer tail
 * draining.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, Mixer, mixingScenePresetJson, Project } from '../dist/index.js';

describe('Project counts and timeline metadata (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('round-trips entity counts as tracks/sources are added', () => {
    const project = new Project();
    try {
      expect(project.trackCount()).toBe(0);
      project.addTrack({ kind: 'audio', name: 'lead' });
      expect(project.trackCount()).toBe(1);
      project.addTrack({ kind: 'midi', name: 'keys' });
      expect(project.trackCount()).toBe(2);
      expect(typeof project.sourceCount()).toBe('number');
    } finally {
      project.delete();
    }
  });

  it('sets and counts tempo segments', () => {
    const project = new Project();
    try {
      project.setTempoSegments([
        { startPpq: 0, bpm: 120 },
        { startPpq: 1920, bpm: 140, endBpm: 160 },
      ]);
      expect(project.tempoSegmentCount()).toBe(2);
    } finally {
      project.delete();
    }
  });

  it('sets and counts time-signature segments', () => {
    const project = new Project();
    try {
      project.setTimeSignatures([
        { startPpq: 0, numerator: 4, denominator: 4 },
        { startPpq: 1920, numerator: 6, denominator: 8 },
      ]);
      expect(project.timeSignatureCount()).toBe(2);
    } finally {
      project.delete();
    }
  });

  it('sets a marker and returns its id', () => {
    const project = new Project();
    try {
      const id = project.setMarker(0, 480, 'intro');
      expect(typeof id).toBe('number');
      expect(id).toBeGreaterThan(0);
    } finally {
      project.delete();
    }
  });

  it('reads sample rate and round-trips the overlap policy', () => {
    const project = new Project();
    try {
      project.setSampleRate(44100);
      expect(project.getSampleRate()).toBe(44100);
      project.setOverlapPolicy(1);
      expect(project.getOverlapPolicy()).toBe(1);
    } finally {
      project.delete();
    }
  });

  it('accepts a mixer scene JSON without throwing', () => {
    const project = new Project();
    try {
      const sceneJson = mixingScenePresetJson('vocalReverbSend');
      expect(() => project.setMixerSceneJson(sceneJson)).not.toThrow();
    } finally {
      project.delete();
    }
  });

  it('returns a compile-result object from lastBounceCompileResult', () => {
    const project = new Project();
    try {
      const result = project.lastBounceCompileResult();
      expect(result).toBeTypeOf('object');
      expect(result).toHaveProperty('hasTimeline');
      expect(typeof result.hasTimeline).toBe('boolean');
    } finally {
      project.delete();
    }
  });
});

describe('Mixer tail draining (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('reports tailSamples as a number', () => {
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 48000, 512);
    try {
      mixer.compile();
      const tail = mixer.tailSamples();
      expect(typeof tail).toBe('number');
      expect(tail).toBeGreaterThanOrEqual(0);
    } finally {
      mixer.delete();
    }
  });

  it('drainTailStereo returns stereo buffers of the requested length', () => {
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 48000, 512);
    try {
      mixer.compile();
      const result = mixer.drainTailStereo(256);
      expect(result.left).toBeInstanceOf(Float32Array);
      expect(result.right).toBeInstanceOf(Float32Array);
      expect(result.left.length).toBe(256);
      expect(result.right.length).toBe(256);
      expect(result.sampleRate).toBe(48000);
    } finally {
      mixer.delete();
    }
  });
});
