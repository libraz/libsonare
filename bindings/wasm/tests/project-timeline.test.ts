/**
 * Compiled project timelines applied to a caller-owned realtime engine: the
 * engine's offline bounce against the project's own bounce, re-apply after an
 * edit, handle lifetime, the stopped-only rule, compile failure and the lane
 * sidechain refusals.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  init,
  midiNoteOff,
  midiNoteOn,
  Project,
  type ProjectTimeline,
  RealtimeEngine,
} from '../dist/index.js';

const SAMPLE_RATE = 48000;
const BLOCK_SIZE = 128;
const FRAMES = BLOCK_SIZE * 500;
const DESTINATION = 9;
const SYNTH = { waveform: 'saw', gain: 0.3, attackMs: 1, releaseMs: 20, polyphony: 4 } as const;

/** A current document whose one clip names a source the document does not carry. */
function danglingSourceJson(): string {
  const project = new Project();
  try {
    const trackId = project.addTrack({ kind: 'audio', name: 'audio' });
    project.addClip({ trackId, startPpq: 0, lengthPpq: 1, sourceUri: 'asset://dangling.wav' });
    const document = JSON.parse(project.toJson());
    document.clips[0].source_id = 99;
    return JSON.stringify(document);
  } finally {
    project.destroy();
  }
}

/** A strip with one ducker insert, the insert a lane sidechain binding keys. */
function duckerStripJson(trackId: number): string {
  return JSON.stringify({
    version: 1,
    strips: [
      {
        id: `track-${trackId}`,
        inserts: [
          {
            slot: 'pre',
            processor: 'dynamics.duckingProcessor',
            params: { thresholdDb: -20, ratio: 20, attackMs: 0.05, releaseMs: 80, rangeDb: 30 },
          },
        ],
      },
    ],
    buses: [],
    connections: [],
  });
}

interface Built {
  project: Project;
  audioClip: number;
}

function buildProject(): Built {
  const project = new Project();
  project.setSampleRate(SAMPLE_RATE);
  const audioTrack = project.addTrack({ kind: 'audio', name: 'audio' });
  const audio = new Float32Array(SAMPLE_RATE);
  for (let i = 0; i < audio.length; i++) {
    audio[i] = 0.25 * Math.sin((2 * Math.PI * 220 * i) / SAMPLE_RATE);
  }
  const audioClip = project.addClip({
    trackId: audioTrack,
    startPpq: 0,
    lengthPpq: 1.5,
    channels: [audio],
    sampleRate: SAMPLE_RATE,
  });
  const { trackId, clipId } = project.addMidiClip(0, 2);
  project.setMidiEvents(clipId, [
    midiNoteOn({ ppq: 0.25, group: 0, channel: 0, note: 60, velocity: 100 }),
    midiNoteOff({ ppq: 1, group: 0, channel: 0, note: 60, velocity: 0 }),
    midiNoteOn({ ppq: 1, group: 0, channel: 0, note: 64, velocity: 100 }),
    midiNoteOff({ ppq: 1.75, group: 0, channel: 0, note: 64, velocity: 0 }),
  ]);
  project.setTrackMidiDestination(trackId, DESTINATION);
  return { project, audioClip };
}

function projectBounce(project: Project): Float32Array[] {
  return project.bounceWithBuiltinInstruments(
    [{ destinationId: DESTINATION, instrument: { ...SYNTH } }],
    { totalFrames: FRAMES, blockSize: BLOCK_SIZE, numChannels: 2 },
  ).channels;
}

function processBlock(engine: RealtimeEngine): void {
  engine.process([new Float32Array(BLOCK_SIZE), new Float32Array(BLOCK_SIZE)]);
}

