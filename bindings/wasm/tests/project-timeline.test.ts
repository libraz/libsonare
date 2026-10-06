/**
 * Compiled project timelines applied to a caller-owned realtime engine: the
 * engine's offline bounce against the project's own bounce, re-apply after an
 * edit, handle lifetime, the stopped-only rule, compile failure and the lane
 * sidechain refusals.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, Project, type ProjectTimeline, RealtimeEngine } from '../dist/index.js';

const SAMPLE_RATE = 48000;
const BLOCK_SIZE = 128;
const FRAMES = BLOCK_SIZE * 500;
const DESTINATION = 9;
const SYNTH = { waveform: 'saw', gain: 0.3, attackMs: 1, releaseMs: 20, polyphony: 4 } as const;

const DANGLING_SOURCE_JSON =
  '{"version":1,"sample_rate":48000,"tracks":[{"id":1,"name":"audio","kind":0,"channel_strip_ref":"","output_target":"","midi_destination_id":0,"automation_lanes":[]}],"clips":[{"id":1,"track_id":1,"source_id":99,"start_ppq":0,"length_ppq":1,"source_offset_ppq":0,"gain":1,"fade_in":{"length_ppq":0,"curve":0},"fade_out":{"length_ppq":0,"curve":0},"loop_mode":0,"loop_length_ppq":0,"warp_ref_id":0,"warp_mode":0}]}';

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
    audio,
    audioChannels: 1,
    audioSampleRate: SAMPLE_RATE,
  });
  const { trackId, clipId } = project.addMidiClip(0, 2);
  project.setMidiEvents(clipId, [
    Project.midiNoteOn(0.25, 0, 0, 60, 100),
    Project.midiNoteOff(1, 0, 0, 60, 0),
    Project.midiNoteOn(1, 0, 0, 64, 100),
    Project.midiNoteOff(1.75, 0, 0, 64, 0),
  ]);
  project.setTrackMidiDestination(trackId, DESTINATION);
  return { project, audioClip };
}

function projectBounce(project: Project): Float32Array {
  return project.bounceWithBuiltinInstrument(
    { destinationId: DESTINATION, ...SYNTH },
    { totalFrames: FRAMES, blockSize: BLOCK_SIZE, numChannels: 2, sampleRate: SAMPLE_RATE },
  );
}

function processBlock(engine: RealtimeEngine): void {
  engine.process([new Float32Array(BLOCK_SIZE), new Float32Array(BLOCK_SIZE)]);
}

/** Plays from the start, bounces FRAMES, then leaves the engine stopped at the start. */
function engineBounce(engine: RealtimeEngine): Float32Array {
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
  expect(engine.getTransportState().playing).toBe(false);
  return result.interleaved;
}

/** Binds a fresh instrument so voice state from an earlier render cannot leak in. */
function compileAndApply(engine: RealtimeEngine, project: Project): ProjectTimeline {
  engine.setBuiltinInstrument({ ...SYNTH }, DESTINATION);
  const { timeline, hasTimeline } = project.compileTimeline();
  expect(hasTimeline).toBe(true);
  expect(timeline).not.toBeNull();
  engine.applyProjectTimeline(timeline as ProjectTimeline);
  return timeline as ProjectTimeline;
}

const peak = (samples: Float32Array): number =>
  samples.reduce((acc, sample) => Math.max(acc, Math.abs(sample)), 0);

