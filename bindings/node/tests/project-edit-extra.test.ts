import { describe, expect, it } from 'vitest';
import type {
  ProjectAutomationPoint,
  ProjectFadeCurve,
  ProjectLoopRecordingDesc,
} from '../src/index.js';
import { masteringInsertNames, Project, RealtimeEngine } from '../src/index.js';
import { addon } from '../src/native.js';
import { expectRefusal } from './_helpers.js';

/** Per clip, the ids of the takes the project JSON marks partial. */
function partialTakeIds(project: { toJson(): string }): number[][] {
  const { clips } = JSON.parse(project.toJson()) as {
    clips: Array<{ takes?: Array<{ id: number; partial?: boolean }> }>;
  };
  return clips.map((clip) =>
    (clip.takes ?? []).filter((take) => take.partial).map((take) => take.id),
  );
}

/** A small project with one audio track + clip; returns the ids alongside it. */
function buildProject(): { project: Project; track: number; clip: number } {
  const project = new Project();
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
    channels: [audio],
    sampleRate: 48000,
  });
  return { project, track, clip };
}

describe('Project edit ops (new bindings)', () => {
  it('removeClip removes a clip and undo restores it', () => {
    const { project, clip } = buildProject();
    const before = project.toJson();
    project.removeClip(clip);
    // removeClip reclaims the now-unreferenced source registry entry as well
    // as decoded PCM, rather than leaving a serialized source orphan behind.
    expect(project.toJson()).toContain('"sources":[]');
    project.undo();
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('setClipGain mutates and undoes', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio' });
    const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4 });
    const before = project.toJson();
    project.setClipGain(clip, 0.5);
    expect(project.toJson()).not.toBe(before);
    project.undo();
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('setClipFade sets fade in/out and undoes', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio' });
    const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4 });
    const before = project.toJson();
    project.setClipFade(clip, {
      fadeInPpq: 0.5,
      fadeInCurve: 'equalPower',
      fadeOutPpq: 0.25,
    });
    expect(project.toJson()).not.toBe(before);
    project.undo();
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('setClipFade accepts named fade curves', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio' });
    const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4 });
    project.setClipFade(clip, {
      fadeInPpq: 0.5,
      fadeInCurve: 'exponential',
      fadeOutCurve: 'equalPower',
    });
    const json = project.toJson();
    expect(json).toContain('"fadeIn":{"curve":"exponential","lengthPpq":0.5}');
    expect(json).toContain('"fadeOut":{"curve":"equalPower","lengthPpq":0}');
    // Aliases and ordinals are refused rather than resolved.
    expect(() => project.setClipFade(clip, { fadeInCurve: 'EXP' as never })).toThrow(RangeError);
    expect(() => project.setClipFade(clip, { fadeOutCurve: 'equal_power' as never })).toThrow(
      RangeError,
    );
    expect(() => project.setClipFade(clip, { fadeInCurve: 1 as never })).toThrow(TypeError);
    project.destroy();
  });

  it('setClipFade accepts omitted sides and partial fade objects', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio' });
    const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4 });

    expect(() => project.setClipFade(clip, { fadeInCurve: 'equalPower' })).not.toThrow();
    let json = project.toJson();
    expect(json).toContain('"fadeIn":{"curve":"equalPower","lengthPpq":0}');
    expect(json).toContain('"fadeOut":{"curve":"linear","lengthPpq":0}');

    expect(() => project.setClipFade(clip, { fadeOutCurve: 'equalPower' })).not.toThrow();
    json = project.toJson();
    expect(json).toContain('"fadeIn":{"curve":"linear","lengthPpq":0}');
    expect(json).toContain('"fadeOut":{"curve":"equalPower","lengthPpq":0}');

    expect(() =>
      project.setClipFade(clip, {
        fadeInPpq: null as unknown as number,
        fadeInCurve: null as unknown as ProjectFadeCurve,
        fadeOutPpq: null as unknown as number,
        fadeOutCurve: null as unknown as ProjectFadeCurve,
      }),
    ).not.toThrow();
    json = project.toJson();
    expect(json).toContain('"fadeIn":{"curve":"linear","lengthPpq":0}');
    expect(json).toContain('"fadeOut":{"curve":"linear","lengthPpq":0}');
    project.destroy();
  });

  it('setClipTakes and setClipCompSegments serialize and undo', () => {
    const { project, clip } = buildProject();
    const before = project.toJson();
    project.setClipTakes(
      clip,
      [
        { id: 1, sourceOffsetPpq: 0, name: 'take A' },
        { id: 2, sourceOffsetPpq: 0.5, name: 'take B' },
      ],
      { activeTakeId: 1 },
    );
    const withTakes = project.toJson();
    expect(withTakes).not.toBe(before);
    expect(withTakes).toContain('"takes"');
    expect(withTakes).toContain('"activeTakeId":1');

    project.setClipCompSegments(clip, [
      { startPpq: 0, endPpq: 2, takeId: 1 },
      { startPpq: 2, endPpq: 4, takeId: 2, crossfadePpq: 0.25 },
    ]);
    const withComp = project.toJson();
    expect(withComp).toContain('"compSegments"');
    expect(withComp).toContain('"crossfadePpq":0.25');

    project.undo();
    expect(project.toJson()).toBe(withTakes);
    project.undo();
    expect(project.toJson()).toBe(before);
    project.redo();
    expect(project.toJson()).toBe(withTakes);

    expect(() =>
      project.setClipTakes(
        clip,
        [
          { id: 1, name: 'duplicate A' },
          { id: 1, name: 'duplicate B' },
        ],
        { activeTakeId: 1 },
      ),
    ).toThrow();
    expect(() =>
      project.setClipCompSegments(clip, [{ startPpq: 0, endPpq: 1, takeId: 99 }]),
    ).toThrow();
    project.destroy();
  });

  it('refuses an undeclared take or comp-segment field by its path without mutating', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio' });
      const clip = project.addClip({
        trackId,
        startPpq: 0,
        lengthPpq: 4,
        sourceUri: 'asset://a.wav',
      });
      const before = project.toJson();
      expect(() => project.setClipTakes(clip, [{ id: 1, sorceOffsetPpq: 2 } as never])).toThrow(
        new TypeError(
          'Project.setClipTakes: unknown key takes[0].sorceOffsetPpq (expected id, name, sourceId, sourceOffsetPpq)',
        ),
      );
      expect(() =>
        project.setClipCompSegments(clip, [
          { startPpq: 0, endPpq: 2 },
          { startPpq: 2, endPpq: 4, crossfadePq: 0.25 } as never,
        ]),
      ).toThrow(
        new TypeError(
          'Project.setClipCompSegments: unknown key segments[1].crossfadePq (expected crossfadePpq, endPpq, startPpq, takeId)',
        ),
      );
      expect(project.toJson()).toBe(before);
    } finally {
      project.destroy();
    }
  });

  it('setClipTakes and setClipCompSegments reject missing required fields without mutating', () => {
    const { project, clip } = buildProject();
    const before = project.toJson();

    expect(() =>
      project.setClipTakes(
        clip,
        [{ sourceId: 5 }, { id: 2 }] as unknown as Parameters<typeof project.setClipTakes>[1],
        { activeTakeId: 1 },
      ),
    ).toThrow(/id must be an integer/);
    expect(project.toJson()).toBe(before);

    expect(() =>
      project.setClipCompSegments(clip, [
        { endPpq: 1, takeId: 1 },
        { startPpq: 1, endPpq: 2, takeId: 2 },
      ] as unknown as Parameters<typeof project.setClipCompSegments>[1]),
    ).toThrow(/startPpq must be a number/);
    expect(project.toJson()).toBe(before);

    expect(() =>
      project.setClipCompSegments(clip, [
        { startPpq: 0, takeId: 1 },
        { startPpq: 1, endPpq: 2, takeId: 2 },
      ] as unknown as Parameters<typeof project.setClipCompSegments>[1]),
    ).toThrow(/endPpq must be a number/);
    expect(project.toJson()).toBe(before);

    // crossfadePpq is optional (DoubleProperty), so a wrong-typed value is
    // refused by name rather than silently substituted.
    expect(() =>
      project.setClipCompSegments(clip, [
        { startPpq: 0, endPpq: 1, takeId: 1, crossfadePpq: 'nope' },
      ] as unknown as Parameters<typeof project.setClipCompSegments>[1]),
    ).toThrow(/crossfadePpq must be a number/);
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('addLoopRecordingTakes splits captured loops into takes', () => {
    const project = new Project();
    project.setSampleRate(48000);
    const track = project.addTrack({ kind: 'audio', name: 'record' });
    const before = project.toJson();
    const audio = new Float32Array(48000);
    audio.fill(0.25, 0, 24000);
    audio.fill(0.75, 24000);

    const result = project.addLoopRecordingTakes({
      trackId: track,
      startPpq: 0,
      loopLengthPpq: 1,
      audio,
      audioChannels: 1,
      audioSampleRate: 48000,
    });
    expect(result.clipId).toBeGreaterThan(0);
    expect(result.takeCount).toBe(2);
    expect(project.toJson()).not.toContain('"partial"');
    expect(project.toJson()).toContain('"activeTakeId":2');
    expect(project.sources().length).toBe(2);
    const added = project.toJson();
    project.undo();
    expect(project.toJson()).toBe(before);
    expect(project.sources().length).toBe(0);
    project.redo();
    expect(project.toJson()).toBe(added);
    expect(project.sources().length).toBe(2);
    project.destroy();
  });

  it('addLoopRecordingTakes keeps a partial last take inactive and honours partialTail', () => {
    const project = new Project();
    project.setSampleRate(48000);
    const track = project.addTrack({ kind: 'audio', name: 'record' });
    const base = {
      trackId: track,
      startPpq: 0,
      loopLengthPpq: 1,
      audioChannels: 1,
      audioSampleRate: 48000,
    };
    const kept = project.addLoopRecordingTakes({
      ...base,
      startPpq: 0,
      audio: new Float32Array(36000).fill(0.5),
    });
    expect(kept.takeCount).toBe(2);
    expect(partialTakeIds(project)).toEqual([[2]]);
    expect(project.toJson()).toContain('"activeTakeId":1');
    const dropped = project.addLoopRecordingTakes({
      ...base,
      startPpq: 4,
      audio: new Float32Array(36000).fill(0.5),
      partialTail: 'drop',
    });
    expect(dropped.takeCount).toBe(1);
    expect(partialTakeIds(project)).toEqual([[2], []]);
    const short = project.addLoopRecordingTakes({
      ...base,
      startPpq: 8,
      audio: new Float32Array(12000).fill(0.5),
    });
    expect(short.takeCount).toBe(1);
    expect(partialTakeIds(project)).toEqual([[2], [], [1]]);
    expect(() =>
      project.addLoopRecordingTakes({
        ...base,
        startPpq: 12,
        audio: new Float32Array(100),
        partialTail: 'activate' as unknown as 'keep',
      }),
    ).toThrow(/partialTail/);
    project.destroy();
  });

  it('addLoopRecordingTakes treats a one-frame remainder as rounding and the partial mark round-trips', () => {
    const project = new Project();
    project.setSampleRate(48000);
    const track = project.addTrack({ kind: 'audio', name: 'record' });
    const base = { trackId: track, loopLengthPpq: 1, audioChannels: 1, audioSampleRate: 48000 };
    const extra = project.addLoopRecordingTakes({
      ...base,
      startPpq: 0,
      audio: new Float32Array(48001).fill(0.5),
    });
    expect(extra.takeCount).toBe(2);
    expect(partialTakeIds(project)).toEqual([[]]);
    project.addLoopRecordingTakes({
      ...base,
      startPpq: 4,
      audio: new Float32Array(36000).fill(0.5),
    });
    expect(partialTakeIds(project)).toEqual([[], [2]]);
    const reloaded = Project.fromJson(project.toJson());
    expect(partialTakeIds(reloaded)).toEqual([[], [2]]);
    reloaded.destroy();
    project.destroy();
  });

  it('addLoopRecordingTakes accepts planar audio equal to the interleaved form', () => {
    const left = new Float32Array(48000).map((_, i) => i / 48000 - 0.5);
    const right = new Float32Array(48000).map((_, i) => 0.5 - i / 48000);
    const interleaved = new Float32Array(96000);
    for (let i = 0; i < 48000; i++) {
      interleaved[2 * i] = left[i];
      interleaved[2 * i + 1] = right[i];
    }
    const run = (
      capture: { audio: Float32Array; audioChannels: number } | { audio: Float32Array[] },
    ) => {
      const project = new Project();
      project.setSampleRate(48000);
      const track = project.addTrack({ kind: 'audio', name: 'record' });
      project.addLoopRecordingTakes({
        trackId: track,
        startPpq: 0,
        loopLengthPpq: 1,
        ...capture,
        audioSampleRate: 48000,
      });
      const json = project.toJson();
      project.destroy();
      return json;
    };
    expect(run({ audio: [left, right] })).toBe(run({ audio: interleaved, audioChannels: 2 }));
  });

  it('addLoopRecordingTakes refuses planar channels of different lengths', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio', name: 'record' });
    expect(() =>
      project.addLoopRecordingTakes({
        trackId: track,
        startPpq: 0,
        loopLengthPpq: 1,
        audio: [new Float32Array(100), new Float32Array(99)],
        audioSampleRate: 48000,
      }),
    ).toThrow(RangeError);
    project.destroy();
  });

  it('addLoopRecordingTakes refuses interleaved audio without its channel count or sample rate', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio', name: 'record' });
    const desc = { trackId: track, loopLengthPpq: 1, audio: new Float32Array(100) };
    const add = (partial: object) => () =>
      project.addLoopRecordingTakes(partial as unknown as ProjectLoopRecordingDesc);
    expectRefusal(add({ ...desc, audioSampleRate: 48000 }), TypeError, /audioChannels/);
    expectRefusal(add({ ...desc, audioChannels: 1 }), TypeError, /audioSampleRate/);
    project.destroy();
  });

  it('addLoopRecordingTakes refuses audioChannels beside planar audio', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio', name: 'record' });
    const planar = [new Float32Array(100), new Float32Array(100)];
    const desc = { trackId: track, loopLengthPpq: 1, audio: planar, audioSampleRate: 48000 };
    const add = (partial: object) => () =>
      project.addLoopRecordingTakes(partial as unknown as ProjectLoopRecordingDesc);
    expectRefusal(add({ ...desc, audioChannels: 2 }), TypeError, /audioChannels/);
    expect(add(desc)).not.toThrow(); // positive control
    project.destroy();
  });

  it('rejects planar clip channels of unequal length and interleaved loop audio that does not divide', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio', name: 'record' });
    expect(() =>
      project.addClip({
        trackId: track,
        startPpq: 0,
        lengthPpq: 1,
        channels: [new Float32Array(5), new Float32Array(4)],
        sampleRate: 48000,
      }),
    ).toThrow(/must all have the same length/);
    expect(() =>
      project.addLoopRecordingTakes({
        trackId: track,
        startPpq: 0,
        loopLengthPpq: 1,
        audio: new Float32Array(5),
        audioChannels: 2,
        audioSampleRate: 48000,
      }),
    ).toThrow(/audio length must be a multiple of audioChannels/);
    project.destroy();
  });

  it('setClipLoop enables looping and undoes', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio' });
    const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4 });
    const before = project.toJson();
    project.setClipLoop(clip, { loopMode: 'loop', loopLengthPpq: 2 });
    expect(project.toJson()).not.toBe(before);
    project.undo();
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('setClipLoop accepts named loop modes', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio' });
    const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4 });
    project.setClipLoop(clip, { loopMode: 'loop', loopLengthPpq: 2 });
    expect(project.toJson()).toContain('"loopMode":"loop"');
    project.setClipLoop(clip, { loopMode: 'off' });
    expect(project.toJson()).toContain('"loopMode":"off"');
    project.destroy();
  });

  it('setClipLoop accepts an optional loop crossfade and rejects a negative one', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio' });
    const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4 });
    project.setClipLoop(clip, { loopMode: 'loop', loopLengthPpq: 2, loopCrossfadePpq: 0.5 });
    expect(project.toJson()).toContain('loopCrossfadePpq');
    expect(() =>
      project.setClipLoop(clip, { loopMode: 'loop', loopLengthPpq: 2, loopCrossfadePpq: -1 }),
    ).toThrow();
    project.destroy();
  });

  it('duplicateClip returns a fresh id and undoes', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio' });
    const clip = project.addClip({ trackId: track, startPpq: 0, lengthPpq: 4 });
    const before = project.toJson();
    const dup = project.duplicateClip(clip, 8);
    expect(dup).toBeGreaterThan(0);
    expect(dup).not.toBe(clip);
    project.undo();
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('removeTrack removes a track and undo restores it', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'midi', name: 'gone' });
    const before = project.toJson();
    project.removeTrack(track);
    expect(project.toJson()).not.toBe(before);
    project.undo();
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('renameTrack changes the name and undoes', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio', name: 'old' });
    project.renameTrack(track, 'renamed');
    expect(project.toJson()).toContain('renamed');
    project.undo();
    expect(project.toJson()).toContain('old');
    project.destroy();
  });

  it('setTrackRoute sets strip / output and undoes', () => {
    const project = new Project();
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

  it('setTrackRoute refuses empty fields and clears omitted fields', () => {
    const project = new Project();
    try {
      const track = project.addTrack({ kind: 'audio' });
      project.setTrackRoute(track, 'strip-1', 'master');
      const before = project.toJson();
      expect(() => project.setTrackRoute(undefined as never)).toThrow(
        new TypeError('Project.setTrackRoute: trackId must be a number'),
      );
      expect(() => project.setTrackRoute(track, '', 'master')).toThrow(
        new RangeError('Project.setTrackRoute: channelStripRef must not be empty'),
      );
      expect(() => project.setTrackRoute(track, 'strip-1', '')).toThrow(
        new RangeError('Project.setTrackRoute: outputTarget must not be empty'),
      );
      expect(project.toJson()).toBe(before);
      project.setTrackRoute(track);
      expect(project.tracks()[0]).not.toHaveProperty('channelStripRef');
      expect(project.tracks()[0]).not.toHaveProperty('outputTarget');
    } finally {
      project.destroy();
    }
  });

  it('addAutomationLane returns its stable lane id; edit + remove undo cleanly', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio' });
    const laneId = project.addAutomationLane(track, {
      targetParamId: 99,
      points: [
        { ppq: 0, value: 0, curveToNext: 'linear' },
        { ppq: 4, value: 1, curveToNext: 'exponential' },
      ],
    });
    expect(laneId).not.toBe(99);

    const afterAdd = project.toJson();
    expect(afterAdd).not.toContain('"targetKind"');
    project.editAutomationLane(track, laneId, {
      targetKind: 'opaque',
      targetParamId: 1,
      points: [{ ppq: 0, value: 0.5 }],
    });
    expect(project.toJson()).not.toBe(afterAdd);
    project.undo();
    expect(project.toJson()).toBe(afterAdd);

    project.removeAutomationLane(track, laneId);
    expect(project.toJson()).not.toBe(afterAdd);
    project.undo();
    expect(project.toJson()).toBe(afterAdd);

    project.destroy();
  });

  it('adds typed fader and pan lanes and round-trips them through the project JSON', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio' });
    project.addAutomationLane(track, {
      targetKind: 'trackFaderDb',
      points: [{ ppq: 0, value: -3 }],
    });
    project.addAutomationLane(track, {
      targetKind: 'trackPan',
      points: [{ ppq: 0, value: 0 }],
    });

    const json = project.toJson();
    expect(json).toContain('"targetKind":"trackFaderDb"');
    expect(json).toContain('"targetKind":"trackPan"');

    const restored = Project.fromJson(json);
    expect(restored.toJson()).toBe(json);
    restored.destroy();
    project.destroy();
  });

  it('rejects a conflicting typed lane atomically', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio' });
    const faderLaneId = project.addAutomationLane(track, {
      targetKind: 'trackFaderDb',
      points: [{ ppq: 0, value: -3 }],
    });
    project.addAutomationLane(track, {
      targetKind: 'trackPan',
      points: [{ ppq: 0, value: 0 }],
    });
    const before = project.toJson();

    expect(() =>
      project.addAutomationLane(track, {
        targetKind: 'trackFaderDb',
        points: [{ ppq: 0, value: -6 }],
      }),
    ).toThrow();
    expect(project.toJson()).toBe(before);

    expect(() =>
      project.editAutomationLane(track, faderLaneId, {
        targetKind: 'trackPan',
        points: [{ ppq: 0, value: -6 }],
      }),
    ).toThrow();
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('serializes a curveToNext token as written and refuses the curve key by name', () => {
    const project = new Project();
    try {
      const track = project.addTrack({ kind: 'audio' });
      project.addAutomationLane(track, {
        targetParamId: 1,
        points: [{ ppq: 0, value: 1, curveToNext: 'sCurve' }],
      });
      const lane = JSON.parse(project.toJson()) as {
        tracks: Array<{ automationLanes: Array<{ points: Array<{ curveToNext: string }> }> }>;
      };
      expect(lane.tracks[0].automationLanes[0].points[0].curveToNext).toBe('sCurve');
      const before = project.toJson();
      expect(() =>
        project.addAutomationLane(track, {
          targetParamId: 2,
          points: [{ ppq: 0, value: 1, curve: 'sCurve' } as unknown as ProjectAutomationPoint],
        }),
      ).toThrow(
        new TypeError(
          'Project.addAutomationLane: unknown key points[0].curve (expected curveToNext, ppq, value)',
        ),
      );
      expect(project.toJson()).toBe(before);
    } finally {
      project.destroy();
    }
  });

  it('refuses a curve ordinal and the retired scurve spelling', () => {
    const project = new Project();
    try {
      const track = project.addTrack({ kind: 'audio' });
      const lane = (curve: unknown) =>
        project.addAutomationLane(track, {
          targetParamId: 1,
          points: [{ ppq: 0, value: 1, curveToNext: curve as never }],
        });
      expect(() => lane(3)).toThrow(TypeError);
      expect(() => lane('scurve')).toThrow(RangeError);
    } finally {
      project.destroy();
    }
  });

  it('legacy automation edits preserve a typed lane kind', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio' });
    const laneId = project.addAutomationLane(track, {
      targetKind: 'trackFaderDb',
      points: [{ ppq: 0, value: -3 }],
    });
    project.editAutomationLane(track, laneId, {
      points: [{ ppq: 0, value: -6 }],
    });
    const json = project.toJson();
    expect(json).toContain('"targetKind":"trackFaderDb"');
    project.destroy();
  });

  it('rejects unknown, out-of-width, and non-finite automation target kinds', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio' });
    const before = project.toJson();
    // A name outside the set is a RangeError; any number is a TypeError.
    for (const [targetKind, error] of [
      ['unknown', RangeError],
      [3, TypeError],
      [Number.NaN, TypeError],
      [Number.POSITIVE_INFINITY, TypeError],
    ] as const) {
      expect(() =>
        project.addAutomationLane(track, {
          targetKind: targetKind as never,
          points: [{ ppq: 0, value: 0 }],
        }),
      ).toThrow(error);
      expect(project.toJson()).toBe(before);
    }
    project.destroy();
  });

  it('rejects zero as the reserved automation target id before mutating', () => {
    const project = new Project();
    const track = project.addTrack({ kind: 'audio' });
    const before = project.toJson();
    expect(() =>
      project.addAutomationLane(track, { targetParamId: 0, points: [{ ppq: 0, value: 1 }] }),
    ).toThrow(RangeError);
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('setClipSource rejects an unknown source id', () => {
    const { project, clip } = buildProject();
    expect(() => project.setClipSource(clip, 999999)).toThrow();
    project.destroy();
  });
});

