/**
 * WASM coverage for the offline mixing assistant:
 *  - planar mono / stereo / ragged-length track sets;
 *  - degenerate input (no tracks, silent tracks) yielding an empty suggestion;
 *  - every decision domain switched off leaving no explanation;
 *  - the high-pass switch, off by default, reaching the scene when asked for;
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

/** Partial weights of the voice-like series below; index 0 is the fundamental. */
const VOICE_PARTIALS = [0.3, 0.7, 0.9, 0.85, 0.8, 0.7, 0.6, 0.55, 0.5, 0.45, 0.4, 0.35];

/**
 * A sustained voice-like harmonic series over 180 Hz with stand rumble under it.
 *
 * The rumble is the point of the fixture. The assistant proposes a high-pass
 * only where the share of a track's energy below its class corner reads as
 * residue -- between 0.5% and 10% -- and `musicalTracks` carries nothing at all
 * under its corners, so on that material `enableHighPass` changes nothing, the
 * two documents come back identical, and the case would be satisfied by a
 * binding that dropped the option. The 40 Hz tone puts about 2% of this track's
 * energy below the 80 Hz vocal corner, which is inside the window.
 */
function voiceWithRumble(durationSec = DURATION): Float32Array {
  const out = new Float32Array(Math.floor(SR * durationSec));
  for (let i = 0; i < out.length; i++) {
    const t = i / SR;
    let voice = 0;
    for (let partial = 0; partial < VOICE_PARTIALS.length; partial++) {
      voice += VOICE_PARTIALS[partial] * Math.sin(2 * Math.PI * 180 * (partial + 1) * t);
    }
    out[i] = 0.045 * voice + 0.014 * Math.sin(2 * Math.PI * 40 * t);
  }
  return out;
}

/** The high-pass case owns its input rather than sharing `musicalTracks`. */
function rumblingVoiceTracks(): MixAssistantTrack[] {
  return [{ id: 'vox', name: 'Lead Vox', left: voiceWithRumble() }];
}

/** Just enough of the scene document to find an insert; it arrives untyped here. */
interface SceneView {
  strips: { id: string; inserts: { processor: string }[] }[];
  buses: { id: string; inserts: { processor: string }[] }[];
}

/** Ids of the strips and buses carrying a high-pass insert, in scene order. */
function highPassOwners(scene: Record<string, unknown>): string[] {
  const view = scene as unknown as SceneView;
  return [...view.strips, ...view.buses]
    .filter((node) => node.inserts.some((insert) => insert.processor === 'eq.cutFilter'))
    .map((node) => node.id);
}

