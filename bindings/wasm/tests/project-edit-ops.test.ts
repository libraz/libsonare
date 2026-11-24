/**
 * WASM coverage for the headless-DAW edit operations, MIR
 * annotations, assist sidecars, the realtime-engine MIDI/parameter ops, and
 * masteringInsertNames. These mirror the Node/Python surface and drive the same
 * C ABI / core through embind.
 */

import { readFileSync } from 'node:fs';

import { beforeAll, describe, expect, it } from 'vitest';
import type {
  ProjectAutomationCurve,
  ProjectAutomationPoint,
  ProjectLoopRecordingDesc,
} from '../dist/index.js';
import {
  ErrorCode,
  init,
  isSonareError,
  masteringInsertNames,
  Project,
  RealtimeEngine,
} from '../dist/index.js';
import { AUTOMATION_CURVE_VALUES } from '../src/codes';
import { expectArgumentRefusal } from './_helpers';

interface AutomationCurveCorpus {
  accepted: Array<{ value: string; ordinal: number }>;
  rejected: string[];
  rejected_numeric: number[];
}

// The same corpus the Node suite reads: the automation-curve vocabulary is one
// declaration both packages resolve against, not a table per surface.
const curveCorpus = JSON.parse(
  readFileSync(
    new URL('../../../tests/conformance/automation_curve_names.json', import.meta.url),
    'utf8',
  ),
) as AutomationCurveCorpus;

/** Per clip, the ids of the takes the project JSON marks partial. */
function partialTakeIds(project: { toJson(): string }): number[][] {
  const { clips } = JSON.parse(project.toJson()) as {
    clips: Array<{ takes?: Array<{ id: number; partial?: boolean }> }>;
  };
  return clips.map((clip) =>
    (clip.takes ?? []).filter((take) => take.partial).map((take) => take.id),
  );
}

