/**
 * WASM coverage for the offline mixing assistant:
 *  - planar mono / stereo / ragged-length track sets;
 *  - degenerate input (no tracks, silent tracks) yielding an empty suggestion;
 *  - every decision domain switched off leaving no explanation;
 *  - the source-class name table and its reverse lookup;
 *  - the input validation the WASM wrappers carry themselves, since the C-ABI
 *    guards are not linked into this surface.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import type { MixAssistantTrack, SuggestMixSceneRequest } from '../dist/index.js';
import {
  init,
  mixSourceClassFromName,
  mixSourceClassNames,
  suggestMixScene,
  suggestMixSceneJson,
} from '../dist/index.js';
import { sine } from './_helpers';

const SR = 48000;
const DURATION = 0.6;

/** A tone with a short attack every beat, so the profiler sees onsets. */
function pulsedTone(freqHz: number, durationSec: number, pulseHz: number): Float32Array {
  const tone = sine(freqHz, durationSec, { sampleRate: SR });
  for (let i = 0; i < tone.length; i++) {
    const phase = (i * pulseHz) / SR;
    tone[i] *= Math.exp(-6 * (phase - Math.floor(phase)));
  }
  return tone;
}

function musicalTracks(): MixAssistantTrack[] {
  return [
    { id: 'bass', name: 'Bass', left: pulsedTone(70, DURATION, 4) },
    { id: 'keys', name: 'Keys', left: pulsedTone(440, DURATION, 2) },
    { id: 'hat', name: 'Hi-Hat', left: pulsedTone(7000, DURATION, 8) },
  ];
}

