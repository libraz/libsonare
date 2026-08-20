/**
 * Mechanical enforcement of the addon's JS-object reader convention.
 *
 * File-local copies of the readers in `sonare_wrap_options.h` are not allowed,
 * and they came back anyway, because nothing checked. These tests are that
 * check. Two properties are asserted:
 *
 *  1. No bare `obj.Has("key")` read outside a small, reasoned allowlist. The
 *     bare form is what makes an explicit `undefined` diverge from an omitted
 *     field, which under NAPI_DISABLE_CPP_EXCEPTIONS aborts the process as soon
 *     as a second throw lands on a pending exception.
 *  2. Every options-accepting entry point is either exercised by the
 *     undefined-equivalence table below or explicitly listed as uncovered, so a
 *     NEW options-accepting function cannot be added unnoticed.
 */

import { describe, expect, it } from 'vitest';
import {
  analyze,
  decomposeStems,
  estimateMeter,
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
  masteringRepairDeclick,
  masteringRepairDeclip,
  masteringRepairDecrackle,
  masteringRepairDehum,
  masteringRepairDenoiseClassical,
  masteringRepairDereverbClassical,
  masteringRepairTrimSilence,
  mixStereo,
  noteSegments,
  Project,
  pcen,
  pitchCorrectTimevarying,
  RealtimeEngine,
  roomMorph,
  spectralEdit,
  synthesizeRir,
} from '../src/index.js';
import { addonEntryPoints, bareHasSites, optionKeysFor } from './_addon_sources.js';

/**
 * `file:key` reads that may stay in the bare form, each with the reason it is
 * already undefined-safe. Keyed by property name rather than line number so the
 * list does not rot when code moves.
 */
const BARE_HAS_ALLOWLIST: ReadonlyMap<string, string> = new Map([
  [
    'sonare_wrap_engine.cpp:events',
    'Both branches feed ReadEngineMidiEvents, which rejects a non-array; undefined and omitted produce the same throw.',
  ],
  [
    'sonare_wrap_effects.cpp:voiced',
    'Guarded by IsInt32Array on the same expression, so a non-array (including undefined) falls through.',
  ],
  [
    'sonare_wrap_effects.cpp:voicedProb',
    'Guarded by IsFloat32Array on the same expression, so a non-array (including undefined) falls through.',
  ],
  [
    'sonare_wrap_synth_patch.h:preset',
    'The body handles IsUndefined/IsNull explicitly before any typed read.',
  ],
  [
    'engine/clips_capture.cpp:warpMode',
    'ParseWarpMode maps undefined and null to SONARE_ENGINE_WARP_MODE_OFF, the omitted default.',
  ],
  [
    'effects/dynamics_repair.cpp:maxClickSamples',
    'The value is read with the type-checked node_int_option and the default (8) is positive, so undefined cannot trip the positivity check.',
  ],
  [
    'effects/dynamics_repair.cpp:paddingSamples',
    'The value is read with the type-checked node_int_option and the default is non-negative, so undefined cannot trip the range check.',
  ],
]);

const SR = 22050;

const sine = (length: number, freq = 440, sampleRate = SR): Float32Array =>
  new Float32Array(length).map((_, i) => 0.25 * Math.sin((2 * Math.PI * freq * i) / sampleRate));

/** A steady click track, so the tempo entry points below have a grid to find. */
const clicks = (bpm: number, seconds: number, sampleRate = SR): Float32Array => {
  const samples = new Float32Array(Math.floor(sampleRate * seconds));
  const clickLength = Math.floor(sampleRate / 100);
  const period = (60 / bpm) * sampleRate;
  for (let start = 0; start < samples.length; start += period) {
    for (let i = 0; i < clickLength && Math.floor(start) + i < samples.length; i++) {
      samples[Math.floor(start) + i] = (1 - i / clickLength) * 0.9;
    }
  }
  return samples;
};

/**
 * How to invoke each covered entry point with an options bag. The option KEYS
 * are not listed here — they are derived from the C++ source, so a newly added
 * option on a covered function is exercised without touching this file.
 *
 * The derived bag is always spread FIRST so the anchor values that make a call
 * meaningful (a band to mute, a clip length) are not themselves overwritten
 * with `undefined`; the remaining keys are what the assertion exercises.
 */
