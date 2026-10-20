/**
 * A caller's refused value on the WASM `Project` is a RangeError; a refusal caused by the
 * object's state stays a coded SonareError.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, isSonareError, Project } from '../dist/index.js';
import { expectArgumentRefusal } from './_helpers';

function thrownBy(call: () => unknown): unknown {
  try {
    call();
  } catch (error) {
    return error;
  }
  return undefined;
}

describe('Project argument refusal', () => {
  beforeAll(async () => {
    await init();
  });

  const UNKNOWN_ID = 999999;

  const valueRefusals: Array<[string, (project: Project, trackId: number) => unknown]> = [
    ['setTrackPan out of range', (p, t) => p.setTrackPan(t, 2)],
    ['setTrackGain unknown track', (p) => p.setTrackGain(UNKNOWN_ID, 1)],
    ['removeTrack unknown track', (p) => p.removeTrack(UNKNOWN_ID)],
    ['removeClip unknown clip', (p) => p.removeClip(UNKNOWN_ID)],
    ['setClipGain unknown clip', (p) => p.setClipGain(UNKNOWN_ID, 0.5)],
    ['setSampleRate fractional', (p) => p.setSampleRate(44100.5)],
  ];

  it.each(valueRefusals)('%s is a RangeError, not a SonareError', (name, call) => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio', name: 'lead' });
      const caught = thrownBy(() => call(project, trackId));
      expectArgumentRefusal(caught, RangeError, name);
      expect(isSonareError(caught)).toBe(false);
    } finally {
      project.delete();
    }
  });

  it('undo on an empty history stays a coded InvalidState', () => {
    const project = new Project();
    try {
      const caught = thrownBy(() => project.undo());
      expect(isSonareError(caught)).toBe(true);
      expect(isSonareError(caught) && caught.code).toBe(ErrorCode.InvalidState);
      expect(caught).not.toBeInstanceOf(RangeError);
    } finally {
      project.delete();
    }
  });

  it('a call on a released project stays a coded InvalidState', () => {
    const project = new Project();
    project.delete();
    const caught = thrownBy(() => project.setTrackPan(1, 0));
    expect(isSonareError(caught)).toBe(true);
    expect(isSonareError(caught) && caught.code).toBe(ErrorCode.InvalidState);
    expect(caught).not.toBeInstanceOf(RangeError);
  });
});
