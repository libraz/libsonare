import { describe, expect, it } from 'vitest';
import { masteringInsertNames, Project, RealtimeEngine } from '../src/index.js';

/** A small project with one audio track + clip; returns the ids alongside it. */
function buildProject(): { project: Project; track: number; clip: number } {
  const project = Project.create();
  project.setSampleRate(48000);
  const track = project.addTrack({ kind: 'audio', name: 'lead' });
  const audio = new Float32Array(480);
  for (let i = 0; i < audio.length; i++) {
    audio[i] = Math.sin(i * 0.05) * 0.25;
  }
  const clip = project.addClip({
    trackId: track,
    startPpq: 0,
    lengthPpq: 4,
    gain: 0.8,
    audio,
    audioChannels: 1,
    audioSampleRate: 48000,
  });
  return { project, track, clip };
}

describe('Project edit ops (new bindings)', () => {
  it('removeClip removes a clip and undo restores it', () => {
    const project = Project.create();
    const track = project.addTrack({ kind: 'audio', name: 'a' });
    const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4, audioChannels: 0 });
    const before = project.toJson();
    project.removeClip(clip);
    expect(project.toJson()).not.toBe(before);
    project.undo();
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('setClipGain mutates and undoes', () => {
    const project = Project.create();
    const track = project.addTrack({ kind: 'audio' });
    const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4, audioChannels: 0 });
    const before = project.toJson();
    project.setClipGain(clip, 0.5);
    expect(project.toJson()).not.toBe(before);
    project.undo();
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('setClipFade sets fade in/out and undoes', () => {
    const project = Project.create();
    const track = project.addTrack({ kind: 'audio' });
    const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4, audioChannels: 0 });
    const before = project.toJson();
    project.setClipFade(clip, { lengthPpq: 0.5, curve: 1 }, { lengthPpq: 0.25 });
    expect(project.toJson()).not.toBe(before);
    project.undo();
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('setClipFade accepts named fade curves', () => {
    const project = Project.create();
    const track = project.addTrack({ kind: 'audio' });
    const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4, audioChannels: 0 });
    project.setClipFade(clip, { lengthPpq: 0.5, curve: 'equalPower' }, { curve: 'logarithmic' });
    const json = project.toJson();
    expect(json).toContain('"fade_in":{"curve":1,"length_ppq":0.5}');
    expect(json).toContain('"fade_out":{"curve":3,"length_ppq":0}');
    project.destroy();
  });

  it('setClipFade accepts omitted sides and partial fade objects', () => {
    const project = Project.create();
    const track = project.addTrack({ kind: 'audio' });
    const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4, audioChannels: 0 });

    expect(() => project.setClipFade(clip, { curve: 1 })).not.toThrow();
    let json = project.toJson();
    expect(json).toContain('"fade_in":{"curve":1,"length_ppq":0}');
    expect(json).toContain('"fade_out":{"curve":0,"length_ppq":0}');

    expect(() => project.setClipFade(clip, {}, { curve: 1 })).not.toThrow();
    json = project.toJson();
    expect(json).toContain('"fade_in":{"curve":0,"length_ppq":0}');
    expect(json).toContain('"fade_out":{"curve":1,"length_ppq":0}');

    expect(() =>
      project.setClipFade(clip, {
        lengthPpq: null as unknown as number,
        curve: null as unknown as number,
      }),
    ).not.toThrow();
    json = project.toJson();
    expect(json).toContain('"fade_in":{"curve":0,"length_ppq":0}');
    expect(json).toContain('"fade_out":{"curve":0,"length_ppq":0}');
    project.destroy();
  });

  it('setClipLoop enables looping and undoes', () => {
    const project = Project.create();
    const track = project.addTrack({ kind: 'audio' });
    const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4, audioChannels: 0 });
    const before = project.toJson();
    project.setClipLoop(clip, 1, 2);
    expect(project.toJson()).not.toBe(before);
    project.undo();
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('setClipLoop accepts named loop modes', () => {
    const project = Project.create();
    const track = project.addTrack({ kind: 'audio' });
    const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4, audioChannels: 0 });
    project.setClipLoop(clip, 'loop', 2);
    expect(project.toJson()).toContain('"loop_mode":1');
    project.setClipLoop(clip, 'off');
    expect(project.toJson()).toContain('"loop_mode":0');
    project.destroy();
  });

  it('duplicateClip returns a fresh id and undoes', () => {
    const project = Project.create();
    const track = project.addTrack({ kind: 'audio' });
    const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4, audioChannels: 0 });
    const before = project.toJson();
    const dup = project.duplicateClip(clip, 8);
    expect(dup).toBeGreaterThan(0);
    expect(dup).not.toBe(clip);
    project.undo();
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('removeTrack removes a track and undo restores it', () => {
    const project = Project.create();
    const track = project.addTrack({ kind: 'midi', name: 'gone' });
    const before = project.toJson();
    project.removeTrack(track);
    expect(project.toJson()).not.toBe(before);
    project.undo();
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('renameTrack changes the name and undoes', () => {
    const project = Project.create();
    const track = project.addTrack({ kind: 'audio', name: 'old' });
    project.renameTrack(track, 'renamed');
    expect(project.toJson()).toContain('renamed');
    project.undo();
    expect(project.toJson()).toContain('old');
    project.destroy();
  });

  it('setTrackRoute sets strip / output and undoes', () => {
    const project = Project.create();
    const track = project.addTrack({ kind: 'audio' });
    const before = project.toJson();
    project.setTrackRoute(track, 'strip-1', 'master');
    const after = project.toJson();
    expect(after).not.toBe(before);
    expect(after).toContain('strip-1');
    project.undo();
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('addAutomationLane returns an index; edit + remove undo cleanly', () => {
    const project = Project.create();
    const track = project.addTrack({ kind: 'audio' });
    const index = project.addAutomationLane(track, {
      targetParamId: 1,
      points: [
        { ppq: 0, value: 0, curve: 0 },
        { ppq: 4, value: 1, curveToNext: 'exponential' },
      ],
    });
    expect(index).toBe(0);

    const afterAdd = project.toJson();
    project.editAutomationLane(track, index, {
      targetParamId: 1,
      points: [{ ppq: 0, value: 0.5 }],
    });
    expect(project.toJson()).not.toBe(afterAdd);
    project.undo();
    expect(project.toJson()).toBe(afterAdd);

    project.removeAutomationLane(track, index);
    expect(project.toJson()).not.toBe(afterAdd);
    project.undo();
    expect(project.toJson()).toBe(afterAdd);

    project.destroy();
  });

  it('setClipSource rejects an unknown source id', () => {
    const { project, clip } = buildProject();
    expect(() => project.setClipSource(clip, 999999)).toThrow();
    project.destroy();
  });
});

describe('Project annotation bindings', () => {
  it('annotateKeys installs a key stream (undoable)', () => {
    const project = Project.create();
    project.addTrack({ kind: 'audio' });
    const before = project.toJson();
    project.annotateKeys([
      { startPpq: 0, endPpq: 16, tonicPc: 0, mode: 1 },
      { startPpq: 16, endPpq: 32, tonicPc: 9, mode: 2 },
    ]);
    expect(project.toJson()).not.toBe(before);
    project.undo();
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('annotateChords installs a chord stream (undoable)', () => {
    const project = Project.create();
    project.addTrack({ kind: 'audio' });
    const before = project.toJson();
    project.annotateChords([
      { startPpq: 0, endPpq: 4, rootPc: 0, quality: 1, extensions: [7], romanNumeral: 'I' },
      {
        startPpq: 4,
        endPpq: 8,
        rootPc: 7,
        quality: 5,
        extensions: [10],
        slashBassPc: 11,
        modulationBoundary: true,
      },
    ]);
    expect(project.toJson()).not.toBe(before);
    project.undo();
    expect(project.toJson()).toBe(before);
    project.destroy();
  });
});

describe('Project assist sidecar bindings', () => {
  it('round-trips a sidecar through set / count / get / list', () => {
    const project = Project.create();
    const track = project.addTrack({ kind: 'audio' });
    const payload = new Uint8Array([1, 2, 3, 4, 5]);
    project.setAssistSidecar({
      moduleId: 'demo.module',
      schemaVersion: 2,
      targetTrackId: track,
      regionStartPpq: 0,
      regionEndPpq: 16,
      payload,
    });

    expect(project.assistSidecarCount()).toBe(1);

    const got = project.getAssistSidecar(0);
    expect(got.moduleId).toBe('demo.module');
    expect(got.schemaVersion).toBe(2);
    expect(got.targetTrackId).toBe(track);
    expect(got.regionStartPpq).toBe(0);
    expect(got.regionEndPpq).toBe(16);
    expect(Array.from(got.payload)).toEqual([1, 2, 3, 4, 5]);

    const list = project.assistSidecars();
    expect(list).toHaveLength(1);
    expect(list[0].moduleId).toBe('demo.module');

    project.destroy();
  });

  it('replaces a sidecar with the same key (undoable)', () => {
    const project = Project.create();
    project.setAssistSidecar({ moduleId: 'm', payload: new Uint8Array([1]) });
    expect(project.assistSidecarCount()).toBe(1);
    project.setAssistSidecar({ moduleId: 'm', payload: new Uint8Array([2, 3]) });
    expect(project.assistSidecarCount()).toBe(1);
    expect(Array.from(project.getAssistSidecar(0).payload)).toEqual([2, 3]);
    project.destroy();
  });
});

describe('RealtimeEngine MIDI / parameter bindings', () => {
  function preparedEngine(): RealtimeEngine {
    return new RealtimeEngine(48000, 128, 1024, 1024);
  }

  it('pushMidiCc and pushMidiPanic do not throw on a prepared engine', () => {
    const engine = preparedEngine();
    expect(() => engine.pushMidiCc(0, 0, 0, 7, 100)).not.toThrow();
    expect(() => engine.pushMidiCc(0, 0, 0, 7, 100, -1)).not.toThrow();
    expect(() => engine.pushMidiPanic()).not.toThrow();
    expect(() => engine.pushMidiPanic(-1)).not.toThrow();
    engine.destroy();
  });

  it('clearParameters lets a duplicate id be re-registered', () => {
    const engine = preparedEngine();
    const info = {
      id: 42,
      name: 'gain',
      unit: 'dB',
      minValue: -60,
      maxValue: 12,
      defaultValue: 0,
      rtSafe: true,
      defaultCurve: 0 as const,
    };
    engine.addParameter(info);
    expect(engine.parameterCount()).toBe(1);
    // Adding a duplicate id is rejected.
    expect(() => engine.addParameter(info)).toThrow();
    engine.clearParameters();
    expect(engine.parameterCount()).toBe(0);
    // After clearing, the same id registers again.
    expect(() => engine.addParameter(info)).not.toThrow();
    expect(engine.parameterCount()).toBe(1);
    engine.destroy();
  });
});

describe('masteringInsertNames', () => {
  it('returns FX insert names as a string array', () => {
    const names = masteringInsertNames();
    expect(Array.isArray(names)).toBe(true);
    expect(names.length).toBeGreaterThan(0);
    for (const name of names) {
      expect(typeof name).toBe('string');
      expect(name.length).toBeGreaterThan(0);
    }
  });
});