const UNDEFINED_EQUIVALENCE: ReadonlyArray<{
  jsName: string;
  invoke: (options: Record<string, unknown>) => unknown;
}> = [
  { jsName: 'pcen', invoke: (o) => Array.from(pcen(sine(32), 4, 8, o)) },
  { jsName: 'analyze', invoke: (o) => analyze(sine(8192), SR, o) },
  {
    jsName: 'estimateMeter',
    invoke: (o) =>
      estimateMeter({
        ...o,
        beatTimes: new Float32Array(32).map((_, i) => i * 0.5),
        beatStrengths: new Float32Array(32).map((_, i) => (i % 4 === 0 ? 1 : 0.3)),
      }),
  },
  {
    jsName: 'noteSegments',
    invoke: (o) =>
      noteSegments({
        f0Hz: new Float32Array(200).fill(440),
        voicedProb: new Float32Array(200).fill(0.9),
        frameRate: 100,
        config: o,
      }),
  },
  {
    jsName: 'spectralEdit',
    invoke: (o) =>
      Array.from(
        spectralEdit(sine(4096), SR, [{ ...o, lowHz: 1000, highHz: 3000, mode: 'mute' }], o),
      ),
  },
  {
    jsName: 'pitchCorrectTimevarying',
    invoke: (o) =>
      Array.from(
        pitchCorrectTimevarying(sine(4096, 220), new Float32Array(16).fill(220), SR, 256, o),
      ),
  },
  {
    jsName: 'midiRouteEvents',
    invoke: (o) =>
      Project.midiRouteEvents([{ ppq: 0, data0: (0x2 << 28) | (0x9 << 20) | (60 << 8) | 100 }], o),
  },
  {
    jsName: 'analyzeTempo',
    invoke: (o) => Project.create().analyzeTempo(clicks(120, 6), SR, o),
  },
  {
    // Reads the installed map back rather than only the returned BPM: the option
    // bag reaches the segmentation as well as the tempo decision, so comparing
    // one number would miss a divergence in how the map was cut.
    jsName: 'autoTempo',
    invoke: (o) => {
      const project = Project.create();
      const bpm = project.autoTempo(clicks(120, 6), SR, 0, false, o);
      const segments = Array.from({ length: project.tempoSegmentCount() }, (_, i) =>
        project.tempoSegmentByIndex(i),
      );
      return [bpm, segments];
    },
  },
  {
    jsName: 'roomMorph',
    invoke: (o) => Array.from(roomMorph(sine(4000, 440, 48000), 48000, o)).slice(0, 32),
  },
  {
    jsName: 'synthesizeRir',
    invoke: (o) => {
      const result = synthesizeRir(o);
      return [result.sampleRate, result.hasError, Array.from(result.rir).slice(0, 32)];
    },
  },
  {
    jsName: 'decomposeStems',
    invoke: (o) => {
      const result = decomposeStems({ ...o, samples: sine(4096), sampleRate: SR, nIter: 5 });
      return [
        result.sampleRate,
        result.components.length,
        Array.from(result.components[0]).slice(0, 8),
      ];
    },
  },
  {
    jsName: 'masteringRepairDeclick',
    invoke: (o) => Array.from(masteringRepairDeclick(sine(2048), SR, o)).slice(0, 32),
  },
  {
    jsName: 'masteringRepairDeclip',
    invoke: (o) => Array.from(masteringRepairDeclip(sine(2048), SR, o)).slice(0, 32),
  },
  {
    jsName: 'masteringRepairDecrackle',
    invoke: (o) => Array.from(masteringRepairDecrackle(sine(2048), SR, o)).slice(0, 32),
  },
  {
    jsName: 'masteringRepairDehum',
    invoke: (o) => Array.from(masteringRepairDehum(sine(2048), SR, o)).slice(0, 32),
  },
  {
    jsName: 'masteringRepairTrimSilence',
    invoke: (o) => Array.from(masteringRepairTrimSilence(sine(2048), SR, o)).slice(0, 32),
  },
  // Both classical restorers are STFT-based and reject a buffer shorter than
  // one analysis window, so these cannot be trimmed below the default nFft.
  {
    jsName: 'masteringRepairDenoiseClassical',
    invoke: (o) => Array.from(masteringRepairDenoiseClassical(sine(2048), SR, o)).slice(0, 32),
  },
  {
    jsName: 'masteringRepairDereverbClassical',
    invoke: (o) => Array.from(masteringRepairDereverbClassical(sine(2048), SR, o)).slice(0, 32),
  },
  {
    jsName: 'masteringDynamicsCompressor',
    invoke: (o) => {
      const result = masteringDynamicsCompressor(sine(2048), SR, o);
      return [Array.from(result.samples).slice(0, 32), result.latencySamples];
    },
  },
  {
    jsName: 'masteringDynamicsGate',
    invoke: (o) => {
      const result = masteringDynamicsGate(sine(2048), SR, o);
      return [Array.from(result.samples).slice(0, 32), result.latencySamples];
    },
  },
  {
    jsName: 'masteringDynamicsTransientShaper',
    invoke: (o) => {
      const result = masteringDynamicsTransientShaper(sine(2048), SR, o);
      return [Array.from(result.samples).slice(0, 32), result.latencySamples];
    },
  },
  {
    jsName: 'mixStereo',
    invoke: (o) => {
      const channel = sine(256);
      const out = mixStereo([channel], [channel], SR, o);
      return [Array.from(out.left).slice(0, 8), Array.from(out.right).slice(0, 8)];
    },
  },
  {
    jsName: 'setMidiClips',
    invoke: (o) =>
      withEngine((engine) =>
        engine.setMidiClips([
          { ...o, id: 1, trackId: 1, lengthSamples: 4800, events: [{ ...o, word0: 0x20903c64 }] },
        ]),
      ),
  },
  {
    jsName: 'setBuiltinInstrument',
    invoke: (o) => withEngine((engine) => engine.setBuiltinInstrument(o, 1)),
  },
  {
    jsName: 'setSf2Instrument',
    invoke: (o) => withEngine((engine) => engine.setSf2Instrument(o)),
  },
  {
    jsName: 'setMetronome',
    invoke: (o) => withEngine((engine) => engine.setMetronome({ ...o, enabled: true })),
  },
  {
    jsName: 'setMarkers',
    invoke: (o) =>
      withEngine((engine) => {
        engine.setMarkers([{ ...o, id: 1, ppq: 0 }]);
        return engine.markerByIndex(0);
      }),
  },
  {
    jsName: 'setTempoSegments',
    invoke: (o) =>
      withEngine((engine) => {
        // No engine-side getter for tempo segments; the property under test is
        // that both forms are accepted identically and neither aborts.
        engine.setTempoSegments([{ ...o, startPpq: 0, bpm: 120 }]);
        return 'accepted';
      }),
  },
  {
    jsName: 'setAutomationLane',
    invoke: (o) =>
      withEngine((engine) => {
        engine.addParameter({ id: 1, name: 'p', minValue: 0, maxValue: 1, defaultValue: 0 });
        engine.setAutomationLane(1, [{ ...o, ppq: 0, value: 0.5 }]);
        return engine.parameterCount();
      }),
  },
  {
    jsName: 'addParameter',
    invoke: (o) =>
      withEngine((engine) => {
        engine.addParameter({ ...o, id: 2, name: 'q' });
        return engine.parameterInfo(2);
      }),
  },
  {
    jsName: 'bindMidiCcBinding',
    invoke: (o) =>
      withEngine((engine) => {
        engine.bindMidiCcBinding({ ...o, ccNumber: 7, paramId: 1 });
        return engine.midiCcBindingCount();
      }),
  },
  { jsName: 'addTrack', invoke: (o) => withProject((p) => p.addTrack({ ...o, kind: 'audio' })) },
  {
    jsName: 'setGraph',
    invoke: (o) =>
      withEngine((engine) => {
        engine.setGraph({
          ...o,
          nodes: [
            { ...o, id: 'in' },
            { ...o, id: 'out' },
          ],
          connections: [{ ...o, sourceNode: 'in', sourcePort: 0, destNode: 'out', destPort: 0 }],
          inputNode: 'in',
          outputNode: 'out',
        });
        return [engine.graphNodeCount(), engine.graphConnectionCount()];
      }),
  },
  {
    jsName: 'setClips',
    invoke: (o) =>
      withEngine((engine) => {
        engine.setClips([
          { ...o, id: 1, startPpq: 0, channels: [new Float32Array(64).fill(0.25)] },
        ]);
        return engine.clipCount();
      }),
  },
  {
    jsName: 'setTrackLanes',
    invoke: (o) =>
      withEngine((engine) => {
        engine.setTrackBuses([{ busId: 1 }]);
        engine.setTrackLanes([{ ...o, trackId: 1, sends: [{ ...o, busId: 1 }] }]);
        return 'accepted';
      }),
  },
  {
    jsName: 'setTrackBuses',
    invoke: (o) =>
      withEngine((engine) => {
        engine.setTrackBuses([{ ...o, busId: 1 }]);
        return 'accepted';
      }),
  },
  {
    jsName: 'setMidiEvents',
    invoke: (o) =>
      withProject((p) => {
        const { clipId } = p.addMidiClip(0, 4);
        p.setMidiEvents(clipId, [
          { ...o, ppq: 0, data0: Project.midiNoteOn(0, 0, 0, 60, 100).data0 },
        ]);
        return p.clipCount();
      }),
  },
];

