import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  init,
  midiCc,
  midiCcLearn,
  Project,
  RealtimeEngine,
  realtimeVoiceChangerPresetNames,
  voiceCharacterPresetId,
} from '../src/index';
import { getSonareModule } from '../src/module_state';

const SAMPLE_RATE = 48000;

describe('native WASM result shapes', () => {
  beforeAll(async () => init());

  it('omits unmeasured streaming loudness gain and writes the measured gain', () => {
    const native = getSonareModule();
    const samples = new Float32Array(SAMPLE_RATE);
    const silent = native.masteringStreamingLoudnessGain(samples, SAMPLE_RATE, null);
    expect(Object.hasOwn(silent, 'loudnessStaticGainDb')).toBe(false);
    expect(silent.integratedLufs).toBe(Number.NEGATIVE_INFINITY);

    for (let i = 0; i < samples.length; i++) {
      samples[i] = 0.125 * Math.sin((2 * Math.PI * 440 * i) / SAMPLE_RATE);
    }
    const measured = native.masteringStreamingLoudnessGain(samples, SAMPLE_RATE, null);
    expect(Object.hasOwn(measured, 'loudnessStaticGainDb')).toBe(true);
    expect(measured.loudnessStaticGainDb).toBeTypeOf('number');
    expect(Number.isFinite(measured.loudnessStaticGainDb)).toBe(true);
  });

  it('returns numbers for frame and sample positions at the raw engine boundary', () => {
    const native = getSonareModule();
    const engine = new native.RealtimeEngine(SAMPLE_RATE, 512, 256, 256);
    try {
      engine.setTempo(120);
      expect(engine.sampleAtPpq(1)).toBe(24000);
      expect(engine.countInEndSample(0, 1)).toBe(96000);
      for (const value of [
        engine.externalMidiScratchRenderFrame(),
        engine.telemetryScratchRenderFrame(),
        engine.telemetryScratchTimelineSample(),
        engine.telemetryScratchAudibleTimelineSample(),
        engine.meterScratchRenderFrame(),
        engine.scopeScratchRenderFrame(),
      ]) {
        expect(value).toBeTypeOf('number');
        expect(Number.isSafeInteger(value)).toBe(true);
      }
    } finally {
      engine.delete();
    }
  });

  it('refuses an unsafe sample position and returns undefined for an empty page queue', () => {
    const engine = new RealtimeEngine({ sampleRate: SAMPLE_RATE, maxBlockSize: 512 });
    try {
      engine.setTempo(120);
      expect(engine.sampleAtPpq(1)).toBe(24000);
      expect(() => engine.sampleAtPpq(1e12)).toThrow(
        expect.objectContaining({
          code: ErrorCode.InvalidFormat,
          message: expect.stringMatching(/sample is not exactly representable as a number$/),
        }),
      );
      expect(engine.popClipPageRequest()).toBeUndefined();
    } finally {
      engine.destroy();
    }
  });

  it('writes token strings for the raw instrument and MIDI CC readers', () => {
    const native = getSonareModule();
    const engine = new native.RealtimeEngine(SAMPLE_RATE, 512, 256, 256);
    try {
      engine.setSynthInstrument(0, { preset: 'clarinet' });
      engine.setControllerNoteTracking(0, 'pressure', 'highest');
      expect(engine.controllerNoteTracking(0, 'pressure')).toBe('highest');
      engine.setArticulation(0, 0, 'monoLegato');
      expect(engine.articulation(0, 0)).toBe('monoLegato');

      const learned = midiCcLearn({
        events: [
          midiCc({ ppq: 0, group: 0, channel: 2, controller: 1, value: 64 }),
          midiCc({ ppq: 0.1, group: 0, channel: 2, controller: 33, value: 12 }),
        ],
        paramId: 77,
      });
      expect(learned?.kind).toBe('controlChange14');
      if (learned === null) {
        throw new Error('MIDI CC learn produced no binding');
      }
      engine.bindMidiCc(learned);
      expect(engine.midiCcBindings()).toMatchObject([{ kind: 'controlChange14' }]);
    } finally {
      engine.delete();
    }
  });

  it('returns own boolean modulation flags and known voice preset tokens', () => {
    const project = new Project();
    try {
      project.annotateChords([
        { startPpq: 0, endPpq: 1, modulationBoundary: true },
        { startPpq: 1, endPpq: 2, modulationBoundary: false },
      ]);
      const chords = project.chordAnnotations();
      expect(chords.map((chord) => chord.modulationBoundary)).toEqual([true, false]);
      expect(chords.every((chord) => Object.hasOwn(chord, 'modulationBoundary'))).toBe(true);
    } finally {
      project.destroy();
    }

    const presets = realtimeVoiceChangerPresetNames();
    expect(presets.length).toBeGreaterThan(0);
    expect(presets.map((_, ordinal) => voiceCharacterPresetId(ordinal))).toEqual(presets);
  });
});
