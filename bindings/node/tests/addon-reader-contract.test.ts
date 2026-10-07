/**
 * Every addon read of a caller's value goes through the shared reader family,
 * so a present value of the wrong type is refused by name instead of being
 * coerced (`'false'` read as true), wrapped (`2**32 + 40` read as 40) or
 * replaced by a default (a non-array clearing a whole map). Each case asserts
 * the refusal AND that the state the call would have written did not move,
 * because a refusal raised after the C-ABI call is the defect in another form.
 */

import { spawnSync } from 'node:child_process';
import { describe, expect, it } from 'vitest';
import { createVocalEditSession } from '../src/index.js';
import { addon } from '../src/native.js';
import { withConfiguredProject, withEngine } from './_abort_guard_handles.js';

// biome-ignore lint/suspicious/noExplicitAny: the native surface is untyped on purpose here.
type Loose = any;

const captureError = (run: () => unknown): Error | undefined => {
  try {
    run();
    return undefined;
  } catch (error) {
    return error as Error;
  }
};

describe('GM and MIDI lookups take the integer the caller wrote', () => {
  const lookups: Array<[string, string, (value: unknown) => unknown]> = [
    ['midiGmInstrumentName', 'program', (v) => addon.midiGmInstrumentName(v)],
    ['midiGmFamilyName', 'family', (v) => addon.midiGmFamilyName(v)],
    ['midiGmFamilyFirstProgram', 'family', (v) => addon.midiGmFamilyFirstProgram(v)],
    ['midiGm2InstrumentName', 'bankLsb', (v) => addon.midiGm2InstrumentName(v, 0)],
    ['midiGm2InstrumentName', 'program', (v) => addon.midiGm2InstrumentName(0, v)],
    ['midiGmDrumName', 'note', (v) => addon.midiGmDrumName(v)],
    ['midiGm2DrumSetName', 'bankLsb', (v) => addon.midiGm2DrumSetName(v)],
    ['midiGm2DrumName', 'bankLsb', (v) => addon.midiGm2DrumName(v, 35)],
    ['midiGm2DrumName', 'note', (v) => addon.midiGm2DrumName(0, v)],
    ['midiCcName', 'controller', (v) => addon.midiCcName(v)],
    ['midiPerNoteControllerName', 'index', (v) => addon.midiPerNoteControllerName(v)],
  ];

  for (const [name, argument, lookup] of lookups) {
    for (const bad of [Number.NaN, 0.9, 2 ** 32 + 40, Number.POSITIVE_INFINITY]) {
      it(`${name}: refuses ${argument} = ${bad} by name rather than wrapping it`, () => {
        expect(() => lookup(bad)).toThrow(RangeError);
        expect(captureError(() => lookup(bad))?.message).toContain(argument);
      });
    }
    it(`${name}: refuses a wrong-typed ${argument} by name`, () => {
      expect(() => lookup('40')).toThrow(TypeError);
      expect(captureError(() => lookup('40'))?.message).toContain(argument);
    });
  }

  it('still answers a valid integer, so the refusals are not a blanket throw', () => {
    expect(addon.midiGmInstrumentName(40)).toBe('Violin');
    expect(addon.midiGmInstrumentName(0)).toBe('Acoustic Grand Piano');
  });

  it('midiBankProgram refuses a bank or program the int cannot hold', () => {
    expect(() => addon.midiBankProgram(0, 0, 0, 2 ** 32 + 1, 0, 24)).toThrow(RangeError);
    expect(() => addon.midiBankProgram(0, 0, 0, 0, 0, 2 ** 32 + 5)).toThrow(/program/);
    expect(() => addon.midiBankProgram(0, 0, 0, 0, 0.5, 24)).toThrow(/bankLsb/);
    expect(addon.midiBankProgram(0, 0, 3, 0x79, 1, 24).length).toBeGreaterThan(0);
  });
});

