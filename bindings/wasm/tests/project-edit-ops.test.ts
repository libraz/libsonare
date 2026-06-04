/**
 * WASM coverage for the headless-DAW edit operations, MIR
 * annotations, assist sidecars, the realtime-engine MIDI/parameter ops, and
 * masteringInsertNames. These mirror the Node/Python surface and drive the same
 * C ABI / core through embind.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringInsertNames, Project, RealtimeEngine } from '../dist/index.js';

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

  it('removeClip + undo round-trips through the edit history', () => {
    const { project, clipId } = buildAudioProject();
    try {
      expect(() => project.removeClip(clipId)).not.toThrow();
      // The clip is gone; operating on it again fails.
      expect(() => project.setClipGain(clipId, 0.5)).toThrow();
      // Undo restores it so the gain edit then succeeds.
      project.undo();
      expect(() => project.setClipGain(clipId, 0.5)).not.toThrow();
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
          { lengthPpq: 0.5, curve: 'equalPower' },
          { lengthPpq: 0.5, curve: 'logarithmic' },
        ),
      ).not.toThrow();
      expect(() => project.setClipLoop(clipId, 'loop', 2)).not.toThrow();
      expect(() => project.setClipLoop(clipId, 'off', 0)).not.toThrow();
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

  it('automation lane add / edit / remove round-trips', () => {
    const { project, trackId } = buildAudioProject();
    try {
      const laneIndex = project.addAutomationLane(trackId, {
        targetParamId: 1,
        points: [
          { ppq: 0, value: 0.0, curve: 'linear' },
          { ppq: 4, value: 1.0, curve: 'exponential' },
        ],
      });
      expect(laneIndex).toBeGreaterThanOrEqual(0);
      expect(() =>
        project.editAutomationLane(trackId, laneIndex, {
          targetParamId: 1,
          points: [
            { ppq: 0, value: 0.5 },
            { ppq: 2, value: 0.25, curve: 'hold' },
          ],
        }),
      ).not.toThrow();
      expect(() => project.removeAutomationLane(trackId, laneIndex)).not.toThrow();
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
