/**
 * WASM coverage for the headless-DAW edit operations, MIR
 * annotations, assist sidecars, the realtime-engine MIDI/parameter ops, and
 * masteringInsertNames. These mirror the Node/Python surface and drive the same
 * C ABI / core through embind.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  init,
  isSonareError,
  masteringInsertNames,
  Project,
  RealtimeEngine,
} from '../dist/index.js';

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
      audio,
      audioChannels: 1,
      audioSampleRate: 48000,
    });
    return { project, trackId, clipId };
  }

  it('accepts a metadata-only audio clip with a source URI', () => {
    const project = new Project();
    try {
      const trackId = project.addTrack({ kind: 'audio' });
      expect(() =>
        project.addClip({ trackId, startPpq: 0, lengthPpq: 4, sourceUri: 'asset://lead.wav' }),
      ).not.toThrow();
      expect(project.toJson()).toContain('asset://lead.wav');
    } finally {
      project.delete();
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
        audio,
        audioChannels: 1,
        audioSampleRate: 48000,
        sourceUri: 'asset://lead.wav',
      });
      const restored = Project.fromJson(project.toJson());
      try {
        const ids = restored.unresolvedAudioSourceIds();
        expect(ids).toHaveLength(1);
        restored.setSourceAudio(ids[0], audio, 1, 48000);
        expect(restored.unresolvedAudioSourceIds()).toEqual([]);
        expect(restored.bounce({ totalFrames: 480, numChannels: 1 })).toHaveLength(480);
      } finally {
        restored.delete();
      }
    } finally {
      project.delete();
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
      project.delete();
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

      // Guard rail: depth must be an integer >= 1.
      expect(() => project.setMaxUndoDepth(0)).toThrow();
      expect(() => project.setMaxUndoDepth(1.5)).toThrow();
    } finally {
      project.delete();
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
      project.delete();
    }
  });

  it('setClipGain / setClipFade / setClipLoop apply without throwing', () => {
    const { project, clipId } = buildAudioProject();
    try {
      expect(() => project.setClipGain(clipId, 0.0)).not.toThrow();
      expect(() => project.setClipGain(clipId, 0.75)).not.toThrow();
      expect(() =>
        project.setClipFade(
          clipId,
          { lengthPpq: 0.5, curve: 'equal-power' },
          { lengthPpq: 0.5, curve: 'equal_power' },
        ),
      ).not.toThrow();
      expect(() => project.setClipLoop(clipId, 'loop', 2)).not.toThrow();
      expect(() => project.setClipLoop(clipId, 'off', 0)).not.toThrow();
      // Optional loop crossfade: accepted when >= 0, rejected when negative.
      expect(() => project.setClipLoop(clipId, 'loop', 2, 0.5)).not.toThrow();
      expect(project.toJson()).toContain('loop_crossfade_ppq');
      expect(() => project.setClipLoop(clipId, 'loop', 2, -1)).toThrow();
      expect(() =>
        project.setClipFade(clipId, { lengthPpq: 0.5, curve: 'equalpower' }, undefined),
      ).not.toThrow();
      expect(() => project.setClipFade(clipId, { curve: 'LOG' as never }, undefined)).not.toThrow();
    } finally {
      project.delete();
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
          1,
        ),
      ).not.toThrow();
      const withTakes = project.toJson();
      expect(withTakes).not.toBe(before);
      expect(withTakes).toContain('"takes"');
      expect(withTakes).toContain('"active_take_id":1');

      expect(() =>
        project.setClipCompSegments(clipId, [
          { startPpq: 0, endPpq: 2, takeId: 1 },
          { startPpq: 2, endPpq: 4, takeId: 2 },
        ]),
      ).not.toThrow();
      expect(project.toJson()).toContain('"comp_segments"');

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
          1,
        ),
      ).toThrow();
      expect(() =>
        project.setClipCompSegments(clipId, [{ startPpq: 0, endPpq: 1, takeId: 99 }]),
      ).toThrow();
    } finally {
      project.delete();
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
      expect(project.toJson()).toContain('"active_take_id":2');
      project.undo();
      expect(project.toJson()).toContain('"clips":[]');
    } finally {
      project.delete();
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
          audio: new Float32Array(5),
          audioChannels: 2,
          audioSampleRate: 48000,
        }),
      ).toThrow();
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
      project.delete();
    }
  });

  it('addLoopRecordingTakes preserves native InvalidState error codes', () => {
    const { project, trackId } = buildAudioProject();
    try {
      project.setOverlapPolicy(0);
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
      project.delete();
    }
  });

  it('duplicateClip returns a fresh id distinct from the source', () => {
    const { project, clipId } = buildAudioProject();
    try {
      const dupId = project.duplicateClip(clipId, 8);
      expect(dupId).toBeGreaterThan(0);
      expect(dupId).not.toBe(clipId);
    } finally {
      project.delete();
    }
  });

  it('renameTrack / setTrackRoute / removeTrack apply without throwing', () => {
    const { project, trackId } = buildAudioProject();
    try {
      expect(() => project.renameTrack(trackId, 'renamed')).not.toThrow();
      expect(() => project.setTrackRoute(trackId, 'strip-1', 'master')).not.toThrow();
      expect(() => project.removeTrack(trackId)).not.toThrow();
    } finally {
      project.delete();
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
      project.delete();
    }
  });

  it('automation lane add / edit / remove round-trips', () => {
    const { project, trackId } = buildAudioProject();
    try {
      const targetParamId = project.addAutomationLane(trackId, {
        targetParamId: 1,
        points: [
          { ppq: 0, value: 0.0, curve: 'linear' },
          { ppq: 4, value: 1.0, curve: 'exponential' },
        ],
      });
      expect(targetParamId).toBe(1);
      expect(() =>
        project.editAutomationLane(trackId, targetParamId, {
          targetParamId: 1,
          points: [
            { ppq: 0, value: 0.5 },
            { ppq: 2, value: 0.25, curve: 'hold' },
          ],
        }),
      ).not.toThrow();
      expect(() => project.removeAutomationLane(trackId, targetParamId)).not.toThrow();
    } finally {
      project.delete();
    }
  });

  it('keeps omitted automation target kind on the legacy schema-v1 route', () => {
    const { project, trackId } = buildAudioProject();
    try {
      project.addAutomationLane(trackId, {
        targetParamId: 10,
        points: [{ ppq: 0, value: 0 }],
      });
      const serialized = JSON.parse(project.toJson()) as {
        version: number;
        tracks: Array<{ automation_lanes: Array<Record<string, unknown>> }>;
      };
      expect(serialized.version).toBe(1);
      expect(serialized.tracks[0].automation_lanes[0]).not.toHaveProperty('target_kind');
    } finally {
      project.delete();
    }
  });

  it('normalizes typed automation target kinds and round-trips schema-v2 JSON', () => {
    const { project, trackId } = buildAudioProject();
    try {
      project.addAutomationLane(trackId, {
        targetParamId: 20,
        targetKind: 'track-fader-db',
        points: [{ ppq: 0, value: -3 }],
      });
      project.addAutomationLane(trackId, {
        targetParamId: 21,
        targetKind: 2,
        points: [{ ppq: 0, value: 0 }],
      });
      const json = project.toJson();
      const serialized = JSON.parse(json) as {
        version: number;
        tracks: Array<{ automation_lanes: Array<Record<string, unknown>> }>;
      };
      expect(serialized.version).toBe(2);
      expect(serialized.tracks[0].automation_lanes).toEqual(
        expect.arrayContaining([
          expect.objectContaining({ target_param_id: 20, target_kind: 1 }),
          expect.objectContaining({ target_param_id: 21, target_kind: 2 }),
        ]),
      );

      const restored = Project.fromJson(json);
      try {
        expect(restored.toJson()).toBe(json);
      } finally {
        restored.delete();
      }
    } finally {
      project.delete();
    }
  });

  it('rejects typed target conflicts atomically and preserves typed lanes on legacy edit', () => {
    const { project, trackId } = buildAudioProject();
    try {
      project.addAutomationLane(trackId, {
        targetParamId: 30,
        targetKind: 'track-fader-db',
        points: [{ ppq: 0, value: -3 }],
      });
      project.addAutomationLane(trackId, {
        targetParamId: 31,
        targetKind: 'track-pan',
        points: [{ ppq: 0, value: 0 }],
      });

      const beforeConflict = project.toJson();
      expect(() =>
        project.addAutomationLane(trackId, {
          targetParamId: 32,
          targetKind: 'track-fader-db',
          points: [{ ppq: 0, value: 6 }],
        }),
      ).toThrow();
      expect(project.toJson()).toBe(beforeConflict);

      expect(() =>
        project.editAutomationLane(trackId, 30, {
          targetParamId: 30,
          targetKind: 'track-pan',
          points: [{ ppq: 0, value: 0.5 }],
        }),
      ).toThrow();
      expect(project.toJson()).toBe(beforeConflict);

      project.editAutomationLane(trackId, 30, {
        targetParamId: 30,
        points: [{ ppq: 0, value: -6 }],
      });
      const edited = JSON.parse(project.toJson()) as {
        tracks: Array<{ automation_lanes: Array<Record<string, unknown>> }>;
      };
      expect(edited.tracks[0].automation_lanes).toEqual(
        expect.arrayContaining([expect.objectContaining({ target_param_id: 30, target_kind: 1 })]),
      );
    } finally {
      project.delete();
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
        null,
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
      project.delete();
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
      project.delete();
    }
  });

  it('accepts the canonical s-curve spelling and rejects unknown curves', () => {
    const { project, trackId } = buildAudioProject();
    try {
      // Canonical 's-curve' (matches Node + the mixer) and legacy 'scurve' both work.
      expect(() =>
        project.addAutomationLane(trackId, {
          targetParamId: 1,
          points: [
            { ppq: 0, value: 0.0, curve: 's-curve' },
            { ppq: 4, value: 1.0, curve: 'scurve' },
          ],
        }),
      ).not.toThrow();
      // A misspelled curve is rejected, not silently coerced to Linear.
      expect(() =>
        project.addAutomationLane(trackId, {
          targetParamId: 1,
          // biome-ignore lint/suspicious/noExplicitAny: exercising an invalid spelling on purpose.
          points: [{ ppq: 0, value: 0.0, curve: 's_curve' as any }],
        }),
      ).toThrow();
    } finally {
      project.delete();
    }
  });

  it('annotateKeys / annotateChords apply and survive serialization', () => {
    const { project } = buildAudioProject();
    try {
      expect(() =>
        project.annotateKeys([{ startPpq: 0, endPpq: 8, tonicPc: 0, mode: 1 }]),
      ).not.toThrow();
      expect(() =>
        project.annotateChords([
          {
            startPpq: 0,
            endPpq: 4,
            rootPc: 0,
            quality: 1,
            extensions: [7],
            slashBassPc: 255,
            romanNumeral: 'I',
            modulationBoundary: false,
          },
        ]),
      ).not.toThrow();
      // The annotation streams are part of the project; serialization succeeds.
      expect(project.toJson().length).toBeGreaterThan(0);
    } finally {
      project.delete();
    }
  });

  it('assist sidecar set / count / get round-trips the payload', () => {
    const { project } = buildAudioProject();
    try {
      const payload = new Uint8Array([1, 2, 3, 4, 5]);
      project.setAssistSidecar('test.module', 1, 0, 0, 8, payload);
      expect(project.assistSidecarCount()).toBe(1);
      const sidecar = project.getAssistSidecar(0);
      expect(sidecar.moduleId).toBe('test.module');
      expect(sidecar.schemaVersion).toBe(1);
      expect(sidecar.regionEndPpq).toBe(8);
      expect(Array.from(sidecar.payload)).toEqual([1, 2, 3, 4, 5]);
    } finally {
      project.delete();
    }
  });
});

describe('Sonare WASM RealtimeEngine MIDI / parameter ops', () => {
  beforeAll(async () => {
    await init();
  });

  it('pushMidiCc / pushMidiPanic queue without throwing', () => {
    const engine = new RealtimeEngine(48000, 128, 1024, 1024);
    try {
      expect(() => engine.pushMidiCc(0, 0, 0, 7, 100, -1)).not.toThrow();
      expect(() => engine.pushMidiPanic(-1)).not.toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('pushMidiCc rejects out-of-range values', () => {
    const engine = new RealtimeEngine(48000, 128, 1024, 1024);
    try {
      expect(() => engine.pushMidiCc(0, 0, 0, 7, 200, -1)).toThrow();
      expect(() => engine.pushMidiCc(0, 99, 0, 7, 10, -1)).toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('clearParameters empties the registry', () => {
    const engine = new RealtimeEngine(48000, 128, 1024, 1024);
    try {
      engine.addParameter({
        id: 1,
        name: 'gain',
        unit: 'dB',
        minValue: -60,
        maxValue: 12,
        defaultValue: 0,
        rtSafe: true,
        defaultCurve: 1,
      });
      expect(engine.parameterCount()).toBe(1);
      engine.clearParameters();
      expect(engine.parameterCount()).toBe(0);
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
          defaultCurve: 1,
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
