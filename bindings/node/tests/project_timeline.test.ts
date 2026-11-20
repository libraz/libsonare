import { describe, expect, it } from 'vitest';
import type { MixSceneStrip } from '../src/index.js';
import {
  ErrorCode,
  midiNoteOff,
  midiNoteOn,
  Project,
  ProjectTimeline,
  RealtimeEngine,
} from '../src/index.js';

/** First strip / bus of a scene document: the object the engine strip setters take. */
const sceneStrip = (json: string): MixSceneStrip => JSON.parse(json).strips[0];

const SAMPLE_RATE = 48000;
const BLOCK_SIZE = 128;
const FRAMES = BLOCK_SIZE * 500;
const DESTINATION = 9;
const SYNTH = { waveform: 'saw' as const, gain: 0.3, attackMs: 1, releaseMs: 20, polyphony: 4 };

function stereoTone(frames: number): Float32Array[] {
  const left = new Float32Array(frames);
  const right = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    const t = i / SAMPLE_RATE;
    left[i] = 0.25 * Math.sin(2 * Math.PI * 220 * t);
    right[i] = 0.2 * Math.sin(2 * Math.PI * 330 * t);
  }
  return [left, right];
}

/** Interleaves planar channels so two bounce paths compare as one array. */
function interleave(channels: Float32Array[]): Float32Array {
  const frames = channels[0].length;
  const out = new Float32Array(frames * channels.length);
  channels.forEach((channel, c) => {
    for (let i = 0; i < frames; i++) {
      out[i * channels.length + c] = channel[i];
    }
  });
  return out;
}

/** One audio clip and one MIDI clip routed to DESTINATION, at 120 BPM. */
function buildProject(): { project: Project; audioClip: number; audioTrack: number } {
  const project = new Project();
  project.setSampleRate(SAMPLE_RATE);
  const audioTrack = project.addTrack({ kind: 'audio', name: 'audio' });
  const audioClip = project.addClip({
    trackId: audioTrack,
    startPpq: 0,
    lengthPpq: 1.5,
    gain: 1,
    channels: stereoTone(SAMPLE_RATE),
    sampleRate: SAMPLE_RATE,
  });
  const { trackId, clipId } = project.addMidiClip(0, 2);
  project.setMidiEvents(clipId, [
    midiNoteOn({ ppq: 0.25, group: 0, channel: 0, note: 60, velocity: 0x60 }),
    midiNoteOff({ ppq: 1, group: 0, channel: 0, note: 60, velocity: 0 }),
    midiNoteOn({ ppq: 1, group: 0, channel: 0, note: 64, velocity: 0x60 }),
    midiNoteOff({ ppq: 1.75, group: 0, channel: 0, note: 64, velocity: 0 }),
  ]);
  project.setTrackMidiDestination(trackId, DESTINATION);
  return { project, audioClip, audioTrack };
}

