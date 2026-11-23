/**
 * NativeSynth WASM binding tests: synthPresetNames / synthPresetPatch,
 * the Project.bounce `synthInstruments` option and the realtime engine
 * setSynthInstrument entry.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import type {
  SynthPatch as DistSynthPatch,
  ProjectBounceOptions,
  SynthEngineMode,
} from '../dist/index.js';
import {
  ARTICULATIONS,
  BUILTIN_SYNTH_WAVEFORMS,
  CONTROLLER_AXES,
  CONTROLLER_INPUTS,
  init,
  isSonareError,
  MPE_DIMENSIONS,
  midiNoteOff,
  midiNoteOn,
  NOTE_TRACKINGS,
  PART_RIG_MODES,
  Project,
  RealtimeEngine,
  SAMPLE_KEY_TRACKS,
  SAMPLE_LOOP_MODES,
  SYNTH_BODY_TYPES,
  SYNTH_ENGINE_MODES,
  SYNTH_FILTER_MODELS,
  SYNTH_FILTER_OUTPUTS,
  SYNTH_LFO_SHAPES,
  SYNTH_MOD_DESTINATIONS,
  SYNTH_MOD_SOURCES,
  SYNTH_OSC_WAVEFORMS,
  SYNTH_RETRIGGERS,
  synthEngineParamInfo,
  synthEnumTables,
  synthPatchParamInfo,
  synthPresetNames,
  synthPresetPatch,
} from '../dist/index.js';
import type { SynthPatch } from '../src/instrument_types.js';
import { synthPatchRoundTripForTest } from '../src/project.js';
import { expectRefusalOf } from './_helpers';
import { shareBuiltModuleWithSource } from './_module_realm.js';

/** Bounce one instrument at its destination and interleave the planar result for sample-wise checks. */
function bounceSynth(
  project: Project,
  config: DistSynthPatch | string,
  options?: ProjectBounceOptions,
): Float32Array {
  const { destinationId, ...instrument } = (
    typeof config === 'string' ? { preset: config } : config
  ) as { destinationId?: number };
  const { channels } = project.bounce({
    ...options,
    synthInstruments: [{ destinationId, instrument }] as never,
  });
  const out = new Float32Array(channels.length * (channels[0]?.length ?? 0));
  channels.forEach((channel, c) => {
    for (let i = 0; i < channel.length; i++) {
      out[i * channels.length + c] = channel[i];
    }
  });
  return out;
}

function peak(audio: Float32Array): number {
  let p = 0;
  for (let i = 0; i < audio.length; i++) {
    const a = Math.abs(audio[i]);
    if (a > p) {
      p = a;
    }
  }
  return p;
}