describe('automation target kind token', () => {
  it('refuses numeric values at the addon boundary', () => {
    // biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose.
    const native = new (addon as any).Project();
    try {
      const track = native.addTrack({ kind: 0, name: 'lead' });
      expectRefusal(
        () =>
          native.addAutomationLane(track, {
            targetParamId: 7,
            targetKind: 3,
            points: [{ ppq: 0, value: 0 }],
          }),
        TypeError,
        /^Project\.addAutomationLane: targetKind must be a string$/,
      );
    } finally {
      native.destroy();
    }
  });
});

describe('Project annotation bindings', () => {
  it('annotateKeys installs a key stream (undoable)', () => {
    const project = new Project();
    project.addTrack({ kind: 'audio' });
    const before = project.toJson();
    project.annotateKeys([
      { startPpq: 0, endPpq: 16, tonicPc: 0, mode: 'major' },
      { startPpq: 16, endPpq: 32, tonicPc: 9, mode: 'minor' },
    ]);
    expect(project.toJson()).not.toBe(before);
    project.undo();
    expect(project.toJson()).toBe(before);
    project.destroy();
  });

  it('annotateChords installs a chord stream (undoable)', () => {
    const project = new Project();
    project.addTrack({ kind: 'audio' });
    const before = project.toJson();
    project.annotateChords([
      { startPpq: 0, endPpq: 4, rootPc: 0, quality: 'major', extensions: [7], romanNumeral: 'I' },
      {
        startPpq: 4,
        endPpq: 8,
        rootPc: 7,
        quality: 'dominant',
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
  it('round-trips a sidecar through set / list', () => {
    const project = new Project();
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

    expect(project.assistSidecars()).toHaveLength(1);

    const got = project.assistSidecars()[0];
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
    const project = new Project();
    project.setAssistSidecar({ moduleId: 'm', payload: new Uint8Array([1]) });
    expect(project.assistSidecars()).toHaveLength(1);
    project.setAssistSidecar({ moduleId: 'm', payload: new Uint8Array([2, 3]) });
    expect(project.assistSidecars()).toHaveLength(1);
    expect(Array.from(project.assistSidecars()[0].payload)).toEqual([2, 3]);
    project.destroy();
  });
});

describe('RealtimeEngine MIDI / parameter bindings', () => {
  function preparedEngine(): RealtimeEngine {
    return new RealtimeEngine({
      sampleRate: 48000,
      maxBlockSize: 128,
      commandCapacity: 1024,
      telemetryCapacity: 1024,
    });
  }

  it('pushMidiCc and pushMidiPanic do not throw on a prepared engine', () => {
    const engine = preparedEngine();
    expect(() => engine.pushMidiCc(0, 0, 0, 7, 100)).not.toThrow();
    expect(() => engine.pushMidiCc(0, 0, 0, 7, 100)).not.toThrow();
    expect(() => engine.pushMidiPanic()).not.toThrow();
    expect(() => engine.pushMidiPanic()).not.toThrow();
    engine.destroy();
  });

  it('pushMidiCc rejects MIDI bytes that would otherwise be truncated', () => {
    const engine = preparedEngine();
    expect(() => engine.pushMidiCc(0, 0, 0, 256, 100)).toThrow(RangeError);
    expect(() => engine.pushMidiCc(0, 0, 0, 7, 300)).toThrow(RangeError);
    expect(() => engine.pushMidiCc(0, 0, 0, 7.5, 100)).toThrow(RangeError);
    engine.destroy();
  });

  it('pushMidiSysex does not throw on a prepared engine', () => {
    const engine = preparedEngine();
    // GM System On (0xF0 0x7E 0x7F 0x09 0x01 0xF7).
    const gmSystemOn = new Uint8Array([0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7]);
    expect(() => engine.pushMidiSysex(0, gmSystemOn)).not.toThrow();
    expect(() => engine.pushMidiSysex(0, gmSystemOn)).not.toThrow();
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
      defaultCurve: 'linear' as const,
    };
    engine.addParameter(info);
    expect(engine.parameters().length).toBe(1);
    // Adding a duplicate id is rejected.
    expect(() => engine.addParameter(info)).toThrow();
    engine.clearParameters();
    expect(engine.parameters().length).toBe(0);
    // After clearing, the same id registers again.
    expect(() => engine.addParameter(info)).not.toThrow();
    expect(engine.parameters().length).toBe(1);
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

describe('wrong-typed domain fields', () => {
  it('refuses wrong-typed time-signature, automation target, warp map id and graph port fields as non-integers', () => {
    const bad = 'x' as unknown as number;
    const project = new Project();
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    try {
      const track = project.addTrack({ kind: 'audio' });
      const anchors = [
        { warpSample: 0, sourceSample: 0 },
        { warpSample: 10, sourceSample: 10 },
      ];
      const graph = {
        nodes: [
          { id: 'in', numPorts: bad },
          { id: 'out', numPorts: 1 },
        ],
        connections: [{ sourceNode: 'in', sourcePort: 0, destNode: 'out', destPort: 0 }],
        inputNode: 'in',
        outputNode: 'out',
      };
      const rows: Array<[string, () => unknown, RegExp]> = [
        [
          'numerator',
          () => project.setTimeSignatures([{ startPpq: 0, numerator: bad, denominator: 4 }]),
          /Project\.setTimeSignatures: segments\[0\]\.numerator must be an integer/,
        ],
        [
          'denominator',
          () => project.setTimeSignatures([{ startPpq: 0, numerator: 4, denominator: bad }]),
          /Project\.setTimeSignatures: segments\[0\]\.denominator must be an integer/,
        ],
        [
          'targetParamId',
          () =>
            project.addAutomationLane(track, {
              targetParamId: bad,
              points: [{ ppq: 0, value: 0 }],
            }),
          /Project\.addAutomationLane: targetParamId must be an integer/,
        ],
        [
          'warp map id',
          () => project.setWarpMap({ id: bad, anchors }),
          /Project\.setWarpMap: id must be an integer/,
        ],
        [
          'graph numPorts',
          () => engine.setGraph(graph),
          /RealtimeEngine\.setGraph: spec\.nodes\[0\]\.numPorts must be an integer/,
        ],
      ];
      for (const [, call, message] of rows) {
        expect(call).toThrow(TypeError);
        expect(call).toThrow(message);
      }
    } finally {
      engine.destroy();
      project.destroy();
    }
  });
});
