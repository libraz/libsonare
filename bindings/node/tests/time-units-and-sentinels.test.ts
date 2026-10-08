import { describe, expect, it } from 'vitest';
import {
  createVocalEditSession,
  fixFrames,
  masteringRepairTrimSilence,
  meteringDynamicRange,
  normalize,
  noteMove,
  noteStretch,
  PolyphonicAnalysis,
  Project,
  RealtimeEngine,
  renderNotes,
  renderPercussiveEvents,
  spectralEdit,
  vqt,
} from '../src/index.js';

const SR = 22050;

function tone(length: number, freq = 440): Float32Array {
  const out = new Float32Array(length);
  for (let i = 0; i < length; i += 1) {
    out[i] = 0.4 * Math.sin((2 * Math.PI * freq * i) / SR);
  }
  return out;
}

/** Deterministic noise, so a moved or gained span is distinguishable from its surroundings. */
function noise(length: number, seed = 1): Float32Array {
  const out = new Float32Array(length);
  let state = seed >>> 0;
  for (let i = 0; i < length; i += 1) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    out[i] = 0.5 * ((state >>> 9) / 4194304 - 1);
  }
  return out;
}

describe('spectralEdit time bounds', () => {
  const samples = tone(SR / 2);
  const op = { lowHz: 300, highHz: 600, gainDb: -30, mode: 'gain' as const };

  it('reads an omitted end as the end of the signal', () => {
    const omitted = spectralEdit({ samples, sampleRate: SR, ops: [op] });
    const explicit = spectralEdit({
      samples,
      sampleRate: SR,
      ops: [{ ...op, endSample: samples.length }],
    });
    expect(omitted).toEqual(explicit);
    expect(omitted).not.toEqual(samples);
  });

  it('refuses a negative start or end instead of clamping it to an empty region', () => {
    expect(() =>
      spectralEdit({ samples, sampleRate: SR, ops: [{ ...op, endSample: -1 }] }),
    ).toThrow(RangeError);
    expect(() =>
      spectralEdit({ samples, sampleRate: SR, ops: [{ ...op, startSample: -1 }] }),
    ).toThrow(RangeError);
    expect(() => spectralEdit({ samples, sampleRate: SR, ops: [{ ...op, endSec: -0.1 }] })).toThrow(
      RangeError,
    );
  });

  it('accepts seconds and matches the same bounds in samples', () => {
    const bySamples = spectralEdit({
      samples,
      sampleRate: SR,
      ops: [{ ...op, startSample: 2205, endSample: 8820 }],
    });
    const bySeconds = spectralEdit({
      samples,
      sampleRate: SR,
      ops: [{ ...op, startSec: 0.1, endSec: 0.4 }],
    });
    expect(bySeconds).toEqual(bySamples);
  });

  it('rounds seconds to the nearest sample', () => {
    const rounded = spectralEdit({
      samples,
      sampleRate: SR,
      ops: [{ ...op, startSample: 2205, endSample: 8820 }],
    });
    const fromSeconds = spectralEdit({
      samples,
      sampleRate: SR,
      ops: [{ ...op, startSec: 0.1 + 0.2 / SR, endSec: 0.4 - 0.2 / SR }],
    });
    expect(fromSeconds).toEqual(rounded);
  });

  it('refuses both spellings of one bound, naming both', () => {
    expect(() =>
      spectralEdit({
        samples,
        sampleRate: SR,
        ops: [{ ...op, startSample: 0, startSec: 0 }],
      }),
    ).toThrow(/startSample.*startSec/);
    expect(() =>
      spectralEdit({ samples, sampleRate: SR, ops: [{ ...op, endSample: 10, endSec: 1 }] }),
    ).toThrow(/endSample.*endSec/);
  });

  it('keeps the positional form equivalent to the request form', () => {
    const ops = [{ ...op, startSec: 0.1, endSec: 0.4 }];
    expect(spectralEdit(samples, SR, ops)).toEqual(spectralEdit({ samples, sampleRate: SR, ops }));
  });
});

