/**
 * The engine and the project read an automation breakpoint through one conversion: an absent
 * curve (`undefined` or `null`) is linear, and an unknown curve is refused under its full path.
 */

import { describe, expect, it } from 'vitest';
import { Project, RealtimeEngine } from '../dist/index.js';

const CURVES = 'linear, exponential, hold, sCurve';

describe('automation breakpoint conversion', () => {
  it('reads a null curve as linear on the engine', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000 });
    try {
      engine.addParameter({ id: 1, name: 'gain' });
      expect(() =>
        engine.setAutomationLane(1, [{ ppq: 0, value: 0, curveToNext: null as never }]),
      ).not.toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('refuses an unknown project curve under its full path', () => {
    const project = new Project();
    try {
      const track = project.addTrack({ kind: 'audio' });
      expect(() =>
        project.addAutomationLane(track, {
          targetParamId: 1,
          points: [
            { ppq: 0, value: 0 },
            { ppq: 1, value: 1, curveToNext: 'bogus' as never },
          ],
        }),
      ).toThrow(
        new RangeError(`Project.addAutomationLane: points[1].curveToNext must be one of ${CURVES}`),
      );
    } finally {
      project.destroy();
    }
  });

  it('adds an empty lane when points is omitted, and edits a lane empty the same way', () => {
    const project = new Project();
    try {
      const track = project.addTrack({ kind: 'audio' });
      const id = project.addAutomationLane(track, { targetParamId: 7 });
      const lanePoints = () =>
        JSON.parse(project.toJson()).tracks[0].automationLanes.find(
          (lane: { targetParamId: number }) => lane.targetParamId === 7,
        ).points;
      expect(lanePoints()).toEqual([]);
      project.editAutomationLane(track, id, {
        targetKind: 'opaque',
        targetParamId: 7,
        points: [{ ppq: 0, value: 1 }],
      });
      expect(lanePoints()).toHaveLength(1);
      project.editAutomationLane(track, id, {});
      expect(lanePoints()).toEqual([]);
    } finally {
      project.destroy();
    }
  });

  it('refuses a points value that is not an array as a TypeError', () => {
    const project = new Project();
    try {
      const track = project.addTrack({ kind: 'audio' });
      expect(() =>
        project.addAutomationLane(track, { targetParamId: 1, points: { ppq: 0 } as never }),
      ).toThrow(new TypeError('Project.addAutomationLane: points must be an array'));
    } finally {
      project.destroy();
    }
  });
});