/** Plays from the start, bounces FRAMES, then leaves the engine stopped at the start. */
function engineBounce(engine: RealtimeEngine): Float32Array[] {
  engine.play();
  const result = engine.bounceOffline({
    totalFrames: FRAMES,
    blockSize: BLOCK_SIZE,
    numChannels: 2,
    sourceSampleRate: SAMPLE_RATE,
    targetSampleRate: SAMPLE_RATE,
  });
  engine.stop();
  engine.seekSample(0);
  processBlock(engine);
  expect(engine.transportState().playing).toBe(false);
  return result.channels;
}

/** Binds a fresh instrument so voice state from an earlier render cannot leak in. */
function compileAndApply(engine: RealtimeEngine, project: Project): ProjectTimeline {
  engine.setBuiltinInstrument(DESTINATION, { ...SYNTH });
  const { timeline } = project.compileTimeline();
  expect(timeline).not.toBeNull();
  engine.applyProjectTimeline(timeline as ProjectTimeline);
  return timeline as ProjectTimeline;
}

const peak = (channels: Float32Array[]): number =>
  channels.reduce(
    (outer, samples) => samples.reduce((acc, sample) => Math.max(acc, Math.abs(sample)), outer),
    0,
  );

describe('Project timeline applied to a RealtimeEngine', () => {
  beforeAll(async () => {
    await init();
  });

  it('renders bit-identically to the project bounce across edits', () => {
    const { project, audioClip } = buildProject();
    const engine = new RealtimeEngine({ sampleRate: SAMPLE_RATE, maxBlockSize: BLOCK_SIZE });
    try {
      const requireMatch = (): Float32Array[] => {
        compileAndApply(engine, project).destroy();
        const live = engineBounce(engine);
        const bounced = projectBounce(project);
        expect(peak(bounced)).toBeGreaterThan(0.01);
        expect(live).toEqual(bounced);
        return bounced;
      };
      const initial = requireMatch();
      project.moveClip(audioClip, 0.5);
      const moved = requireMatch();
      expect(moved).not.toEqual(initial);
    } finally {
      engine.destroy();
      project.destroy();
    }
  });

  it('keeps rendering after the timeline handle is disposed right after apply', () => {
    const { project } = buildProject();
    const engine = new RealtimeEngine({ sampleRate: SAMPLE_RATE, maxBlockSize: BLOCK_SIZE });
    try {
      const timeline = compileAndApply(engine, project);
      timeline.destroy();
      timeline.destroy();
      expect(() => engine.applyProjectTimeline(timeline)).toThrow(
        expect.objectContaining({ code: ErrorCode.InvalidState }),
      );
      expect(engineBounce(engine)).toEqual(projectBounce(project));
    } finally {
      engine.destroy();
      project.destroy();
    }
  });

  it('refuses to apply while the transport is playing and leaves the engine unchanged', () => {
    const { project } = buildProject();
    const engine = new RealtimeEngine({ sampleRate: SAMPLE_RATE, maxBlockSize: BLOCK_SIZE });
    try {
      const { timeline } = project.compileTimeline();
      expect(timeline).not.toBeNull();
      engine.play();
      processBlock(engine);
      expect(engine.transportState().playing).toBe(true);
      const before = engine.clipCount;
      expect(() => engine.applyProjectTimeline(timeline as ProjectTimeline)).toThrow(
        /transport is stopped/,
      );
      expect(engine.clipCount).toBe(before);
      (timeline as ProjectTimeline).destroy();
    } finally {
      engine.destroy();
      project.destroy();
    }
  });

  it('reports a failed compile as its diagnostics with no timeline', () => {
    const { project, diagnostics } = Project.fromJsonWithDiagnostics(danglingSourceJson());
    try {
      expect(diagnostics).toContain('dangling_clip_source');
      const result = project.compileTimeline();
      expect(result).not.toHaveProperty('hasTimeline');
      expect(result).not.toHaveProperty('timeline');
      expect(result.diagnostics.length).toBeGreaterThan(0);
      expect(result.diagnostics.some((d) => d.severity === 'error')).toBe(true);
    } finally {
      project.destroy();
    }
  });

  it('rejects anything but a live ProjectTimeline', () => {
    const engine = new RealtimeEngine({ sampleRate: SAMPLE_RATE, maxBlockSize: BLOCK_SIZE });
    try {
      expect(() => engine.applyProjectTimeline(null as never)).toThrow(TypeError);
      expect(() => engine.applyProjectTimeline({} as never)).toThrow(TypeError);
    } finally {
      engine.destroy();
    }
  });

  it('tells a released timeline id from one never issued at the native layer', () => {
    const { project } = buildProject();
    const engine = new RealtimeEngine({ sampleRate: SAMPLE_RATE, maxBlockSize: BLOCK_SIZE });
    try {
      const { timeline } = project.compileTimeline();
      expect(timeline).not.toBeNull();
      const releasedId = (timeline as unknown as { native: { id: number } }).native.id;
      (timeline as ProjectTimeline).destroy();
      const native = (engine as unknown as { native: { applyProjectTimeline(id: number): void } })
        .native;
      expect(() => native.applyProjectTimeline(releasedId)).toThrow(
        expect.objectContaining({ code: ErrorCode.InvalidState }),
      );
      expect(() => native.applyProjectTimeline(0x7fffffff)).toThrow(RangeError);
    } finally {
      engine.destroy();
      project.destroy();
    }
  });

  it('rejects forged timeline identities and leaves the engine unchanged', () => {
    const { project: current } = buildProject();
    const { project: forgedSource } = buildProject();
    forgedSource.addMidiClip(0, 1);
    const engine = new RealtimeEngine({ sampleRate: SAMPLE_RATE, maxBlockSize: BLOCK_SIZE });
    let currentTimeline: ProjectTimeline | null = null;
    let forgedTimeline: ProjectTimeline | null = null;
    try {
      currentTimeline = compileAndApply(engine, current);
      const compiled = forgedSource.compileTimeline();
      forgedTimeline = compiled.timeline ?? null;
      expect(forgedTimeline).not.toBeNull();
      const nativeId = (forgedTimeline as unknown as { native: { id: number } }).native.id;
      const before = engine.clipCount;
      const copied = { nativeId } as unknown as ProjectTimeline;
      const inherited = Object.create({ nativeId }) as ProjectTimeline;

      expect(() => engine.applyProjectTimeline(copied)).toThrow(TypeError);
      expect(() => engine.applyProjectTimeline(inherited)).toThrow(TypeError);
      expect(engine.clipCount).toBe(before);
    } finally {
      forgedTimeline?.destroy();
      currentTimeline?.destroy();
      engine.destroy();
      forgedSource.destroy();
      current.destroy();
    }
  });
});

