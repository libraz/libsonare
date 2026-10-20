import { describe, expect, it } from 'vitest';
import { ErrorCode, Project, SonareError } from '../src/index.js';

const UNKNOWN_ID = 987654;

/** Builds a project with one track and one clip, returning their ids. */
function withProject(body: (project: Project, trackId: number, clipId: number) => void): void {
  const project = Project.create();
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
    ['trackByIndex past the count', (p) => p.trackByIndex(99)],
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
    const project = Project.create();
    try {
      expect(() => project.undo()).toThrow(
        expect.objectContaining({ code: ErrorCode.InvalidState }),
      );
    } finally {
      project.destroy();
    }
  });

  it('a destroyed project refuses with InvalidState', () => {
    const project = Project.create();
    project.destroy();
    expect(() => project.trackCount()).toThrow(
      expect.objectContaining({ code: ErrorCode.InvalidState }),
    );
    expect(() => project.setTrackPan(1, 0)).toThrow(
      expect.objectContaining({ code: ErrorCode.InvalidState }),
    );
  });
});