function withEngine<T>(body: (engine: RealtimeEngine) => T): T {
  const engine = new RealtimeEngine(48000, 128);
  try {
    return body(engine);
  } finally {
    engine.destroy();
  }
}

function withProject<T>(body: (project: Project) => T): T {
  const project = Project.create();
  try {
    return body(project);
  } finally {
    project.destroy();
  }
}

/**
 * Options-accepting entry points not yet driven by the table, each with why.
 * This list is the visible-gap register: adding a new options-accepting
 * function without covering it fails the coverage test below rather than
 * silently shipping unchecked.
 */
const UNCOVERED_OPTION_READERS: ReadonlyMap<string, string> = new Map(
  (
    [
      ['_synthPatchRoundTrip', 'Internal round-trip probe, covered by synth-patch.test.ts.'],
      ['addAutomationLane', 'Needs a project with a parameterised track.'],
      ['addClip', 'Needs a project with a registered audio source.'],
      ['addLoopRecordingTakes', 'Needs a capture session with recorded takes.'],
      [
        'analyzeAsync',
        'Shares its reader with analyze, which the table drives; the table compares synchronous return values.',
      ],
      ['annotateChords', 'Needs a project carrying analysed chord data.'],
      ['annotateKeys', 'Needs a project carrying analysed key data.'],
      ['bounce', 'Covered by project.test.ts; needs a fully built project.'],
      ['bounceOffline', 'Needs a prepared engine graph.'],
      ['bounceWithBuiltinInstruments', 'Covered by project.test.ts.'],
      ['bounceWithSf2Instruments', 'Needs a loaded SoundFont.'],
      ['bounceWithSynthInstruments', 'Covered by project.test.ts.'],
      ['detectKey', 'Instance method on Audio; needs a decoded Audio handle.'],
      ['detectKeyCandidates', 'Instance method on Audio; needs a decoded Audio handle.'],
      ['detectOnsets', 'Positional-only options; no object bag on the public facade.'],
      ['editAutomationLane', 'Needs a project with an existing automation lane.'],
      ['estimateRoom', 'Needs a measured impulse response.'],
      ['freezeOffline', 'Needs a prepared engine graph.'],
      ['importExternalStems', 'Needs external stem buffers; covered by project-edit tests.'],
      ['meteringSpectrum', 'Covered by metering-and-scale.test.ts.'],
      ['meteringSpectrumFrame', 'Covered by metering-and-scale.test.ts.'],
      ['midiCcLearn', 'Covered by public-input-conformance.test.ts.'],
      ['midiCcToBreakpoint', 'Reads a binding array rather than an options bag.'],
      ['midiParamToCc', 'Reads a binding array rather than an options bag.'],
      ['readFramesI16', 'Needs a live StreamAnalyzer session.'],
      ['readFramesU8', 'Needs a live StreamAnalyzer session.'],
      ['setAssistSidecar', 'Needs a project with assist sidecar data.'],
      ['setClipCompSegments', 'Needs a project with comp takes.'],
      ['setClipFade', 'Needs a project with an existing clip.'],
      ['setClipTakes', 'Needs a project with an existing clip.'],
      ['setMarkerEx', 'Project-side marker writer; covered by project.test.ts.'],
      ['setSurroundPan', 'Needs a mixer strip handle.'],
      ['setSynthInstrument', 'Covered by synth-patch.test.ts and soundfont.test.ts.'],
    ] as const
  ).map(([name, reason]) => [name, reason]),
);