describe('project booleans and numbers are refused by name, never coerced', () => {
  it('setTrackMute refuses the string "false" and leaves the track as it was', () => {
    withConfiguredProject(({ project, trackId }) => {
      const before = project.toJson();
      expect(() => (project as Loose).setTrackMute(trackId, 'false')).toThrow(TypeError);
      expect(() => (project as Loose).setTrackSolo(trackId, 1)).toThrow(/solo/);
      expect(() => (project as Loose).setTrackMute(trackId)).toThrow(/mute/);
      expect(project.toJson()).toBe(before);
    });
  });

  it('addClip refuses a non-boolean isMidi and a non-number startPpq', () => {
    withConfiguredProject(({ project, trackId }) => {
      const before = project.toJson();
      const p = project as Loose;
      expect(() => p.addClip({ trackId, isMidi: 'false', lengthPpq: 4 })).toThrow(/isMidi/);
      expect(() => p.addClip({ trackId, startPpq: '1', lengthPpq: 4 })).toThrow(/startPpq/);
      expect(() => p.addClip({ trackId, lengthPpq: 4, sourceUri: 7 })).toThrow(/sourceUri/);
      expect(project.toJson()).toBe(before);
    });
  });

  it('setClipFade refuses a string fade length instead of reading it as no fade', () => {
    withConfiguredProject(({ project, clipId }) => {
      const before = project.toJson();
      expect(() => (project as Loose).setClipFade(clipId, { lengthPpq: '2' })).toThrow(/lengthPpq/);
      expect(project.toJson()).toBe(before);
    });
  });

  it('autoTempo refuses a non-boolean applyTimeSignatures', () => {
    withConfiguredProject(({ project }) => {
      const before = project.toJson();
      const audio = new Float32Array(4096);
      expect(() => (project as Loose).autoTempo(audio, 48000, 0, 'false')).toThrow(
        /applyTimeSignatures/,
      );
      expect(project.toJson()).toBe(before);
    });
  });
});

describe('annotation setters refuse a malformed array instead of clearing', () => {
  const chord = { startPpq: 0, endPpq: 4, rootPc: 0, quality: 0 };

  it('annotateChords refuses a non-array and keeps the annotations it had', () => {
    withConfiguredProject(({ project }) => {
      const p = project as Loose;
      p.annotateChords([chord]);
      const before = project.toJson();
      expect(() => p.annotateChords(chord)).toThrow(TypeError);
      expect(() => p.annotateKeys({ startPpq: 0, endPpq: 4 })).toThrow(TypeError);
      expect(project.toJson()).toBe(before);
    });
  });

  it('annotateChords refuses an extension past a byte rather than wrapping 300 to 44', () => {
    withConfiguredProject(({ project }) => {
      const p = project as Loose;
      const before = project.toJson();
      expect(() => p.annotateChords([{ ...chord, extensions: [7, 300] }])).toThrow(
        /extensions\[1\]/,
      );
      expect(() => p.annotateChords([{ ...chord, extensions: 7 }])).toThrow(/extensions/);
      expect(project.toJson()).toBe(before);
    });
  });

  it('annotateChords refuses wrong-typed fields by name', () => {
    withConfiguredProject(({ project }) => {
      const p = project as Loose;
      expect(() => p.annotateChords([{ ...chord, startPpq: '0' }])).toThrow(/startPpq/);
      expect(() => p.annotateChords([{ ...chord, romanNumeral: 5 }])).toThrow(/romanNumeral/);
      expect(() => p.annotateChords([{ ...chord, modulationBoundary: 'false' }])).toThrow(
        /modulationBoundary/,
      );
      expect(() => p.annotateKeys([{ startPpq: 0 }])).toThrow(/endPpq/);
      // A negative pitch class used to wrap to 4294967295 through a uint32 cast.
      expect(() => p.annotateChords([{ ...chord, rootPc: -1 }])).toThrow(/rootPc/);
    });
  });
});

describe('realtime engine record readers refuse wrong-typed fields', () => {
  it('setTempoSegments refuses a bare object and keeps the tempo map', () => {
    withEngine((engine) => {
      const e = engine as Loose;
      e.setTempoSegments([{ startPpq: 0, bpm: 90 }]);
      const before = e.sampleAtPpq(4);
      expect(() => e.setTempoSegments({ startPpq: 0, bpm: 140 })).toThrow(TypeError);
      expect(() => e.setTimeSignatureSegments({ startPpq: 0 })).toThrow(TypeError);
      expect(e.sampleAtPpq(4)).toBe(before);
      // Positive control: a well-formed map does move the reading.
      e.setTempoSegments([{ startPpq: 0, bpm: 140 }]);
      expect(e.sampleAtPpq(4)).not.toBe(before);
    });
  });

  it('setAutomationLane and setMarkers name a missing or wrong-typed ppq', () => {
    withEngine((engine) => {
      const e = engine as Loose;
      expect(() => e.setAutomationLane(1, [{ value: 0.5 }])).toThrow(/ppq/);
      expect(() => e.setAutomationLane(1, [{ ppq: 'x', value: 0.5 }])).toThrow(/ppq/);
      expect(() => e.setMarkers([{ id: 1, ppq: 'x' }])).toThrow(/ppq/);
      expect(() => e.setMarkers([{ id: 1, ppq: 0, name: 5 }])).toThrow(/name/);
    });
  });

  it('setMidiClips and setSf2Instrument refuse a string where a boolean belongs', () => {
    withEngine((engine) => {
      const e = engine as Loose;
      expect(() =>
        e.setMidiClips([{ id: 1, trackId: 1, events: [], lengthSamples: 64, loop: 'false' }]),
      ).toThrow(/loop/);
      expect(() => e.setSf2Instrument(1, { clearBankRig: 'false' })).toThrow(/clearBankRig/);
      expect(() => e.setSf2Instrument(1, { preferModelForModeledFamilies: 0 })).toThrow(
        /preferModelForModeledFamilies/,
      );
    });
  });
});

