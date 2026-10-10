import { describe, expect, it } from 'vitest';
import { Project, ProjectTimeline, RealtimeEngine } from '../src/index.js';

const SAMPLE_RATE = 48000;
const BLOCK_SIZE = 128;
const FRAMES = BLOCK_SIZE * 500;
const DESTINATION = 9;
const SYNTH = { waveform: 1, gain: 0.3, attackMs: 1, releaseMs: 20, polyphony: 4 };

function stereoTone(frames: number): Float32Array {
  const data = new Float32Array(frames * 2);
  for (let i = 0; i < frames; i++) {
    const t = i / SAMPLE_RATE;
    data[i * 2] = 0.25 * Math.sin(2 * Math.PI * 220 * t);
    data[i * 2 + 1] = 0.2 * Math.sin(2 * Math.PI * 330 * t);
  }
  return data;
}

/** One audio clip and one MIDI clip routed to DESTINATION, at 120 BPM. */
function buildProject(): { project: Project; audioClip: number; audioTrack: number } {
  const project = Project.create();
  project.setSampleRate(SAMPLE_RATE);
  const audioTrack = project.addTrack({ kind: 'audio', name: 'audio' });
  const audioClip = project.addClip({
    trackId: audioTrack,
    startPpq: 0,
    lengthPpq: 1.5,
    gain: 1,
    audio: stereoTone(SAMPLE_RATE),
    audioChannels: 2,
    audioSampleRate: SAMPLE_RATE,
  });
  const { trackId, clipId } = project.addMidiClip(0, 2);
  project.setMidiEvents(clipId, [
    Project.midiNoteOn(0.25, 0, 0, 60, 0x60),
    Project.midiNoteOff(1, 0, 0, 60, 0),
    Project.midiNoteOn(1, 0, 0, 64, 0x60),
    Project.midiNoteOff(1.75, 0, 0, 64, 0),
  ]);
  project.setTrackMidiDestination(trackId, DESTINATION);
  return { project, audioClip, audioTrack };
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

function preparedEngine(): RealtimeEngine {
  const engine = new RealtimeEngine(SAMPLE_RATE, BLOCK_SIZE);
  engine.prepare(SAMPLE_RATE, BLOCK_SIZE, 64, 16);
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
    dither: 0,
  });
  engine.stop();
  engine.seekSample(0);
  processBlock(engine);
  expect(engine.getTransportState().playing).toBe(false);
  return result.interleaved;
}

function projectBounce(project: Project): Float32Array {
  return project.bounceWithBuiltinInstruments([{ ...SYNTH, destinationId: DESTINATION }], {
    totalFrames: FRAMES,
    blockSize: BLOCK_SIZE,
    numChannels: 2,
    sampleRate: SAMPLE_RATE,
  });
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
  engine.setBuiltinInstrument(SYNTH, DESTINATION);
  const { timeline, hasTimeline } = project.compileTimeline();
  expect(hasTimeline).toBe(true);
  expect(timeline).toBeInstanceOf(ProjectTimeline);
  engine.applyProjectTimeline(timeline as ProjectTimeline);
  (timeline as ProjectTimeline).dispose();
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
    timeline?.dispose();
    timeline?.dispose();
    expect(() => engine.applyProjectTimeline(timeline as ProjectTimeline)).toThrow(/disposed/);
    engine.destroy();
    project.destroy();
  });

  it('refuses an apply while playing and leaves the engine usable', () => {
    const { project } = buildProject();
    const engine = preparedEngine();
    const { timeline } = project.compileTimeline();
    engine.play();
    processBlock(engine);
    expect(engine.getTransportState().playing).toBe(true);
    expect(() => engine.applyProjectTimeline(timeline as ProjectTimeline)).toThrow();
    expect(engine.clipCount()).toBe(0);
    engine.stop();
    processBlock(engine);
    engine.applyProjectTimeline(timeline as ProjectTimeline);
    expect(engine.clipCount()).toBeGreaterThan(0);
    timeline?.dispose();
    engine.destroy();
    project.destroy();
  });

  it('reports a compile failure as a null timeline with diagnostics', () => {
    const project = Project.create();
    project.setSampleRate(SAMPLE_RATE);
    const trackId = project.addTrack({ kind: 'audio' });
    // A clip whose audio source was never supplied cannot be compiled.
    project.addClip({ trackId, startPpq: 0, lengthPpq: 1, sourceUri: 'asset://missing.wav' });
    const result = project.compileTimeline();
    expect(result.hasTimeline).toBe(false);
    expect(result.timeline).toBeNull();
    expect(result.diagnostics.length).toBeGreaterThan(0);
    const plain = project.compile();
    expect(result.diagnostics).toEqual(plain.diagnostics);
    expect(result.messages).toBe(plain.messages);
    project.destroy();
  });

  it('rejects a self key and a cycle in lane sidechain bindings, keeping existing ones', () => {
    const engine = preparedEngine();
    engine.setTrackLanes([1, 2]);
    engine.setTrackStripJson(1, duckerStripJson(1));
    engine.setTrackStripJson(2, duckerStripJson(2));
    engine.setLaneSidechain(1, 0, 2);
    expect(() => engine.setLaneSidechain(1, 0, 1)).toThrow();
    expect(() => engine.setLaneSidechain(2, 0, 1)).toThrow();
    // The first binding survived: re-setting it and removing it still work.
    engine.setLaneSidechain(1, 0, 2);
    engine.setLaneSidechain(1, 0, 0);
    // With it removed, the reverse direction is no longer a cycle.
    engine.setLaneSidechain(2, 0, 1);
    engine.destroy();
  });
});
