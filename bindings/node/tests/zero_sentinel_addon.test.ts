/**
 * A fractional number is refused on a key whose zero means "keep the default".
 *
 * Truncation is harmless where it only changes a magnitude, but on these keys
 * anything in (-1, 1) lands on the sentinel: the call succeeds and returns
 * exactly what a caller who asked for nothing would have got, so the parameter
 * silently stops being read. Each case opens with a positive control — an
 * integral value whose result differs from the omitted one — because without it
 * "the fraction gave the default" cannot be told from a key nothing reads.
 *
 * Driven against the addon rather than the TypeScript facade throughout. Some of
 * these keys have no facade entry point at all; the rest are guarded there too,
 * and for those the point is that a guard the facade owns does nothing for a
 * generated binding or a direct consumer reaching the addon.
 */

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { addon } from '../src/native.js';

/* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
const native = addon as any;

const FRACTIONS = [0.5, 1.5];

const sf2Bytes = new Uint8Array(
  readFileSync(
    join(dirname(fileURLToPath(import.meta.url)), '../../../tests/fixtures/sf2/minimal_gs.sf2'),
  ),
);

const midi1Word = (status: number, channel: number, d1: number, d2: number) =>
  ((0x2 << 28) | (status << 20) | (channel << 16) | (d1 << 8) | d2) >>> 0;

/** Hash of a rendered buffer, so two renders compare as one value. */
function digest(samples: Float32Array): string {
  let hash = 0;
  for (let i = 0; i < samples.length; i++) {
    hash = (hash * 31 + Math.round(samples[i] * 1e6)) % 2147483647;
  }
  return `${samples.length}:${hash}`;
}

