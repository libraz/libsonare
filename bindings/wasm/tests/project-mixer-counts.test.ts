/**
 * Coverage for the WASM Project/Mixer methods newly surfaced from the C ABI:
 * project counts, tempo/time-signature maps, markers, sample-rate / overlap
 * policy accessors, mixer scene JSON, last-bounce diagnostics, and mixer tail
 * draining.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, Mixer, mixingScenePreset, Project } from '../dist/index.js';

function accelerando(sampleRate = 22050, startBpm = 100, endBpm = 160, seconds = 24): Float32Array {
  const audio = new Float32Array(Math.round(sampleRate * seconds));
  let t = 0;
  while (t < seconds) {
    const at = Math.round(t * sampleRate);
    for (let i = 0; i < 64 && at + i < audio.length; i++) {
      audio[at + i] += Math.exp(-i / 12) * (i % 2 === 0 ? 1 : -1);
    }
    t += 60 / (startBpm + (endBpm - startBpm) * (t / seconds));
  }
  return audio;
}

describe('Project counts and timeline metadata (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('round-trips entity counts as tracks/sources are added', () => {
    const project = new Project();
    try {
      expect(project.tracks().length).toBe(0);
      project.addTrack({ kind: 'audio', name: 'lead' });
      expect(project.tracks().length).toBe(1);
      project.addTrack({ kind: 'midi', name: 'keys' });
      expect(project.tracks().length).toBe(2);
      expect(typeof project.sources().length).toBe('number');
    } finally {
      project.destroy();
    }
  });

  it('reads stored tracks, clips, and sources by index', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio', name: 'readback' });
      const clipId = project.addClip({
        trackId,
        startPpq: 2,
        lengthPpq: 4,
        sourceOffsetPpq: 1,
        gain: 0.75,
        sourceUri: 'asset://readback.wav',
      });
      expect(project.tracks()[0]).toMatchObject({
        id: trackId,
        kind: 'audio',
        name: 'readback',
        gain: 1,
        pan: 0,
        mute: false,
        solo: false,
      });
      expect(project.clips()[0]).toMatchObject({
        id: clipId,
        trackId,
        sourceKind: 'audio',
        startPpq: 2,
        lengthPpq: 4,
        sourceOffsetPpq: 1,
        gain: 0.75,
        loopMode: 'off',
      });
      expect(project.clips()[0]).not.toHaveProperty('loopLengthPpq');
      expect(project.sources()[0]).toMatchObject({
        id: project.clips()[0].sourceId,
        kind: 'audio',
        nameOrUri: 'asset://readback.wav',
      });
      expect(project.tracks()[1]).toBeUndefined();
      expect(project.clips()[1]).toBeUndefined();
      expect(project.sources()[1]).toBeUndefined();
    } finally {
      project.destroy();
    }
  });

  it('sets and counts tempo segments', () => {
    const project = new Project();
    try {
      project.setTempoSegments([
        { startPpq: 0, bpm: 120 },
        { startPpq: 1920, bpm: 140, endBpm: 160 },
      ]);
      expect(project.tempoSegments().length).toBe(2);
    } finally {
      project.destroy();
    }
  });

  it('reads tempo segments back in the shape the setter takes', () => {
    // The count is only usable alongside a way to read what it counts, and a
    // segment that reads back in the setter's own shape can be moved between
    // projects without a translation step.
    const source = new Project();
    const target = new Project();
    try {
      const segments = [
        { startPpq: 0, bpm: 120, endBpm: 0 },
        { startPpq: 1920, bpm: 140, endBpm: 160 },
      ];
      source.setTempoSegments(segments);
      const readBack = [source.tempoSegments()[0], source.tempoSegments()[1]];
      expect(readBack).toEqual(segments);
      // startSample is never returned: the setter ignores it and a project keeps
      // musical positions only.
      expect('startSample' in readBack[0]).toBe(false);
      expect(source.tempoSegments()[2]).toBeUndefined();

      target.setTempoSegments(readBack);
      expect(target.tempoSegments()[1]).toEqual(readBack[1]);
    } finally {
      source.destroy();
      target.destroy();
    }
  });

  it('sets and counts time-signature segments', () => {
    const project = new Project();
    try {
      project.setTimeSignatures([
        { startPpq: 0, numerator: 4, denominator: 4 },
        { startPpq: 1920, numerator: 6, denominator: 8 },
      ]);
      expect(project.timeSignatures().length).toBe(2);
      expect(project.timeSignatures()[1]).toEqual({
        startPpq: 1920,
        numerator: 6,
        denominator: 8,
      });
      expect(project.timeSignatures()[2]).toBeUndefined();
    } finally {
      project.destroy();
    }
  });

  it('sets a marker and returns its id', () => {
    const project = new Project();
    try {
      const id = project.setMarker({ ppq: 480, name: 'intro' });
      expect(typeof id).toBe('number');
      expect(id).toBeGreaterThan(0);
    } finally {
      project.destroy();
    }
  });

  it('round-trips markers through setMarker/markers and counts them', () => {
    const project = new Project();
    try {
      expect(project.markers().length).toBe(0);
      const keyId = project.setMarker({
        ppq: 960,
        name: 'chorus',
        kind: 'keySignature',
        keyFifths: -3,
        keyMinor: true,
      });
      const cueId = project.setMarker({
        ppq: 1920,
        name: 'drop',
        kind: 'cuePoint',
      });
      expect(keyId).toBeGreaterThan(0);
      expect(cueId).toBeGreaterThan(0);

      const count = project.markers().length;
      expect(count).toBe(2);
      const markers = project.markers();
      const key = markers.find((m) => m.id === keyId);
      expect(key?.ppq).toBe(960);
      expect(key?.name).toBe('chorus');
      expect(key?.kind).toBe('keySignature');
      expect(key?.keyFifths).toBe(-3);
      expect(key?.keyMinor).toBe(true);
      const cue = markers.find((m) => m.id === cueId);
      expect(cue?.kind).toBe('cuePoint');
      expect(cue?.keyMinor).toBe(false);

      expect(markers[count]).toBeUndefined();
    } finally {
      project.destroy();
    }
  });

  it('round-trips a long UTF-8 marker name without splitting or truncating it', () => {
    const project = new Project();
    try {
      const name = 'あいうえお'.repeat(7);
      expect(Array.from(name)).toHaveLength(35);
      project.setMarker({ ppq: 960, name });
      expect(project.markers()[0].name).toBe(name);
      expect(JSON.parse(project.toJson()).markers[0].name).toBe(name);
    } finally {
      project.destroy();
    }
  });

  it('reads sample rate and round-trips the overlap policy', () => {
    const project = new Project();
    try {
      project.setSampleRate(44100);
      expect(project.sampleRate).toBe(44100);
      project.setOverlapPolicy('allow');
      expect(project.overlapPolicy).toBe('allow');
    } finally {
      project.destroy();
    }
  });

  it('accepts a mixer scene JSON without throwing', () => {
    const project = new Project();
    try {
      const sceneJson = JSON.stringify(mixingScenePreset('vocalReverbSend'));
      expect(() => project.setMixerSceneJson(sceneJson)).not.toThrow();
    } finally {
      project.destroy();
    }
  });

  it('reports no last bounce compile result on a project no bounce has run on', () => {
    const project = new Project();
    try {
      expect(project.lastBounceCompileResult()).toBeUndefined();
    } finally {
      project.destroy();
    }
  });

  it('reports empty diagnostics after a clean bounce', () => {
    const project = new Project();
    try {
      project.setSampleRate(48000);
      project.bounce({ totalFrames: 256 });
      expect(project.lastBounceCompileResult()).toEqual({ diagnostics: [] });
    } finally {
      project.destroy();
    }
  });

  it('reports the error diagnostics of a failed bounce', () => {
    const project = new Project();
    try {
      project.setSampleRate(48000);
      const trackId = project.addTrack({ kind: 'audio' });
      // A clip whose audio source was never supplied cannot be compiled.
      project.addClip({ trackId, startPpq: 0, lengthPpq: 1, sourceUri: 'asset://missing.wav' });
      expect(() => project.bounce({ totalFrames: 256 })).toThrow();
      const result = project.lastBounceCompileResult();
      expect(result).toBeDefined();
      expect(Object.keys(result ?? {})).toEqual(['diagnostics']);
      expect(result?.diagnostics.some((diagnostic) => diagnostic.severity === 'error')).toBe(true);
    } finally {
      project.destroy();
    }
  });
});

describe('Mixer tail draining (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('reports tailSamples as a number', () => {
    const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
      sampleRate: 48000,
      maxBlockSize: 512,
    });
    try {
      mixer.compile();
      const tail = mixer.tailSamples;
      expect(typeof tail).toBe('number');
      expect(tail).toBeGreaterThanOrEqual(0);
    } finally {
      mixer.destroy();
    }
  });

  it('reports latencySamples as a number', () => {
    const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
      sampleRate: 48000,
      maxBlockSize: 512,
    });
    try {
      mixer.compile();
      const latency = mixer.latencySamples;
      expect(typeof latency).toBe('number');
      expect(latency).toBeGreaterThanOrEqual(0);
    } finally {
      mixer.destroy();
    }
  });

  it('drainTailStereo returns stereo buffers of the requested length', () => {
    const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
      sampleRate: 48000,
      maxBlockSize: 512,
    });
    try {
      mixer.compile();
      const result = mixer.drainTailStereo(256);
      expect(result.left).toBeInstanceOf(Float32Array);
      expect(result.right).toBeInstanceOf(Float32Array);
      expect(result.left.length).toBe(256);
      expect(result.right.length).toBe(256);
      expect(result.sampleRate).toBe(48000);
    } finally {
      mixer.destroy();
    }
  });

  it('follows a tempo that moves only when asked to', () => {
    // Without adaptiveTempo the tracker fits one tempo to the whole take, so the
    // map it produces describes the take's average rather than its shape.
    const audio = accelerando();
    const segmentsFor = (options?: Record<string, unknown>) => {
      const project = new Project();
      try {
        project.autoTempo(audio, 22050, options);
        return project.tempoSegments();
      } finally {
        project.destroy();
      }
    };

    const fixed = segmentsFor();
    const adaptive = segmentsFor({ adaptiveTempo: true });
    expect(adaptive.length).toBeGreaterThan(fixed.length);

    // The reported tempo has to span the sweep, not sit on its average.
    const bpms = adaptive.map((segment) => segment.bpm);
    expect(Math.max(...bpms) - Math.min(...bpms)).toBeGreaterThan(20);

    // A coarser ramp threshold merges more of the take into constant stretches.
    const coarse = segmentsFor({ adaptiveTempo: true, rampThreshold: 0.2 });
    expect(coarse.length).toBeLessThan(adaptive.length);

    // Passing nothing must not quietly enable anything.
    expect(segmentsFor({})).toEqual(fixed);
  });

  it('rejects a tempo option value the bridge cannot use', () => {
    const audio = accelerando();
    const project = new Project();
    try {
      expect(() => project.autoTempo(audio, 22050, { tempoUpdateIntervalBeats: 0 })).toThrow();
      expect(() => project.analyzeTempo(audio, 22050, { rampThreshold: -1 })).toThrow();
      expect(project.analyzeTempo(audio, 22050, { includeOctaveCandidates: false })).toHaveLength(
        1,
      );
      expect(
        project.analyzeTempo(audio, 22050, { includeOctaveCandidates: true }).length,
      ).toBeGreaterThan(1);
    } finally {
      project.destroy();
    }
  });

  it('snaps a near-beat coordinate to the requested grid line', () => {
    const project = new Project();
    try {
      project.setSampleRate(48000);
      expect(project.snapToGrid(1.02, 1.0)).toBe(1.0);
      expect(project.snapToGrid(0.27, 1.0, 4)).toBe(0.25);
      expect(project.snapToGrid(4.02, 1.0, 0)).toBe(4.0);
      expect(() => project.snapToGrid(1.0, 1.0, -1)).toThrow();
    } finally {
      project.destroy();
    }
  });
});