describe('Sonare WASM Project edit ops', () => {
  beforeAll(async () => {
    await init();
  });

  function buildAudioProject(): { project: Project; trackId: number; clipId: number } {
    const project = new Project();
    project.setSampleRate(48000);
    const trackId = project.addTrack({ kind: 'audio', name: 'lead' });
    const audio = new Float32Array(480);
    for (let i = 0; i < audio.length; i++) {
      audio[i] = Math.sin(i * 0.05) * 0.25;
    }
    const clipId = project.addClip({
      trackId,
      startPpq: 0,
      lengthPpq: 4,
      channels: [audio],
      sampleRate: 48000,
    });
    return { project, trackId, clipId };
  }

  it('refuses an undeclared take or comp-segment field by its path without mutating', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio' });
      const clip = project.addClip({
        trackId,
        startPpq: 0,
        lengthPpq: 4,
        sourceUri: 'asset://a.wav',
      });
      const before = project.toJson();
      expect(() => project.setClipTakes(clip, [{ id: 1, sorceOffsetPpq: 2 } as never])).toThrow(
        new TypeError(
          'Project.setClipTakes: unknown key takes[0].sorceOffsetPpq (expected id, name, sourceId, sourceOffsetPpq)',
        ),
      );
      expect(() =>
        project.setClipCompSegments(clip, [
          { startPpq: 0, endPpq: 2 },
          { startPpq: 2, endPpq: 4, crossfadePq: 0.25 } as never,
        ]),
      ).toThrow(
        new TypeError(
          'Project.setClipCompSegments: unknown key segments[1].crossfadePq (expected crossfadePpq, endPpq, startPpq, takeId)',
        ),
      );
      expect(project.toJson()).toBe(before);
    } finally {
      project.destroy();
    }
  });

  it('accepts a metadata-only audio clip with a source URI', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio' });
      expect(() =>
        project.addClip({ trackId, startPpq: 0, lengthPpq: 4, sourceUri: 'asset://lead.wav' }),
      ).not.toThrow();
      expect(project.toJson()).toContain('asset://lead.wav');
    } finally {
      project.destroy();
    }
  });

  it('rebinds deserialized source audio before bouncing', () => {
    const project = new Project();
    const audio = new Float32Array(480).fill(0.25);
    try {
      const trackId = project.addTrack({ kind: 'audio' });
      project.addClip({
        trackId,
        startPpq: 0,
        lengthPpq: 1,
        channels: [audio],
        sampleRate: 48000,
        sourceUri: 'asset://lead.wav',
      });
      const restored = Project.fromJson(project.toJson());
      try {
        const ids = restored.unresolvedAudioSourceIds();
        expect(ids).toHaveLength(1);
        restored.setSourceAudio({ sourceId: ids[0], channels: [audio], sampleRate: 48000 });
        expect(restored.unresolvedAudioSourceIds()).toEqual([]);
        expect(restored.bounce({ totalFrames: 480, numChannels: 1 }).channels[0]).toHaveLength(480);
      } finally {
        restored.destroy();
      }
    } finally {
      project.destroy();
    }
  });

  it('describes unresolved audio sources with the full untruncated URI', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio' });
      const longUri = `asset://${'a'.repeat(300)}.wav`;
      project.addClip({ trackId, startPpq: 0, lengthPpq: 1, sourceUri: longUri });
      project.addClip({ trackId, startPpq: 2, lengthPpq: 1, sourceUri: 'asset://stem.wav' });
      const restored = Project.fromJson(project.toJson());
      try {
        const ids = restored.unresolvedAudioSourceIds();
        expect(ids).toHaveLength(2);
        restored.setAudioSourceMetadata({
          sourceId: ids[1],
          contentHash: 'sha256:abc',
          externalStemRole: 'vocals',
        });
        const sources = restored.unresolvedAudioSources();
        expect(sources.map((source) => source.id)).toEqual(ids);
        expect(sources[0].nameOrUri).toBe(longUri);
        expect(sources[0].kind).toBe('audio');
        expect(sources[1].nameOrUri).toBe('asset://stem.wav');
        expect(sources[1].contentHash).toBe('sha256:abc');
        expect(sources[1].externalStemRole).toBe('vocals');
        expect(restored.sources()[0].nameOrUri).toBe(longUri);
        restored.setSourceAudio({
          sourceId: ids[0],
          channels: [new Float32Array(480).fill(0.25)],
          sampleRate: 48000,
        });
        expect(restored.unresolvedAudioSources().map((source) => source.id)).toEqual([ids[1]]);
      } finally {
        restored.destroy();
      }
    } finally {
      project.destroy();
    }
  });

  it('reports no unresolved audio sources for a MIDI-only project', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'midi' });
      project.addClip({ trackId, startPpq: 0, lengthPpq: 1, isMidi: true });
      expect(project.unresolvedAudioSources()).toEqual([]);
    } finally {
      project.destroy();
    }
  });

  it('removeClip + undo round-trips through the edit history', () => {
    const { project, clipId } = buildAudioProject();
    try {
      expect(() => project.removeClip(clipId)).not.toThrow();
      // The public removal also releases its unreferenced source registry
      // entry, so serialization cannot retain an orphan source.
      expect(project.toJson()).toContain('"sources":[]');
      // The clip is gone; operating on it again fails.
      expect(() => project.setClipGain(clipId, 0.5)).toThrow();
      // Undo restores it so the gain edit then succeeds.
      project.undo();
      expect(() => project.setClipGain(clipId, 0.5)).not.toThrow();
    } finally {
      project.destroy();
    }
  });

  it('setMaxUndoDepth caps how far undo can rewind, clearHistory empties it', () => {
    const { project, clipId } = buildAudioProject();
    try {
      // Apply several undoable edits.
      for (let i = 0; i < 5; i++) {
        project.setClipGain(clipId, 0.1 * (i + 1));
      }
      // Cap the history at two entries; older edits are evicted.
      project.setMaxUndoDepth(2);
      let undoCount = 0;
      for (;;) {
        try {
          project.undo();
          undoCount++;
        } catch {
          break;
        }
      }
      expect(undoCount).toBe(2);

      // A fresh batch of edits, then clearHistory leaves nothing to undo.
      project.setClipGain(clipId, 0.3);
      project.setClipGain(clipId, 0.6);
      project.clearHistory();
      expect(() => project.undo()).toThrow();

      // The depth is a size_t: 0 is clamped to 1 as the C ABI does, a fraction is refused.
      expect(() => project.setMaxUndoDepth(0)).not.toThrow();
      expect(() => project.setMaxUndoDepth(1.5)).toThrow(
        new RangeError(
          'Project.setMaxUndoDepth: depth must be a finite non-negative integer no greater than ' +
            'Number.MAX_SAFE_INTEGER or the native size_t maximum',
        ),
      );
    } finally {
      project.destroy();
    }
  });

  it('setMaxHistoryBytes validates without mutating history and supports zero retention', () => {
    const { project, clipId } = buildAudioProject();
    try {
      const invoke = (...args: unknown[]) =>
        Reflect.apply(project.setMaxHistoryBytes, project, args);
      const invalidValues: readonly unknown[] = [
        undefined,
        null,
        '64',
        Number.NaN,
        Number.POSITIVE_INFINITY,
        Number.NEGATIVE_INFINITY,
        1.5,
        -1,
        0xffff_ffff + 1,
        Number.MAX_SAFE_INTEGER,
      ];
      for (const value of invalidValues) {
        const before = project.toJson();
        expect(() => invoke(value)).toThrow();
        expect(project.toJson()).toBe(before);
      }
      expect(() => invoke()).toThrow();

      // Invalid calls leave the existing history usable, and a normal edit is
      // still reversible under the largest valid wasm32 cap.
      project.setMaxHistoryBytes(0xffff_ffff);
      const beforeEdit = project.toJson();
      project.setClipGain(clipId, 0.5);
      expect(project.toJson()).not.toBe(beforeEdit);
      project.undo();
      expect(project.toJson()).toBe(beforeEdit);

      // Zero is a successful no-retention policy: the edit mutates state but
      // cannot be undone.
      expect(() => project.setMaxHistoryBytes(0)).not.toThrow();
      project.setClipGain(clipId, 0.75);
      const afterZeroEdit = project.toJson();
      expect(afterZeroEdit).not.toBe(beforeEdit);
      expect(() => project.undo()).toThrow();
      expect(project.toJson()).toBe(afterZeroEdit);
    } finally {
      project.destroy();
    }
  });

  it('setClipGain / setClipFade / setClipLoop apply without throwing', () => {
    const { project, clipId } = buildAudioProject();
    try {
      expect(() => project.setClipGain(clipId, 0.0)).not.toThrow();
      expect(() => project.setClipGain(clipId, 0.75)).not.toThrow();
      expect(() =>
        project.setClipFade(clipId, {
          fadeInPpq: 0.5,
          fadeInCurve: 'equalPower',
          fadeOutPpq: 0.5,
          fadeOutCurve: 'equalPower',
        }),
      ).not.toThrow();
      expect(() =>
        project.setClipLoop(clipId, { loopMode: 'loop', loopLengthPpq: 2 }),
      ).not.toThrow();
      expect(() => project.setClipLoop(clipId, { loopMode: 'off' })).not.toThrow();
      // Optional loop crossfade: accepted when >= 0, rejected when negative.
      expect(() =>
        project.setClipLoop(clipId, { loopMode: 'loop', loopLengthPpq: 2, loopCrossfadePpq: 0.5 }),
      ).not.toThrow();
      expect(project.toJson()).toContain('loopCrossfadePpq');
      expect(() =>
        project.setClipLoop(clipId, { loopMode: 'loop', loopLengthPpq: 2, loopCrossfadePpq: -1 }),
      ).toThrow();
      // Curve spellings other than the lowerCamel token are refused.
      for (const curve of ['equal-power', 'equal_power', 'equalpower', 'LOG']) {
        expect(() =>
          project.setClipFade(clipId, { fadeInPpq: 0.5, fadeInCurve: curve as never }),
        ).toThrow(RangeError);
      }
    } finally {
      project.destroy();
    }
  });

  it('setClipTakes and setClipCompSegments serialize and undo', () => {
    const { project, clipId } = buildAudioProject();
    try {
      const before = project.toJson();
      expect(() =>
        project.setClipTakes(
          clipId,
          [
            { id: 1, sourceOffsetPpq: 0, name: 'take A' },
            { id: 2, sourceOffsetPpq: 0.5, name: 'take B' },
          ],
          { activeTakeId: 1 },
        ),
      ).not.toThrow();
      const withTakes = project.toJson();
      expect(withTakes).not.toBe(before);
      expect(withTakes).toContain('"takes"');
      expect(withTakes).toContain('"activeTakeId":1');

      expect(() =>
        project.setClipCompSegments(clipId, [
          { startPpq: 0, endPpq: 2, takeId: 1 },
          { startPpq: 2, endPpq: 4, takeId: 2 },
        ]),
      ).not.toThrow();
      expect(project.toJson()).toContain('"compSegments"');

      project.undo();
      expect(project.toJson()).toBe(withTakes);
      project.undo();
      expect(project.toJson()).toBe(before);
      project.redo();
      expect(project.toJson()).toBe(withTakes);

      expect(() =>
        project.setClipTakes(
          clipId,
          [
            { id: 1, name: 'duplicate A' },
            { id: 1, name: 'duplicate B' },
          ],
          { activeTakeId: 1 },
        ),
      ).toThrow();
      expect(() =>
        project.setClipCompSegments(clipId, [{ startPpq: 0, endPpq: 1, takeId: 99 }]),
      ).toThrow();
    } finally {
      project.destroy();
    }
  });

  it('setClipCompSegments crossfadePpq reaches the module and refuses a wrong type', () => {
    const { project, clipId } = buildAudioProject();
    try {
      expect(() =>
        project.setClipTakes(
          clipId,
          [
            { id: 1, sourceOffsetPpq: 0, name: 'take A' },
            { id: 2, sourceOffsetPpq: 0.5, name: 'take B' },
          ],
          { activeTakeId: 1 },
        ),
      ).not.toThrow();

      expect(() =>
        project.setClipCompSegments(clipId, [
          { startPpq: 0, endPpq: 2, takeId: 1 },
          { startPpq: 2, endPpq: 4, takeId: 2, crossfadePpq: 0.25 },
        ]),
      ).not.toThrow();
      expect(project.toJson()).toContain('"crossfadePpq":0.25');

      let caught: unknown;
      try {
        project.setClipCompSegments(clipId, [
          { startPpq: 0, endPpq: 2, takeId: 1 },
          { startPpq: 2, endPpq: 4, takeId: 2, crossfadePpq: 'bad' as never },
        ]);
      } catch (error) {
        caught = error;
      }
      const caughtRefusal = expectArgumentRefusal(caught, TypeError);
      expect(caughtRefusal.message).toContain('crossfadePpq');
    } finally {
      project.destroy();
    }
  });

  it('addLoopRecordingTakes splits captured loops into takes', () => {
    const project = new Project();
    try {
      project.setSampleRate(48000);
      const trackId = project.addTrack({ kind: 'audio', name: 'record' });
      const audio = new Float32Array(48000);
      audio.fill(0.25, 0, 24000);
      audio.fill(0.75, 24000);
      const result = project.addLoopRecordingTakes({
        trackId,
        startPpq: 0,
        loopLengthPpq: 1,
        audio,
        audioChannels: 1,
        audioSampleRate: 48000,
      });
      expect(result.clipId).toBeGreaterThan(0);
      expect(result.takeCount).toBe(2);
      expect(project.toJson()).not.toContain('"partial"');
      expect(project.toJson()).toContain('"activeTakeId":2');
      project.undo();
      expect(project.toJson()).toContain('"clips":[]');
    } finally {
      project.destroy();
    }
  });

  it('addLoopRecordingTakes keeps a partial last take inactive and honours partialTail', () => {
    const project = new Project();
    try {
      project.setSampleRate(48000);
      const trackId = project.addTrack({ kind: 'audio', name: 'record' });
      const base = {
        trackId,
        startPpq: 0,
        loopLengthPpq: 1,
        audioChannels: 1,
        audioSampleRate: 48000,
      };
      const kept = project.addLoopRecordingTakes({
        ...base,
        startPpq: 0,
        audio: new Float32Array(36000).fill(0.5),
      });
      expect(kept.takeCount).toBe(2);
      expect(partialTakeIds(project)).toEqual([[2]]);
      expect(project.toJson()).toContain('"activeTakeId":1');
      const dropped = project.addLoopRecordingTakes({
        ...base,
        startPpq: 4,
        audio: new Float32Array(36000).fill(0.5),
        partialTail: 'drop',
      });
      expect(dropped.takeCount).toBe(1);
      expect(partialTakeIds(project)).toEqual([[2], []]);
      const short = project.addLoopRecordingTakes({
        ...base,
        startPpq: 8,
        audio: new Float32Array(12000).fill(0.5),
      });
      expect(short.takeCount).toBe(1);
      expect(partialTakeIds(project)).toEqual([[2], [], [1]]);
      expect(() =>
        project.addLoopRecordingTakes({
          ...base,
          startPpq: 12,
          audio: new Float32Array(100),
          partialTail: 'activate' as unknown as 'keep',
        }),
      ).toThrow(/partialTail/);
    } finally {
      project.destroy();
    }
  });

  it('addLoopRecordingTakes treats a one-frame remainder as rounding and the partial mark round-trips', () => {
    const project = new Project();
    try {
      project.setSampleRate(48000);
      const trackId = project.addTrack({ kind: 'audio', name: 'record' });
      const base = { trackId, loopLengthPpq: 1, audioChannels: 1, audioSampleRate: 48000 };
      const extra = project.addLoopRecordingTakes({
        ...base,
        startPpq: 0,
        audio: new Float32Array(48001).fill(0.5),
      });
      expect(extra.takeCount).toBe(2);
      expect(partialTakeIds(project)).toEqual([[]]);
      project.addLoopRecordingTakes({
        ...base,
        startPpq: 4,
        audio: new Float32Array(36000).fill(0.5),
      });
      expect(partialTakeIds(project)).toEqual([[], [2]]);
      const reloaded = Project.fromJson(project.toJson());
      try {
        expect(partialTakeIds(reloaded)).toEqual([[], [2]]);
      } finally {
        reloaded.destroy();
      }
    } finally {
      project.destroy();
    }
  });

  it('addLoopRecordingTakes accepts planar audio equal to the interleaved form', () => {
    const left = new Float32Array(48000).map((_, i) => i / 48000 - 0.5);
    const right = new Float32Array(48000).map((_, i) => 0.5 - i / 48000);
    const interleaved = new Float32Array(96000);
    for (let i = 0; i < 48000; i++) {
      interleaved[2 * i] = left[i];
      interleaved[2 * i + 1] = right[i];
    }
    const run = (
      capture: { audio: Float32Array; audioChannels: number } | { audio: Float32Array[] },
    ) => {
      const project = new Project();
      try {
        project.setSampleRate(48000);
        const trackId = project.addTrack({ kind: 'audio', name: 'record' });
        project.addLoopRecordingTakes({
          trackId,
          startPpq: 0,
          loopLengthPpq: 1,
          ...capture,
          audioSampleRate: 48000,
        });
        return project.toJson();
      } finally {
        project.destroy();
      }
    };
    expect(run({ audio: [left, right] })).toBe(run({ audio: interleaved, audioChannels: 2 }));
  });

  it('addLoopRecordingTakes refuses planar channels of different lengths', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio', name: 'record' });
      expect(() =>
        project.addLoopRecordingTakes({
          trackId,
          startPpq: 0,
          loopLengthPpq: 1,
          audio: [new Float32Array(100), new Float32Array(99)],
          audioSampleRate: 48000,
        }),
      ).toThrow(RangeError);
    } finally {
      project.destroy();
    }
  });

  it('addLoopRecordingTakes refuses interleaved audio without its channel count or sample rate', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio', name: 'record' });
      const desc = { trackId, loopLengthPpq: 1, audio: new Float32Array(100) };
      const refusal = (partial: object): unknown => {
        try {
          project.addLoopRecordingTakes(partial as unknown as ProjectLoopRecordingDesc);
        } catch (err) {
          return err;
        }
        return undefined;
      };
      const noChannels = expectArgumentRefusal(
        refusal({ ...desc, audioSampleRate: 48000 }),
        TypeError,
      );
      expect(noChannels.message).toMatch(/audioChannels/);
      const noRate = expectArgumentRefusal(refusal({ ...desc, audioChannels: 1 }), TypeError);
      expect(noRate.message).toMatch(/audioSampleRate/);
    } finally {
      project.destroy();
    }
  });

  it('addLoopRecordingTakes refuses audioChannels beside planar audio', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio', name: 'record' });
      const planar = [new Float32Array(100), new Float32Array(100)];
      const desc = { trackId, loopLengthPpq: 1, audio: planar, audioSampleRate: 48000 };
      let caught: unknown;
      try {
        project.addLoopRecordingTakes({
          ...desc,
          audioChannels: 2,
        } as unknown as ProjectLoopRecordingDesc);
      } catch (err) {
        caught = err;
      }
      expect(expectArgumentRefusal(caught, TypeError).message).toMatch(/audioChannels/);
      expect(() => project.addLoopRecordingTakes(desc)).not.toThrow(); // positive control
    } finally {
      project.destroy();
    }
  });

  it('rejects interleaved audio lengths that do not match audioChannels', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio', name: 'record' });
      expect(() =>
        project.addClip({
          trackId,
          startPpq: 0,
          lengthPpq: 1,
          channels: [new Float32Array(5), new Float32Array(4)],
          sampleRate: 48000,
        }),
      ).toThrow(RangeError);
      expect(() =>
        project.addLoopRecordingTakes({
          trackId,
          startPpq: 0,
          loopLengthPpq: 1,
          audio: new Float32Array(5),
          audioChannels: 2,
          audioSampleRate: 48000,
        }),
      ).toThrow();
    } finally {
      project.destroy();
    }
  });

  it('addLoopRecordingTakes preserves native InvalidState error codes', () => {
    const { project, trackId } = buildAudioProject();
    try {
      project.setOverlapPolicy('disallow');
      const audio = new Float32Array(48000);
      audio.fill(0.25);
      let caught: unknown;
      try {
        project.addLoopRecordingTakes({
          trackId,
          startPpq: 0,
          loopLengthPpq: 2,
          audio,
          audioChannels: 1,
          audioSampleRate: 48000,
        });
      } catch (error) {
        caught = error;
      }
      expect(isSonareError(caught)).toBe(true);
      if (!isSonareError(caught)) {
        throw new Error('expected SonareError');
      }
      expect(caught.code).toBe(ErrorCode.InvalidState);
      expect(caught.codeName).toBe('InvalidState');
    } finally {
      project.destroy();
    }
  });

  it('duplicateClip returns a fresh id distinct from the source', () => {
    const { project, clipId } = buildAudioProject();
    try {
      const dupId = project.duplicateClip(clipId, 8);
      expect(dupId).toBeGreaterThan(0);
      expect(dupId).not.toBe(clipId);
    } finally {
      project.destroy();
    }
  });

  it('renameTrack / setTrackRoute / removeTrack apply without throwing', () => {
    const { project, trackId } = buildAudioProject();
    try {
      expect(() => project.renameTrack(trackId, 'renamed')).not.toThrow();
      expect(() => project.setTrackRoute(trackId, 'strip-1', 'master')).not.toThrow();
      expect(() => project.setTrackRoute(undefined as never)).toThrow(
        new TypeError('Project.setTrackRoute: trackId must be a number'),
      );
      expect(() => project.setTrackRoute(trackId, '', 'master')).toThrow(/channelStripRef.*empty/);
      expect(() => project.setTrackRoute(trackId, 'strip-1', '')).toThrow(/outputTarget.*empty/);
      expect(project.tracks().find(({ id }) => id === trackId)).toMatchObject({
        channelStripRef: 'strip-1',
        outputTarget: 'master',
      });
      project.setTrackRoute(trackId);
      const cleared = project.tracks().find(({ id }) => id === trackId);
      expect(cleared).not.toHaveProperty('channelStripRef');
      expect(cleared).not.toHaveProperty('outputTarget');
      expect(() => project.removeTrack(trackId)).not.toThrow();
    } finally {
      project.destroy();
    }
  });

  it('setTrackGain / setTrackMute / setTrackSolo / setTrackPan apply without throwing', () => {
    const { project, trackId } = buildAudioProject();
    try {
      expect(() => project.setTrackGain(trackId, 0.5)).not.toThrow();
      expect(() => project.setTrackMute(trackId, true)).not.toThrow();
      expect(() => project.setTrackSolo(trackId, true)).not.toThrow();
      expect(() => project.setTrackPan(trackId, -0.25)).not.toThrow();
      expect(() => project.setTrackGain(999999, 1)).toThrow();
    } finally {
      project.destroy();
    }
  });

  it('automation lane add / edit / remove round-trips', () => {
    const { project, trackId } = buildAudioProject();
    try {
      const laneId = project.addAutomationLane(trackId, {
        targetParamId: 77,
        points: [
          { ppq: 0, value: 0.0, curveToNext: 'linear' },
          { ppq: 4, value: 1.0, curveToNext: 'exponential' },
        ],
      });
      expect(laneId).not.toBe(77);
      expect(() =>
        project.editAutomationLane(trackId, laneId, {
          points: [
            { ppq: 0, value: 0.5 },
            { ppq: 2, value: 0.25, curveToNext: 'hold' },
          ],
        }),
      ).not.toThrow();
      expect(() => project.removeAutomationLane(trackId, laneId)).not.toThrow();
    } finally {
      project.destroy();
    }
  });

  it('resolves every documented curve spelling and stores it as written', () => {
    // The vocabulary comes from the shared corpus rather than being restated
    // here, and the Node suite reads the same file: a spelling one facade takes
    // must land on the same curve on the other, not throw or fall back to
    // Linear at the boundary.
    expect(AUTOMATION_CURVE_VALUES).toEqual(
      Object.fromEntries(curveCorpus.accepted.map(({ value, ordinal }) => [value, ordinal])),
    );
    const laneWith = (curve: unknown): string => {
      const project = new Project();
      try {
        project.setSampleRate(48000);
        const trackId = project.addTrack({ kind: 'audio' });
        project.addAutomationLane(trackId, {
          targetParamId: 1,
          points: [{ ppq: 0, value: 1, curveToNext: curve as ProjectAutomationCurve }],
        });
        const doc = JSON.parse(project.toJson()) as {
          tracks: Array<{ automationLanes: Array<{ points: Array<{ curveToNext: string }> }> }>;
        };
        return doc.tracks[0].automationLanes[0].points[0].curveToNext;
      } finally {
        project.destroy();
      }
    };
    for (const { value } of curveCorpus.accepted) {
      expect(laneWith(value)).toBe(value);
    }
    // A Project curve is a token on every path, so an ordinal is a type error.
    for (const value of curveCorpus.rejected_numeric) {
      expect(() => laneWith(value)).toThrow(TypeError);
    }
    for (const value of curveCorpus.rejected) {
      expect(() => laneWith(value)).toThrow(RangeError);
    }

    // The point's field is `curveToNext`; the `curve` key is refused by name.
    const project = new Project();
    try {
      project.setSampleRate(48000);
      const trackId = project.addTrack({ kind: 'audio' });
      const before = project.toJson();
      expect(() =>
        project.addAutomationLane(trackId, {
          targetParamId: 1,
          points: [{ ppq: 0, value: 1, curve: 'sCurve' } as unknown as ProjectAutomationPoint],
        }),
      ).toThrow(
        new TypeError(
          'Project.addAutomationLane: unknown key points[0].curve (expected curveToNext, ppq, value)',
        ),
      );
      expect(project.toJson()).toBe(before);
    } finally {
      project.destroy();
    }
  });

  it('reads a breakpoint curve from curveToNext alone at the embind layer', () => {
    // The embind reader reads the keys it knows and nothing else, so a raw call
    // spelling the field `curve` keeps the default curve instead of taking it.
    const project = new Project();
    try {
      project.setSampleRate(48000);
      const trackId = project.addTrack({ kind: 'audio' });
      const native = (
        project as unknown as {
          native: { addAutomationLane: (trackId: number, desc: unknown) => number };
        }
      ).native;
      native.addAutomationLane(trackId, {
        targetParamId: 1,
        points: [{ ppq: 0, value: 1, curve: 3 }],
      });
      native.addAutomationLane(trackId, {
        targetParamId: 2,
        points: [{ ppq: 0, value: 1, curveToNext: 3 }],
      });
      const doc = JSON.parse(project.toJson()) as {
        tracks: Array<{
          automationLanes: Array<{ targetParamId: number; points: Array<{ curveToNext: string }> }>;
        }>;
      };
      const curveOf = (id: number) =>
        doc.tracks[0].automationLanes.find((lane) => lane.targetParamId === id)?.points[0]
          .curveToNext;
      expect(curveOf(1)).toBe('linear');
      expect(curveOf(2)).toBe('sCurve');
    } finally {
      project.destroy();
    }
  });

  it('writes no target kind for a lane that omits it', () => {
    const { project, trackId } = buildAudioProject();
    try {
      project.addAutomationLane(trackId, {
        targetParamId: 10,
        points: [{ ppq: 0, value: 0 }],
      });
      const serialized = JSON.parse(project.toJson()) as {
        tracks: Array<{ automationLanes: Array<Record<string, unknown>> }>;
      };
      expect(serialized.tracks[0].automationLanes[0]).not.toHaveProperty('targetKind');
    } finally {
      project.destroy();
    }
  });

  it('normalizes typed automation target kinds and round-trips them through the project JSON', () => {
    const { project, trackId } = buildAudioProject();
    try {
      const faderLaneId = project.addAutomationLane(trackId, {
        targetKind: 'trackFaderDb',
        points: [{ ppq: 0, value: -3 }],
      });
      const panLaneId = project.addAutomationLane(trackId, {
        targetKind: 'trackPan',
        points: [{ ppq: 0, value: 0 }],
      });
      const json = project.toJson();
      const serialized = JSON.parse(json) as {
        tracks: Array<{ automationLanes: Array<Record<string, unknown>> }>;
      };
      expect(serialized.tracks[0].automationLanes).toEqual(
        expect.arrayContaining([
          expect.objectContaining({ targetKind: 'trackFaderDb' }),
          expect.objectContaining({ targetKind: 'trackPan' }),
        ]),
      );
      const lanes = project.automationLanes();
      expect(lanes.find((lane) => lane.laneId === faderLaneId)).not.toHaveProperty('targetParamId');
      expect(lanes.find((lane) => lane.laneId === panLaneId)).not.toHaveProperty('targetParamId');

      const restored = Project.fromJson(json);
      try {
        expect(restored.toJson()).toBe(json);
      } finally {
        restored.destroy();
      }
    } finally {
      project.destroy();
    }
  });

  it('rejects typed target conflicts atomically and preserves identity on a points-only edit', () => {
    const { project, trackId } = buildAudioProject();
    try {
      const faderLaneId = project.addAutomationLane(trackId, {
        targetKind: 'trackFaderDb',
        points: [{ ppq: 0, value: -3 }],
      });
      project.addAutomationLane(trackId, {
        targetKind: 'trackPan',
        points: [{ ppq: 0, value: 0 }],
      });

      const beforeConflict = project.toJson();
      expect(() =>
        project.addAutomationLane(trackId, {
          targetKind: 'trackFaderDb',
          points: [{ ppq: 0, value: 6 }],
        }),
      ).toThrow();
      expect(project.toJson()).toBe(beforeConflict);

      expect(() =>
        project.editAutomationLane(trackId, faderLaneId, {
          targetKind: 'trackPan',
          points: [{ ppq: 0, value: 0.5 }],
        }),
      ).toThrow();
      expect(project.toJson()).toBe(beforeConflict);

      project.editAutomationLane(trackId, faderLaneId, {
        points: [{ ppq: 0, value: -6 }],
      });
      const edited = JSON.parse(project.toJson()) as {
        tracks: Array<{ automationLanes: Array<Record<string, unknown>> }>;
      };
      expect(edited.tracks[0].automationLanes).toEqual(
        expect.arrayContaining([expect.objectContaining({ targetKind: 'trackFaderDb' })]),
      );
      expect(
        project.automationLanes().find((lane) => lane.targetKind === 'trackFaderDb')?.laneId,
      ).toBe(faderLaneId);
    } finally {
      project.destroy();
    }
  });

  it('rejects unknown, width-three, and non-finite automation target kinds before mutation', () => {
    const { project, trackId } = buildAudioProject();
    try {
      const invalidKinds: readonly unknown[] = [
        3,
        -1,
        1.5,
        Number.NaN,
        Number.POSITIVE_INFINITY,
        'unknown',
        true,
      ];
      for (const invalidKind of invalidKinds) {
        const before = project.toJson();
        expect(() =>
          project.addAutomationLane(trackId, {
            targetParamId: 40,
            // Invalid runtime values intentionally exercise the JS boundary.
            targetKind: invalidKind as never,
            points: [{ ppq: 0, value: 0 }],
          }),
        ).toThrow();
        expect(project.toJson()).toBe(before);
      }
    } finally {
      project.destroy();
    }
  });

  it('rejects zero as the reserved automation target id', () => {
    const { project, trackId } = buildAudioProject();
    try {
      expect(() =>
        project.addAutomationLane(trackId, {
          targetParamId: 0,
          points: [{ ppq: 0, value: 0.0 }],
        }),
      ).toThrow(RangeError);
    } finally {
      project.destroy();
    }
  });

  it('accepts the canonical sCurve spelling and rejects unknown curves', () => {
    const { project, trackId } = buildAudioProject();
    try {
      expect(() =>
        project.addAutomationLane(trackId, {
          targetParamId: 1,
          points: [
            { ppq: 0, value: 0.0, curveToNext: 'sCurve' },
            { ppq: 4, value: 1.0, curveToNext: 'hold' },
          ],
        }),
      ).not.toThrow();
      // A misspelled curve is rejected, not silently coerced to Linear.
      expect(() =>
        project.addAutomationLane(trackId, {
          targetParamId: 1,
          // biome-ignore lint/suspicious/noExplicitAny: exercising an invalid spelling on purpose.
          points: [{ ppq: 0, value: 0.0, curveToNext: 's_curve' as any }],
        }),
      ).toThrow();
    } finally {
      project.destroy();
    }
  });

  it('annotateKeys / annotateChords apply and survive serialization', () => {
    const { project } = buildAudioProject();
    try {
      expect(() =>
        project.annotateKeys([{ startPpq: 0, endPpq: 8, tonicPc: 0, mode: 'major' }]),
      ).not.toThrow();
      expect(() =>
        project.annotateChords([
          {
            startPpq: 0,
            endPpq: 4,
            rootPc: 0,
            quality: 'major',
            extensions: [7],
            romanNumeral: 'I',
            modulationBoundary: false,
          },
        ]),
      ).not.toThrow();
      // The annotation streams are part of the project; serialization succeeds.
      expect(project.toJson().length).toBeGreaterThan(0);
    } finally {
      project.destroy();
    }
  });

  it('assist sidecar set / count / get round-trips the payload', () => {
    const { project } = buildAudioProject();
    try {
      const payload = new Uint8Array([1, 2, 3, 4, 5]);
      project.setAssistSidecar({
        moduleId: 'test.module',
        schemaVersion: 1,
        targetTrackId: 0,
        regionStartPpq: 0,
        regionEndPpq: 8,
        payload,
      });
      expect(project.assistSidecars().length).toBe(1);
      const sidecar = project.assistSidecars()[0];
      expect(sidecar.moduleId).toBe('test.module');
      expect(sidecar.schemaVersion).toBe(1);
      expect(sidecar.regionEndPpq).toBe(8);
      expect(Array.from(sidecar.payload)).toEqual([1, 2, 3, 4, 5]);
    } finally {
      project.destroy();
    }
  });
});

