import { describe, expect, it } from 'vitest';
import { ErrorCode, Project, SonareError } from '../src/index.js';

const UNKNOWN_ID = 987654;

/** Builds a project with one track and one clip, returning their ids. */
function withProject(body: (project: Project, trackId: number, clipId: number) => void): void {
  const project = new Project();
  try {
    const trackId = project.addTrack({ kind: 'audio', name: 'lead' });
    const clipId = project.addClip({
      trackId,
      startPpq: 0,
      lengthPpq: 4,
      sourceUri: 'asset://refusal.wav',
    });
    body(project, trackId, clipId);
  } finally {
    project.destroy();
  }
}

describe('Project value refusals are RangeError, not SonareError', () => {
  const cases: Array<[string, (p: Project, track: number, clip: number) => void]> = [
    ['setTrackPan out of range', (p, track) => p.setTrackPan(track, 2)],
    ['setTrackGain negative', (p, track) => p.setTrackGain(track, -1)],
    ['setTrackGain unknown track', (p) => p.setTrackGain(UNKNOWN_ID, 1)],
    ['removeTrack unknown id', (p) => p.removeTrack(UNKNOWN_ID)],
    ['removeClip unknown id', (p) => p.removeClip(UNKNOWN_ID)],
    ['setClipGain unknown clip', (p) => p.setClipGain(UNKNOWN_ID, 1)],
    ['moveClip unknown clip', (p) => p.moveClip(UNKNOWN_ID, 0)],
  ];

  it.each(cases)('%s', (_name, invoke) => {
    withProject((project, trackId, clipId) => {
      let caught: unknown;
      try {
        invoke(project, trackId, clipId);
      } catch (error) {
        caught = error;
      }
      expect(caught).toBeInstanceOf(RangeError);
      expect(caught).not.toBeInstanceOf(SonareError);
    });
  });
});

describe('Project state refusals stay coded SonareError', () => {
  it('undo on an empty history is InvalidState', () => {
    const project = new Project();
    try {
      expect(() => project.undo()).toThrow(
        expect.objectContaining({ code: ErrorCode.InvalidState }),
      );
    } finally {
      project.destroy();
    }
  });

  it('a destroyed project refuses with InvalidState', () => {
    const project = new Project();
    project.destroy();
    expect(() => project.tracks()).toThrow(
      expect.objectContaining({ code: ErrorCode.InvalidState }),
    );
    expect(() => project.setTrackPan(1, 0)).toThrow(
      expect.objectContaining({ code: ErrorCode.InvalidState }),
    );
  });
});

describe('Project edit targets require a position', () => {
  const cases: Array<[string, string, (p: Project, clip: number, at: number) => unknown]> = [
    ['addMidiClip', 'startPpq', (p, _clip, at) => p.addMidiClip(at, 4)],
    ['trimClip', 'newStartPpq', (p, clip, at) => p.trimClip(clip, at, 2)],
    ['moveClip', 'newStartPpq', (p, clip, at) => p.moveClip(clip, at)],
    [
      'duplicateClip',
      'newStartPpq',
      (p, _clip, at) => {
        const midi = p.addMidiClip(8, 4);
        return p.duplicateClip(midi.clipId, at);
      },
    ],
  ];

  it.each(cases)('%s refuses omission and accepts explicit zero', (_name, subject, edit) => {
    withProject((project, _track, clip) => {
      const missing = () => edit(project, clip, undefined as never);
      expect(missing).toThrow(TypeError);
      expect(missing).toThrow(subject);
      edit(project, clip, 0);
    });
  });
});

describe('Project descriptor starts require a position', () => {
  it('addClip requires startPpq and accepts explicit zero', () => {
    withProject((project) => {
      const trackId = project.addTrack({ kind: 'audio' });
      const before = project.toJson();
      const missing = () =>
        project.addClip({ trackId, lengthPpq: 4, sourceUri: 'asset://required.wav' } as never);
      expect(missing).toThrow(new TypeError('Project.addClip: startPpq must be a number'));
      expect(project.toJson()).toBe(before);

      const clipId = project.addClip({
        trackId,
        startPpq: 0,
        lengthPpq: 4,
        sourceUri: 'asset://required.wav',
      });
      expect(clipId).toBeGreaterThan(0);
      expect(project.clips().find((clip) => clip.id === clipId)?.startPpq).toBe(0);
    });
  });

  it('addLoopRecordingTakes requires startPpq and accepts explicit zero', () => {
    withProject((project) => {
      const trackId = project.addTrack({ kind: 'audio' });
      const audio = new Float32Array(480).fill(0.25);
      const before = project.toJson();
      const missing = () =>
        project.addLoopRecordingTakes({
          trackId,
          loopLengthPpq: 1,
          audio,
          audioChannels: 1,
          audioSampleRate: 48000,
        } as never);
      expect(missing).toThrow(
        new TypeError('Project.addLoopRecordingTakes: startPpq must be a number'),
      );
      expect(project.toJson()).toBe(before);

      const result = project.addLoopRecordingTakes({
        trackId,
        startPpq: 0,
        loopLengthPpq: 1,
        audio,
        audioChannels: 1,
        audioSampleRate: 48000,
      });
      expect(result.clipId).toBeGreaterThan(0);
      expect(project.clips().find((clip) => clip.id === result.clipId)?.startPpq).toBe(0);
    });
  });
});
