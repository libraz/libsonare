/**
 * Mechanical enforcement of the addon's JS-object reader convention.
 *
 * File-local copies of the readers in `sonare_wrap_options.h` are not allowed.
 * Four properties are asserted:
 *
 *  1. No key reader is DEFINED outside the shared header, decided by the
 *     definition's parameter shape rather than its name — the name-based half
 *     cannot see a copy it was never told about, and a TU with its own
 *     `NumberKey` / `BoolKey` / `StringKey` makes a 25-key entry point read as
 *     taking no options at all while every other assertion here passes.
 *  2. The scanner's name list stays in step with the shared header, so a new
 *     member of the family cannot be added without the scanner learning it.
 *  3. No bare `obj.Has("key")` outside a small, reasoned allowlist: the bare
 *     form makes an explicit `undefined` diverge from an omitted field, which
 *     under NAPI_DISABLE_CPP_EXCEPTIONS aborts on the second throw.
 *  4. Every options-accepting entry point is either exercised by the
 *     undefined-equivalence table or explicitly listed as uncovered.
 *
 * (1) and (2) are what make (4) meaningful: they close the set of places a
 * reader can be defined and the gap between that set and the names the scanner
 * matches, and only then does (4) say something about all of them.
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
  StreamingEqualizer,
  spectralEdit,
  synthesizeRir,
} from '../src/index.js';
import {
  addonEntryPoints,
  bareHasSites,
  isSanctionedReaderName,
  optionKeysFor,
  readerShapedDefinitions,
  SHARED_READER_FILE,
} from './_addon_sources.js';

/**
 * `file:key` reads that may stay in the bare form, each with the reason it is
 * already undefined-safe. Keyed by property name rather than line number so the
 * list does not rot when code moves.
 */
/**
 * Reader-shaped definitions that may live outside {@link SHARED_READER_FILE},
 * each with the reason the shared families do not cover it. Keyed by
 * `file:name`. A definition that is not here and not in the shared header is a
 * file-local copy of a shared reader, which is the thing the convention forbids.
 */
const FILE_LOCAL_READER_ALLOWLIST: ReadonlyMap<string, string> = new Map([
  [
    'effects/mixing.cpp:OptionAt',
    'Per-strip scalar-or-array accessor: reads either one value or the index-th element of an array-valued key, a shape the scalar families do not model. Type-checks at each call site and is already named in OPTION_READER.',
  ],
  [
    'sonare_wrap_acoustic.cpp:NodeFloatArrayOption',
    'Reads a float ARRAY option into a vector; the shared families are scalar-only, so there is nothing to delegate to.',
  ],
  [
    'sonare_wrap_synth_patch.h:SynthEnumProperty',
    "Resolves a patch enum from either its string spelling or its ordinal, which no scalar reader expresses; it lives in the synth patch feature header shared by that feature's TUs, not in one TU.",
  ],
  [
    'sonare_wrap_synth_patch.h:SynthFieldPresent',
    'Presence probe used to decide whether a patch field was supplied at all (distinct from reading it with a default); shared by the synth patch TUs from the same header.',
  ],
  [
    'sonare_wrap_utils.h:ReadMeterCandidateNumerators',
    'Reads a fixed-capacity list into a C array and reports the count, with its own overflow and non-numeric rejection. Shared from the other addon-wide header, not a per-file copy.',
  ],
  ['sonare_wrap_utils.cpp:ReadMeterCandidateNumerators', 'Definition of the declaration above.'],
  [
    'sonare_wrap_streaming.cpp:FlattenChainConfig',
    'Not a key reader: its second parameter is the accumulated dot-path prefix for the recursion, not a key to look up. Matched only because the shape check is deliberately loose about the key parameter type.',
  ],
]);

/**
 * Readers in the shared header that are NOT options-bag readers, so
 * OPTION_READER is right not to name them. The Required* family takes an
 * out-pointer rather than a default: a missing or wrong-typed value is an error
 * there, not an omission, so a call to one says nothing about whether the entry
 * point accepts an options bag.
 */
