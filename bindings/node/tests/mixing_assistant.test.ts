import { describe, expect, it } from 'vitest';
import {
  type MixAssistantTrack,
  Mixer,
  type MixSceneDocument,
  mixSourceClassFromName,
  mixSourceClassNames,
  suggestMixScene,
  suggestMixSceneJson,
} from '../src/index.js';
import { sine } from './_helpers.js';

const SR = 22050;

/** A steady click train, so the assistant sees a percussive source. */
function clicks(durationSec: number, intervalSec: number, sampleRate = SR): Float32Array {
  const out = new Float32Array(Math.floor(sampleRate * durationSec));
  const period = Math.floor(sampleRate * intervalSec);
  const clickLength = Math.floor(sampleRate / 200);
  for (let start = 0; start < out.length; start += period) {
    for (let i = 0; i < clickLength && start + i < out.length; i++) {
      out[start + i] = (1 - i / clickLength) * 0.9;
    }
  }
  return out;
}

function baseTracks(): MixAssistantTrack[] {
  return [
    { id: 'kick', name: 'Kick', left: clicks(1.0, 0.5) },
    { id: 'bass', name: 'Bass', left: sine(80, 1.0, { sampleRate: SR, amp: 0.4 }) },
    { id: 'lead', name: 'Lead', left: sine(880, 1.0, { sampleRate: SR, amp: 0.2 }) },
  ];
}

/** Partial weights of the voice-like series below; index 0 is the fundamental. */
const VOICE_PARTIALS = [0.3, 0.7, 0.9, 0.85, 0.8, 0.7, 0.6, 0.55, 0.5, 0.45, 0.4, 0.35];

/**
 * A sustained voice-like harmonic series over 180 Hz with stand rumble under it.
 *
 * The rumble is the point of the fixture. The assistant proposes a high-pass
 * only where the share of a track's energy below its class corner reads as
 * residue -- between 0.5% and 10% -- and `baseTracks` carries nothing at all
 * under its corners, so on that material `enableHighPass` changes nothing, the
 * two documents come back identical, and the case would be satisfied by a
 * binding that dropped the option. The 40 Hz tone puts about 2% of this track's
 * energy below the 80 Hz vocal corner, which is inside the window.
 */
function voiceWithRumble(durationSec = 0.6, sampleRate = SR): Float32Array {
  const out = new Float32Array(Math.floor(sampleRate * durationSec));
  for (let i = 0; i < out.length; i++) {
    const t = i / sampleRate;
    let voice = 0;
    for (let partial = 0; partial < VOICE_PARTIALS.length; partial++) {
      voice += VOICE_PARTIALS[partial] * Math.sin(2 * Math.PI * 180 * (partial + 1) * t);
    }
    out[i] = 0.045 * voice + 0.014 * Math.sin(2 * Math.PI * 40 * t);
  }
  return out;
}

/** The high-pass case owns its input rather than sharing `baseTracks`. */
function rumblingVoiceTracks(): MixAssistantTrack[] {
  return [{ id: 'vox', name: 'Lead Vox', left: voiceWithRumble() }];
}

/** Ids of the strips and buses carrying a high-pass insert, in scene order. */
function highPassOwners(scene: MixSceneDocument): string[] {
  return [...scene.strips, ...scene.buses]
    .filter((node) => node.inserts.some((insert) => insert.processor === 'eq.cutFilter'))
    .map((node) => node.id);
}

/** The reasoning line a proposed high-pass writes, matched on its opening. */
const HIGH_PASS_REASON = /^high-passed vox at 80 Hz, where /;