/** A project holding `notes` as one MIDI clip routed to destination 0. */
/* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
function midiProject(notes: number[]): any {
  const project = new native.Project();
  project.setSampleRate(48000);
  const { trackId, clipId } = project.addMidiClip(0, 2);
  project.setTrackMidiDestination(trackId, 0);
  project.setMidiEvents(clipId, [
    ...notes.map((note) => ({ ppq: 0, data0: midi1Word(0x9, 0, note, 100), data1: 0 })),
    ...notes.map((note) => ({ ppq: 1, data0: midi1Word(0x8, 0, note, 0), data1: 0 })),
  ]);
  return project;
}

/** A prepared engine holding a three-note chord against destination 5. */
/* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
function chordEngine(): any {
  const engine = new native.RealtimeEngine(48000, 128);
  engine.prepare(48000, 128, 1024, 1024, 8);
  engine.setMidiClips([
    {
      id: 1,
      trackId: 5,
      destinationId: 5,
      lengthSamples: 1 << 20,
      events: [60, 64, 67].map((note) => ({
        renderFrame: 0,
        word0: midi1Word(0x9, 0, note, 100),
        wordCount: 1,
      })),
    },
  ]);
  return engine;
}

/* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
function renderEngine(engine: any): string {
  engine.play();
  const [left] = engine.renderOffline([new Float32Array(8192), new Float32Array(8192)], 128);
  engine.destroy();
  return digest(left);
}

describe('a synth patch field whose zero keeps the base preset', () => {
  const roundTrip = (key: string, value: number | undefined) => {
    const patch: Record<string, unknown> = { preset: 'saw-lead' };
    if (value !== undefined) {
      patch[key] = value;
    }
    return native._synthPatchRoundTrip(patch)[key];
  };

  it.each([
    ['unison', 5],
    ['polyphony', 8],
  ])('refuses a fractional %s', (key, control) => {
    expect(roundTrip(key, undefined)).toBe(0);
    expect(roundTrip(key, 0)).toBe(0);
    expect(roundTrip(key, control)).toBe(control);
    expect(roundTrip(key, control)).not.toBe(roundTrip(key, undefined)); // positive control
    for (const value of FRACTIONS) {
      expect(() => roundTrip(key, value), `${key} ${value}`).toThrow(RangeError);
    }
  });

  it('still reads a keymap set of zero as the set it addresses, not as a sentinel', () => {
    // sampleSet carries no presence bit and its zero is a real keymap set, so it
    // keeps the family's plain truncation rather than joining the refusal above.
    expect(roundTrip('sampleSet', 2)).toBe(2);
    expect(roundTrip('sampleSet', 0.5)).toBe(0);
  });
});

describe('an instrument polyphony whose zero keeps the library default', () => {
  const bounceBuiltin = (polyphony: number | undefined) => {
    const config: Record<string, unknown> = { gain: 0.5 };
    if (polyphony !== undefined) {
      config.polyphony = polyphony;
    }
    const project = midiProject([60, 64, 67]);
    try {
      return digest(project.bounceWithBuiltinInstruments([config], { totalFrames: 24000 }));
    } finally {
      project.destroy();
    }
  };

  const bounceSf2 = (polyphony: number | undefined) => {
    const config: Record<string, unknown> = { destinationId: 0, gain: 0.5 };
    if (polyphony !== undefined) {
      config.polyphony = polyphony;
    }
    const project = midiProject([60, 64, 67]);
    project.loadSoundFont(sf2Bytes);
    try {
      return digest(project.bounceWithSf2Instruments([config], { totalFrames: 24000 }));
    } finally {
      project.destroy();
    }
  };

  it.each([
    ['a built-in instrument bounce', bounceBuiltin],
    ['a SoundFont instrument bounce', bounceSf2],
  ])('refuses a fractional polyphony on %s', (_label, bounce) => {
    const omitted = bounce(undefined);
    expect(bounce(0)).toBe(omitted);
    expect(bounce(1)).not.toBe(omitted); // positive control
    for (const value of FRACTIONS) {
      expect(() => bounce(value), `polyphony ${value}`).toThrow(RangeError);
    }
  });

  it('refuses a fractional polyphony on the realtime built-in instrument', () => {
    const render = (polyphony: number | undefined) => {
      const config: Record<string, unknown> = { gain: 0.5 };
      if (polyphony !== undefined) {
        config.polyphony = polyphony;
      }
      const engine = chordEngine();
      engine.setBuiltinInstrument(5, config);
      return renderEngine(engine);
    };
    const omitted = render(undefined);
    expect(render(0)).toBe(omitted);
    expect(render(1)).not.toBe(omitted); // positive control
    for (const value of FRACTIONS) {
      const engine = chordEngine();
      expect(
        () => engine.setBuiltinInstrument(5, { gain: 0.5, polyphony: value }),
        `polyphony ${value}`,
      ).toThrow(RangeError);
      engine.destroy();
    }
  });

  it('refuses a fractional polyphony on the realtime SoundFont instrument', () => {
    const render = (polyphony: number | undefined) => {
      const config: Record<string, unknown> = { gain: 0.5 };
      if (polyphony !== undefined) {
        config.polyphony = polyphony;
      }
      const engine = chordEngine();
      engine.loadSoundFont(sf2Bytes);
      engine.setSf2Instrument(5, config);
      return renderEngine(engine);
    };
    const omitted = render(undefined);
    expect(render(0)).toBe(omitted);
    expect(render(1)).not.toBe(omitted); // positive control
    for (const value of FRACTIONS) {
      const engine = chordEngine();
      engine.loadSoundFont(sf2Bytes);
      expect(
        () => engine.setSf2Instrument(5, { gain: 0.5, polyphony: value }),
        `polyphony ${value}`,
      ).toThrow(RangeError);
      engine.destroy();
    }
  });
});

describe('a bounce option whose zero derives the value', () => {
  const bounce = (key: string, value: number | undefined) => {
    const options: Record<string, unknown> = {};
    if (key !== 'totalFrames') {
      options.totalFrames = 24000;
    }
    if (value !== undefined) {
      options[key] = value;
    }
    const project = midiProject([60]);
    try {
      return digest(project.bounceWithSynthInstruments([{ preset: 'saw-lead' }], options));
    } finally {
      project.destroy();
    }
  };

  it.each([
    ['numChannels', 1],
    ['totalFrames', 12000],
  ])('refuses a fractional %s', (key, control) => {
    const omitted = bounce(key, undefined);
    expect(bounce(key, 0)).toBe(omitted);
    expect(bounce(key, control)).not.toBe(omitted); // positive control
    for (const value of FRACTIONS) {
      expect(() => bounce(key, value), `${key} ${value}`).toThrow(RangeError);
    }
  });

  it('leaves blockSize truncating, because no render can tell the two apart', () => {
    // Documented as "<= 0 => 128", but the rendered buffer is identical at every
    // block size, so the sentinel collision has nothing to report.
    const omitted = bounce('blockSize', undefined);
    expect(bounce('blockSize', 64)).toBe(omitted);
    expect(bounce('blockSize', 0.5)).toBe(omitted);
  });
});

describe('an analysis band count whose zero keeps the library default', () => {
  const impulseResponse = (): Float32Array => {
    const sampleRate = 48000;
    const out = new Float32Array(sampleRate);
    let seed = 12345;
    for (let i = 0; i < out.length; i++) {
      seed = (seed * 1103515245 + 12345) & 0x7fffffff;
      out[i] = ((seed / 0x7fffffff) * 2 - 1) * Math.exp(-i / (sampleRate * 0.25));
    }
    return out;
  };

  it('refuses a fractional nOctaveBands', () => {
    const ir = impulseResponse();
    const bands = (value: number | undefined) => {
      const options: Record<string, unknown> = { mode: 2 };
      if (value !== undefined) {
        options.nOctaveBands = value;
      }
      return native.estimateRoom(ir, 48000, options).rt60Bands.length;
    };
    expect(bands(0)).toBe(bands(undefined));
    expect(bands(4)).not.toBe(bands(undefined)); // positive control
    for (const value of FRACTIONS) {
      expect(() => bands(value), `nOctaveBands ${value}`).toThrow(RangeError);
    }
  });
});

describe('a clip channel count whose zero means mono', () => {
  const addClip = (channels: number | undefined, rate = 48000) => {
    const project = new native.Project();
    project.setSampleRate(48000);
    const trackId = project.addTrack(0, 'audio');
    const audio = new Float32Array(960);
    for (let i = 0; i < audio.length; i++) {
      audio[i] = Math.sin(i * 0.05) * 0.5;
    }
    const desc: Record<string, unknown> = {
      trackId,
      startPpq: 0,
      lengthPpq: 1,
      audio,
      audioSampleRate: rate,
    };
    if (channels !== undefined) {
      desc.audioChannels = channels;
    }
    try {
      project.addClip(desc);
      const source = project.sourceByIndex(0);
      return { channelCount: source.channelCount, sampleRateHint: source.sampleRateHint };
    } finally {
      project.destroy();
    }
  };

  it('refuses a fractional audioChannels', () => {
    expect(addClip(undefined).channelCount).toBe(1);
    expect(addClip(0).channelCount).toBe(1);
    expect(addClip(2).channelCount).not.toBe(addClip(undefined).channelCount); // positive control
    for (const value of FRACTIONS) {
      expect(() => addClip(value), `audioChannels ${value}`).toThrow(RangeError);
    }
  });

  it('leaves audioSampleRate truncating, because the field is mandatory', () => {
    // Its zero is not a sentinel: once decoded audio is supplied the rate has no
    // default at all, so omitting it is the same error that 0 is, and a fraction
    // outside (-1, 1) truncates and succeeds like any other magnitude.
    expect(addClip(1, 44100).sampleRateHint).toBe(44100);
    expect(addClip(1, 48000.5).sampleRateHint).toBe(48000);
    expect(() => addClip(1, 0)).toThrow(/Invalid parameter/);
    expect(() => addClip(1, 0.5)).toThrow(/Invalid parameter/);
  });
});

/**
 * The keys below are also guarded in the TypeScript facade. Driving the addon
 * directly is the point: a facade guard leaves the reader open to a generated
 * binding or a direct consumer, and only an assertion that bypasses the facade
 * shows the refusal moved down to the boundary the C ABI is actually called from.
 */