describe('addon JS-object readers stay on the shared helper families', () => {
  it('has no bare Has("key") read outside the reasoned allowlist', () => {
    const unlisted = bareHasSites().filter((site) => !BARE_HAS_ALLOWLIST.has(site.id));
    expect(
      unlisted.map((site) => `${site.file}:${site.line} Has("${site.key}")`),
      'A bare Has("key") treats an explicit `undefined` differently from an omitted field. ' +
        'Use the node_*_option or *Property helpers from sonare_wrap_options.h, or add the site ' +
        'to BARE_HAS_ALLOWLIST with the reason it is already undefined-safe.',
    ).toEqual([]);
  });

  it('keeps the allowlist free of entries that no longer exist', () => {
    const live = new Set(bareHasSites().map((site) => site.id));
    expect([...BARE_HAS_ALLOWLIST.keys()].filter((id) => !live.has(id))).toEqual([]);
  });

  it('self-checks the source scanner against known entry points', () => {
    // If these regexes stop matching, every other assertion in this file would
    // pass vacuously, so pin a baseline that must keep resolving.
    const entryPoints = addonEntryPoints();
    expect(entryPoints.length).toBeGreaterThan(300);
    const byName = new Map(entryPoints.map((entry) => [entry.jsName, entry]));
    for (const name of [
      'pcen',
      'spectralEdit',
      'midiRouteEvents',
      'setMidiClips',
      'noteSegments',
    ]) {
      expect(byName.get(name)?.readsOptionsBag, `${name} should be seen as options-accepting`).toBe(
        true,
      );
    }
    // A function with no options bag must not be flagged, or the coverage list
    // below would fill with noise.
    expect(byName.get('hzToMel')?.readsOptionsBag).toBe(false);
    expect(optionKeysFor('SonareWrap::Pcen')).toContain('timeConstant');
    expect(optionKeysFor('MidiRouteEvents')).toContain('thru');
  });

  it('accounts for every options-accepting entry point', () => {
    const covered = new Set(UNDEFINED_EQUIVALENCE.map((entry) => entry.jsName));
    const unaccounted = addonEntryPoints()
      .filter((entry) => entry.readsOptionsBag)
      .map((entry) => entry.jsName)
      .filter((name) => !covered.has(name) && !UNCOVERED_OPTION_READERS.has(name));
    expect(
      unaccounted,
      'A new options-accepting addon entry point must either be driven by ' +
        'UNDEFINED_EQUIVALENCE or listed in UNCOVERED_OPTION_READERS with a reason.',
    ).toEqual([]);
  });

  it('keeps the uncovered register free of stale names', () => {
    const optionReaders = new Set(
      addonEntryPoints()
        .filter((entry) => entry.readsOptionsBag)
        .map((entry) => entry.jsName),
    );
    expect([...UNCOVERED_OPTION_READERS.keys()].filter((name) => !optionReaders.has(name))).toEqual(
      [],
    );
  });
});

describe('an explicit undefined option equals an omitted one', () => {
  const symbolFor = new Map(addonEntryPoints().map((entry) => [entry.jsName, entry.symbol]));

  for (const { jsName, invoke } of UNDEFINED_EQUIVALENCE) {
    it(`${jsName}: every derived option key reads as its default when undefined`, () => {
      const symbol = symbolFor.get(jsName);
      expect(symbol, `${jsName} is not a registered addon entry point`).toBeDefined();
      const keys = optionKeysFor(symbol as string);
      // A covered entry point with no derived keys means the scanner lost track
      // of it; fail rather than assert nothing.
      expect(keys.length, `no option keys derived for ${jsName}`).toBeGreaterThan(0);
      const allUndefined = Object.fromEntries(keys.map((key) => [key, undefined]));
      expect(invoke(allUndefined)).toEqual(invoke({}));
    });
  }
});