describe('Sonare WASM NativeSynth', () => {
  beforeAll(async () => {
    await init();
    shareBuiltModuleWithSource();
  });

  function buildMidiOnlyProject(note = 60): Project {
    const project = new Project();
    project.setSampleRate(48000);
    const { trackId, clipId } = project.addMidiClip(0, 4);
    project.setTrackMidiDestination(trackId, 0);
    project.setMidiEvents(clipId, [
      midiNoteOn({ ppq: 0, group: 0, channel: 0, note, velocity: 100 }),
      midiNoteOff({ ppq: 2, group: 0, channel: 0, note, velocity: 0 }),
    ]);
    return project;
  }

  it('lists the preset catalog and fetches patches', () => {
    const names = synthPresetNames();
    for (const expected of [
      'sine',
      'sawLead',
      'warmPad',
      'ePiano',
      'electricGuitar',
      'harp',
      'marimba',
      'organ',
      'drumKit',
      'acousticPiano',
    ]) {
      expect(names).toContain(expected);
    }
    const pad = synthPresetPatch('warmPad');
    expect(pad.preset).toBe('warmPad');
    expect(pad.engineMode).toBe('subtractive');
    expect(pad.waveform).toBe('saw');
    expect(pad.unison).toBe(2);
    // The "va:" routing prefix is accepted.
    expect(synthPresetPatch('va:ePiano').engineMode).toBe('fm');
    expect(synthPresetPatch('clarinet').engineMode).toBe('reed');
    expect(expectRefusalOf(() => synthPresetPatch('no-such-preset'), RangeError).message).toBe(
      'synthPresetPatch: name must be one of sine, saw, square, triangle, sawLead, squareLead, subBass, warmPad, ePiano, bell, brass, pluck, classicalGuitar, steelGuitar, electricGuitar, harp, harpsichord, bassAcoustic, bassFingered, bassPicked, bassFretless, bassSlap, marimba, glass, organ, drumKit, acousticPiano, churchOrgan, churchFlute, churchBourdon, churchTrumpet, violin, viola, cello, contrabass, clarinet, sopranoSax, altoSax, tenorSax, baritoneSax, oboe, englishHorn, bassoon, trumpet, trombone, tuba, frenchHorn, mutedTrumpet, cornet, flugelhorn, euphonium, concertFlute, piccolo, recorder, panFlute, shakuhachi, tinWhistle, ocarina, blownBottle, harpPlucked, koto, sitar, tanpura, choirAah, choirOoh, voiceEeh, accordion, harmonica, bandoneon, reedOrgan, singingVoice',
    );
  });

  it('keeps every NativeSynth enum table in parity with native round-trip names', () => {
    expect(synthEnumTables()).toEqual({
      engineModes: [...SYNTH_ENGINE_MODES],
      waveforms: [...SYNTH_OSC_WAVEFORMS],
      builtinWaveforms: [...BUILTIN_SYNTH_WAVEFORMS],
      filterModels: [...SYNTH_FILTER_MODELS],
      filterOutputs: [...SYNTH_FILTER_OUTPUTS],
      bodyTypes: [...SYNTH_BODY_TYPES],
      modSources: [...SYNTH_MOD_SOURCES],
      modDestinations: [...SYNTH_MOD_DESTINATIONS],
      controllerInputs: [...CONTROLLER_INPUTS],
      controllerAxes: [...CONTROLLER_AXES],
      articulations: [...ARTICULATIONS],
      mpeDimensions: [...MPE_DIMENSIONS],
      noteTrackings: [...NOTE_TRACKINGS],
      sampleLoopModes: [...SAMPLE_LOOP_MODES],
      sampleKeyTracks: [...SAMPLE_KEY_TRACKS],
      retriggers: [...SYNTH_RETRIGGERS],
      lfoShapes: [...SYNTH_LFO_SHAPES],
      partRigModes: [...PART_RIG_MODES],
      sampleDescLoopModes: ['none', 'continuous', 'keyDown'],
    });

    for (const name of SYNTH_ENGINE_MODES) {
      expect(synthPatchRoundTripForTest({ engineMode: name }).engineMode).toBe(name);
    }
    for (const name of SYNTH_OSC_WAVEFORMS) {
      expect(synthPatchRoundTripForTest({ waveform: name }).waveform).toBe(name);
    }
    for (const name of SYNTH_FILTER_MODELS) {
      expect(synthPatchRoundTripForTest({ filterModel: name }).filterModel).toBe(name);
    }
    // The pre-rename spelling is refused like any other unknown name.
    expect(() =>
      synthPatchRoundTripForTest({ filterModel: 'moog-ladder' } as unknown as SynthPatch),
    ).toThrow(RangeError);
    for (const name of SYNTH_FILTER_OUTPUTS) {
      expect(synthPatchRoundTripForTest({ filterOutput: name }).filterOutput).toBe(name);
    }
    for (const retired of ['lowpass', 'bandpass', 'highpass']) {
      expect(() =>
        synthPatchRoundTripForTest({ filterOutput: retired } as unknown as SynthPatch),
      ).toThrow(RangeError);
    }
    for (const name of SYNTH_BODY_TYPES) {
      expect(synthPatchRoundTripForTest({ body: name }).body).toBe(name);
    }
    for (const name of SYNTH_MOD_SOURCES) {
      const echoed = synthPatchRoundTripForTest({
        modRoutings: [{ source: name, destination: 'pitchCents', depth: 1 }],
      });
      expect(echoed.modRoutings?.[0]?.source).toBe(name);
    }
    for (const name of SYNTH_MOD_DESTINATIONS) {
      const echoed = synthPatchRoundTripForTest({
        modRoutings: [{ source: 'lfo1', destination: name, depth: 1 }],
      });
      expect(echoed.modRoutings?.[0]?.destination).toBe(name);
    }
  });

  it('round-trips the retrigger mode by name and refuses unknown values', () => {
    for (const name of SYNTH_RETRIGGERS) {
      expect(synthPatchRoundTripForTest({ retrigger: name }).retrigger).toBe(name);
    }
    expect(synthPatchRoundTripForTest({}).retrigger).toBe('default');
    expect(
      expectRefusalOf(
        () => synthPatchRoundTripForTest({ retrigger: 'phase' } as unknown as SynthPatch),
        RangeError,
      ).message,
    ).toBe("synthPatchRoundTripForTest: Unknown retrigger mode name: 'phase'");
    expect(() =>
      synthPatchRoundTripForTest({ retrigger: SYNTH_RETRIGGERS.length } as never),
    ).toThrow(TypeError);
    expect(synthPresetPatch('sawLead').retrigger).toBe('free');
  });

  it('round-trips the LFO shape by name and refuses unknown values', () => {
    expect([...SYNTH_LFO_SHAPES]).toEqual(['default', 'triangle', 'sine', 'random']);
    for (const name of SYNTH_LFO_SHAPES) {
      expect(synthPatchRoundTripForTest({ lfoShape: name }).lfoShape).toBe(name);
    }
    expect(synthPatchRoundTripForTest({}).lfoShape).toBe('default');
    expect(
      expectRefusalOf(
        () => synthPatchRoundTripForTest({ lfoShape: 'square' } as unknown as SynthPatch),
        RangeError,
      ).message,
    ).toBe("synthPatchRoundTripForTest: Unknown LFO shape name: 'square'");
    expect(() =>
      synthPatchRoundTripForTest({ lfoShape: SYNTH_LFO_SHAPES.length } as never),
    ).toThrow(TypeError);
    expect(synthPresetPatch('sawLead').lfoShape).toBe('triangle');
  });

  it('round-trips the sample-engine block by name', () => {
    // The sample block has no presence bits: only a sample patch reads it, so
    // set 0 stays addressable and a plain zero is not a "keep the base" sentinel.
    for (const name of SAMPLE_LOOP_MODES) {
      expect(synthPatchRoundTripForTest({ sampleLoop: name }).sampleLoop).toBe(name);
    }
    for (const name of SAMPLE_KEY_TRACKS) {
      expect(synthPatchRoundTripForTest({ sampleKeyTrack: name }).sampleKeyTrack).toBe(name);
    }
    expect(
      synthPatchRoundTripForTest({
        engineMode: 'sample',
        sampleSet: 2,
        sampleLevel: 0.75,
        sampleStartOffset: 0.25,
      }),
    ).toMatchObject({
      engineMode: 'sample',
      sampleSet: 2,
      sampleLevel: 0.75,
      sampleStartOffset: 0.25,
    });
    expect(synthPatchRoundTripForTest({}).sampleLoop).toBe('default');
    expect(synthPatchRoundTripForTest({}).sampleKeyTrack).toBe('default');
    expect(
      expectRefusalOf(
        () => synthPatchRoundTripForTest({ sampleLoop: 'sometimes' } as unknown as SynthPatch),
        RangeError,
      ).message,
    ).toBe("synthPatchRoundTripForTest: Unknown sample loop mode name: 'sometimes'");
  });

  it('sets drift rate, envelope delay and hold and one-shot, and reads them back', () => {
    const preset = synthPresetPatch('sawLead');
    expect(preset.driftRateHz).toBeGreaterThan(0);
    for (const key of ['ampDelayMs', 'ampHoldMs', 'filterDelayMs', 'filterHoldMs'] as const) {
      expect(typeof preset[key]).toBe('number');
    }
    expect(preset.oneShot).toBe(false);
    const echoed = synthPatchRoundTripForTest({ ampHoldMs: 12, filterDelayMs: 34, oneShot: true });
    expect(echoed.ampHoldMs).toBe(12);
    expect(echoed.filterDelayMs).toBe(34);
    expect(echoed.oneShot).toBe(true);

    const project = buildMidiOnlyProject();
    try {
      // Long enough to pass the note-off, so a one-shot that ignores it differs.
      const frames = { totalFrames: 72000 };
      const base: DistSynthPatch = {
        preset: 'sawLead',
        driftCents: 40,
        ampDecayMs: 200,
        ampSustain: 0.5,
        cutoffHz: 1500,
        envToCutoffCents: 2400,
        filterDecayMs: 200,
        filterSustain: 0.3,
      };
      const reference = bounceSynth(project, base, frames);
      expect(peak(reference)).toBeGreaterThan(0);
      for (const edit of [
        { driftRateHz: 7 },
        { ampDelayMs: 80 },
        { ampHoldMs: 80 },
        { filterDelayMs: 80 },
        { filterHoldMs: 80 },
        { oneShot: true },
        // The lowest rate the field accepts, which the base does not use.
        { driftRateHz: 0.01 },
      ]) {
        const edited = bounceSynth(project, { ...base, ...edit }, frames);
        expect(edited, JSON.stringify(edit)).not.toEqual(reference);
      }
      expect(bounceSynth(project, { ...base, oneShot: undefined }, frames)).toEqual(reference);
      expect(() => bounceSynth(project, { ...base, oneShot: 1 as never }, frames)).toThrow(
        TypeError,
      );
    } finally {
      project.destroy();
    }
  });

  it('reads an absent preset back as absent and refuses an empty or over-long one', () => {
    for (const preset of [undefined, null]) {
      const patch = synthPatchRoundTripForTest({ preset } as unknown as SynthPatch);
      expect('preset' in patch, String(preset)).toBe(false);
    }
    for (const preset of ['', 'va:']) {
      expect(() => synthPatchRoundTripForTest({ preset }), preset).toThrow(
        /synth patch preset must not be empty; omit it for the init patch/,
      );
    }
    expect(() => synthPatchRoundTripForTest({ preset: 'x'.repeat(32) })).toThrow(
      /synth patch preset must be at most 31 bytes/,
    );
  });

  it('rejects non-object synth patch descriptors instead of applying defaults', () => {
    expect(() => synthPatchRoundTripForTest(42 as unknown as SynthPatch)).toThrow(/synth patch/);
    expect(() => synthPatchRoundTripForTest(true as unknown as SynthPatch)).toThrow(/synth patch/);
  });

  it('reads a null enum field as absent and refuses a wrong type by name', () => {
    // The reader family's contract: only undefined/null take the default.
    const base = synthPatchRoundTripForTest({});
    for (const field of ['engineMode', 'waveform', 'filterModel', 'filterOutput'] as const) {
      const absent = synthPatchRoundTripForTest({ [field]: null } as unknown as SynthPatch);
      expect(absent[field], field).toBe(base[field]);
      for (const wrong of [true, {}, 1]) {
        expect(
          expectRefusalOf(
            () => synthPatchRoundTripForTest({ [field]: wrong } as unknown as SynthPatch),
            TypeError,
          ).message,
        ).toBe(`synthPatchRoundTripForTest: ${field} must be a name`);
      }
    }
  });

  it('bounces preset patches deterministically', () => {
    const project = buildMidiOnlyProject();
    try {
      for (const preset of ['va:sawLead', 'ePiano', 'harp']) {
        const audio = bounceSynth(project, preset, { totalFrames: 24000 });
        expect(audio.length).toBe(48000);
        expect(peak(audio)).toBeGreaterThan(0);
      }
      const first = bounceSynth(project, 'sawLead', { totalFrames: 24000 });
      const second = bounceSynth(project, 'sawLead', { totalFrames: 24000 });
      expect(first).toEqual(second);
      expect(
        expectRefusalOf(
          () => bounceSynth(project, 'no-such-preset', { totalFrames: 128 }),
          RangeError,
        ).message,
      ).toBe('Project.bounce: synthInstruments[0].patch.preset must name a synth catalog preset');
    } finally {
      project.destroy();
    }
  });

  it('takes an explicit zero as an override, not as "keep the base"', () => {
    const project = buildMidiOnlyProject();
    try {
      // warmPad carries a non-zero stereo spread and bus drive, so turning
      // either off is a real edit. Before the patch carried presence bits a
      // zero was indistinguishable from an omitted key and the render was
      // unchanged, which made "no spread" impossible to ask for.
      const reference = bounceSynth(project, 'warmPad', { totalFrames: 24000 });
      expect(peak(reference)).toBeGreaterThan(0);

      const noSpread = bounceSynth(
        project,
        { preset: 'warmPad', stereoSpread: 0 },
        { totalFrames: 24000 },
      );
      expect(noSpread).not.toEqual(reference);

      const noDrive = bounceSynth(
        project,
        { preset: 'warmPad', busDrive: 0 },
        { totalFrames: 24000 },
      );
      expect(noDrive).not.toEqual(reference);

      // Omitting the key still keeps the base value.
      const untouched = bounceSynth(project, { preset: 'warmPad' }, { totalFrames: 24000 });
      expect(untouched).toEqual(reference);

      // An empty routing array clears the base matrix; omitting it keeps it.
      const wobble = bounceSynth(
        project,
        {
          preset: 'warmPad',
          lfoRateHz: 6,
          modRoutings: [{ source: 'lfo1', destination: 'pitchCents', depth: 80 }],
        },
        { totalFrames: 24000 },
      );
      const cleared = bounceSynth(
        project,
        { preset: 'warmPad', lfoRateHz: 6, modRoutings: [] },
        { totalFrames: 24000 },
      );
      expect(cleared).not.toEqual(wobble);
    } finally {
      project.destroy();
    }
  });

  it('takes gain: 0 as silence, not the base level', () => {
    const project = buildMidiOnlyProject();
    try {
      const reference = bounceSynth(project, 'warmPad', { totalFrames: 24000 });
      expect(peak(reference)).toBeGreaterThan(0);

      const silent = bounceSynth(project, { preset: 'warmPad', gain: 0 }, { totalFrames: 24000 });
      expect(peak(silent)).toBe(0);

      const quiet = bounceSynth(project, { preset: 'warmPad', gain: 0.01 }, { totalFrames: 24000 });
      expect(peak(quiet)).toBeGreaterThan(0);
    } finally {
      project.destroy();
    }
  });

  it('refuses a field outside the range synthPatchParamInfo publishes, naming it', () => {
    const project = buildMidiOnlyProject();
    try {
      const rejection = (patch: DistSynthPatch): unknown => {
        try {
          bounceSynth(project, patch, { totalFrames: 128 });
        } catch (error) {
          return error;
        }
        throw new Error('expected the bounce to be refused');
      };
      const info = synthPatchParamInfo();
      const cutoff = info.find((field) => field.name === 'cutoffHz');
      expect(cutoff?.max).toBe(22000);
      for (const [patch, key] of [
        [{ cutoffHz: 1e6 }, 'cutoffHz'],
        [{ stereoSpread: 5 }, 'stereoSpread'],
        [{ polyphony: 500 }, 'polyphony'],
        [{ gain: 5 }, 'gain'],
      ] as [DistSynthPatch, string][]) {
        const error = rejection(patch);
        expect(error, key).toBeInstanceOf(RangeError);
        expect((error as Error).message, key).toContain(
          `synthInstruments[0].patch.${key} must be `,
        );
      }
      // The published ends are accepted.
      expect(() =>
        bounceSynth(project, { cutoffHz: cutoff?.max }, { totalFrames: 128 }),
      ).not.toThrow();
    } finally {
      project.destroy();
    }
  });

  it('refuses a mod routing naming none on either end', () => {
    const project = buildMidiOnlyProject();
    try {
      expect(
        expectRefusalOf(
          () =>
            bounceSynth(
              project,
              { modRoutings: [{ source: 'none', destination: 'pitchCents', depth: 80 }] },
              { totalFrames: 128 },
            ),
          RangeError,
        ).message,
      ).toBe(
        'Project.bounce: synthInstruments[0].patch.modRoutings[0].source must be an integer in [1, 12]',
      );
      expect(
        expectRefusalOf(
          () =>
            bounceSynth(
              project,
              { modRoutings: [{ source: 'lfo1', destination: 'none', depth: 80 }] },
              { totalFrames: 128 },
            ),
          RangeError,
        ).message,
      ).toBe(
        'Project.bounce: synthInstruments[0].patch.modRoutings[0].destination must be an integer in [1, 12]',
      );
      // Not vacuous: a real routing on both ends still passes.
      const audio = bounceSynth(
        project,
        { modRoutings: [{ source: 'lfo1', destination: 'pitchCents', depth: 80 }] },
        { totalFrames: 24000 },
      );
      expect(peak(audio)).toBeGreaterThan(0);
    } finally {
      project.destroy();
    }
  });

  it('applies field overrides and the mod matrix', () => {
    const project = buildMidiOnlyProject();
    try {
      const plain = bounceSynth(project, {}, { totalFrames: 24000 });
      expect(peak(plain)).toBeGreaterThan(0);
      const dark = bounceSynth(project, { cutoffHz: 300, resonanceQ: 4 }, { totalFrames: 24000 });
      expect(dark).not.toEqual(plain);
      const wobble = bounceSynth(
        project,
        {
          lfoRateHz: 6,
          modRoutings: [{ source: 'lfo1', destination: 'pitchCents', depth: 80 }],
        },
        { totalFrames: 24000 },
      );
      expect(wobble).not.toEqual(plain);
      expect(
        expectRefusalOf(
          () =>
            // @ts-expect-error unknown waveform name is rejected at runtime
            bounceSynth(project, { waveform: 'sawtooth-ish' }, { totalFrames: 128 }),
          RangeError,
        ).message,
      ).toBe("Project.bounce: Unknown oscillator waveform name: 'sawtooth-ish'");
    } finally {
      project.destroy();
    }
  });

  it('plays the GM drum map through the drumKit preset', () => {
    // Note 38 = acoustic snare in the GM drum map.
    const project = buildMidiOnlyProject(38);
    try {
      const audio = bounceSynth(project, 'drumKit', { totalFrames: 24000 });
      expect(peak(audio)).toBeGreaterThan(0);
    } finally {
      project.destroy();
    }
  });

  // Bounces a one-note project and returns the interleaved render.
  function bounce(patch: DistSynthPatch | string): Float32Array {
    const project = buildMidiOnlyProject();
    try {
      return bounceSynth(project, patch, { totalFrames: 24000 });
    } finally {
      project.destroy();
    }
  }

  function thrownBy(fn: () => unknown): unknown {
    try {
      fn();
    } catch (error) {
      return error;
    }
    return undefined;
  }

  it('voices every engine mode except sample from a patch that sets only the mode', () => {
    for (const mode of SYNTH_ENGINE_MODES) {
      if (mode === 'default' || mode === 'sample') {
        continue;
      }
      // -60 dBFS on middle C.
      expect(peak(bounce({ engineMode: mode })), mode).toBeGreaterThan(0.001);
    }
    expect(peak(bounce({ engineMode: 'sample' }))).toBe(0);
  });

  it('seeds a mismatched engine from its base preset', () => {
    expect(peak(bounce({ preset: 'violin', engineMode: 'fm' }))).toBeGreaterThan(0.001);
  });

  it('describes engine sections and wrapper fields', () => {
    for (const mode of ['default', 'sample'] as const) {
      expect(synthEngineParamInfo(mode)).toEqual([]);
    }
    const bowed = synthEngineParamInfo('bowedString');
    const force = bowed.find((info) => info.name === 'bowForce');
    expect(force).toBeDefined();
    expect(force?.type).toBe('number');
    expect(typeof force?.default).toBe('number');
    expect(typeof force?.unit).toBe('string');
    for (const mode of SYNTH_ENGINE_MODES) {
      const names = synthEngineParamInfo(mode).map((info) => info.name);
      expect(new Set(names).size, mode).toBe(names.length);
    }
    expect(() => synthEngineParamInfo('no-such-engine' as unknown as SynthEngineMode)).toThrow(
      RangeError,
    );
    // biome-ignore lint/suspicious/noExplicitAny: direct JS callers bypass TypeScript unions.
    expect(() => synthEngineParamInfo(1 as any)).toThrow(TypeError);

    const wrapper = synthPatchParamInfo();
    const names = wrapper.map((info) => info.name);
    for (const expected of ['cutoffHz', 'resonanceQ', 'ampAttackMs', 'gain', 'polyphony']) {
      expect(names).toContain(expected);
    }
    expect(wrapper.find((info) => info.name === 'resonanceQ')?.unit).toBe('ratio');
    expect(wrapper.find((info) => info.name === 'cutoffHz')?.unit).toBe('Hz');
    for (const expected of [
      'driftRateHz',
      'ampDelayMs',
      'ampHoldMs',
      'filterDelayMs',
      'filterHoldMs',
    ]) {
      expect(names).toContain(expected);
    }
    expect(wrapper.find((info) => info.name === 'oneShot')?.type).toBe('boolean');
  });

  it('states a gate only where a field has one, and a switch default as a boolean', () => {
    const percussion = synthEngineParamInfo('percussion');
    const gmKit = percussion.find((field) => field.name === 'gmKit');
    expect(gmKit?.type).toBe('boolean');
    expect(typeof gmKit?.default).toBe('boolean');
    expect(gmKit && 'activeWhen' in gmKit).toBe(false);
    const kitOff = { key: 'gmKit', scope: 'engine', relation: 'eq', value: false };
    expect(percussion.find((field) => field.name === 'modeRatios6')?.activeWhen).toEqual([
      [kitOff, { key: 'numModes', scope: 'engine', relation: 'gt', value: 6 }],
    ]);
    // Reached through the direct contact or through the plate's: two alternatives.
    expect(percussion.find((field) => field.name === 'contactMs')?.activeWhen).toEqual([
      [kitOff, { key: 'contact', scope: 'engine', relation: 'gt', value: 0 }],
      [
        kitOff,
        { key: 'plateGain', scope: 'engine', relation: 'gt', value: 0 },
        { key: 'plateContact', scope: 'engine', relation: 'gt', value: 0 },
      ],
    ]);
    expect(
      synthEngineParamInfo('bowedString').find((field) => field.name === 'corpusScale')?.activeWhen,
    ).toEqual([
      [
        { key: 'body', scope: 'patch', relation: 'eq', value: 'violin' },
        { key: 'bodyMix', scope: 'patch', relation: 'gt', value: 0 },
      ],
    ]);
    for (const mode of SYNTH_ENGINE_MODES) {
      for (const field of synthEngineParamInfo(mode)) {
        if (field.activeWhen === undefined) {
          continue;
        }
        expect(field.activeWhen.length, `${mode} ${field.name}`).toBeGreaterThan(0);
        for (const alternative of field.activeWhen) {
          expect(alternative.length, `${mode} ${field.name}`).toBeGreaterThan(0);
        }
      }
    }
  });

  it('accepts the subtractive section by key', () => {
    const names = synthEngineParamInfo('subtractive').map((info) => info.name);
    expect(names.sort()).toEqual([
      'bandNoiseHighHz',
      'bandNoiseHighKeyTrack',
      'bandNoiseLevel',
      'bandNoiseLowHz',
      'bandNoiseLowKeyTrack',
      'noiseAm',
      'noiseLevel',
      'osc2Dry',
      'osc2IntervalCents',
      'osc2Level',
      'pulseWidth',
      'sustainKeyBreak',
      'sustainKeySlopeDb',
      'unisonWidth',
      'velocityExponent',
    ]);
    const plain = bounce({ engineMode: 'subtractive' });
    const layered = bounce({
      engineMode: 'subtractive',
      engineParams: { osc2IntervalCents: 702, osc2Level: 0.5, noiseLevel: 0.1 },
    });
    expect(peak(plain)).toBeGreaterThan(0);
    expect(layered).not.toEqual(plain);
    expect(() =>
      bounce({ engineMode: 'subtractive', engineParams: { osc2IntervalCents: 2401 } }),
    ).toThrow(/osc2IntervalCents/);
  });

  it('lets engineParams change a render and keeps absent keys at the base', () => {
    const bowed = synthEngineParamInfo('bowedString');
    const force = bowed.find((info) => info.name === 'bowForce');
    expect(force?.min).toBeDefined();
    expect(force?.max).toBeDefined();
    const lo = force?.min ?? 0;
    const hi = force?.max ?? 1;
    const def = typeof force?.default === 'number' ? force.default : 0;
    const changed = Math.abs(def - lo) > Math.abs(def - hi) ? lo : hi;
    const base = bounce({ engineMode: 'bowedString' });
    const edited = bounce({ engineMode: 'bowedString', engineParams: { bowForce: changed } });
    expect(edited).not.toEqual(base);
    expect(bounce({ engineMode: 'bowedString', engineParams: {} })).toEqual(base);
    // Setting a field to its own base value is the base render.
    expect(bounce({ engineMode: 'bowedString', engineParams: { bowForce: def } })).toEqual(base);
  });

  it('applies engineParams on the realtime engine path too', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      engine.setSynthInstrument(7, { engineMode: 'bowedString', engineParams: { bowForce: 0.5 } });
      engine.pushMidiNoteOn(7, 0, 0, 60, 100);
      const out = engine.process([new Float32Array(128), new Float32Array(128)]);
      expect(Math.max(...out.map((channel) => peak(channel)))).toBeGreaterThan(0);
      const error = thrownBy(() =>
        engine.setSynthInstrument(8, {
          engineMode: 'bowedString',
          engineParams: { noSuchKey: 1 },
        }),
      );
      expect(error).toBeInstanceOf(RangeError);
      expect(isSonareError(error)).toBe(false);
      expect((error as Error).message).toContain('noSuchKey');
    } finally {
      engine.destroy();
    }
  });

  it('refuses a bad engine param as a RangeError naming the key', () => {
    const bowed = synthEngineParamInfo('bowedString');
    const force = bowed.find((info) => info.name === 'bowForce');
    const reedKey = synthEngineParamInfo('reed')[0].name;
    expect(bowed.map((info) => info.name)).not.toContain(reedKey);
    const bad: [string, Record<string, number>][] = [
      ['noSuchKey', { noSuchKey: 1 }],
      [reedKey, { [reedKey]: 0.5 }],
      ['bowForce', { bowForce: Number.NaN }],
      ['bowForce', { bowForce: Number.POSITIVE_INFINITY }],
      ['bowForce', { bowForce: (force?.max ?? 1) + 1000 }],
      ['bowForce', { bowForce: (force?.min ?? 0) - 1000 }],
    ];
    for (const [key, engineParams] of bad) {
      const error = thrownBy(() => bounce({ engineMode: 'bowedString', engineParams }));
      expect(error, key).toBeInstanceOf(RangeError);
      expect((error as Error).message, key).toContain(key);
    }

    // A whole-number field refuses a fraction.
    let integerKey: string | undefined;
    let integerMode: SynthPatch['engineMode'];
    for (const mode of SYNTH_ENGINE_MODES) {
      const found = synthEngineParamInfo(mode).find((info) => info.integer === true);
      if (found !== undefined) {
        integerKey = found.name;
        integerMode = mode;
        break;
      }
    }
    expect(integerKey).toBeDefined();
    const fraction = thrownBy(() =>
      bounce({ engineMode: integerMode, engineParams: { [integerKey as string]: 1.5 } }),
    );
    expect(fraction).toBeInstanceOf(RangeError);
    expect((fraction as Error).message).toContain(integerKey as string);
  });

  it('refuses a malformed engineParams value before the C call', () => {
    for (const engineParams of [[1], 'x', 3, { bowForce: '0.5' }, { bowForce: true }]) {
      expect(() =>
        bounce({ engineMode: 'bowedString', engineParams: engineParams as never }),
      ).toThrow(TypeError);
    }
  });

  it('returns patches without engineParams from the read direction', () => {
    const viaRoundTrip = synthPatchRoundTripForTest({
      engineMode: 'bowedString',
      engineParams: { bowForce: 0.5 },
    });
    expect('engineParams' in viaRoundTrip).toBe(false);
    expect('engineParams' in synthPresetPatch('violin')).toBe(false);
  });

  it('refuses a preset-name string as the patch and leaves the binding alone', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      expect(() => engine.setSynthInstrument(7, 'sawLead' as never)).toThrow(TypeError);
      expect(engine.midiInstrumentCount).toBe(0);
    } finally {
      engine.destroy();
    }
  });

  it('renders live MIDI through the engine synth instrument', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      engine.setSynthInstrument(7, { preset: 'sawLead' });
      engine.pushMidiNoteOn(7, 0, 0, 60, 100);
      const out = engine.process([new Float32Array(128), new Float32Array(128)]);
      let p = 0;
      for (const channel of out) {
        for (const sample of channel) {
          p = Math.max(p, Math.abs(sample));
        }
      }
      expect(p).toBeGreaterThan(0);
      expect(
        expectRefusalOf(
          () => engine.setSynthInstrument(7, { preset: 'no-such-preset' }),
          RangeError,
        ).message,
      ).toBe('RealtimeEngine.setSynthInstrument: patch.preset must name a synth catalog preset');
      expect(engine.midiInstrumentCount).toBe(1);
    } finally {
      engine.destroy();
    }
  });
});