describe('a facade-guarded sentinel key, reached past the facade', () => {
  const SAMPLE_RATE = 22050;

  function signal(): Float32Array {
    const out = new Float32Array(SAMPLE_RATE / 2);
    for (let i = 0; i < out.length; i++) {
      out[i] = 0.3 * Math.sin((2 * Math.PI * 220 * i) / SAMPLE_RATE) + (i % 2205 < 50 ? 0.5 : 0);
    }
    return out;
  }

  it('refuses a fractional nFft and octaveFraction on the metering spectrum', () => {
    const samples = signal();
    const spectrum = (options: Record<string, unknown>) =>
      native.meteringSpectrum(samples, SAMPLE_RATE, options);

    expect(spectrum({}).nFft).toBe(2048);
    expect(spectrum({ nFft: 0 }).nFft).toBe(2048);
    expect(spectrum({ nFft: 512 }).nFft).not.toBe(spectrum({}).nFft); // positive control

    const smoothed = (fraction?: number) =>
      digest(
        spectrum(
          fraction === undefined
            ? { applyOctaveSmoothing: true }
            : { applyOctaveSmoothing: true, octaveFraction: fraction },
        ).magnitude,
      );
    expect(smoothed(0)).toBe(smoothed(undefined));
    expect(smoothed(6)).not.toBe(smoothed(undefined)); // positive control

    for (const value of FRACTIONS) {
      expect(() => spectrum({ nFft: value }), `nFft ${value}`).toThrow(RangeError);
      expect(
        () => spectrum({ applyOctaveSmoothing: true, octaveFraction: value }),
        `octaveFraction ${value}`,
      ).toThrow(RangeError);
    }
    for (const value of FRACTIONS) {
      expect(
        () => native.meteringSpectrumFrame(samples, SAMPLE_RATE, 0, { nFft: value }),
        `frame nFft ${value}`,
      ).toThrow(RangeError);
    }
  });

  it('refuses a fractional nComponents on the stem decomposition', () => {
    const samples = new Float32Array(4096).map(
      (_, i) => 0.3 * Math.sin(i * 0.1) + 0.1 * Math.sin(i * 0.37),
    );
    const width = (nComponents?: number) =>
      native.decomposeStems(samples, SAMPLE_RATE, nComponents === undefined ? {} : { nComponents })
        .w.length;

    expect(width(0)).toBe(width(undefined));
    expect(width(2)).not.toBe(width(undefined)); // positive control
    for (const value of FRACTIONS) {
      expect(
        () => native.decomposeStems(samples, SAMPLE_RATE, { nComponents: value }),
        `nComponents ${value}`,
      ).toThrow(RangeError);
    }
  });

  it('refuses a fractional healRadiusFrames on a spectral edit', () => {
    const samples = signal();
    const edits = [{ startSample: 1000, endSample: 5000, minHz: 100, maxHz: 1000, mode: 'heal' }];
    const healed = (healRadiusFrames?: number) =>
      digest(
        native.spectralEdit(
          samples,
          SAMPLE_RATE,
          edits,
          healRadiusFrames === undefined ? {} : { healRadiusFrames },
        ),
      );

    expect(healed(0)).toBe(healed(undefined));
    expect(healed(8)).not.toBe(healed(undefined)); // positive control
    for (const value of FRACTIONS) {
      expect(
        () => native.spectralEdit(samples, SAMPLE_RATE, edits, { healRadiusFrames: value }),
        `healRadiusFrames ${value}`,
      ).toThrow(RangeError);
    }
  });

  it('refuses a fractional onsetWait on the percussive extraction', () => {
    const samples = signal();
    const events = (onsetWait?: number) =>
      native.extractPercussiveEvents(
        samples,
        SAMPLE_RATE,
        onsetWait === undefined ? { onsetDelta: -20 } : { onsetDelta: -20, onsetWait },
      ).length;

    expect(events(0)).toBe(events(undefined));
    expect(events(64)).not.toBe(events(undefined)); // positive control
    for (const value of FRACTIONS) {
      expect(
        () => native.extractPercussiveEvents(samples, SAMPLE_RATE, { onsetWait: value }),
        `onsetWait ${value}`,
      ).toThrow(RangeError);
    }
  });

  it('refuses a fractional seed on a synthesized impulse response', () => {
    const rir = (seed?: number) => {
      const options: Record<string, unknown> = {
        sampleRate: 48000,
        roomWidth: 5,
        roomDepth: 4,
        roomHeight: 3,
        ismOrder: 1,
        rt60: 0.4,
      };
      if (seed !== undefined) {
        options.seed = seed;
      }
      return digest(native.synthesizeRir(options).rir);
    };

    expect(rir(0)).toBe(rir(undefined));
    expect(rir(7)).not.toBe(rir(undefined)); // positive control
    for (const value of FRACTIONS) {
      expect(() => rir(value), `seed ${value}`).toThrow(RangeError);
    }
  });

  it('refuses a fractional analysis count, and reports it instead of aborting', () => {
    // These reach the reader from an ObjectWrap constructor. A constructor has
    // the same catch harness as a method, so the refusal arrives as a catchable
    // RangeError rather than as an uncaught throw out of the N-API callback.
    const samples = new Float32Array(SAMPLE_RATE / 2).map(
      (_, i) =>
        0.3 * Math.sin((2 * Math.PI * 220 * i) / SAMPLE_RATE) +
        0.3 * Math.sin((2 * Math.PI * 277 * i) / SAMPLE_RATE) +
        (i % 2205 < 50 ? 0.5 : 0),
    );
    const notes = (config: Record<string, unknown>) => {
      const analysis = new native.PolyphonicAnalysis(samples, SAMPLE_RATE, config);
      try {
        return analysis.noteCount();
      } finally {
        analysis.destroy();
      }
    };

    expect(notes({ nFft: 0 })).toBe(notes({}));
    expect(notes({ nFft: 1024 })).not.toBe(notes({})); // positive control
    expect(notes({ maxPolyphony: 0 })).toBe(notes({}));
    expect(notes({ maxPolyphony: 1 })).not.toBe(notes({})); // positive control

    for (const key of [
      'nFft',
      'hopLength',
      'winLength',
      'salienceHarmonics',
      'maxPolyphony',
      'maskHarmonics',
      'inharmonicityMinPartials',
      'windowFrames',
    ]) {
      for (const value of FRACTIONS) {
        expect(() => notes({ [key]: value }), `${key} ${value}`).toThrow(RangeError);
      }
    }
    // The value that aborted the process before the constructor grew a harness.
    expect(() => notes({ nFft: 2 ** 32 })).toThrow(RangeError);
  });

  it('reports a refused constructor argument instead of terminating', () => {
    // A constructor reaches the same throwing readers a method does. Each value
    // below took the process down before the constructors grew a catch harness,
    // so a regression does not fail this assertion — it kills the test worker.
    expect(() => new native.Mixer('{"tracks":[]}', 2 ** 32, 512)).toThrow(RangeError);
    expect(() => new native.Mixer('{"tracks":[]}', 48000, 2 ** 32)).toThrow(RangeError);
    expect(() => new native.RealtimeEngine(48000, 2 ** 32)).toThrow(RangeError);
    expect(() => new native.RealtimeEngine(48000, 128, 2 ** 63)).toThrow(RangeError);
    expect(() => new native.StreamingRetune({ grainSize: 2 ** 32 })).toThrow(RangeError);
    expect(() => new native.StreamingRetune({ semitones: 1e40 })).toThrow(RangeError);
  });

  it('refuses a fractional take sourceId', () => {
    const takeSourceId = (sourceId?: number) => {
      const project = new native.Project();
      project.setSampleRate(48000);
      const trackId = project.addTrack(0, 'audio');
      const clipId = project.addClip({
        trackId,
        startPpq: 0,
        lengthPpq: 4,
        audio: new Float32Array(960),
        audioChannels: 1,
        audioSampleRate: 48000,
      });
      const take: Record<string, unknown> = { id: 1, sourceOffsetPpq: 0 };
      if (sourceId !== undefined) {
        take.sourceId = sourceId;
      }
      try {
        project.setClipTakes(clipId, [take], 1);
        return JSON.parse(project.toJson()).clips[0].takes[0].source_id;
      } finally {
        project.destroy();
      }
    };

    expect(takeSourceId(0)).toBe(takeSourceId(undefined));
    expect(takeSourceId(1)).not.toBe(takeSourceId(undefined)); // positive control
    for (const value of FRACTIONS) {
      expect(() => takeSourceId(value), `sourceId ${value}`).toThrow(RangeError);
    }
  });
});
