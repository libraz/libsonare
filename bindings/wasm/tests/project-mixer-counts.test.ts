/**
 * Coverage for the WASM Project/Mixer methods newly surfaced from the C ABI:
 * project counts, tempo/time-signature maps, markers, sample-rate / overlap
 * policy accessors, mixer scene JSON, last-bounce diagnostics, and mixer tail
 * draining.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, MarkerKind, Mixer, mixingScenePresetJson, Project } from '../dist/index.js';

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
      expect(project.trackByIndex(0)).toMatchObject({
        id: trackId,
        kind: 0,
        name: 'readback',
        gain: 1,
        pan: 0,
        mute: false,
        solo: false,
      });
      expect(project.clipByIndex(0)).toMatchObject({
        id: clipId,
        trackId,
        sourceKind: 0,
        startPpq: 2,
        lengthPpq: 4,
        sourceOffsetPpq: 1,
        gain: 0.75,
        loopMode: 0,
        loopLengthPpq: 0,
      });
      expect(project.sourceByIndex(0)).toMatchObject({
        id: project.clipByIndex(0).sourceId,
        kind: 0,
        nameOrUri: 'asset://readback.wav',
      });
      expect(() => project.trackByIndex(1)).toThrow();
      expect(() => project.clipByIndex(1)).toThrow();
      expect(() => project.sourceByIndex(1)).toThrow();
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
      const readBack = [source.tempoSegmentByIndex(0), source.tempoSegmentByIndex(1)];
      expect(readBack).toEqual(segments);
      // startSample is never returned: the setter ignores it and a project keeps
      // musical positions only.
      expect('startSample' in readBack[0]).toBe(false);
      expect(() => source.tempoSegmentByIndex(2)).toThrow();

      target.setTempoSegments(readBack);
      expect(target.tempoSegmentByIndex(1)).toEqual(readBack[1]);
    } finally {
      source.delete();
      target.delete();
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
      expect(project.timeSignatureByIndex(1)).toEqual({
        startPpq: 1920,
        numerator: 6,
        denominator: 8,
      });
      expect(() => project.timeSignatureByIndex(2)).toThrow();
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

  it('round-trips markers through setMarkerEx/markerByIndex and counts them', () => {
    const project = new Project();
    try {
      expect(project.markerCount()).toBe(0);
      const keyId = project.setMarkerEx({
        id: 0,
        ppq: 960,
        name: 'chorus',
        kind: MarkerKind.keySignature,
        keyFifths: -3,
        keyMinor: true,
      });
      const cueId = project.setMarkerEx({
        id: 0,
        ppq: 1920,
        name: 'drop',
        kind: MarkerKind.cuePoint,
      });
      expect(keyId).toBeGreaterThan(0);
      expect(cueId).toBeGreaterThan(0);

      const count = project.markerCount();
      expect(count).toBe(2);
      const markers = [];
      for (let i = 0; i < count; i++) {
        markers.push(project.markerByIndex(i));
      }
      const key = markers.find((m) => m.id === keyId);
      expect(key?.ppq).toBe(960);
      expect(key?.name).toBe('chorus');
      expect(key?.kind).toBe(MarkerKind.keySignature);
      expect(key?.keyFifths).toBe(-3);
      expect(key?.keyMinor).toBe(true);
      const cue = markers.find((m) => m.id === cueId);
      expect(cue?.kind).toBe(MarkerKind.cuePoint);
      expect(cue?.keyMinor).toBe(false);

      expect(() => project.markerByIndex(count)).toThrow();
    } finally {
      project.delete();
    }
  });

  it('round-trips a long UTF-8 marker name without splitting or truncating it', () => {
    const project = new Project();
    try {
      const name = 'あいうえお'.repeat(7);
      expect(Array.from(name)).toHaveLength(35);
      project.setMarkerEx({ id: 0, ppq: 960, name });
      expect(project.markerByIndex(0).name).toBe(name);
      expect(JSON.parse(project.toJson()).markers[0].name).toBe(name);
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
      // No bounce has run on this project, so the result is empty in full.
      // A failed bounce is told apart by its diagnostics, never by hasTimeline
      // alone: losing the timeline always leaves an error diagnostic behind.
      expect(result.hasTimeline).toBe(false);
      expect(result.diagnostics).toHaveLength(0);
      expect(result.diagnosticCount).toBe(0);
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

  it('reports latencySamples as a number', () => {
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 48000, 512);
    try {
      mixer.compile();
      const latency = mixer.latencySamples();
      expect(typeof latency).toBe('number');
      expect(latency).toBeGreaterThanOrEqual(0);
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

  it('follows a tempo that moves only when asked to', () => {
    // Without adaptiveTempo the tracker fits one tempo to the whole take, so the
    // map it produces describes the take's average rather than its shape.
    const audio = accelerando();
    const segmentsFor = (options?: Record<string, unknown>) => {
      const project = new Project();
      try {
        project.autoTempo(audio, 22050, 0, false, options);
        return Array.from({ length: project.tempoSegmentCount() }, (_, i) =>
          project.tempoSegmentByIndex(i),
        );
      } finally {
        project.delete();
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
      expect(() =>
        project.autoTempo(audio, 22050, 0, false, { tempoUpdateIntervalBeats: 0 }),
      ).toThrow();
      expect(() => project.analyzeTempo(audio, 22050, { rampThreshold: -1 })).toThrow();
      expect(project.analyzeTempo(audio, 22050, { includeOctaveCandidates: false })).toHaveLength(
        1,
      );
      expect(
        project.analyzeTempo(audio, 22050, { includeOctaveCandidates: true }).length,
      ).toBeGreaterThan(1);
    } finally {
      project.delete();
    }
  });
});
