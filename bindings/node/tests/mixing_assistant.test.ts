import { describe, expect, it } from 'vitest';
import {
  type MixAssistantTrack,
  Mixer,
  type MixSceneDocument,
  type MixSourceClass,
  mixingAssistantSourceClassFromName,
  mixingAssistantSourceClassNames,
  mixingAssistantSuggest,
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
    { id: 'kick', name: 'Kick', channels: [clicks(1.0, 0.5)] },
    { id: 'bass', name: 'Bass', channels: [sine(80, 1.0, { sampleRate: SR, amp: 0.4 })] },
    { id: 'lead', name: 'Lead', channels: [sine(880, 1.0, { sampleRate: SR, amp: 0.2 })] },
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

describe('mixing assistant native binding', () => {
  it('suggests a scene from mono tracks', () => {
    const result = mixingAssistantSuggest({ tracks: baseTracks(), sampleRate: SR });

    // The named lead is sent to the effect buses, whose returns are strips too.
    expect(result.scene.strips.map((strip) => strip.id)).toEqual([
      'kick',
      'bass',
      'lead',
      'reverbReturn',
      'delayReturn',
    ]);
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
    const result = mixingAssistantSuggest({
      tracks: [
        { id: 'mono', channels: [sine(220, 1.0, { sampleRate: SR })] },
        { id: 'stereo', channels: [stereoLeft, stereoRight] },
      ],
      sampleRate: SR,
    });

    expect(result.tracks.map((track) => track.numChannels)).toEqual([1, 2]);
  });

  it('accepts tracks of different lengths', () => {
    const result = mixingAssistantSuggest({
      tracks: [
        { id: 'short', channels: [sine(220, 0.4, { sampleRate: SR })] },
        { id: 'long', channels: [sine(660, 1.2, { sampleRate: SR })] },
      ],
      sampleRate: SR,
    });

    const durations = result.tracks.map((track) => track.durationSec);
    expect(durations[0]).toBeLessThan(durations[1]);
    expect(result.scene.strips).toHaveLength(2);
  });

  it('returns an empty suggestion for zero tracks', () => {
    const result = mixingAssistantSuggest({ tracks: [], sampleRate: SR });

    expect(result.tracks).toEqual([]);
    expect(result.scene.strips).toEqual([]);
    expect(result.explanation).toEqual([]);
    expect(result.mix.trackCount).toBe(0);
  });

  it('marks a silent track unusable without failing the request', () => {
    const result = mixingAssistantSuggest({
      tracks: [
        { id: 'silent', channels: [new Float32Array(SR)] },
        { id: 'tone', channels: [sine(440, 1.0, { sampleRate: SR })] },
      ],
      sampleRate: SR,
    });

    const silent = result.tracks.find((track) => track.stripId === 'silent');
    expect(silent?.usable).toBe(false);
    expect(silent?.exclusionReason).not.toBe('');
    expect(result.tracks.find((track) => track.stripId === 'tone')?.usable).toBe(true);
  });

  it('states a silent track at the infinite ends of its levels and omits its crest factor', () => {
    const result = mixingAssistantSuggest({
      tracks: [
        { id: 'mono', channels: [new Float32Array(SR)] },
        { id: 'stereo', channels: [new Float32Array(SR), new Float32Array(SR)] },
      ],
      sampleRate: SR,
    });

    for (const track of result.tracks) {
      expect(track.usable, track.stripId).toBe(false);
      expect(track.integratedLufs, track.stripId).toBe(Number.NEGATIVE_INFINITY);
      expect(track.truePeakDbtp, track.stripId).toBe(Number.NEGATIVE_INFINITY);
      // A peak over a zero RMS is not measured, so the key is absent rather than null.
      expect(track, track.stripId).not.toHaveProperty('crestFactorDb');
      const nullFields = Object.entries(track)
        .filter(([, value]) => value === null)
        .map(([key]) => key);
      expect(nullFields, track.stripId).toEqual([]);
    }
  });
});

describe('mixing assistant options', () => {
  it('produces no explanation when every decision domain is off', () => {
    const result = mixingAssistantSuggest({
      tracks: baseTracks(),
      sampleRate: SR,
      enableStructure: false,
      enableGain: false,
      enableBalance: false,
      enableEq: false,
      enableDynamics: false,
      enableImage: false,
    });

    expect(result.explanation).toEqual([]);
  });

  it('a zero suggestion strength leaves the staged levels alone', () => {
    const full = mixingAssistantSuggest({ tracks: baseTracks(), sampleRate: SR });
    const none = mixingAssistantSuggest({
      tracks: baseTracks(),
      sampleRate: SR,
      suggestionStrength: 0,
    });

    expect(none.scene.strips.every((strip) => strip.inputTrimDb === 0)).toBe(true);
    expect(full.scene.strips.some((strip) => strip.inputTrimDb !== 0)).toBe(true);
  });

  it('honours the target track loudness', () => {
    // Gain staging writes the level decision as strip input trim, one static
    // offset from each measured level towards the absolute target, so a higher
    // target must raise every trim.
    const quiet = mixingAssistantSuggest({
      tracks: baseTracks(),
      sampleRate: SR,
      targetTrackLufs: -30,
    });
    const loud = mixingAssistantSuggest({
      tracks: baseTracks(),
      sampleRate: SR,
      targetTrackLufs: -12,
    });

    // Effect returns carry no measured level, so only track strips are staged.
    const trackIds = new Set(baseTracks().map((track) => track.id));
    const trims = (strips: { id: string; inputTrimDb: number }[]) =>
      strips.filter((strip) => trackIds.has(strip.id)).map((strip) => strip.inputTrimDb);
    expect(trims(loud.scene.strips)).toHaveLength(trims(quiet.scene.strips).length);
    trims(loud.scene.strips).forEach((db, index) => {
      expect(db).toBeGreaterThan(trims(quiet.scene.strips)[index]);
    });
  });

  it('switching one domain off drops only that domain of suggestions', () => {
    const all = mixingAssistantSuggest({ tracks: baseTracks(), sampleRate: SR });
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
      const off = mixingAssistantSuggest({
        tracks: baseTracks(),
        sampleRate: SR,
        [domain]: false,
      });
      expect(off.explanation.length, domain).toBeLessThan(all.explanation.length);
      // With structure off there is no master bus, so the headroom trim it would
      // carry is reported instead: the one line a switched-off domain adds.
      // The dynamics levels are set against the staged trim, so with gain off the
      // same decisions come back with different dB figures; compare their wording.
      const shape = (line: string) =>
        domain === 'enableGain' ? line.replace(/-?\d+(\.\d+)?/g, '#') : line;
      const allShapes = all.explanation.map(shape);
      const added = off.explanation.filter((line) => !allShapes.includes(shape(line)));
      if (domain === 'enableStructure') {
        expect(added, domain).toHaveLength(1);
        expect(added[0], domain).toMatch(/no master bus was suggested to carry that trim/);
      } else {
        expect(added, domain).toEqual([]);
      }
    }
  });

  it('proposes a high-pass only once the switch is on', () => {
    const off = mixingAssistantSuggest({ tracks: rumblingVoiceTracks(), sampleRate: SR });
    const on = mixingAssistantSuggest({
      tracks: rumblingVoiceTracks(),
      sampleRate: SR,
      enableHighPass: true,
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
    const omitted = mixingAssistantSuggest({ tracks: baseTracks(), sampleRate: SR });
    const explicit = mixingAssistantSuggest({
      tracks: baseTracks(),
      sampleRate: SR,
      targetTrackLufs: undefined,
      nFft: undefined,
      enableEq: undefined,
    });

    expect(explicit).toEqual(omitted);
  });
});

describe('mixing assistant option keys reach the core', () => {
  it('refuses a misspelt key rather than dropping it', () => {
    expect(() =>
      mixingAssistantSuggest({
        tracks: baseTracks(),
        sampleRate: SR,
        suggestionStrenght: 0.5,
      } as never),
    ).toThrow(TypeError);
  });

  it('refuses a snake_case alias and the removed options bag by name', () => {
    expect(() =>
      mixingAssistantSuggest({
        tracks: baseTracks(),
        sampleRate: SR,
        suggestion_strength: 0,
      } as never),
    ).toThrow(TypeError);
    expect(() =>
      mixingAssistantSuggest({
        tracks: baseTracks(),
        sampleRate: SR,
        options: { suggestionStrength: 0 },
      } as never),
    ).toThrow(TypeError);
  });

  it('refuses a wrongly typed value', () => {
    expect(() =>
      mixingAssistantSuggest({
        tracks: baseTracks(),
        sampleRate: SR,
        suggestionStrength: 'strong',
      } as never),
    ).toThrow(/suggestionStrength/);
  });
});

describe('mixing assistant STFT sizes', () => {
  const suggest = (options: Record<string, unknown>) =>
    mixingAssistantSuggest({ tracks: baseTracks(), sampleRate: SR, ...options } as never);

  it.each([1023, 0, 3.5, -4, 2 ** 20])('refuses nFft %p with the core reader text', (nFft) => {
    expect(() => suggest({ nFft })).toThrow(
      new RangeError('mixingAssistantSuggest: nFft must be an even integer in [2, 524288]'),
    );
  });

  it.each([0, 2.5, 2 ** 31])('refuses hopLength %p with the core reader text', (hopLength) => {
    expect(() => suggest({ hopLength })).toThrow(
      new RangeError('mixingAssistantSuggest: hopLength must be an integer in [1, 2147483647]'),
    );
  });

  it.each([
    ['nFft', 524288, 'nFft rescaled to sampleRate must fit the largest FFT size'],
    ['hopLength', 2 ** 31 - 1, 'hopLength rescaled to sampleRate must fit the largest hop length'],
  ])('names %s, not params, when its rescale outgrows the reader', (key, value, text) => {
    expect(() =>
      mixingAssistantSuggest({ tracks: baseTracks(), sampleRate: 96000, [key]: value } as never),
    ).toThrow(new RangeError(`mixingAssistantSuggest: ${text}`));
  });

  it.each(['nFft', 'hopLength'])('refuses a %s that is not a number', (key) => {
    for (const value of [true, '1024']) {
      expect(() => suggest({ [key]: value })).toThrow(
        new TypeError(`mixingAssistantSuggest: ${key} must be a number`),
      );
    }
  });
});

describe('mixingAssistantSuggest scene', () => {
  it('is a scene the Mixer can instantiate', () => {
    const json = JSON.stringify(
      mixingAssistantSuggest({ tracks: baseTracks(), sampleRate: SR }).scene,
    );
    const mixer = Mixer.fromJson(json, { sampleRate: SR, maxBlockSize: 256 });
    try {
      // Three tracks plus the reverb and delay returns the named lead feeds.
      expect(mixer.stripCount).toBe(5);
      expect(mixer.stripById('bass')).not.toBeNull();
    } finally {
      mixer.destroy();
    }
  });
});

describe('mixing assistant source classes', () => {
  it('resolves every reported name to a non-negative ordinal', () => {
    const names = mixingAssistantSourceClassNames();

    // Asserted as a structural property of the table rather than against a
    // fixed list of class names, so growing the taxonomy does not fail here.
    expect(names.length).toBeGreaterThan(0);
    expect(names.every((name) => typeof name === 'string' && name.length > 0)).toBe(true);
    for (const name of names) {
      expect(mixingAssistantSourceClassFromName(name), name).toBeGreaterThanOrEqual(0);
    }
    // The ordinals are the table's own indices, so they must be a permutation
    // of [0, names.length).
    expect(
      names.map((name) => mixingAssistantSourceClassFromName(name)).sort((a, b) => a - b),
    ).toEqual(names.map((_, index) => index));
  });

  it('refuses an unknown name with a RangeError listing the classes', () => {
    const domain = `mixingAssistantSourceClassFromName: name must be one of ${mixingAssistantSourceClassNames().join(', ')}`;
    expect(() =>
      mixingAssistantSourceClassFromName('definitelyNotASourceClass' as MixSourceClass),
    ).toThrow(new RangeError(domain));
    expect(() => mixingAssistantSourceClassFromName('' as MixSourceClass)).toThrow(RangeError);
    expect(() => mixingAssistantSourceClassFromName(5 as never)).toThrow(new TypeError(domain));
  });
});

describe('mixing assistant input validation', () => {
  it('refuses a zero, negative or non-finite tempo by name instead of reading it as the fallback', () => {
    for (const tempoBpm of [0, -120, Number.NaN, Number.POSITIVE_INFINITY]) {
      expect(() =>
        mixingAssistantSuggest({ tracks: baseTracks(), sampleRate: SR, tempoBpm }),
      ).toThrow(
        new RangeError(
          "mixingAssistantSuggest: tempoBpm must be finite and greater than 0; omit it to use the transport's fallback tempo",
        ),
      );
    }
  });

  it('rejects a non-positive sample rate', () => {
    expect(() => mixingAssistantSuggest({ tracks: baseTracks(), sampleRate: 0 })).toThrow(
      RangeError,
    );
    expect(() => mixingAssistantSuggest({ tracks: baseTracks(), sampleRate: -1 })).toThrow(
      RangeError,
    );
  });

  it('rejects duplicate track ids', () => {
    const left = sine(440, 0.4, { sampleRate: SR });
    expect(() =>
      mixingAssistantSuggest({
        tracks: [
          { id: 'same', channels: [left] },
          { id: 'same', channels: [left] },
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
    const result = mixingAssistantSuggest({
      tracks: [{ id: 'poisoned', channels: [left] }],
      sampleRate: SR,
    });
    expect(result.tracks[0].usable).toBe(false);
    expect(result.tracks[0].exclusionReason).toBe('track has non-finite samples');
  });

  it('rejects a missing or empty track id', () => {
    const left = sine(440, 0.4, { sampleRate: SR });
    expect(() =>
      mixingAssistantSuggest({
        tracks: [{ channels: [left] } as MixAssistantTrack],
        sampleRate: SR,
      }),
    ).toThrow(TypeError);
    expect(() =>
      mixingAssistantSuggest({ tracks: [{ id: '', channels: [left] }], sampleRate: SR }),
    ).toThrow(TypeError);
  });

  it('takes one or two channels of equal length and refuses any other track shape', () => {
    const plane = sine(440, 0.4, { sampleRate: SR });
    const suggest = (track: unknown) => () =>
      mixingAssistantSuggest({ tracks: [track as MixAssistantTrack], sampleRate: SR });
    expect(suggest({ id: 'a' })).toThrow(TypeError);
    expect(suggest({ id: 'a', channels: plane })).toThrow(TypeError);
    expect(suggest({ id: 'a', channels: [Array.from(plane)] })).toThrow(TypeError);
    expect(suggest({ id: 'a', channels: [] })).toThrow(/tracks\[0\]\.channels must hold 1 or 2/);
    expect(suggest({ id: 'a', channels: [] })).toThrow(RangeError);
    expect(suggest({ id: 'a', channels: [plane, plane, plane] })).toThrow(RangeError);
    expect(suggest({ id: 'a', channels: [plane, sine(440, 0.2, { sampleRate: SR })] })).toThrow(
      RangeError,
    );
  });
});