/** A strip with one ducker insert, the insert a lane sidechain binding keys. */
function duckerStripJson(trackId: number): string {
  return JSON.stringify({
    version: 2,
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

function preparedEngine(): RealtimeEngine {
  const engine = new RealtimeEngine({ sampleRate: SAMPLE_RATE, maxBlockSize: BLOCK_SIZE });
  engine.prepare({
    sampleRate: SAMPLE_RATE,
    maxBlockSize: BLOCK_SIZE,
    commandCapacity: 64,
    telemetryCapacity: 16,
  });
  return engine;
}

function processBlock(engine: RealtimeEngine): void {
  engine.process([new Float32Array(BLOCK_SIZE), new Float32Array(BLOCK_SIZE)]);
}

/** Plays from the start, bounces FRAMES, then leaves the engine stopped at 0. */
function engineBounce(engine: RealtimeEngine): Float32Array {
  engine.play();
  const result = engine.bounceOffline({
    totalFrames: FRAMES,
    blockSize: BLOCK_SIZE,
    numChannels: 2,
    sourceSampleRate: SAMPLE_RATE,
    targetSampleRate: SAMPLE_RATE,
    normalizeLufs: false,
  });
  engine.stop();
  engine.seekSample(0);
  processBlock(engine);
  expect(engine.transportState().playing).toBe(false);
  return interleave(result.channels);
}

function projectBounce(project: Project): Float32Array {
  const result = project.bounceWithBuiltinInstruments(
    [{ destinationId: DESTINATION, instrument: SYNTH }],
    {
      totalFrames: FRAMES,
      blockSize: BLOCK_SIZE,
      numChannels: 2,
    },
  );
  expect(result.sampleRate).toBe(SAMPLE_RATE);
  return interleave(result.channels);
}

function peak(data: Float32Array): number {
  let value = 0;
  for (const sample of data) {
    value = Math.max(value, Math.abs(sample));
  }
  return value;
}

/** Binds a fresh instrument (so voice state cannot leak), compiles and applies. */
function applyProject(engine: RealtimeEngine, project: Project): void {
  engine.setBuiltinInstrument(DESTINATION, SYNTH);
  const { timeline } = project.compileTimeline();
  expect(timeline).toBeInstanceOf(ProjectTimeline);
  engine.applyProjectTimeline(timeline as ProjectTimeline);
  (timeline as ProjectTimeline).destroy();
}

describe('project timeline', () => {
  it('renders bit-identically to the project bounce, across edits', () => {
    const { project, audioClip } = buildProject();
    const engine = preparedEngine();
    const matchStep = (): Float32Array => {
      applyProject(engine, project); // the handle is disposed right after apply
      const live = engineBounce(engine);
      const bounced = projectBounce(project);
      expect(peak(bounced)).toBeGreaterThan(0.01);
      expect(live.length).toBe(bounced.length);
      expect(Array.from(live)).toEqual(Array.from(bounced));
      return bounced;
    };
    const initial = matchStep();
    project.moveClip(audioClip, 0.5);
    const moved = matchStep();
    expect(Array.from(moved)).not.toEqual(Array.from(initial));
    engine.destroy();
    project.destroy();
  });

  it('dispose is idempotent and a disposed timeline cannot be applied', () => {
    const { project } = buildProject();
    const engine = preparedEngine();
    const { timeline } = project.compileTimeline();
    expect(timeline).not.toBeNull();
    timeline?.destroy();
    timeline?.destroy();
    expect(() => engine.applyProjectTimeline(timeline as ProjectTimeline)).toThrow(
      expect.objectContaining({ code: ErrorCode.InvalidState }),
    );
    engine.destroy();
    project.destroy();
  });

  it('refuses an apply while playing and leaves the engine usable', () => {
    const { project } = buildProject();
    const engine = preparedEngine();
    const { timeline } = project.compileTimeline();
    engine.play();
    processBlock(engine);
    expect(engine.transportState().playing).toBe(true);
    expect(() => engine.applyProjectTimeline(timeline as ProjectTimeline)).toThrow();
    expect(engine.clipCount).toBe(0);
    engine.stop();
    processBlock(engine);
    engine.applyProjectTimeline(timeline as ProjectTimeline);
    expect(engine.clipCount).toBeGreaterThan(0);
    timeline?.destroy();
    engine.destroy();
    project.destroy();
  });

  it('reports a compile failure as diagnostics with no timeline', () => {
    const project = new Project();
    project.setSampleRate(SAMPLE_RATE);
    const trackId = project.addTrack({ kind: 'audio' });
    // A clip whose audio source was never supplied cannot be compiled.
    project.addClip({ trackId, startPpq: 0, lengthPpq: 1, sourceUri: 'asset://missing.wav' });
    const result = project.compileTimeline();
    expect(result).not.toHaveProperty('hasTimeline');
    expect(result).not.toHaveProperty('timeline');
    expect(result.diagnostics.length).toBeGreaterThan(0);
    expect(result.diagnostics.some((diagnostic) => diagnostic.severity === 'error')).toBe(true);
    const plain = project.compile();
    expect(result.diagnostics).toEqual(plain.diagnostics);
    project.destroy();
  });

  it('rejects a self key and a cycle in lane sidechain bindings, keeping existing ones', () => {
    const engine = preparedEngine();
    engine.setTrackLanes([{ trackId: 1 }, { trackId: 2 }]);
    engine.setStrip(1, sceneStrip(duckerStripJson(1)));
    engine.setStrip(2, sceneStrip(duckerStripJson(2)));
    engine.setStripSidechain(1, 0, { kind: 'track', id: 2 });
    expect(() => engine.setStripSidechain(1, 0, { kind: 'track', id: 1 })).toThrow();
    expect(() => engine.setStripSidechain(2, 0, { kind: 'track', id: 1 })).toThrow();
    // The first binding survived: re-setting it and removing it still work.
    engine.setStripSidechain(1, 0, { kind: 'track', id: 2 });
    engine.setStripSidechain(1, 0, null);
    // With it removed, the reverse direction is no longer a cycle.
    engine.setStripSidechain(2, 0, { kind: 'track', id: 1 });
    engine.destroy();
  });
});