describe('marker keyFifths is stored as written or refused, never wrapped', () => {
  const keySignature = (keyFifths: number) => ({
    id: 1,
    kind: 4,
    keyFifths,
    keyMinor: false,
    ppq: 0,
    name: 'key',
  });

  it('RealtimeEngine.setMarkers', () => {
    withEngine((engine) => {
      const e = engine as Loose;
      for (const fifths of [7, -7]) {
        e.setMarkers([keySignature(fifths)]);
        expect(e.markerByIndex(0).keyFifths).toBe(fifths);
      }
      for (const bad of [256, -129, 200]) {
        expect(() => e.setMarkers([keySignature(bad)])).toThrow(RangeError);
        expect(captureError(() => e.setMarkers([keySignature(bad)]))?.message).toContain(
          'keyFifths',
        );
        expect(e.markerByIndex(0).keyFifths).toBe(-7);
      }
      expect(() => e.setMarkers([{ ...keySignature(0), kind: 260 }])).toThrow(/kind/);
    });
  });

  it('Project.setMarkerEx', () => {
    withConfiguredProject(({ project }) => {
      const p = project as Loose;
      for (const fifths of [7, -7]) {
        const id = p.setMarkerEx({ ...keySignature(fifths), id: 0 });
        let found: number | undefined;
        for (let i = 0; i < p.markerCount(); i++) {
          const marker = p.markerByIndex(i);
          if (marker.id === id) {
            found = marker.keyFifths;
          }
        }
        expect(found).toBe(fifths);
      }
      const before = project.toJson();
      for (const bad of [256, -129, 200]) {
        expect(() => p.setMarkerEx({ ...keySignature(bad), id: 0 })).toThrow(RangeError);
        expect(
          captureError(() => p.setMarkerEx({ ...keySignature(bad), id: 0 }))?.message,
        ).toContain('keyFifths');
      }
      expect(() => p.setMarkerEx({ ...keySignature(0), id: -1 })).toThrow(RangeError);
      expect(project.toJson()).toBe(before);
    });
  });
});

describe('optional positional flags refuse a wrong type instead of taking their default', () => {
  const tone = new Float32Array(4096).map((_, i) => Math.sin((2 * Math.PI * 440 * i) / 22050));

  it('tempogram refuses center and norm given as strings', () => {
    expect(() => addon.tempogram(tone, 22050, 512, 384, undefined, 'false')).toThrow(/center/);
    expect(() => addon.tempogram(tone, 22050, 512, 384, undefined, true, 0)).toThrow(/norm/);
    expect(addon.tempogram(tone, 22050, 512, 384, undefined, false, true).nFrames).toBeGreaterThan(
      0,
    );
  });

  it('Audio.fromBuffer refuses a string sample rate instead of reading 48000', () => {
    expect(() => addon.Audio.fromBuffer(tone, '22050')).toThrow(/sampleRate/);
  });
});

describe('bounceOffline refuses a non-finite targetLufs instead of normalizing to -14', () => {
  it('names targetLufs for NaN and infinity, and still accepts a finite target', () => {
    withEngine((engine) => {
      const e = engine as Loose;
      for (const bad of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY]) {
        const request = { totalFrames: 441, blockSize: 128, normalizeLufs: true, targetLufs: bad };
        expect(() => e.bounceOffline(request)).toThrow(RangeError);
        expect(captureError(() => e.bounceOffline(request))?.message).toContain('targetLufs');
      }
      expect(() =>
        e.bounceOffline({ totalFrames: 441, blockSize: 128, normalizeLufs: true, targetLufs: -20 }),
      ).not.toThrow();
      expect(() => e.bounceOffline({ totalFrames: 441, blockSize: 128, ditherSeed: -1 })).toThrow(
        /ditherSeed/,
      );
    });
  });
});