describe('Project timeline applied to a RealtimeEngine', () => {
  beforeAll(async () => {
    await init();
  });

  it('renders bit-identically to the project bounce across edits', () => {
    const { project, audioClip } = buildProject();
    const engine = new RealtimeEngine(SAMPLE_RATE, BLOCK_SIZE);
    try {
      const requireMatch = (): Float32Array => {
        compileAndApply(engine, project).dispose();
        const live = engineBounce(engine);
        const bounced = projectBounce(project);
        expect(peak(bounced)).toBeGreaterThan(0.01);
        expect(live).toEqual(bounced);
        return bounced;
      };
      const initial = requireMatch();
      project.moveClip(audioClip, 0.5, 0);
      const moved = requireMatch();
      expect(moved).not.toEqual(initial);
    } finally {
      engine.destroy();
      project.delete();
    }
  });

  it('keeps rendering after the timeline handle is disposed right after apply', () => {
    const { project } = buildProject();
    const engine = new RealtimeEngine(SAMPLE_RATE, BLOCK_SIZE);
    try {
      const timeline = compileAndApply(engine, project);
      timeline.dispose();
      timeline.dispose();
      expect(() => engine.applyProjectTimeline(timeline)).toThrow();
      expect(engineBounce(engine)).toEqual(projectBounce(project));
    } finally {
      engine.destroy();
      project.delete();
    }
  });

  it('refuses to apply while the transport is playing and leaves the engine unchanged', () => {
    const { project } = buildProject();
    const engine = new RealtimeEngine(SAMPLE_RATE, BLOCK_SIZE);
    try {
      const { timeline } = project.compileTimeline();
      expect(timeline).not.toBeNull();
      engine.play();
      processBlock(engine);
      expect(engine.getTransportState().playing).toBe(true);
      const before = engine.clipCount();
      expect(() => engine.applyProjectTimeline(timeline as ProjectTimeline)).toThrow(
        /transport is stopped/,
      );
      expect(engine.clipCount()).toBe(before);
      (timeline as ProjectTimeline).dispose();
    } finally {
      engine.destroy();
      project.delete();
    }
  });

  it('reports a failed compile as a null timeline with its diagnostics', () => {
    const { project, diagnostics } = Project.fromJsonWithDiagnostics(DANGLING_SOURCE_JSON);
    try {
      expect(diagnostics).toContain('dangling_clip_source');
      const result = project.compileTimeline();
      expect(result.hasTimeline).toBe(false);
      expect(result.timeline).toBeNull();
      expect(result.diagnosticCount).toBeGreaterThan(0);
    } finally {
      project.delete();
    }
  });

  it('rejects anything but a live ProjectTimeline', () => {
    const engine = new RealtimeEngine(SAMPLE_RATE, BLOCK_SIZE);
    try {
      expect(() => engine.applyProjectTimeline(null as never)).toThrow(TypeError);
      expect(() => engine.applyProjectTimeline({} as never)).toThrow(TypeError);
    } finally {
      engine.destroy();
    }
  });

  it('rejects forged timeline identities and leaves the engine unchanged', () => {
    const { project: current } = buildProject();
    const { project: forgedSource } = buildProject();
    forgedSource.addMidiClip(0, 1);
    const engine = new RealtimeEngine(SAMPLE_RATE, BLOCK_SIZE);
    let currentTimeline: ProjectTimeline | null = null;
    let forgedTimeline: ProjectTimeline | null = null;
    try {
      currentTimeline = compileAndApply(engine, current);
      const compiled = forgedSource.compileTimeline();
      forgedTimeline = compiled.timeline;
      expect(forgedTimeline).not.toBeNull();
      const nativeId = (forgedTimeline as unknown as { nativeId: number }).nativeId;
      const before = engine.clipCount();
      const copied = { nativeId } as unknown as ProjectTimeline;
      const inherited = Object.create({ nativeId }) as ProjectTimeline;

      expect(() => engine.applyProjectTimeline(copied)).toThrow(TypeError);
      expect(() => engine.applyProjectTimeline(inherited)).toThrow(TypeError);
      expect(engine.clipCount()).toBe(before);
    } finally {
      forgedTimeline?.dispose();
      currentTimeline?.dispose();
      engine.destroy();
      forgedSource.delete();
      current.delete();
    }
  });
});

describe('RealtimeEngine lane sidechain refusals', () => {
  beforeAll(async () => {
    await init();
  });

  it('refuses a self key and a cycle, and keeps the existing bindings', () => {
    const engine = new RealtimeEngine(SAMPLE_RATE, BLOCK_SIZE);
    try {
      engine.setTrackLanes([1, 2]);
      expect(() => engine.setLaneSidechain(1, 0, 1)).toThrow();
      engine.setLaneSidechain(1, 0, 2);
      // 2 keyed by 1 would close a cycle with the binding above.
      expect(() => engine.setLaneSidechain(2, 0, 1)).toThrow();
      // The refusal left 1 <- 2 in place: removing it is what makes the reverse legal.
      engine.setLaneSidechain(1, 0, 0);
      engine.setLaneSidechain(2, 0, 1);
      expect(() => engine.setLaneSidechain(1, 0, 2)).toThrow();
    } finally {
      engine.destroy();
    }
  });
});
