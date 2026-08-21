import { describe, expect, it } from 'vitest';
import {
  type MixAssistantTrack,
  Mixer,
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

  it('accepts the high-pass switch, which is off unless asked for', () => {
    const off = suggestMixScene({ tracks: baseTracks(), sampleRate: SR });
    const on = suggestMixScene({
      tracks: baseTracks(),
      sampleRate: SR,
      options: { enableHighPass: true },
    });

    expect(on.scene.strips.map((strip) => strip.id)).toEqual(
      off.scene.strips.map((strip) => strip.id),
    );
    // The switch only ever adds a filter. A track that gains one can lose the
    // peaking cuts it made redundant, but never more lines than it gained, so
    // the reasoning cannot get shorter.
    expect(on.explanation.length).toBeGreaterThanOrEqual(off.explanation.length);
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