describe('mixing assistant native binding', () => {
  it('suggests a scene from mono tracks', () => {
    const result = suggestMixScene({ tracks: baseTracks(), sampleRate: SR });

    expect(result.scene.strips.map((strip) => strip.id)).toEqual(['kick', 'bass', 'lead']);
    expect(result.tracks).toHaveLength(3);
    expect(result.tracks[0].stripId).toBe('kick');
    expect(result.tracks[0].bandOccupancy).toHaveProperty('sub');
    expect(result.mix.trackCount).toBe(3);
    expect(Array.isArray(result.explanation)).toBe(true);
    expect(result.explanation.length).toBeGreaterThan(0);
  });

  it('accepts a mix of mono and stereo tracks', () => {
    const stereoLeft = sine(440, 1.0, { sampleRate: SR, amp: 0.3 });
    const stereoRight = sine(441, 1.0, { sampleRate: SR, amp: 0.3 });
    const result = suggestMixScene({
      tracks: [
        { id: 'mono', left: sine(220, 1.0, { sampleRate: SR }) },
        { id: 'stereo', left: stereoLeft, right: stereoRight },
      ],
      sampleRate: SR,
    });

    expect(result.tracks.map((track) => track.channelCount)).toEqual([1, 2]);
  });

  it('accepts tracks of different lengths', () => {
    const result = suggestMixScene({
      tracks: [
        { id: 'short', left: sine(220, 0.4, { sampleRate: SR }) },
        { id: 'long', left: sine(660, 1.2, { sampleRate: SR }) },
      ],
      sampleRate: SR,
    });

    const durations = result.tracks.map((track) => track.durationSec);
    expect(durations[0]).toBeLessThan(durations[1]);
    expect(result.scene.strips).toHaveLength(2);
  });

  it('returns an empty suggestion for zero tracks', () => {
    const result = suggestMixScene({ tracks: [], sampleRate: SR });

    expect(result.tracks).toEqual([]);
    expect(result.scene.strips).toEqual([]);
    expect(result.explanation).toEqual([]);
    expect(result.mix.trackCount).toBe(0);
  });

  it('marks a silent track unusable without failing the request', () => {
    const result = suggestMixScene({
      tracks: [
        { id: 'silent', left: new Float32Array(SR) },
        { id: 'tone', left: sine(440, 1.0, { sampleRate: SR }) },
      ],
      sampleRate: SR,
    });

    const silent = result.tracks.find((track) => track.stripId === 'silent');
    expect(silent?.usable).toBe(false);
    expect(silent?.exclusionReason).not.toBe('');
    expect(result.tracks.find((track) => track.stripId === 'tone')?.usable).toBe(true);
  });
});