/** The reasoning line a proposed high-pass writes, matched on its opening. */
const HIGH_PASS_REASON = /^high-passed vox at 80 Hz, where /;

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
          enableHighPass: true,
          nFft: 1024,
          hopLength: 256,
        },
      });

      expect(result.tracks.map((track) => track.stripId)).toEqual(['bass', 'keys', 'hat']);
      expect(result.mix.trackCount).toBe(3);
      expect(Array.isArray(result.explanation)).toBe(true);
    });

    it('proposes a high-pass only once the switch is on', () => {
      const off = suggestMixScene({ tracks: rumblingVoiceTracks(), sampleRate: SR });
      const on = suggestMixScene({
        tracks: rumblingVoiceTracks(),
        sampleRate: SR,
        options: { enableHighPass: true },
      });

      // Off by default: the measurement is not taken at all, so no filter and
      // no line about one.
      expect(highPassOwners(off.scene)).toEqual([]);
      expect(off.explanation.filter((line) => HIGH_PASS_REASON.test(line))).toEqual([]);

      // On: the vocal track's 80 Hz corner earns a pre-fader filter of its own.
      expect(highPassOwners(on.scene)).toEqual(['vox']);
      expect(on.explanation.filter((line) => HIGH_PASS_REASON.test(line))).toHaveLength(1);
    });
  });

  describe('suggestMixSceneJson', () => {
    it('returns only the scene, carrying one strip per track', () => {
      const json = suggestMixSceneJson({ tracks: musicalTracks(), sampleRate: SR });
      const scene = JSON.parse(json) as {
        strips: { id: string }[];
        buses: { id: string }[];
        connections: { source: string; destination: string }[];
      };

      // A scene carries one strip per track, and one more for each effect bus
      // the structure stage proposes: an effect return is a strip too. Those are
      // told apart structurally — a return strip is the one a bus feeds — rather
      // than by counting or by name, because how many effect buses the assistant
      // suggests is a decision this test has no stake in, and pinning it here
      // would freeze it. What is asserted is the part the name promises: every
      // track reaches the scene, once, in input order.
      const busIds = new Set(scene.buses.map((bus) => bus.id));
      const returnStrips = new Set(
        scene.connections
          .filter((connection) => busIds.has(connection.source))
          .map((connection) => connection.destination),
      );
      const trackStrips = scene.strips
        .map((strip) => strip.id)
        .filter((id) => !returnStrips.has(id));

      expect(trackStrips).toEqual(['bass', 'keys', 'hat']);
      expect(scene).not.toHaveProperty('explanation');
    });

    it('acts on a source class only a track name can supply', () => {
      // `keys` is one of the four classes no measurement separates from its
      // neighbours, so the classifier takes it from the track's name and only
      // when the decision table resolved nothing. Before that path existed the
      // class was advertised by mixSourceClassNames() and reachable from no
      // entry point, and this track came back unclassified — which every
      // decision stage skips, so it was routed to master and left alone.
      const result = suggestMixScene({ tracks: musicalTracks(), sampleRate: SR });
      const keys = result.tracks.find((track) => track.stripId === 'keys');

      expect(keys?.source).toBe('keys');
      expect(keys?.sourceConfidence).toBeGreaterThan(0);

      // Classified means acted on, so the class has to reach the scene and not
      // only the report. Inserts and sends are the evidence because every stage
      // that emits one reads the class first: an unclassified track carries a
      // staging trim and nothing else, which is what this track used to get.
      //
      // Do not simplify this to a string match on `explanation`. Searching the
      // lines for the track's name looks equivalent and is not: an unclassified
      // track is named there too, by the line saying it was routed straight to
      // master *because* it was never classified. That assertion passes whether
      // or not the class was resolved, which is the one outcome this case
      // exists to tell apart.
      const strip = result.scene.strips.find((candidate) => candidate.id === 'keys');
      expect(strip?.inserts.length ?? 0).toBeGreaterThan(0);
      expect(strip?.sends.length ?? 0).toBeGreaterThan(0);
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

    it('requires a sample rate rather than inventing one', () => {
      // This surface used to default to 48000, which read 44.1 kHz material
      // 8.8% off across every band edge, every filter corner and every
      // alignment lag without saying so. Node and Python both demand it.
      const request = { tracks: musicalTracks() } as unknown as SuggestMixSceneRequest;
      expect(() => suggestMixScene(request)).toThrow();
      expect(() => suggestMixSceneJson(request)).toThrow();
    });

    it('rejects two tracks sharing an id', () => {
      // The guard lives in the core rather than in this file, so all four
      // surfaces reject the same input. Absorbed instead, the duplicate ships a
      // scene the mixer refuses to load, and the refusal names the scene rather
      // than the two tracks that collided.
      const left = pulsedTone(440, DURATION, 2);
      expect(() =>
        suggestMixScene({
          tracks: [
            { id: 'same', left },
            { id: 'same', left },
          ],
          sampleRate: SR,
        }),
      ).toThrow();
    });

    it('reports a non-finite sample as its own exclusion, not as silence', () => {
      // One NaN reaches the integrated loudness as -inf, so without the core
      // guard the track comes back excluded for being silent -- a statement
      // about the material rather than about the buffer.
      const left = pulsedTone(440, DURATION, 2);
      left[100] = Number.NaN;
      const result = suggestMixScene({
        tracks: [{ id: 'poisoned', left }],
        sampleRate: SR,
      });
      expect(result.tracks[0].usable).toBe(false);
      expect(result.tracks[0].exclusionReason).toBe('track has non-finite samples');
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