describe('the mixer entry points refuse a sample rate outside [8000, 384000]', () => {
  const scene = JSON.stringify({ tracks: [] });
  const tone = new Float32Array(64);

  it('Mixer constructor (Mixer.fromSceneJson)', () => {
    for (const bad of [7999, 384001, 1_000_000]) {
      expect(() => new addon.Mixer(scene, bad, 512)).toThrow(
        'Mixer: sampleRate out of supported range [8000, 384000]',
      );
    }
    for (const good of [8000, 48000, 384000]) {
      const mixer = new addon.Mixer(scene, good, 512);
      mixer.destroy?.();
    }
  });

  it('mixStereo', () => {
    for (const bad of [7999, 384001]) {
      expect(() => addon.mixStereo([tone], [tone], bad)).toThrow(
        'mixStereo: sampleRate out of supported range [8000, 384000]',
      );
    }
    expect(addon.mixStereo([tone], [tone], 48000).sampleRate).toBe(48000);
  });

  it('mixStereo refuses a per-strip option array longer than the strip list', () => {
    for (const key of ['inputTrimDb', 'faderDb', 'pan', 'panMode', 'width', 'muted']) {
      const surplus =
        key === 'muted' ? [false, true] : key === 'panMode' ? ['balance', 'balance'] : [0, 0];
      expect(() => addon.mixStereo([tone], [tone], 48000, { [key]: surplus }), key).toThrow(
        `mixStereo: '${key}' has more entries than strips (1)`,
      );
    }
    // Shorter stays legal: the remaining strip keeps its default.
    expect(addon.mixStereo([tone, tone], [tone, tone], 48000, { faderDb: [-6] }).sampleRate).toBe(
      48000,
    );
  });
});

describe('the streaming chain constructor refuses a config leaf it cannot read', () => {
  it('names a string leaf instead of dropping it from the chain', () => {
    expect(() => new addon.StreamingMasteringChain({ limiter: { ceilingDb: '-1' } })).toThrow(
      /limiter\.ceilingDb/,
    );
    expect(() => new addon.StreamingMasteringChain({ limiter: null })).toThrow(TypeError);
    expect(() => new addon.StreamingMasteringChain({ loudnessStaticGainDb: 'x' })).toThrow(
      /loudnessStaticGainDb/,
    );
  });
});

describe('vocal analysis supplied without an algorithm id is labelled host', () => {
  it('matches the C ABI and WASM default', () => {
    const samples = new Float32Array(4096).map(
      (_, i) => 0.25 * Math.sin((2 * Math.PI * 440 * i) / 16000),
    );
    const frames = Math.ceil(samples.length / 128) + 1;
    const session = createVocalEditSession({
      samples,
      sampleRate: 16000,
      frameLengthSamples: 256,
      hopLengthSamples: 128,
      analysis: {
        frameOriginSample: 0,
        samplesPerFrame: 128,
        frameLengthSamples: 256,
        f0Hz: new Float32Array(frames).fill(440),
        voiced: new Uint8Array(frames).fill(1),
      },
    });
    try {
      expect(session.analysis().algorithmId).toBe('host');
    } finally {
      session.dispose();
    }
  });
});

describe('a refusal raised before the old harness position no longer aborts', () => {
  it('exits 0 in a child process (not 134)', () => {
    const script = `
      const addon = require(${JSON.stringify(new URL('../build/Release/sonare-node.node', import.meta.url).pathname)});
      const swallow = (fn) => { try { fn(); } catch { /* a catchable error is the point */ } };
      for (let round = 0; round < 2; round++) {
        swallow(() => new addon.StreamingEqualizer({ sampleRate: 'x' }));
        swallow(() => new addon.StreamingEqualizer({ maxBlockSize: 1e40 }));
        swallow(() => new addon.StreamingMasteringChain({ loudnessStaticGainDb: 'x' }));
        swallow(() => addon.renderPlayback(new Float32Array(4), 2 ** 40, 1.5, '{}'));
        swallow(() => addon.tempogram(new Float32Array(64), 2 ** 40));
        swallow(() => addon.mixStereo([new Float32Array(4)], [new Float32Array(4)], 1e40));
        swallow(() => addon.midiGmInstrumentName(Number.NaN));
        swallow(() => addon.midiBankProgram(0, 0, 0, 2 ** 32 + 1, 0, 2 ** 32 + 5));
        const e = new addon.RealtimeEngine(48000, 128);
        swallow(() => e.setCaptureBufferExtent(2 ** 40, 1.5));
        swallow(() => e.setAutomationLane(1, [{}, {}]));
        swallow(() => e.setMarkers([{ id: 1 }, { id: 2 }]));
        e.destroy();
        const p = new addon.Project();
        swallow(() => p.annotateKeys([{}, {}]));
        swallow(() => p.annotateChords([{ startPpq: 0, endPpq: 1, extensions: [300, 300] }]));
        p.destroy();
      }
      process.exit(0);
    `;
    const result = spawnSync(process.execPath, ['-e', script], { encoding: 'utf8' });
    expect(
      { status: result.status, signal: result.signal },
      `child stderr:\n${result.stderr}`,
    ).toEqual({ status: 0, signal: null });
  });
});
