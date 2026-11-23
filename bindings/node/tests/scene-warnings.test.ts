import { describe, expect, it } from 'vitest';
import type { MixSceneStrip } from '../src/index.js';
import { ErrorCode, isSonareError, Project, RealtimeEngine } from '../src/index.js';

/** First strip / bus of a scene document: the object the engine strip setters take. */
const sceneStrip = (json: string): MixSceneStrip => JSON.parse(json).strips[0];

describe('unknown mixing scene keys', () => {
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
        () => engine.setStrip(10, sceneStrip(typo)),
        () => engine.setStrip('master', sceneStrip(typo)),
      ]) {
        let error: unknown;
        try {
          call();
        } catch (caught) {
          error = caught;
        }
        // A key refused inside the settings object is a refused argument, not a document error.
        expect(error).toBeInstanceOf(TypeError);
        expect(isSonareError(error)).toBe(false);
        expect((error as Error).message).toMatch(
          /^RealtimeEngine\.setStrip: unknown key settings\.faderDB\b/,
        );
      }
      for (const ref of [10, 'master'] as const) {
        let error: unknown;
        try {
          engine.setStrip(ref, { pan: 5 } as never);
        } catch (caught) {
          error = caught;
        }
        expect(error).toBeInstanceOf(RangeError);
        expect(isSonareError(error)).toBe(false);
        expect((error as Error).message).toMatch(/^RealtimeEngine\.setStrip: settings\.pan must /);
      }
      const annotated = '{"$schema":"s","version":2,"strips":[{"id":"s","faderDb":-3,"x-t":1}]}';
      expect(() => engine.setStrip(10, sceneStrip(annotated))).not.toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('the bus strip setter refuses an unknown key as an argument', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    try {
      // A key or value refused inside the settings object is a refused argument, not a document error.
      expect(() => engine.setBusStrip(1, { id: '1', faderDB: -3 } as never)).toThrow(TypeError);
      expect(() => engine.setBusStrip(1, { id: '1', faderDB: -3 } as never)).toThrow(
        /^RealtimeEngine\.setBusStrip: unknown key settings\.faderDB\b/,
      );
    } finally {
      engine.destroy();
    }
  });
});