describe('note stretch and move time bounds', () => {
  const samples = tone(SR / 2);

  it('noteStretch seconds equal samples', () => {
    const bySamples = noteStretch({
      samples,
      sampleRate: SR,
      onsetSample: 2205,
      offsetSample: 6615,
      stretchRatio: 1.5,
    });
    const bySeconds = noteStretch({
      samples,
      sampleRate: SR,
      onsetSec: 0.1,
      offsetSec: 0.3,
      stretchRatio: 1.5,
    });
    expect(bySeconds).toEqual(bySamples);
    expect(noteStretch(samples, SR, { onsetSec: 0.1, offsetSec: 0.3, stretchRatio: 1.5 })).toEqual(
      bySamples,
    );
  });

  it('noteMove seconds equal samples, including the target onset', () => {
    const bySamples = noteMove({
      samples,
      sampleRate: SR,
      onsetSample: 2205,
      offsetSample: 4410,
      targetOnsetSample: 6615,
    });
    const bySeconds = noteMove({
      samples,
      sampleRate: SR,
      onsetSec: 0.1,
      offsetSec: 0.2,
      targetOnsetSec: 0.3,
    });
    expect(bySeconds).toEqual(bySamples);
  });

  it('refuses both spellings and negative bounds', () => {
    expect(() =>
      noteStretch({ samples, sampleRate: SR, onsetSample: 0, onsetSec: 0, stretchRatio: 1 }),
    ).toThrow(/onsetSample.*onsetSec/);
    expect(() => noteStretch({ samples, sampleRate: SR, offsetSample: -1 })).toThrow(RangeError);
    expect(() =>
      noteMove({ samples, sampleRate: SR, targetOnsetSample: 5, targetOnsetSec: 1 }),
    ).toThrow(/targetOnsetSample.*targetOnsetSec/);
    expect(() => noteMove({ samples, sampleRate: SR, targetOnsetSec: -0.5 })).toThrow(RangeError);
  });
});

describe('note objects and percussive events in seconds', () => {
  const samples = noise(SR);

  it('renderNotes spans and edit offsets in seconds equal samples', () => {
    const bySamples = renderNotes({
      samples,
      sampleRate: SR,
      notes: [
        { onsetSample: 2205, offsetSample: 4410, edit: { timeOffsetSamples: 4410, gainDb: -6 } },
      ],
    });
    const bySeconds = renderNotes({
      samples,
      sampleRate: SR,
      notes: [{ onsetSec: 0.1, offsetSec: 0.2, edit: { timeOffsetSec: 0.2, gainDb: -6 } }],
    });
    expect(bySeconds).toEqual(bySamples);
    expect(bySamples).not.toEqual(samples);
  });

  it('renderNotes refuses both spellings and a negative span', () => {
    expect(() =>
      renderNotes({
        samples,
        sampleRate: SR,
        notes: [{ onsetSample: 0, onsetSec: 0, offsetSample: 10 }],
      }),
    ).toThrow(/onsetSample.*onsetSec/);
    expect(() =>
      renderNotes({
        samples,
        sampleRate: SR,
        notes: [
          { onsetSample: 0, offsetSample: 10, edit: { timeOffsetSamples: 1, timeOffsetSec: 1 } },
        ],
      }),
    ).toThrow(/timeOffsetSamples.*timeOffsetSec/);
    expect(() =>
      renderNotes({ samples, sampleRate: SR, notes: [{ onsetSample: -1, offsetSample: 10 }] }),
    ).toThrow(RangeError);
  });

  it('renderPercussiveEvents spans and offsets in seconds equal samples', () => {
    const bySamples = renderPercussiveEvents({
      samples,
      sampleRate: SR,
      events: [
        { onsetSample: 2205, offsetSample: 4410, edit: { timeOffsetSamples: -1103, gainDb: -6 } },
      ],
    });
    const bySeconds = renderPercussiveEvents({
      samples,
      sampleRate: SR,
      events: [{ onsetSec: 0.1, offsetSec: 0.2, edit: { timeOffsetSec: -1103 / SR, gainDb: -6 } }],
    });
    expect(bySeconds).toEqual(bySamples);
    expect(bySamples).not.toEqual(samples);
  });

  it('renderPercussiveEvents refuses both spellings', () => {
    expect(() =>
      renderPercussiveEvents({
        samples,
        sampleRate: SR,
        events: [{ onsetSample: 0, offsetSample: 100, offsetSec: 1 }],
      }),
    ).toThrow(/offsetSample.*offsetSec/);
  });
});

