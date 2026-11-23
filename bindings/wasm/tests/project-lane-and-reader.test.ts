import { beforeAll, describe, expect, it } from 'vitest';
import {
  automationTargetKindNames,
  init,
  Project,
  type ProjectAutomationLaneDesc,
  type ProjectAutomationLaneEditDesc,
} from '../dist/index.js';

beforeAll(async () => {
  await init();
});

describe('project automation lanes', () => {
  it('writes and reads every target identity, and keeps lane ids across edits', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio' });
      const point = [{ ppq: 0, value: 0.5, curveToNext: 'hold' as const }];
      const descriptors: ProjectAutomationLaneDesc[] = [
        { targetParamId: 101, points: point },
        { targetKind: 'trackFaderDb', points: [{ ppq: 0, value: -3 }] },
        { targetKind: 'trackPan', points: [{ ppq: 0, value: 0.25 }] },
        { targetKind: 'trackSendDb', sendId: 'send-main', points: [{ ppq: 0, value: -6 }] },
        { targetKind: 'trackWidth', points: [{ ppq: 0, value: 1.25 }] },
        {
          targetKind: 'trackInsertParam',
          insertIndex: 0,
          processor: 'utility.gain',
          paramName: 'levelDb',
          points: [{ ppq: 0, value: -1 }],
        },
      ];
      const laneIds = descriptors.map((desc) => project.addAutomationLane(trackId, desc));
      const lanes = project.automationLanes();
      expect([...lanes.map((lane) => lane.laneId)].sort()).toEqual([...laneIds].sort());
      expect(lanes).toEqual(
        expect.arrayContaining([
          expect.objectContaining({
            trackId,
            laneId: laneIds[0],
            targetKind: 'opaque',
            targetParamId: 101,
          }),
          expect.objectContaining({ trackId, laneId: laneIds[1], targetKind: 'trackFaderDb' }),
          expect.objectContaining({ trackId, laneId: laneIds[2], targetKind: 'trackPan' }),
          expect.objectContaining({
            trackId,
            laneId: laneIds[3],
            targetKind: 'trackSendDb',
            sendId: 'send-main',
          }),
          expect.objectContaining({ trackId, laneId: laneIds[4], targetKind: 'trackWidth' }),
          expect.objectContaining({
            trackId,
            laneId: laneIds[5],
            targetKind: 'trackInsertParam',
            insertIndex: 0,
            processor: 'utility.gain',
            paramName: 'levelDb',
          }),
        ]),
      );
      const beforeEdit = project.toJson();
      for (const lane of lanes) {
        const { trackId: ownerTrackId, laneId, ...desc } = lane;
        project.editAutomationLane(ownerTrackId, laneId, desc as ProjectAutomationLaneEditDesc);
      }
      expect(project.toJson()).toBe(beforeEdit);
      expect([...project.automationLanes().map((lane) => lane.laneId)].sort()).toEqual(
        [...laneIds].sort(),
      );

      project.editAutomationLane(trackId, laneIds[1], { points: [{ ppq: 1, value: -6 }] });
      expect(project.automationLanes().find((lane) => lane.laneId === laneIds[1])).toMatchObject({
        laneId: laneIds[1],
        targetKind: 'trackFaderDb',
        points: [{ ppq: 1, value: -6 }],
      });
    } finally {
      project.destroy();
    }
  });

  it('uses C target names and names typed parameter and unknown lane refusals', () => {
    expect(automationTargetKindNames()).toEqual([
      'opaque',
      'trackFaderDb',
      'trackPan',
      'trackSendDb',
      'trackWidth',
      'trackInsertParam',
    ]);
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio' });
      expect(() =>
        project.addAutomationLane(trackId, {
          targetKind: 'trackPan',
          targetParamId: 0,
          points: [],
        } as never),
      ).toThrow(/targetParamId/);
      expect(() =>
        project.addAutomationLane(trackId, { targetKind: 'trackPan', targetParamId: undefined }),
      ).not.toThrow();
      expect(() =>
        project.addAutomationLane(trackId, { targetParamId: 7, insertIndex: -1 } as never),
      ).toThrow(/insertIndex/);
      const laneId = project.addAutomationLane(trackId, { targetParamId: 7, points: [] });
      expect(() =>
        project.editAutomationLane(trackId, laneId, {
          targetParamId: 7,
          points: [],
        } as never),
      ).toThrow(/targetParamId.*explicit targetKind/);
      expect(() => project.editAutomationLane(trackId, 999, { points: [] })).toThrow(/laneId/);
    } finally {
      project.destroy();
    }
  });
});

describe('project metadata readers', () => {
  it('reads track routes and the typed mixer scene', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio' });
      expect(project.tracks()[0]).not.toHaveProperty('channelStripRef');
      project.setTrackRoute(trackId, 'track-strip', 'master');
      expect(project.tracks()[0]).toMatchObject({
        channelStripRef: 'track-strip',
        outputTarget: 'master',
      });
      expect(project.mixerScene()).toBeUndefined();
      project.setMixerSceneJson(
        JSON.stringify({ version: 2, buses: [{ id: 'master', role: 'master', layout: 'stereo' }] }),
      );
      expect(project.mixerScene()?.buses[0]?.id).toBe('master');
    } finally {
      project.destroy();
    }
  });

  it('reads unresolved SoundFont URIs from a loaded project document', () => {
    const project = Project.fromJson(
      JSON.stringify({
        version: 7,
        instruments: [
          {
            destinationId: 5,
            kind: 'sf2',
            sf2: {
              gain: 0.5,
              polyphony: 48,
              preferModelForModeledFamilies: 0,
              clearBankRig: 0,
              gsEfxRealization: 1,
              soundFontUri: 'fonts/orchestra.sf2',
            },
          },
        ],
      }),
    );
    try {
      expect(project.unresolvedSoundFontUris()).toEqual(['fonts/orchestra.sf2']);
    } finally {
      project.destroy();
    }
  });
});