const NON_OPTION_SHARED_READERS: ReadonlySet<string> = new Set([
  'RequiredIntProperty',
  'RequiredUint32Property',
  'RequiredFloatProperty',
  'RequiredDoubleProperty',
  'RequiredStringProperty',
]);

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
  {
    // Reads 25 band keys through EqBandFromObject. It was invisible to the
    // scanner for as long as that helper used file-local readers, so it never
    // appeared here or in the uncovered register.
    jsName: 'setBand',
    invoke: (o) => {
      const eq = new StreamingEqualizer({ sampleRate: SR, maxBlockSize: 512 });
      try {
        eq.setBand(0, { ...o, type: 'Peak', frequencyHz: 1000, gainDb: 6, enabled: true });
        // Filtered audio as well as the response curve: comparing the curve
        // alone would hold between two silent EQs if it were ever unpopulated.
        const block = sine(512, 1000);
        const out = eq.processStereo(block, block);
        // spectrum().seq counts snapshots, so compare the response alone.
        return [Array.from(eq.spectrum().bandGainDb), Array.from(out.left)];
      } finally {
        eq.destroy();
      }
    },
  },
  {
    // Same TU, same blind spot: its options were read with the file-local
    // helpers too, so it was not counted either.
    jsName: 'match',
    invoke: (o) => {
      const eq = new StreamingEqualizer({ sampleRate: SR, maxBlockSize: 512 });
      try {
        eq.match(sine(4096, 200), sine(4096, 4000), o);
        const block = sine(512, 1000);
        const out = eq.processStereo(block, block);
        return [Array.from(eq.spectrum().bandGainDb), Array.from(out.left)];
      } finally {
        eq.destroy();
      }
    },
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
      [
        'setConfig',
        'StreamingRetune instance method; its undefined-equivalence is asserted in streaming-retune.test.ts.',
      ],
      ['setMarkerEx', 'Project-side marker writer; covered by project.test.ts.'],
      ['setSurroundPan', 'Needs a mixer strip handle.'],
      ['setSynthInstrument', 'Covered by synth-patch.test.ts and soundfont.test.ts.'],
    ] as const
  ).map(([name, reason]) => [name, reason]),
);

describe('addon JS-object readers stay on the shared helper families', () => {
  it('self-checks the reader-shape scanner, so the checks below cannot pass by matching nothing', () => {
    const shaped = readerShapedDefinitions();
    expect(shaped.length).toBeGreaterThanOrEqual(20);
    const inShared = shaped
      .filter((site) => site.file === SHARED_READER_FILE)
      .map((site) => site.name);
    // Both shared families, and the two arities (with and without a leading
    // Napi::Env), have to keep resolving or the shape check is blind.
    for (const name of [
      'node_int_option',
      'node_string_option',
      'BoolProperty',
      'MidiByteProperty',
      'RequiredStringProperty',
    ]) {
      expect(inShared, `${name} should be seen as reader-shaped`).toContain(name);
    }
  });

  it('defines no key reader outside the shared header', () => {
    // A name list can only recognise the readers it was told about, so it can
    // never see the next file-local copy. This does: a copy of a shared reader
    // has to take (object, key, ...) to be a copy at all, and that is what is
    // matched here — under any name, in any translation unit.
    const unlisted = readerShapedDefinitions().filter(
      (site) => site.file !== SHARED_READER_FILE && !FILE_LOCAL_READER_ALLOWLIST.has(site.id),
    );
    expect(
      unlisted.map((site) => `${site.file}:${site.line} ${site.name}(...)`),
      `A reader that takes (Napi::Object, key, ...) belongs in ${SHARED_READER_FILE}. ` +
        'Use the node_*_option / *Property / Required* family there, extend it if the shape ' +
        'you need is missing, or add the site to FILE_LOCAL_READER_ALLOWLIST with the reason ' +
        'the shared families cannot express it. A file-local copy is invisible to the ' +
        'name-based scan, which is how a 25-key entry point once read as taking no options.',
    ).toEqual([]);
  });

  it('keeps the file-local reader allowlist free of entries that no longer exist', () => {
    const live = new Set(readerShapedDefinitions().map((site) => site.id));
    expect([...FILE_LOCAL_READER_ALLOWLIST.keys()].filter((id) => !live.has(id))).toEqual([]);
  });

  it('recognises every options reader the shared header defines', () => {
    // Closes the other half: the shape check above guarantees readers only live
    // in the shared header, and this guarantees the scanner's name list covers
    // that header. Adding a family member without teaching OPTION_READER fails
    // here instead of silently un-counting whatever calls it.
    const unrecognised = readerShapedDefinitions()
      .filter((site) => site.file === SHARED_READER_FILE)
      .map((site) => site.name)
      .filter((name) => !isSanctionedReaderName(name) && !NON_OPTION_SHARED_READERS.has(name));
    expect(
      unrecognised,
      'A new reader in the shared header must be added to OPTION_READER / OPTION_READER_KEY ' +
        'in _addon_sources.ts, or listed in NON_OPTION_SHARED_READERS with the reason a call ' +
        'to it does not make its caller an options-accepting entry point.',
    ).toEqual([]);
  });

  it('keeps the non-option register free of stale names', () => {
    const shared = new Set(
      readerShapedDefinitions()
        .filter((site) => site.file === SHARED_READER_FILE)
        .map((site) => site.name),
    );
    expect([...NON_OPTION_SHARED_READERS].filter((name) => !shared.has(name))).toEqual([]);
  });

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