describe('polyphonic note edit offset in seconds', () => {
  it('setNoteEdit takes timeOffsetSec and refuses both spellings', () => {
    using analysis = new PolyphonicAnalysis({ samples: tone(SR), sampleRate: SR });
    if (analysis.noteCount === 0) {
      return;
    }
    expect(() => analysis.setNoteEdit(0, { timeOffsetSec: 0.01 })).not.toThrow();
    expect(() => analysis.setNoteEdit(0, { timeOffsetSamples: 1, timeOffsetSec: 0.01 })).toThrow(
      /timeOffsetSamples.*timeOffsetSec/,
    );
  });
});

describe('trim padding in seconds', () => {
  const samples = new Float32Array(SR);
  samples.set(tone(SR / 4), SR / 4);

  it('paddingSec equals paddingSamples', () => {
    const bySamples = masteringRepairTrimSilence({
      samples,
      sampleRate: SR,
      paddingSamples: 2205,
    });
    const bySeconds = masteringRepairTrimSilence({ samples, sampleRate: SR, paddingSec: 0.1 });
    expect(bySeconds).toEqual(bySamples);
  });

  it('refuses both spellings and a negative padding', () => {
    expect(() =>
      masteringRepairTrimSilence({ samples, sampleRate: SR, paddingSamples: 1, paddingSec: 0.1 }),
    ).toThrow(/paddingSamples.*paddingSec/);
    expect(() => masteringRepairTrimSilence({ samples, sampleRate: SR, paddingSec: -0.1 })).toThrow(
      RangeError,
    );
  });
});

describe('engine punch-in window', () => {
  it('seconds equal samples at the engine rate, and bounds are required and non-negative', () => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      engine.setCapturePunch({ startSec: 1, endSample: 48128 });
      expect(engine.captureStatus().punchEnabled).toBe(true);
      engine.setCapturePunch({ startSample: 48000, endSample: 48128, enabled: false });
      expect(engine.captureStatus().punchEnabled).toBe(false);
      engine.setCapturePunch(48000, 48128);
      expect(engine.captureStatus().punchEnabled).toBe(true);
      expect(() => engine.setCapturePunch({ startSample: 1, startSec: 1, endSample: 2 })).toThrow(
        /startSample.*startSec/,
      );
      expect(() => engine.setCapturePunch({ startSample: -1, endSample: 2 })).toThrow(RangeError);
      expect(() => engine.setCapturePunch({ startSample: 1 })).toThrow(RangeError);
    } finally {
      engine.destroy();
    }
  });
});