describe('RealtimeEngine lane sidechain refusals', () => {
  beforeAll(async () => {
    await init();
  });

  it('refuses a self key and a cycle, and keeps the existing bindings', () => {
    const engine = new RealtimeEngine({ sampleRate: SAMPLE_RATE, maxBlockSize: BLOCK_SIZE });
    try {
      engine.setTrackLanes([{ trackId: 1 }, { trackId: 2 }]);
      engine.setStrip(1, JSON.parse(duckerStripJson(1)).strips[0]);
      engine.setStrip(2, JSON.parse(duckerStripJson(2)).strips[0]);
      expect(() => engine.setStripSidechain(1, 0, { kind: 'track', id: 1 })).toThrow();
      engine.setStripSidechain(1, 0, { kind: 'track', id: 2 });
      // 2 keyed by 1 would close a cycle with the binding above.
      expect(() => engine.setStripSidechain(2, 0, { kind: 'track', id: 1 })).toThrow();
      // The refusal left 1 <- 2 in place: removing it is what makes the reverse legal.
      engine.setStripSidechain(1, 0, null);
      engine.setStripSidechain(2, 0, { kind: 'track', id: 1 });
      expect(() => engine.setStripSidechain(1, 0, { kind: 'track', id: 2 })).toThrow();
    } finally {
      engine.destroy();
    }
  });
});
