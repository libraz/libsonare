import { describe, expect, it } from 'vitest';
import type {
  MixSceneDocument,
  ProjectAutomationLane,
  ProjectAutomationLaneDesc,
  ProjectAutomationLaneEditDesc,
} from '../src/index.js';
import { automationTargetKindNames, Project } from '../src/index.js';
import { expectRefusal } from './_helpers.js';

const LANE_DESCRIPTORS: ProjectAutomationLaneDesc[] = [
  { targetKind: 'opaque', targetParamId: 101, points: [{ ppq: 0, value: 0.5 }] },
  { targetKind: 'trackFaderDb', points: [{ ppq: 0, value: -3 }] },
  { targetKind: 'trackPan', points: [{ ppq: 0, value: -0.25 }] },
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

function laneEditDescriptor(lane: ProjectAutomationLane): ProjectAutomationLaneEditDesc {
  const desc: Record<string, unknown> = { ...lane };
  delete desc.trackId;
  delete desc.laneId;
  return desc as ProjectAutomationLaneEditDesc;
}

describe('project automation lane readers', () => {
  it('reads all target kinds and preserves identity through a no-op edit', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio', name: 'automation' });
      const laneIds = LANE_DESCRIPTORS.map((desc) => project.addAutomationLane(trackId, desc));
      expect(automationTargetKindNames()).toEqual([
        'opaque',
        'trackFaderDb',
        'trackPan',
        'trackSendDb',
        'trackWidth',
        'trackInsertParam',
      ]);
      expect(new Set(laneIds).size).toBe(laneIds.length);

      const beforeEdit = project.toJson();
      const lanes = project.automationLanes();
      expect(
        lanes.map(({ trackId: owner, laneId, targetKind }) => [owner, laneId, targetKind]),
      ).toEqual(
        laneIds.map((laneId, index) => [trackId, laneId, LANE_DESCRIPTORS[index].targetKind]),
      );
      expect(lanes[0]).toMatchObject({ targetKind: 'opaque', targetParamId: 101 });
      for (const lane of lanes.slice(1)) {
        expect(lane).not.toHaveProperty('targetParamId');
      }
      expect(lanes[3]).toMatchObject({ targetKind: 'trackSendDb', sendId: 'send-main' });
      expect(lanes[5]).toMatchObject({
        targetKind: 'trackInsertParam',
        insertIndex: 0,
        processor: 'utility.gain',
        paramName: 'levelDb',
      });

      for (const lane of lanes) {
        project.editAutomationLane(lane.trackId, lane.laneId, laneEditDescriptor(lane));
      }
      expect(project.toJson()).toBe(beforeEdit);
      expect(project.automationLanes().map(({ laneId }) => laneId)).toEqual(laneIds);
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

  it('names invalid lane identities and refuses a typed target parameter id', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio' });
      const laneId = project.addAutomationLane(trackId, {
        targetKind: 'trackFaderDb',
        points: [{ ppq: 0, value: -3 }],
      });
      expectRefusal(
        () =>
          project.addAutomationLane(trackId, {
            targetKind: 'trackPan',
            targetParamId: 0,
            points: [],
          } as never),
        RangeError,
        /^Project\.addAutomationLane: targetParamId must be omitted for a typed target$/,
      );
      expectRefusal(
        () =>
          project.editAutomationLane(trackId, laneId, {
            sendId: 'send-main',
            points: [],
          } as never),
        RangeError,
        /^Project\.editAutomationLane: sendId must be omitted when targetKind is omitted$/,
      );
      expectRefusal(
        () => project.editAutomationLane(trackId, 999999, { points: [] }),
        RangeError,
        /laneId/,
      );
      expectRefusal(() => project.removeAutomationLane(trackId, 999999), RangeError, /laneId/);
    } finally {
      project.destroy();
    }
  });
});

describe('project scene and route readers', () => {
  it('reads track routing and the stored mixer scene', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio', name: 'routed' });
      expect(project.tracks()[0]).not.toHaveProperty('channelStripRef');
      expect(project.tracks()[0]).not.toHaveProperty('outputTarget');
      project.setTrackRoute(trackId, 'strip-main', 'master');
      expect(project.tracks()[0]).toMatchObject({
        id: trackId,
        channelStripRef: 'strip-main',
        outputTarget: 'master',
      });

      expect(project.mixerScene()).toBeUndefined();
      project.setMixerSceneJson(
        JSON.stringify({ version: 2, strips: [], buses: [], vcaGroups: [], connections: [] }),
      );
      expect(project.mixerScene()).toMatchObject({
        version: 2,
        strips: [],
        buses: [],
        vcaGroups: [],
        connections: [],
      } satisfies Partial<MixSceneDocument>);
    } finally {
      project.destroy();
    }
  });

  it('reads unresolved SoundFont URIs as strings', () => {
    const project = Project.fromJson(
      JSON.stringify({
        version: 7,
        sampleRate: 48000,
        tracks: [
          {
            id: 1,
            name: 'keys',
            kind: 'midi',
            midiDestinationId: 5,
            channelStripRef: '',
            outputTarget: '',
            automationLanes: [],
          },
        ],
        clips: [],
        instruments: [
          {
            destinationId: 5,
            kind: 'sf2',
            sf2: {
              gain: 1,
              polyphony: 32,
              preferModelForModeledFamilies: 0,
              clearBankRig: 0,
              gsEfxRealization: 0,
              soundFontUri: 'fonts/missing.sf2',
            },
          },
        ],
      }),
    );
    try {
      expect(project.unresolvedSoundFontUris()).toEqual(['fonts/missing.sf2']);
    } finally {
      project.destroy();
    }
  });
});