describe('absent is omission and -1 is refused', () => {
  it('engine renderFrame: omitted is immediate, negative is refused', () => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      expect(() => engine.play()).not.toThrow();
      expect(() => engine.play(0)).not.toThrow();
      expect(() => engine.play(-1)).toThrow(RangeError);
      expect(() => engine.seekSample(0, -1)).toThrow(RangeError);
      expect(() => engine.setParameter(1, 0.5, -1)).toThrow(RangeError);
      expect(() => engine.pushMidiPanic(-1)).toThrow(RangeError);
    } finally {
      engine.destroy();
    }
  });

  it('fixFrames: omitted xMax is unbounded, negative is refused', () => {
    const frames = new Int32Array([0, 5, 20]);
    expect(Array.from(fixFrames({ frames }))).toEqual([0, 5, 20]);
    expect(Array.from(fixFrames({ frames, xMax: 10 }))).toEqual([0, 5, 10]);
    expect(() => fixFrames({ frames, xMax: -1 })).toThrow(RangeError);
  });

  it('vqt gamma: omitted is automatic, 0 is constant-Q, negative or NaN is refused', () => {
    const samples = tone(SR / 2);
    const automatic = vqt({ samples, sampleRate: SR, nBins: 24 });
    expect(vqt({ samples, sampleRate: SR, nBins: 24, gamma: 0 })).not.toEqual(automatic);
    expect(() => vqt({ samples, sampleRate: SR, nBins: 24, gamma: -1 })).toThrow(RangeError);
    expect(() => vqt({ samples, sampleRate: SR, nBins: 24, gamma: Number.NaN })).toThrow(
      RangeError,
    );
  });

  it('metering percentiles: omitted is the default, 0 is real, negative is refused', () => {
    const samples = noise(SR * 4);
    const defaults = meteringDynamicRange({ samples, sampleRate: SR });
    expect(
      meteringDynamicRange({ samples, sampleRate: SR, lowPercentile: 0.1, highPercentile: 0.95 }),
    ).toEqual(defaults);
    expect(() => meteringDynamicRange({ samples, sampleRate: SR, lowPercentile: -1 })).toThrow(
      RangeError,
    );
    expect(() => meteringDynamicRange({ samples, sampleRate: SR, highPercentile: -1 })).toThrow(
      RangeError,
    );
  });

  it('project setProgram bank: omitted is no Bank Select, negative is refused', () => {
    const project = Project.create();
    try {
      project.setSampleRate(48000);
      const { clipId } = project.addMidiClip(0, 4);
      expect(() => project.setProgram(clipId, 24)).not.toThrow();
      expect(() => project.setProgram(clipId, 24, 0)).not.toThrow();
      expect(() => project.setProgram(clipId, 24, -1)).toThrow(RangeError);
      expect(() => project.setProgramOnChannel(clipId, 0, 0, 24, -1)).toThrow(RangeError);
    } finally {
      project.destroy();
    }
  });
});

describe('mono normalize RMS default', () => {
  it('defaults to -20 dB in rms mode and 0 dB in peak mode, in both forms', () => {
    const samples = noise(SR, 7);
    const explicit = normalize({ samples, sampleRate: SR, targetDb: -20, mode: 'rms' });
    expect(normalize({ samples, sampleRate: SR, mode: 'rms' })).toEqual(explicit);
    expect(normalize(samples, SR, undefined, 'rms')).toEqual(explicit);
    expect(normalize({ samples, sampleRate: SR })).toEqual(
      normalize({ samples, sampleRate: SR, targetDb: 0, mode: 'peak' }),
    );
  });
});

describe('vocal render range', () => {
  const rate = 16000;
  const count = 4096;
  const hop = 128;
  function session() {
    const samples = new Float32Array(count);
    for (let i = 0; i < count; i += 1) {
      samples[i] = 0.25 * Math.sin((2 * Math.PI * 440 * i) / rate);
    }
    const frames = Math.ceil(count / hop) + 1;
    return createVocalEditSession({
      samples,
      sampleRate: rate,
      frameLengthSamples: 256,
      hopLengthSamples: hop,
      analysis: {
        frameOriginSample: 0,
        samplesPerFrame: hop,
        frameLengthSamples: 256,
        f0Hz: new Float32Array(frames).fill(440),
        voiced: new Uint8Array(frames).fill(1),
        algorithmId: 'libsonare.pyin',
        algorithmVersion: 1,
      },
    });
  }

  it('seconds equal samples; omitted bounds are the whole output; negatives are refused', () => {
    const edit = session();
    const snapshot = edit.captureRenderSnapshot();
    try {
      const bySamples = snapshot.render({ range: { startSample: 1600, endSample: 3200 } });
      const bySeconds = snapshot.render({ range: { startSec: 0.1, endSec: 0.2 } });
      expect(bySeconds.samples).toEqual(bySamples.samples);
      expect(snapshot.render({ range: { startSample: 1600 } }).samples.length).toBe(count - 1600);
      expect(snapshot.render({ range: { endSec: 0.1 } }).samples.length).toBe(1600);
      expect(() => snapshot.render({ range: { endSample: -1 } })).toThrow(RangeError);
      expect(() => snapshot.render({ range: { startSample: 0, startSec: 0 } })).toThrow(
        /startSample.*startSec/,
      );
    } finally {
      snapshot.dispose();
      edit.dispose();
    }
  });
});