describe('mixing assistant options', () => {
  it('produces no explanation when every decision domain is off', () => {
    const result = suggestMixScene({
      tracks: baseTracks(),
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
  });

  it('a zero suggestion strength leaves the staged levels alone', () => {
    const full = suggestMixScene({ tracks: baseTracks(), sampleRate: SR });
    const none = suggestMixScene({
      tracks: baseTracks(),
      sampleRate: SR,
      options: { suggestionStrength: 0 },
    });

    expect(none.scene.strips.every((strip) => strip.inputTrimDb === 0)).toBe(true);
    expect(full.scene.strips.some((strip) => strip.inputTrimDb !== 0)).toBe(true);
  });

  it('honours the target track loudness', () => {
    // Gain staging writes the level decision as strip input trim, one static
    // offset from each measured level towards the absolute target, so a higher
    // target must raise every trim.
    const quiet = suggestMixScene({
      tracks: baseTracks(),
      sampleRate: SR,
      options: { targetTrackLufs: -30 },
    });
    const loud = suggestMixScene({
      tracks: baseTracks(),
      sampleRate: SR,
      options: { targetTrackLufs: -12 },
    });

    const trims = (strips: { inputTrimDb: number }[]) => strips.map((strip) => strip.inputTrimDb);
    expect(trims(loud.scene.strips)).toHaveLength(trims(quiet.scene.strips).length);
    trims(loud.scene.strips).forEach((db, index) => {
      expect(db).toBeGreaterThan(trims(quiet.scene.strips)[index]);
    });
  });

  it('switching one domain off drops only that domain of suggestions', () => {
    const all = suggestMixScene({ tracks: baseTracks(), sampleRate: SR });
    // The EQ domain is omitted here on purpose: this material provokes no EQ
    // decision even with the domain on, so switching it off would prove nothing.
    // The all-off case above is what covers it.
    const domains = [
      'enableStructure',
      'enableGain',
      'enableBalance',
      'enableDynamics',
      'enableImage',
    ] as const;

    for (const domain of domains) {
      const off = suggestMixScene({
        tracks: baseTracks(),
        sampleRate: SR,
        options: { [domain]: false },
      });
      expect(off.explanation.length, domain).toBeLessThan(all.explanation.length);
      for (const line of off.explanation) {
        expect(all.explanation, domain).toContain(line);
      }
    }
  });

  it('proposes a high-pass only once the switch is on', () => {
    const off = suggestMixScene({ tracks: rumblingVoiceTracks(), sampleRate: SR });
    const on = suggestMixScene({
      tracks: rumblingVoiceTracks(),
      sampleRate: SR,
      options: { enableHighPass: true },
    });

    // Off by default: the measurement is not taken at all, so no filter and no
    // line about one.
    expect(highPassOwners(off.scene)).toEqual([]);
    expect(off.explanation.filter((line) => HIGH_PASS_REASON.test(line))).toEqual([]);

    // On: the vocal track's 80 Hz corner earns a pre-fader filter of its own.
    expect(highPassOwners(on.scene)).toEqual(['vox']);
    expect(on.explanation.filter((line) => HIGH_PASS_REASON.test(line))).toHaveLength(1);
  });

  it('an omitted option equals an explicit undefined', () => {
    const omitted = suggestMixScene({ tracks: baseTracks(), sampleRate: SR });
    const explicit = suggestMixScene({
      tracks: baseTracks(),
      sampleRate: SR,
      options: { targetTrackLufs: undefined, nFft: undefined, enableEq: undefined },
    });

    expect(explicit).toEqual(omitted);
  });
});

describe('suggestMixSceneJson', () => {
  it('returns a scene the Mixer can instantiate', () => {
    const json = suggestMixSceneJson({ tracks: baseTracks(), sampleRate: SR });
    const mixer = Mixer.fromSceneJson(json, SR, 256);
    try {
      expect(mixer.stripCount()).toBe(3);
      expect(mixer.stripById('bass')).not.toBeNull();
    } finally {
      mixer.destroy();
    }
  });

  it('matches the scene embedded in the full result', () => {
    const full = suggestMixScene({ tracks: baseTracks(), sampleRate: SR });
    const sceneOnly = JSON.parse(suggestMixSceneJson({ tracks: baseTracks(), sampleRate: SR }));

    expect(sceneOnly).toEqual(full.scene);
  });
});

describe('mixing assistant source classes', () => {
  it('resolves every reported name to a non-negative ordinal', () => {
    const names = mixSourceClassNames();

    // Asserted as a structural property of the table rather than against a
    // fixed list of class names, so growing the taxonomy does not fail here.
    expect(names.length).toBeGreaterThan(0);
    expect(names.every((name) => typeof name === 'string' && name !== '')).toBe(true);
    for (const name of names) {
      expect(mixSourceClassFromName(name), name).toBeGreaterThanOrEqual(0);
    }
    // The ordinals are the table's own indices, so they must be a permutation
    // of [0, names.length).
    expect(names.map(mixSourceClassFromName).sort((a, b) => a - b)).toEqual(
      names.map((_, index) => index),
    );
  });

  it('returns -1 for an unknown name', () => {
    expect(mixSourceClassFromName('definitelyNotASourceClass')).toBe(-1);
    expect(mixSourceClassFromName('')).toBe(-1);
  });
});

describe('mixing assistant input validation', () => {
  it('rejects a non-positive sample rate', () => {
    expect(() => suggestMixScene({ tracks: baseTracks(), sampleRate: 0 })).toThrow(RangeError);
    expect(() => suggestMixScene({ tracks: baseTracks(), sampleRate: -1 })).toThrow(RangeError);
    expect(() => suggestMixSceneJson({ tracks: baseTracks(), sampleRate: 0 })).toThrow(RangeError);
  });

  it('rejects duplicate track ids', () => {
    const left = sine(440, 0.4, { sampleRate: SR });
    expect(() =>
      suggestMixScene({
        tracks: [
          { id: 'same', left },
          { id: 'same', left },
        ],
        sampleRate: SR,
      }),
    ).toThrow(RangeError);
  });

  it('reports a non-finite sample as its own exclusion, not as silence', () => {
    // The guard is in the core, so every surface answers the same way. One NaN
    // reaches the integrated loudness as -inf, and without it the track comes
    // back excluded for being silent -- a statement about the material rather
    // than about the buffer.
    const left = sine(440, 0.4, { sampleRate: SR });
    left[100] = Number.NaN;
    const result = suggestMixScene({ tracks: [{ id: 'poisoned', left }], sampleRate: SR });
    expect(result.tracks[0].usable).toBe(false);
    expect(result.tracks[0].exclusionReason).toBe('track has non-finite samples');
  });

  it('rejects a missing or empty track id', () => {
    const left = sine(440, 0.4, { sampleRate: SR });
    expect(() =>
      suggestMixScene({ tracks: [{ left } as MixAssistantTrack], sampleRate: SR }),
    ).toThrow(TypeError);
    expect(() => suggestMixScene({ tracks: [{ id: '', left }], sampleRate: SR })).toThrow(
      TypeError,
    );
  });

  it('rejects a missing left channel and a mismatched right channel', () => {
    expect(() =>
      suggestMixScene({ tracks: [{ id: 'a' } as unknown as MixAssistantTrack], sampleRate: SR }),
    ).toThrow(TypeError);
    expect(() =>
      suggestMixScene({
        tracks: [
          {
            id: 'a',
            left: sine(440, 0.4, { sampleRate: SR }),
            right: sine(440, 0.2, { sampleRate: SR }),
          },
        ],
        sampleRate: SR,
      }),
    ).toThrow(RangeError);
  });
});