describe('mixing assistant (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  describe('suggestMixScene', () => {
    it('profiles a mono-only track set and explains the suggested scene', () => {
      const result = suggestMixScene({ tracks: musicalTracks(), sampleRate: SR });

      expect(result.tracks.map((track) => track.stripId)).toEqual(['bass', 'keys', 'hat']);
      expect(result.tracks.every((track) => track.channelCount === 1)).toBe(true);
      expect(result.mix.trackCount).toBe(3);
      expect(Array.isArray(result.explanation)).toBe(true);
      expect(result.explanation.length).toBeGreaterThan(0);
      expect(result.scene).toBeTypeOf('object');
    });

    it('accepts a set mixing mono and stereo tracks', () => {
      const right = pulsedTone(441, DURATION, 2);
      const result = suggestMixScene({
        tracks: [
          { id: 'bass', left: pulsedTone(70, DURATION, 4) },
          { id: 'keys', left: pulsedTone(440, DURATION, 2), right },
        ],
        sampleRate: SR,
      });

      expect(result.tracks.map((track) => track.channelCount)).toEqual([1, 2]);
    });

    it('keeps each track at its own length instead of truncating to the shortest', () => {
      const result = suggestMixScene({
        tracks: [
          { id: 'long', left: pulsedTone(220, 1.0, 2) },
          { id: 'short', left: pulsedTone(660, 0.5, 4) },
        ],
        sampleRate: SR,
      });

      const [long, short] = result.tracks;
      expect(long.durationSec).toBeGreaterThan(short.durationSec);
      expect(short.durationSec).toBeGreaterThan(0);
    });

    it('returns an empty suggestion for no tracks', () => {
      const result = suggestMixScene({ tracks: [], sampleRate: SR });

      expect(result.tracks).toEqual([]);
      expect(result.explanation).toEqual([]);
      expect(result.mix.trackCount).toBe(0);
    });

    it('marks a silent track unusable without excluding the rest', () => {
      const result = suggestMixScene({
        tracks: [
          { id: 'silent', left: new Float32Array(Math.floor(SR * DURATION)) },
          { id: 'keys', left: pulsedTone(440, DURATION, 2) },
        ],
        sampleRate: SR,
      });

      const silent = result.tracks.find((track) => track.stripId === 'silent');
      const keys = result.tracks.find((track) => track.stripId === 'keys');
      expect(silent?.usable).toBe(false);
      expect(silent?.exclusionReason).not.toBe('');
      expect(keys?.usable).toBe(true);
    });

    it('suggests nothing when every decision domain is off', () => {
      const result = suggestMixScene({
        tracks: musicalTracks(),
        sampleRate: SR,
        options: {
          enableStructure: false,
          enableGain: false,
          enableBalance: false,
          enableEq: false,
          enableDynamics: false,
          enableImage: false,
        },
      });

      expect(result.explanation).toEqual([]);
      expect(result.tracks).toHaveLength(3);
    });

    it('accepts every assistant option and keeps the result shape', () => {
      const result = suggestMixScene({
        tracks: musicalTracks(),
        sampleRate: SR,
        options: {
          targetTrackLufs: -20,
          suggestionStrength: 0.5,
          eqMaxCutDb: 3,
          mixBusHeadroomDbtp: -8,
          enableStructure: true,
          enableGain: true,
          enableBalance: true,
          enableEq: true,
          enableDynamics: true,
          enableImage: true,
          nFft: 1024,
          hopLength: 256,
        },
      });

      expect(result.tracks.map((track) => track.stripId)).toEqual(['bass', 'keys', 'hat']);
      expect(result.mix.trackCount).toBe(3);
      expect(Array.isArray(result.explanation)).toBe(true);
    });
  });

  describe('suggestMixSceneJson', () => {
    it('returns only the scene, carrying one strip per track', () => {
      const json = suggestMixSceneJson({ tracks: musicalTracks(), sampleRate: SR });
      const scene = JSON.parse(json) as { strips: { id: string }[] };

      expect(scene.strips.map((strip) => strip.id)).toEqual(['bass', 'keys', 'hat']);
      expect(scene).not.toHaveProperty('explanation');
    });

    it('agrees with the scene nested in the full result', () => {
      const request: SuggestMixSceneRequest = { tracks: musicalTracks(), sampleRate: SR };

      expect(JSON.parse(suggestMixSceneJson(request))).toEqual(suggestMixScene(request).scene);
    });
  });

  describe('source classes', () => {
    it('reports a non-empty name table each of whose entries resolves', () => {
      const names = mixSourceClassNames();

      expect(names.length).toBeGreaterThan(0);
      for (const name of names) {
        expect(mixSourceClassFromName(name)).toBeGreaterThanOrEqual(0);
      }
    });

    it('resolves a name to its index in the table', () => {
      const names = mixSourceClassNames();

      expect(mixSourceClassFromName(names[0])).toBe(0);
      expect(mixSourceClassFromName(names[names.length - 1])).toBe(names.length - 1);
    });

    it('reports -1 for an unknown name', () => {
      expect(mixSourceClassFromName('definitely-not-a-source-class')).toBe(-1);
      expect(mixSourceClassFromName('')).toBe(-1);
    });
  });

  describe('input validation', () => {
    it('rejects a non-positive sample rate', () => {
      expect(() => suggestMixScene({ tracks: musicalTracks(), sampleRate: 0 })).toThrow();
      expect(() => suggestMixScene({ tracks: musicalTracks(), sampleRate: -48000 })).toThrow();
    });

    it('rejects a missing or empty track id', () => {
      const left = pulsedTone(440, DURATION, 2);
      expect(() =>
        suggestMixScene({
          tracks: [{ left } as unknown as MixAssistantTrack],
          sampleRate: SR,
        }),
      ).toThrow();
      expect(() => suggestMixScene({ tracks: [{ id: '', left }], sampleRate: SR })).toThrow();
    });

    it('rejects a missing or wrongly typed sample buffer', () => {
      expect(() =>
        suggestMixScene({
          tracks: [{ id: 'keys' } as unknown as MixAssistantTrack],
          sampleRate: SR,
        }),
      ).toThrow();
      expect(() =>
        suggestMixScene({
          tracks: [{ id: 'keys', left: [0, 1, 0] as unknown as Float32Array }],
          sampleRate: SR,
        }),
      ).toThrow();
    });

    it('rejects a right plane whose length differs from its left', () => {
      expect(() =>
        suggestMixScene({
          tracks: [
            {
              id: 'keys',
              left: pulsedTone(440, DURATION, 2),
              right: pulsedTone(441, DURATION / 2, 2),
            },
          ],
          sampleRate: SR,
        }),
      ).toThrow();
    });

    it('rejects a tracks value that is not an array', () => {
      expect(() =>
        suggestMixScene({ tracks: null as unknown as MixAssistantTrack[], sampleRate: SR }),
      ).toThrow();
    });

    it('rejects an option value that is neither a number nor a boolean', () => {
      expect(() =>
        suggestMixScene({
          tracks: musicalTracks(),
          sampleRate: SR,
          options: { nFft: '2048' as unknown as number },
        }),
      ).toThrow();
    });
  });
});
