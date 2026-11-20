import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, isSonareError, Project, RealtimeEngine } from '../dist/index.js';

describe('unknown mixing scene keys (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('Project.setMixerSceneJson refuses an unknown key by name as InvalidFormat', () => {
    const project = new Project();
    try {
      const typo =
        '{"version":2,"$schema":"s","strips":[{"id":"lead","faderDB":-3,"x-note":1}],"buses":[]}';
      let error: unknown;
      try {
        project.setMixerSceneJson(typo);
      } catch (caught) {
        error = caught;
      }
      expect(isSonareError(error)).toBe(true);
      expect((error as { code: number }).code).toBe(ErrorCode.InvalidFormat);
      expect((error as Error).message).toContain('unknown key strips[0].faderDB');
      // `$`- and `x-`-prefixed keys are annotations, not unknown keys.
      const annotated = '{"version":2,"$schema":"s","strips":[{"id":"lead","x-note":1}]}';
      expect(project.setMixerSceneJson(annotated)).toBeUndefined();
    } finally {
      project.destroy();
    }
  });

  it('engine strip setters refuse an unknown key by name', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    try {
      engine.setTrackLanes([{ trackId: 10 }]);
      const typo = '{"version":2,"strips":[{"id":"s","faderDB":-3}],"buses":[]}';
      for (const call of [
        () => engine.setStrip(10, JSON.parse(typo).strips[0]),
        () => engine.setStrip('master', JSON.parse(typo).strips[0]),
      ]) {
        let error: unknown;
        try {
          call();
        } catch (caught) {
          error = caught;
        }
        expect(isSonareError(error)).toBe(true);
        expect((error as { code: number }).code).toBe(ErrorCode.InvalidFormat);
        expect((error as Error).message).toContain('unknown key strips[0].faderDB');
      }
      const annotated = '{"$schema":"s","version":2,"strips":[{"id":"s","faderDb":-3,"x-t":1}]}';
      expect(() => engine.setStrip(10, JSON.parse(annotated).strips[0])).not.toThrow();
    } finally {
      engine.destroy();
    }
  });
});
