import { describe, expect, it } from 'vitest';
import { ErrorCode, isSonareError, Project, RealtimeEngine } from '../src/index.js';

describe('unknown mixing scene keys', () => {
  it('Project.setMixerSceneJson returns one warning per unknown key', () => {
    const project = Project.create();
    try {
      const typo =
        '{"version":1,"$schema":"s","strips":[{"id":"lead","faderDB":-3,"x-note":1}],"buses":[]}';
      expect(project.setMixerSceneJson(typo)).toEqual(["unknown scene key 'strips[0].faderDB'"]);
      expect(project.setMixerSceneJson('{"version":1,"strips":[{"id":"lead"}]}')).toEqual([]);
    } finally {
      project.destroy();
    }
  });

  it('engine strip setters refuse an unknown key by name', () => {
    const engine = new RealtimeEngine(48000, 256);
    try {
      engine.setTrackLanes([10]);
      const typo = '{"version":1,"strips":[{"id":"s","faderDB":-3}],"buses":[]}';
      for (const call of [
        () => engine.setTrackStripJson(10, typo),
        () => engine.setMasterStripJson(typo),
      ]) {
        let error: unknown;
        try {
          call();
        } catch (caught) {
          error = caught;
        }
        expect(isSonareError(error)).toBe(true);
        if (!isSonareError(error)) {
          throw new Error('expected SonareError');
        }
        expect(error.code).toBe(ErrorCode.InvalidParameter);
        expect(error.message).toContain("unknown strip key 'strips[0].faderDB'");
      }
      const annotated = '{"$schema":"s","version":1,"strips":[{"id":"s","faderDb":-3,"x-t":1}]}';
      expect(() => engine.setTrackStripJson(10, annotated)).not.toThrow();
    } finally {
      engine.destroy();
    }
  });
});
