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
import type {
  MixAssistantTrack,
  MixingAssistantSuggestRequest,
  MixSceneDocument,
  MixSourceClass,
} from '../dist/index.js';
import {
  init,
  mixingAssistantSourceClassFromName,
  mixingAssistantSourceClassNames,
  mixingAssistantSuggest,
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
    { id: 'bass', name: 'Bass', channels: [pulsedTone(70, DURATION, 4)] },
    { id: 'keys', name: 'Keys', channels: [pulsedTone(440, DURATION, 2)] },
    { id: 'hat', name: 'Hi-Hat', channels: [pulsedTone(7000, DURATION, 8)] },
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
  return [{ id: 'vox', name: 'Lead Vox', channels: [voiceWithRumble()] }];
}

/** Ids of the strips and buses carrying a high-pass insert, in scene order. */
function highPassOwners(scene: MixSceneDocument): string[] {
  return [...scene.strips, ...scene.buses]
    .filter((node) => node.inserts.some((insert) => insert.processor === 'eq.cutFilter'))
    .map((node) => node.id);
}

/** The reasoning line a proposed high-pass writes, matched on its opening. */
const HIGH_PASS_REASON = /^high-passed vox at 80 Hz, where /;

describe('mixing assistant (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  describe('mixingAssistantSuggest', () => {
    it('profiles a mono-only track set and explains the suggested scene', () => {
      const result = mixingAssistantSuggest({ tracks: musicalTracks(), sampleRate: SR });

      expect(result.tracks.map((track) => track.stripId)).toEqual(['bass', 'keys', 'hat']);
      expect(result.tracks.every((track) => track.numChannels === 1)).toBe(true);
      expect(result.mix.trackCount).toBe(3);
      expect(Array.isArray(result.explanation)).toBe(true);
      expect(result.explanation.length).toBeGreaterThan(0);
      expect(result.scene).toBeTypeOf('object');
    });

    it('accepts a set mixing mono and stereo tracks', () => {
      const right = pulsedTone(441, DURATION, 2);
      const result = mixingAssistantSuggest({
        tracks: [
          { id: 'bass', channels: [pulsedTone(70, DURATION, 4)] },
          { id: 'keys', channels: [pulsedTone(440, DURATION, 2), right] },
        ],
        sampleRate: SR,
      });

      expect(result.tracks.map((track) => track.numChannels)).toEqual([1, 2]);
    });

    it('keeps each track at its own length instead of truncating to the shortest', () => {
      const result = mixingAssistantSuggest({
        tracks: [
          { id: 'long', channels: [pulsedTone(220, 1.0, 2)] },
          { id: 'short', channels: [pulsedTone(660, 0.5, 4)] },
        ],
        sampleRate: SR,
      });

      const [long, short] = result.tracks;
      expect(long.durationSec).toBeGreaterThan(short.durationSec);
      expect(short.durationSec).toBeGreaterThan(0);
    });

    it('returns an empty suggestion for no tracks', () => {
      const result = mixingAssistantSuggest({ tracks: [], sampleRate: SR });

      expect(result.tracks).toEqual([]);
      expect(result.explanation).toEqual([]);
      expect(result.mix.trackCount).toBe(0);
    });

    it("omits the exclusion reason for a usable track and names a silent track's exclusion", () => {
      const result = mixingAssistantSuggest({
        tracks: [
          { id: 'silent', channels: [new Float32Array(Math.floor(SR * DURATION))] },
          { id: 'keys', channels: [pulsedTone(440, DURATION, 2)] },
        ],
        sampleRate: SR,
      });

      const silent = result.tracks.find((track) => track.stripId === 'silent');
      const keys = result.tracks.find((track) => track.stripId === 'keys');
      expect(silent?.usable).toBe(false);
      expect(silent?.exclusionReason).toBe('track is silent');
      expect(keys?.usable).toBe(true);
      expect(keys).not.toHaveProperty('exclusionReason');
    });

    it('suggests nothing when every decision domain is off', () => {
      const result = mixingAssistantSuggest({
        tracks: musicalTracks(),
        sampleRate: SR,
        enableStructure: false,
        enableGain: false,
        enableBalance: false,
        enableEq: false,
        enableDynamics: false,
        enableImage: false,
      });

      expect(result.explanation).toEqual([]);
      expect(result.tracks).toHaveLength(3);
    });

    it('accepts every assistant option and keeps the result shape', () => {
      const result = mixingAssistantSuggest({
        tracks: musicalTracks(),
        sampleRate: SR,
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
      });

      expect(result.tracks.map((track) => track.stripId)).toEqual(['bass', 'keys', 'hat']);
      expect(result.mix.trackCount).toBe(3);
      expect(Array.isArray(result.explanation)).toBe(true);
    });

    it('proposes a high-pass only once the switch is on', () => {
      const off = mixingAssistantSuggest({ tracks: rumblingVoiceTracks(), sampleRate: SR });
      const on = mixingAssistantSuggest({
        tracks: rumblingVoiceTracks(),
        sampleRate: SR,
        enableHighPass: true,
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

  describe('mixingAssistantSuggest scene', () => {
    it('carries one strip per track', () => {
      const scene = mixingAssistantSuggest({ tracks: musicalTracks(), sampleRate: SR }).scene;

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
    });

    it('acts on a source class only a track name can supply', () => {
      // `keys` is one of the classes no measurement separates from its
      // neighbours, so the classifier takes it from the track's name unless the
      // measurement contradicts it. Before that path existed the
      // class was advertised by mixingAssistantSourceClassNames() and reachable from no
      // entry point, and this track came back unclassified — which every
      // decision stage skips, so it was routed to master and left alone.
      const result = mixingAssistantSuggest({ tracks: musicalTracks(), sampleRate: SR });
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
  });

  describe('source classes', () => {
    it('reports a non-empty name table each of whose entries resolves', () => {
      const names = mixingAssistantSourceClassNames();

      expect(names.length).toBeGreaterThan(0);
      for (const name of names) {
        expect(mixingAssistantSourceClassFromName(name)).toBeGreaterThanOrEqual(0);
      }
    });

    it('resolves a name to its index in the table', () => {
      const names = mixingAssistantSourceClassNames();

      expect(mixingAssistantSourceClassFromName(names[0])).toBe(0);
      expect(mixingAssistantSourceClassFromName(names[names.length - 1])).toBe(names.length - 1);
    });

    it('refuses an unknown name with a RangeError', () => {
      expect(() =>
        mixingAssistantSourceClassFromName('definitely-not-a-source-class' as MixSourceClass),
      ).toThrow(RangeError);
      expect(() => mixingAssistantSourceClassFromName('' as MixSourceClass)).toThrow(RangeError);
    });
  });

  describe('input validation', () => {
    it('refuses a zero, negative or non-finite tempo by name instead of reading it as the fallback', () => {
      for (const tempoBpm of [0, -120, Number.NaN, Number.POSITIVE_INFINITY]) {
        expect(() =>
          mixingAssistantSuggest({ tracks: musicalTracks(), sampleRate: SR, tempoBpm }),
        ).toThrow(
          new RangeError(
            "mixingAssistantSuggest: tempoBpm must be finite and greater than 0; omit it to use the transport's fallback tempo",
          ),
        );
      }
    });

    it('rejects a non-positive sample rate', () => {
      expect(() => mixingAssistantSuggest({ tracks: musicalTracks(), sampleRate: 0 })).toThrow();
      expect(() =>
        mixingAssistantSuggest({ tracks: musicalTracks(), sampleRate: -48000 }),
      ).toThrow();
    });

    it('requires a sample rate rather than inventing one', () => {
      // This surface used to default to 48000, which read 44.1 kHz material
      // 8.8% off across every band edge, every filter corner and every
      // alignment lag without saying so. Node and Python both demand it.
      const request = { tracks: musicalTracks() } as unknown as MixingAssistantSuggestRequest;
      expect(() => mixingAssistantSuggest(request)).toThrow();
    });

    it('accepts exactly the integer sample rates within [8000, 384000]', () => {
      const at = (sampleRate: number) => () =>
        mixingAssistantSuggest({ tracks: musicalTracks(), sampleRate });
      expect(at(8000)).not.toThrow();
      expect(at(384000)).not.toThrow();
      expect(at(7999)).toThrow(RangeError);
      expect(at(384001)).toThrow(RangeError);
      expect(at(8000.5)).toThrow(RangeError);
    });

    it('rejects two tracks sharing an id', () => {
      // The guard lives in the core rather than in this file, so all four
      // surfaces reject the same input. Absorbed instead, the duplicate ships a
      // scene the mixer refuses to load, and the refusal names the scene rather
      // than the two tracks that collided.
      const left = pulsedTone(440, DURATION, 2);
      expect(() =>
        mixingAssistantSuggest({
          tracks: [
            { id: 'same', channels: [left] },
            { id: 'same', channels: [left] },
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
      const result = mixingAssistantSuggest({
        tracks: [{ id: 'poisoned', channels: [left] }],
        sampleRate: SR,
      });
      expect(result.tracks[0].usable).toBe(false);
      expect(result.tracks[0].exclusionReason).toBe('track has non-finite samples');
    });

    it('rejects a missing or empty track id', () => {
      const left = pulsedTone(440, DURATION, 2);
      expect(() =>
        mixingAssistantSuggest({
          tracks: [{ channels: [left] } as unknown as MixAssistantTrack],
          sampleRate: SR,
        }),
      ).toThrow();
      expect(() =>
        mixingAssistantSuggest({ tracks: [{ id: '', channels: [left] }], sampleRate: SR }),
      ).toThrow();
    });

    it('rejects a missing or wrongly typed sample buffer', () => {
      expect(() =>
        mixingAssistantSuggest({
          tracks: [{ id: 'keys' } as unknown as MixAssistantTrack],
          sampleRate: SR,
        }),
      ).toThrow();
      expect(() =>
        mixingAssistantSuggest({
          tracks: [{ id: 'keys', channels: [[0, 1, 0] as unknown as Float32Array] }],
          sampleRate: SR,
        }),
      ).toThrow();
    });

    it('takes one or two channels of equal length and refuses any other track shape', () => {
      const plane = pulsedTone(440, DURATION, 2);
      const suggest = (track: unknown) => () =>
        mixingAssistantSuggest({ tracks: [track as MixAssistantTrack], sampleRate: SR });
      expect(suggest({ id: 'keys' })).toThrow(TypeError);
      expect(suggest({ id: 'keys', channels: plane })).toThrow(TypeError);
      expect(suggest({ id: 'keys', channels: [Array.from(plane)] })).toThrow(TypeError);
      expect(suggest({ id: 'keys', channels: [] })).toThrow(
        /tracks\[0\]\.channels must hold 1 or 2/,
      );
      expect(suggest({ id: 'keys', channels: [] })).toThrow(RangeError);
      expect(suggest({ id: 'keys', channels: [plane, plane, plane] })).toThrow(RangeError);
      expect(suggest({ id: 'keys', channels: [plane, pulsedTone(441, DURATION / 2, 2)] })).toThrow(
        RangeError,
      );
    });

    it('rejects a tracks value that is not an array', () => {
      expect(() =>
        mixingAssistantSuggest({ tracks: null as unknown as MixAssistantTrack[], sampleRate: SR }),
      ).toThrow();
    });

    it.each([
      ['nFft', 524288, 'nFft rescaled to sampleRate must fit the largest FFT size'],
      [
        'hopLength',
        2 ** 31 - 1,
        'hopLength rescaled to sampleRate must fit the largest hop length',
      ],
    ])('names %s, not params, when its rescale outgrows the reader', (key, value, text) => {
      expect(() =>
        mixingAssistantSuggest({ tracks: musicalTracks(), sampleRate: 96000, [key]: value }),
      ).toThrow(new RangeError(`mixingAssistantSuggest: ${text}`));
    });

    it('rejects an option value that is neither a number nor a boolean', () => {
      expect(() =>
        mixingAssistantSuggest({
          tracks: musicalTracks(),
          sampleRate: SR,
          nFft: '2048' as unknown as number,
        }),
      ).toThrow();
    });
  });
});
