import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, isSonareError, Mixer, Project, RealtimeEngine } from '../dist/index.js';

const REFUSAL = Symbol.for('libsonare.refusal');

describe('unknown mixing scene keys (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('Project.setMixerScene names an unknown scene key as a TypeError', () => {
    const project = new Project();
    try {
      const typo = {
        version: 2,
        $schema: 's',
        strips: [{ id: 'lead', faderDB: -3, 'x-note': 1 }],
        buses: [],
      };
      expect(() => project.setMixerScene(typo as never)).toThrow(TypeError);
      expect(() => project.setMixerScene(typo as never)).toThrow(
        /unknown key scene\.strips\[0\]\.faderDB \(expected /,
      );
      expect(() => project.setMixerScene({ strips: 'lead' } as never)).toThrow(TypeError);
      expect(() => project.setMixerScene({ version: 1 })).toThrow(RangeError);
      expect(() => project.setMixerScene({ toJSON: () => undefined } as never)).toThrow(
        new TypeError('Project.setMixerScene: scene must serialize as an object'),
      );
      expect('setMixerSceneJson' in project).toBe(false);
      expect(project.mixerScene()).toBeUndefined();
      expect(
        project.setMixerScene({
          ...typo,
          strips: [{ id: 'lead', 'x-note': 1 }],
        } as never),
      ).toBeUndefined();
    } finally {
      project.destroy();
    }
  });

  it('names unread insert params in typed scenes and loaded documents', () => {
    const project = new Project();
    try {
      const scene = {
        version: 2,
        strips: [
          { id: 'lead', inserts: [{ processor: 'utility.gain', params: { unrelated: 1 } }] },
        ],
      };
      const path = /unknown key scene\.strips\[0\]\.inserts\[0\]\.params\.unrelated \(expected /;
      const setting = () => project.setMixerScene(scene as never);
      expect(setting).toThrow(TypeError);
      expect(setting).toThrow(path);

      const projectJson = JSON.stringify({ ...JSON.parse(project.toJson()), scene });
      for (const load of [Project.fromJson, Project.fromJsonWithDiagnostics]) {
        expect(() => load(projectJson)).toThrow(
          expect.objectContaining({
            code: ErrorCode.InvalidFormat,
            message: expect.stringMatching(path),
          }),
        );
      }
      expect(() => Mixer.fromJson(JSON.stringify(scene), { sampleRate: 48000 })).toThrow(
        expect.objectContaining({
          code: ErrorCode.InvalidFormat,
          message: expect.stringMatching(
            /unknown key strips\[0\]\.inserts\[0\]\.params\.unrelated \(expected /,
          ),
        }),
      );
    } finally {
      project.destroy();
    }
  });

  it('classifies the typed Mixer scene as an argument and the JSON scene as a document', () => {
    const sampleRate = 48000;
    const scene = { strips: [{ id: 'lead', bad_key: 1 }] };
    const construct = () => new Mixer({ sampleRate, scene });
    expect(construct).toThrow(TypeError);
    expect(construct).toThrow(/^Mixer: unknown key scene\.strips\[0\]\.bad_key \(expected /);
    expect(() => new Mixer({ sampleRate, scene: { version: 1 } })).toThrow(
      new RangeError('Mixer: scene.version must be 2'),
    );
    const serializedArray = { version: 2, toJSON: () => [] };
    expect(() => new Mixer({ sampleRate, scene: serializedArray })).toThrow(
      new TypeError('Mixer: scene must serialize as an object'),
    );
    expect(() => Mixer.fromJson(JSON.stringify(scene), { sampleRate })).toThrow(
      expect.objectContaining({
        code: ErrorCode.InvalidFormat,
        message: expect.stringMatching(
          /^Mixer\.fromJson: unknown key strips\[0\]\.bad_key \(expected /,
        ),
      }),
    );
  });

  it('classifies structured scene refusals and preserves the key as written', () => {
    using project = new Project();
    expect(() => project.setMixerScene({ version: 1 })).toThrow(
      new RangeError('Project.setMixerScene: scene.version must be 2'),
    );
    const serializedArray = { version: 2, toJSON: () => [] };
    expect(() => project.setMixerScene(serializedArray)).toThrow(
      new TypeError('Project.setMixerScene: scene must serialize as an object'),
    );
    expect(() => project.setMixerScene({ strips: [{ id: 'lead', panMode: 1 }] } as never)).toThrow(
      new TypeError(
        'Project.setMixerScene: scene.strips[0].panMode must be one of balance, stereoPan, dualPan',
      ),
    );
    const sceneWithInsert = (processor: string, params: Record<string, unknown>) => ({
      strips: [{ id: 'lead', inserts: [{ slot: 'post', processor, params }] }],
    });
    const efx = sceneWithInsert('effects.gsEfx', { typeMsb: 1 });
    const efxPath = 'scene.strips[0].inserts[0].params';
    expect(() => project.setMixerScene(efx as never)).toThrow(
      new RangeError(
        `Project.setMixerScene: ${efxPath}.typeLsb must be given when ${efxPath}.typeMsb is given`,
      ),
    );
    const typo = sceneWithInsert('saturation.softClipper', { bad_key: 1 });
    const refusal =
      'unknown key strips[0].inserts[0].params.bad_key (expected aliasing, ceiling, driveDb, mix)';
    expect(() => project.setMixerScene(typo as never)).toThrow(
      new TypeError(`Project.setMixerScene: ${refusal.replace('strips', 'scene.strips')}`),
    );
    expect(() => Mixer.fromJson(JSON.stringify(typo), { sampleRate: 48000 })).toThrow(
      expect.objectContaining({
        code: ErrorCode.InvalidFormat,
        message: `Mixer.fromJson: ${refusal}`,
      }),
    );
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
      expect(() => engine.setStrip(10, JSON.parse(annotated).strips[0])).not.toThrow();
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
  it('engine strip setters refuse unread insert params by path', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
    try {
      engine.setTrackLanes([{ trackId: 10 }]);
      engine.setTrackBuses([{ busId: 1 }]);
      const valid = { slot: 'post', processor: 'saturation.softClipper', params: { driveDb: 1 } };
      const invalid = { ...valid, params: { drive: 1 } };
      for (const [index, inserts] of [[invalid], [valid, invalid]].entries()) {
        const settings = { inserts } as never;
        const subject = `settings.inserts[${index}].params.drive`;
        const text = `unknown key ${subject} (expected aliasing, ceiling, driveDb, mix)`;
        for (const [entry, run] of [
          ['setStrip', () => engine.setStrip(10, settings)],
          ['setStrip', () => engine.setStrip('master', settings)],
          ['setBusStrip', () => engine.setBusStrip(1, settings)],
        ] as const) {
          expect(run).toThrow(TypeError);
          expect(run).toThrow(
            expect.objectContaining({
              message: `RealtimeEngine.${entry}: ${text}`,
              [REFUSAL]: expect.objectContaining({ form: 'unknownKey', subjects: [subject] }),
            }),
          );
        }
      }
    } finally {
      engine.destroy();
    }
  });
});