describe('Sonare WASM RealtimeEngine MIDI / parameter ops', () => {
  beforeAll(async () => {
    await init();
  });

  it('pushMidiCc / pushMidiPanic queue without throwing', () => {
    const engine = new RealtimeEngine({
      sampleRate: 48000,
      maxBlockSize: 128,
      commandCapacity: 1024,
      telemetryCapacity: 1024,
    });
    try {
      expect(() => engine.pushMidiCc(0, 0, 0, 7, 100)).not.toThrow();
      expect(() => engine.pushMidiPanic()).not.toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('pushMidiCc rejects out-of-range values', () => {
    const engine = new RealtimeEngine({
      sampleRate: 48000,
      maxBlockSize: 128,
      commandCapacity: 1024,
      telemetryCapacity: 1024,
    });
    try {
      expect(() => engine.pushMidiCc(0, 0, 0, 7, 200)).toThrow();
      expect(() => engine.pushMidiCc(0, 99, 0, 7, 10)).toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('clearParameters empties the registry', () => {
    const engine = new RealtimeEngine({
      sampleRate: 48000,
      maxBlockSize: 128,
      commandCapacity: 1024,
      telemetryCapacity: 1024,
    });
    try {
      engine.addParameter({
        id: 1,
        name: 'gain',
        unit: 'dB',
        minValue: -60,
        maxValue: 12,
        defaultValue: 0,
        rtSafe: true,
        defaultCurve: 'exponential',
      });
      expect(engine.parameters().length).toBe(1);
      engine.clearParameters();
      expect(engine.parameters().length).toBe(0);
      // The id is free to re-register after a clear.
      expect(() =>
        engine.addParameter({
          id: 1,
          name: 'gain2',
          unit: 'dB',
          minValue: -60,
          maxValue: 12,
          defaultValue: 0,
          rtSafe: true,
          defaultCurve: 'exponential',
        }),
      ).not.toThrow();
    } finally {
      engine.destroy();
    }
  });
});

describe('Sonare WASM masteringInsertNames', () => {
  beforeAll(async () => {
    await init();
  });

  it('returns a non-empty string[] including a reverb insert', () => {
    const names = masteringInsertNames();
    expect(Array.isArray(names)).toBe(true);
    expect(names.length).toBeGreaterThan(0);
    expect(names.every((n) => typeof n === 'string')).toBe(true);
    expect(names.some((n) => n.startsWith('effects.reverb.'))).toBe(true);
  });
});

describe('Sonare WASM wrong-typed domain fields', () => {
  beforeAll(async () => {
    await init();
  });

  it('refuses wrong-typed time-signature, automation target, warp map id and graph port fields as non-integers', () => {
    const bad = 'x' as unknown as number;
    const project = new Project();
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      const track = project.addTrack({ kind: 'audio' });
      const anchors = [
        { warpSample: 0, sourceSample: 0 },
        { warpSample: 10, sourceSample: 10 },
      ];
      const graph = {
        nodes: [
          { id: 'in', numPorts: bad },
          { id: 'out', numPorts: 1 },
        ],
        connections: [{ sourceNode: 'in', sourcePort: 0, destNode: 'out', destPort: 0 }],
        inputNode: 'in',
        outputNode: 'out',
      };
      const rows: Array<[string, () => unknown, RegExp]> = [
        [
          'numerator',
          () => project.setTimeSignatures([{ startPpq: 0, numerator: bad, denominator: 4 }]),
          /Project\.setTimeSignatures: segments\[0\]\.numerator must be an integer/,
        ],
        [
          'denominator',
          () => project.setTimeSignatures([{ startPpq: 0, numerator: 4, denominator: bad }]),
          /Project\.setTimeSignatures: segments\[0\]\.denominator must be an integer/,
        ],
        [
          'targetParamId',
          () =>
            project.addAutomationLane(track, {
              targetParamId: bad,
              points: [{ ppq: 0, value: 0 }],
            }),
          /Project\.addAutomationLane: targetParamId must be an integer/,
        ],
        [
          'warp map id',
          () => project.setWarpMap({ id: bad, anchors }),
          /Project\.setWarpMap: id must be an integer/,
        ],
        [
          'graph numPorts',
          () => engine.setGraph(graph),
          /RealtimeEngine\.setGraph: spec\.nodes\[0\]\.numPorts must be an integer/,
        ],
      ];
      for (const [, call, message] of rows) {
        expect(call).toThrow(TypeError);
        expect(call).toThrow(message);
      }
    } finally {
      engine.destroy();
      project.destroy();
    }
  });
});
