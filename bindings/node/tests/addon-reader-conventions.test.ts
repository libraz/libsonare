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
  Audio,
  alignTakeToReference,
  analyze,
  analyzePolyphonic,
  assignNoteTargets,
  autoTune,
  decomposeStems,
  decomposeStemsLinked,
  detectBoundaries,
  downmix,
  estimateMeter,
  extractNotes,
  extractPercussiveEvents,
  Mixer,
  type MixStereoStrip,
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
  masteringRepairDeclick,
  masteringRepairDeclickStereo,
  masteringRepairDeclip,
  masteringRepairDeclipStereo,
  masteringRepairDecrackle,
  masteringRepairDecrackleStereo,
  masteringRepairDehum,
  masteringRepairDehumStereo,
  masteringRepairDenoiseClassical,
  masteringRepairDenoiseClassicalLinked,
  masteringRepairDenoiseClassicalStereo,
  masteringRepairDereverbClassical,
  masteringRepairDereverbClassicalLinked,
  masteringRepairDereverbClassicalStereo,
  masteringRepairDereverbConfigForRoom,
  masteringRepairDetectClicks,
  masteringRepairDetectClipping,
  masteringRepairDetectCrackle,
  masteringRepairDetectHum,
  masteringRepairDetectNoiseFloor,
  masteringRepairDetectReverb,
  masteringRepairDetectTrimRange,
  masteringRepairDetectTrimRangeStereo,
  masteringRepairNoiseBandBins,
  masteringRepairTrimSilence,
  masteringRepairTrimSilenceStereo,
  mergeNotes,
  midiNoteOn,
  midiRouteEvents,
  mixingScenePreset,
  mixStereo,
  noteSegments,
  Project,
  pcen,
  pitchCorrectTimevarying,
  RealtimeEngine,
  renderNotes,
  renderPercussiveEvents,
  roomMorph,
  roomMorphStereo,
  SampleBank,
  StreamAnalyzer,
  StreamingEqualizer,
  StreamingMasteringChain,
  StreamingRetune,
  spectralEdit,
  splitNote,
  synthesizeRir,
  transcribe,
} from '../src/index.js';
import { addon } from '../src/native.js';
import { REQUEST_KEYS } from '../src/request_keys.js';
import type { EngineMetronomeConfig } from '../src/types.js';
import {
  addonEntryPoints,
  bareHasSites,
  classifyObjectKeyDefinitions,
  isSanctionedReaderName,
  optionKeysFor,
  readerShapedDefinitions,
  SHARED_READER_FILE,
  writerShapedDefinitions,
} from './_addon_sources.js';
import { glide } from './_helpers.js';

/** The part of `options` the request itself declares; the rest belongs to a nested object. */
const requestFields = (
  name: keyof typeof REQUEST_KEYS,
  options: Record<string, unknown>,
): Record<string, unknown> =>
  Object.fromEntries(
    Object.entries(options).filter(([key]) => Object.hasOwn(REQUEST_KEYS[name], key)),
  );

/**
 * Re-keys a bag of native option names to the facade's names. A native key the
 * facade does not accept maps to `null` and is dropped.
 */
const facadeKeys = (
  options: Record<string, unknown>,
  renames: Record<string, string | null>,
): Record<string, unknown> =>
  Object.fromEntries(
    Object.entries(options).flatMap(([key, value]) => {
      const mapped = key in renames ? renames[key] : key;
      return mapped === null ? [] : [[mapped, value]];
    }),
  );

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
    'project/midi_mir.cpp:RefuseUnknownAnnotationKeys',
    'Checks the annotation row schema against its own enumerable keys. The array name labels the refusal.',
  ],
  [
    'effects/mixing.cpp:OptionAt',
    'Per-strip scalar-or-array accessor: reads either one value or the index-th element of an array-valued key, a shape the scalar families do not model. Type-checks at each call site and is already named in OPTION_READER.',
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
]);

/**
 * Readers in the shared header that are NOT options-bag readers, so
 * OPTION_READER is right not to name them. The Required* family takes an
 * out-pointer rather than a default: a missing or wrong-typed value is an error
 * there, not an omission, so a call to one says nothing about whether the entry
 * point accepts an options bag.
 */
const NON_OPTION_SHARED_READERS: ReadonlySet<string> = new Set([
  // Reads a per-band array off a RESULT record (a room estimate), not off an
  // options bag, so a call to it says nothing about its caller's options.
  'FloatArrayProperty',
  'RequiredIntProperty',
  'RequiredInt64Property',
  'RequiredUint32Property',
  'RequiredFloatProperty',
  'RequiredDoubleProperty',
  'RequiredStringProperty',
]);

const BARE_HAS_ALLOWLIST: ReadonlyMap<string, string> = new Map([
  [
    'engine/midi_instruments.cpp:events',
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
 * One decoded handle for the meter entry points, which take an options bag but
 * read their samples from the handle. The first half stays quiet and the second
 * is driven into the rails, so there are clipping regions and a spread of levels
 * to find: on a flat tone the clipping and dynamic-range reports come back empty
 * whatever the options say, and the equivalence below would hold vacuously.
 * Long enough to carry more than one dynamic-range window at the default framing.
 */
let meterAudioHandle: Audio | undefined;
const meterAudio = (): Audio => {
  if (!meterAudioHandle) {
    const samples = sine(SR * 4);
    for (let i = samples.length / 2; i < samples.length; i++) {
      samples[i] = Math.max(-1, Math.min(1, (samples[i] as number) * 6));
    }
    meterAudioHandle = Audio.fromBuffer(samples, SR);
  }
  return meterAudioHandle;
};

/**
 * The part of the derived keys that belongs to one nesting level: the facade
 * refuses a key at a level that does not declare it, while the reader scan
 * derives one flat list for the whole call.
 */
const levelOf = (o: Record<string, unknown>, keys: readonly string[]): Record<string, unknown> =>
  Object.fromEntries(Object.entries(o).filter(([key]) => keys.includes(key)));
const GRAPH_NODE_KEYS = ['type', 'gainDb', 'numPorts'] as const;
const GRAPH_CONNECTION_KEYS = ['mix'] as const;
const TRACK_SEND_KEYS = ['levelDb', 'enabled', 'sendTiming'] as const;

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
  {
    jsName: 'pcen',
    invoke: (o) => Array.from(pcen({ ...o, values: { data: sine(32), rows: 4, cols: 8 } }).data),
  },
  {
    jsName: 'analyze',
    invoke: (o) =>
      analyze(
        sine(8192),
        SR,
        facadeKeys(o, { chromaHighpassHz: 'chromaHighPassHz', tuning: 'tuningCents' }),
      ),
  },
  {
    // A glide rather than a tone, and two lengths rather than one: a held pair
    // leaves the alignment path unconstrained, so both runs would agree on
    // anchors that say nothing about the options.
    jsName: 'alignTakeToReference',
    invoke: (o) =>
      alignTakeToReference({
        ...o,
        reference: glide(261.63, 0.5, { sampleRate: SR }),
        take: glide(261.63, 0.75, { sampleRate: SR }),
        sampleRate: SR,
      }),
  },
  {
    // Long enough for the detector's kernel; the novelty curve is compared as
    // well as the boundary list, so two runs that both found nothing still
    // separate on the curve.
    jsName: 'detectBoundaries',
    invoke: (o) => {
      const result = detectBoundaries({
        ...facadeKeys(o, {
          peakDistance: 'peakDistanceSec',
          referenceWindow: 'referenceWindowSec',
        }),
        samples: sine(SR * 6),
        sampleRate: SR,
      });
      return [result.boundaries, Array.from(result.noveltyCurve), result.noveltyPeak];
    },
  },
  {
    jsName: 'estimateMeter',
    invoke: (o) =>
      estimateMeter({
        ...o,
        observations: {
          accent: Array.from({ length: 32 }, (_, i) => (i % 4 === 0 ? 1 : 0.3)),
          hasAttack: [],
          lowFrequencyEnergy: [],
          subdivisionAccent: [],
          chordChange: [],
        },
      }),
  },
  {
    jsName: 'noteSegments',
    invoke: (o) =>
      noteSegments({
        ...o,
        f0Hz: new Float32Array(200).fill(440),
        voicedProb: new Float32Array(200).fill(0.9),
        frameRate: 100,
      }),
  },
  {
    jsName: 'spectralEdit',
    invoke: (o) =>
      Array.from(
        spectralEdit(sine(4096), SR, {
          ...requestFields('spectralEdit', o),
          ops: [{ ...o, lowHz: 1000, highHz: 3000, mode: 'mute' }],
        }),
      ),
  },
  {
    jsName: 'pitchCorrectTimevarying',
    invoke: (o) =>
      Array.from(
        pitchCorrectTimevarying(sine(4096, 220), SR, {
          ...o,
          f0Hz: new Float32Array(16).fill(220),
          hopLength: 256,
        }),
      ),
  },
  {
    jsName: 'autoTune',
    invoke: (o) =>
      Array.from(
        autoTune({
          ...o,
          samples: sine(4096, 220),
          sampleRate: SR,
          key: { root: 9, mode: 'minor' },
        }).samples,
      ),
  },
  {
    jsName: 'extractNotes',
    invoke: (o) =>
      extractNotes({
        ...o,
        samples: sine(4096, 220),
        sampleRate: SR,
        f0Hz: new Float32Array(16).fill(220),
        voicedProb: new Float32Array(16).fill(0.9),
        frameRate: SR / 256,
      }),
  },
  {
    jsName: 'renderNotes',
    invoke: (o) =>
      Array.from(
        renderNotes({
          ...requestFields('renderNotes', o),
          samples: sine(4096, 220),
          sampleRate: SR,
          notes: [{ ...o, onsetSample: 512, offsetSample: 2048, edit: { ...o, gainDb: -6 } }],
        }),
      ),
  },
  {
    jsName: 'splitNote',
    invoke: (o) => {
      const frames = 40;
      const track = {
        samples: sine(frames * 512, 441),
        sampleRate: SR,
        f0Hz: new Float32Array(frames).fill(441),
        voicedProb: new Float32Array(frames).fill(0.9),
        frameRate: SR / 512,
      };
      return splitNote({
        ...requestFields('splitNote', o),
        ...track,
        notes: extractNotes(track),
        index: 0,
        frame: 20,
      });
    },
  },
  {
    jsName: 'mergeNotes',
    invoke: (o) => {
      // Two unvoiced frames, so the track segments into the two notes a merge
      // needs; one note is a run of one, which the C ABI rejects.
      const frames = 40;
      const f0Hz = new Float32Array(frames).fill(441);
      const voicedProb = new Float32Array(frames).fill(0.9);
      for (let frame = 10; frame < 12; frame += 1) {
        f0Hz[frame] = 0;
        voicedProb[frame] = 0;
      }
      const track = {
        samples: sine(frames * 512, 441),
        sampleRate: SR,
        f0Hz,
        voicedProb,
        frameRate: SR / 512,
      };
      return mergeNotes({
        ...requestFields('mergeNotes', o),
        ...track,
        notes: extractNotes(track),
        first: 0,
        last: 1,
      });
    },
  },
  {
    // The note's edit is all zeros, which is the identity the C struct spells,
    // so an edit key read as undefined answers exactly as the omitted one.
    jsName: 'assignNoteTargets',
    invoke: (o) => {
      const note = {
        onsetSample: 0,
        offsetSample: SR / 2,
        frameStart: 0,
        frameEnd: 0,
        medianHz: 440,
        medianCents: 0,
        f0Stability: 1,
        amplitude: new Float32Array(0),
        edit: {
          timeOffsetSamples: 0,
          amplitudeEnvelope: new Float32Array(0),
          pitchSemitones: 0,
          gainDb: 0,
          timeStretchRatio: 0,
          formantSemitones: 0,
          vibratoDepthChange: 0,
          driftChange: 0,
          muted: false,
        },
      };
      const result = assignNoteTargets({
        ...requestFields('assignNoteTargets', o),
        notes: [
          {
            ...note,
            ...o,
            onsetSample: 0,
            offsetSample: SR / 2,
            medianHz: 440,
            edit: { ...note.edit, ...o },
          },
        ],
        sampleRate: SR,
        targets: [{ startSec: 0, endSec: 0.5, targetMidi: 72 }],
      });
      return {
        assignedCount: result.assignedCount,
        edits: result.notes.map((assigned) => [assigned.edit.pitchSemitones, assigned.edit.muted]),
      };
    },
  },
  {
    jsName: 'extractPercussiveEvents',
    invoke: (o) => extractPercussiveEvents({ ...o, samples: sine(4096, 220), sampleRate: SR }),
  },
  {
    jsName: 'renderPercussiveEvents',
    invoke: (o) =>
      Array.from(
        renderPercussiveEvents({
          ...requestFields('renderPercussiveEvents', o),
          samples: sine(4096, 220),
          sampleRate: SR,
          events: [{ ...o, onsetSample: 512, offsetSample: 2048, edit: { ...o, gainDb: -6 } }],
        }),
      ),
  },
  {
    jsName: 'midiRouteEvents',
    invoke: (o) =>
      midiRouteEvents({
        ...requestFields('midiRouteEvents', o),
        events: [{ ppq: 0, word0: (0x2 << 28) | (0x9 << 20) | (60 << 8) | 100 }],
      }),
  },
  {
    jsName: 'analyzeTempo',
    invoke: (o) => new Project().analyzeTempo(clicks(120, 6), SR, o),
  },
  {
    // Reads the installed map back rather than only the returned BPM: the option
    // bag reaches the segmentation as well as the tempo decision, so comparing
    // one number would miss a divergence in how the map was cut.
    jsName: 'autoTempo',
    invoke: (o) => {
      const project = new Project();
      const bpm = project.autoTempo(clicks(120, 6), SR, o);
      return [bpm, project.tempoSegments()];
    },
  },
  {
    jsName: 'roomMorph',
    invoke: (o) => Array.from(roomMorph(sine(4000, 440, 48000), 48000, o).audio).slice(0, 32),
  },
  {
    jsName: 'roomMorphStereo',
    invoke: (o) =>
      Array.from(
        roomMorphStereo({
          ...o,
          left: sine(4000, 440, 48000),
          right: sine(4000, 440, 48000),
          sampleRate: 48000,
        }).left,
      ).slice(0, 32),
  },
  {
    jsName: 'synthesizeRir',
    invoke: (o) => {
      const result = synthesizeRir({ ...o, sampleRate: 48000 });
      return [result.sampleRate, Array.from(result.rir).slice(0, 32)];
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
    jsName: 'decomposeStemsLinked',
    invoke: (o) => {
      const result = decomposeStemsLinked({
        ...o,
        channels: [sine(4096), sine(4096)],
        sampleRate: SR,
        nIter: 5,
      });
      return [
        result.sampleRate,
        result.components.length,
        Array.from(result.components[0][1]).slice(0, 8),
      ];
    },
  },
  {
    jsName: 'masteringRepairDeclick',
    invoke: (o) => Array.from(masteringRepairDeclick(sine(2048), SR, o).samples).slice(0, 32),
  },
  {
    jsName: 'masteringRepairDeclickStereo',
    invoke: (o) =>
      Array.from(
        masteringRepairDeclickStereo({
          ...o,
          left: sine(2048),
          right: sine(2048),
          sampleRate: SR,
        }).channels[0],
      ).slice(0, 32),
  },
  {
    jsName: 'masteringRepairDeclip',
    invoke: (o) => Array.from(masteringRepairDeclip(sine(2048), SR, o).samples).slice(0, 32),
  },
  {
    jsName: 'masteringRepairDeclipStereo',
    invoke: (o) =>
      Array.from(
        masteringRepairDeclipStereo({
          ...o,
          left: sine(2048),
          right: sine(2048),
          sampleRate: SR,
        }).channels[0],
      ).slice(0, 32),
  },
  {
    jsName: 'masteringRepairDecrackle',
    invoke: (o) => Array.from(masteringRepairDecrackle(sine(2048), SR, o).samples).slice(0, 32),
  },
  {
    jsName: 'masteringRepairDecrackleStereo',
    invoke: (o) =>
      Array.from(
        masteringRepairDecrackleStereo({
          ...o,
          left: sine(2048),
          right: sine(2048),
          sampleRate: SR,
        }).channels[0],
      ).slice(0, 32),
  },
  {
    jsName: 'masteringRepairDehum',
    invoke: (o) => Array.from(masteringRepairDehum(sine(2048), SR, o).samples).slice(0, 32),
  },
  {
    jsName: 'masteringRepairDehumStereo',
    invoke: (o) =>
      Array.from(
        masteringRepairDehumStereo({
          ...o,
          left: sine(2048),
          right: sine(2048),
          sampleRate: SR,
        }).channels[0],
      ).slice(0, 32),
  },
  {
    jsName: 'masteringRepairTrimSilence',
    invoke: (o) => Array.from(masteringRepairTrimSilence(sine(2048), SR, o).samples).slice(0, 32),
  },
  {
    // The whole result, not a slice of one channel: this entry's output length
    // and its three ranges are what an option key moves, and comparing 32
    // samples of a tone would hold across any trim that kept the onset.
    jsName: 'masteringRepairTrimSilenceStereo',
    invoke: (o) => {
      const result = masteringRepairTrimSilenceStereo({
        ...o,
        left: sine(2048),
        right: sine(2048),
        sampleRate: SR,
      });
      return [
        Array.from(result.channels[0] as Float32Array).slice(0, 32),
        (result.channels[0] as Float32Array).length,
        result.reports[0],
        result.ranges[0],
        result.ranges[1],
      ];
    },
  },
  // Both classical restorers are STFT-based and reject a buffer shorter than
  // one analysis window, so these cannot be trimmed below the default nFft.
  {
    jsName: 'masteringRepairDenoiseClassical',
    invoke: (o) =>
      Array.from(masteringRepairDenoiseClassical(sine(2048), SR, o).samples).slice(0, 32),
  },
  {
    jsName: 'masteringRepairDenoiseClassicalStereo',
    invoke: (o) =>
      Array.from(
        masteringRepairDenoiseClassicalStereo({
          ...o,
          left: sine(2048),
          right: sine(2048),
          sampleRate: SR,
        }).channels[0],
      ).slice(0, 32),
  },
  {
    jsName: 'masteringRepairDenoiseClassicalLinked',
    invoke: (o) =>
      Array.from(
        masteringRepairDenoiseClassicalLinked({
          ...o,
          channels: [sine(2048), sine(2048, 660)],
          sampleRate: SR,
        }).channels[0],
      ).slice(0, 32),
  },
  {
    jsName: 'masteringRepairDereverbClassical',
    invoke: (o) =>
      Array.from(masteringRepairDereverbClassical(sine(2048), SR, o).samples).slice(0, 32),
  },
  {
    jsName: 'masteringRepairDereverbClassicalStereo',
    invoke: (o) =>
      Array.from(
        masteringRepairDereverbClassicalStereo({
          ...o,
          left: sine(2048),
          right: sine(2048),
          sampleRate: SR,
        }).channels[0],
      ).slice(0, 32),
  },
  {
    jsName: 'masteringRepairDereverbClassicalLinked',
    invoke: (o) =>
      Array.from(
        masteringRepairDereverbClassicalLinked({
          ...o,
          channels: [sine(2048), sine(2048, 660)],
          sampleRate: SR,
        }).channels[0],
      ).slice(0, 32),
  },
  {
    // `volume` is read from the estimate record, not the request, which refuses it
    // as an unknown key; the estimate is spread last so it survives the all-undefined bag.
    jsName: 'masteringRepairDereverbConfigForRoom',
    invoke: ({ volume: _volume, ...o }) =>
      masteringRepairDereverbConfigForRoom({
        ...o,
        estimate: {
          volume: 2500,
          lengthM: 0,
          widthM: 0,
          heightM: 0,
          drrDb: 0,
          confidence: 0,
          bandAbsorption: new Float32Array(0),
          rt60Bands: new Float32Array([9, 9, 1, 2, 9, 9]),
        },
      }),
  },
  // The detectors measure without repairing, so the whole result is compared:
  // it is a handful of scalars, and there is no buffer to slice.
  {
    jsName: 'masteringRepairDetectClicks',
    invoke: (o) => masteringRepairDetectClicks({ ...o, samples: sine(2048), sampleRate: SR }),
  },
  {
    jsName: 'masteringRepairDetectNoiseFloor',
    invoke: (o) => masteringRepairDetectNoiseFloor({ ...o, samples: sine(2048), sampleRate: SR }),
  },
  {
    // Reads its request off the bag; only the required sampleRate is added.
    jsName: 'masteringRepairNoiseBandBins',
    invoke: (o) => Array.from(masteringRepairNoiseBandBins({ ...o, sampleRate: SR })),
  },
  {
    jsName: 'masteringRepairDetectClipping',
    invoke: (o) =>
      masteringRepairDetectClipping({
        ...o,
        samples: sine(2048),
        sampleRate: SR,
      }),
  },
  {
    jsName: 'masteringRepairDetectCrackle',
    invoke: (o) => masteringRepairDetectCrackle({ ...o, samples: sine(2048), sampleRate: SR }),
  },
  {
    jsName: 'masteringRepairDetectHum',
    invoke: (o) => masteringRepairDetectHum({ ...o, samples: sine(2048), sampleRate: SR }),
  },
  {
    jsName: 'masteringRepairDetectReverb',
    invoke: (o) => masteringRepairDetectReverb({ ...o, samples: sine(2048), sampleRate: SR }),
  },
  {
    jsName: 'masteringRepairDetectTrimRange',
    invoke: (o) =>
      masteringRepairDetectTrimRange({
        ...o,
        samples: sine(2048),
        sampleRate: SR,
      }),
  },
  {
    jsName: 'masteringRepairDetectTrimRangeStereo',
    invoke: (o) =>
      masteringRepairDetectTrimRangeStereo({
        ...o,
        left: sine(2048),
        right: sine(2048),
        sampleRate: SR,
      }),
  },
  {
    jsName: 'masteringDynamicsCompressor',
    invoke: (o) => {
      // The addon's flag and corner are one facade field; the flag is implied by the corner.
      const { sidechainHpfEnabled: _enabled, sidechainHpfHz, ...rest } = o;
      const result = masteringDynamicsCompressor(sine(2048), SR, {
        ...rest,
        sidechainHighPassHz: sidechainHpfHz as number | undefined,
      });
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
    jsName: 'downmix',
    invoke: (o) =>
      downmix(
        [0.1, 0.2, 0.3, 0.4, 0.05, 0.15].map((v) => new Float32Array(8).fill(v)),
        'stereo',
        o,
      ).map((plane) => Array.from(plane)),
  },
  {
    jsName: 'mixStereo',
    invoke: (o) => {
      const channel = sine(256);
      const out = mixStereo({
        strips: [{ left: channel, right: channel, ...o } as MixStereoStrip],
        sampleRate: SR,
      });
      return [Array.from(out.left).slice(0, 8), Array.from(out.right).slice(0, 8)];
    },
  },
  {
    jsName: 'addSample',
    invoke: (o) =>
      withSampleBank((bank) => [
        bank.addSample(new Float32Array(64).fill(0.5), { ...o }),
        bank.sampleCount,
      ]),
  },
  {
    jsName: 'addZone',
    invoke: (o) =>
      withSampleBank((bank) => {
        bank.addSample(new Float32Array(64).fill(0.5));
        bank.addZone({ ...o });
        return bank.setCount;
      }),
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
    invoke: (o) => withEngine((engine) => engine.setBuiltinInstrument(1, o)),
  },
  {
    jsName: 'setSf2Instrument',
    invoke: (o) => withEngine((engine) => engine.setSf2Instrument(0, o)),
  },
  {
    jsName: 'setMetronome',
    // The reader's `clickSeconds` is spelled `clickSec` on the facade.
    invoke: (o) =>
      withEngine((engine) => {
        const { clickSeconds, ...rest } = o as Record<string, unknown>;
        engine.setMetronome({
          ...rest,
          ...('clickSeconds' in o ? { clickSec: clickSeconds } : {}),
          enabled: true,
        } as EngineMetronomeConfig);
      }),
  },
  {
    jsName: 'bindController',
    invoke: (o) =>
      withEngine((engine) => {
        // The call returns nothing, so the observable is the audio the binding
        // shapes: lo / hi / curve only surface once the bound CC moves.
        engine.setSynthInstrument(0, { preset: 'clarinet' });
        engine.clearControllerBindings(0);
        engine.bindController(0, {
          ...o,
          input: 'controlChange',
          index: 2,
          axis: 'excitation',
        });
        engine.play();
        engine.pushMidiNoteOn(0, 0, 0, 60, 100);
        const rendered: number[] = [];
        for (let block = 0; block < 8; block++) {
          engine.pushMidiCc(0, 0, 0, 2, block * 16);
          rendered.push(...engine.process([new Float32Array(128), new Float32Array(128)])[0]);
        }
        return rendered;
      }),
  },
  {
    jsName: 'setMarkers',
    invoke: (o) =>
      withEngine((engine) => {
        engine.setMarkers([{ ...o, id: 1, ppq: 0 }]);
        return engine.markers()[0];
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
    jsName: 'addParameter',
    invoke: (o) =>
      withEngine((engine) => {
        engine.addParameter({ ...o, id: 2, name: 'q' });
        return engine.parameterInfo(2);
      }),
  },
  {
    jsName: 'bindMidiCc',
    invoke: (o) =>
      withEngine((engine) => {
        engine.bindMidiCc({ ...o, ccNumber: 7, paramId: 1 });
        return engine.midiCcBindingCount;
      }),
  },
  { jsName: 'addTrack', invoke: (o) => withProject((p) => p.addTrack({ ...o, kind: 'audio' })) },
  {
    jsName: 'addAutomationLane',
    invoke: (o) =>
      withAddonProject((p) => {
        const trackId = p.addTrack({ kind: 0 });
        const laneId = p.addAutomationLane(trackId, {
          ...o,
          targetParamId: 7,
          points: [{ ppq: 0, value: 0 }],
        });
        return [laneId, p.automationLanes()];
      }),
  },
  {
    jsName: 'editAutomationLane',
    invoke: (o) =>
      withAddonProject((p) => {
        const trackId = p.addTrack({ kind: 0 });
        const laneId = p.addAutomationLane(trackId, {
          targetParamId: 7,
          points: [{ ppq: 0, value: 0 }],
        });
        p.editAutomationLane(trackId, laneId, { ...o, points: [{ ppq: 480, value: 1 }] });
        return p.automationLanes();
      }),
  },
  {
    jsName: 'encodeAudio',
    invoke: (o) => addon.encodeAudio([sine(512)], SR, { ...o, format: 'wav' }),
  },
  {
    jsName: 'setDestinationInstrument',
    invoke: (o) =>
      withAddonProject((p) => {
        const instruments = [
          {
            kind: 'synth',
            ...levelOf(o, ['useGmPrograms']),
            patch: {
              ...levelOf(o, [
                'engineParams',
                'oneShot',
                'sampleLevel',
                'sampleSet',
                'sampleStartOffset',
              ]),
              preset: 'sine',
            },
          },
          {
            kind: 'builtin',
            config: levelOf(o, [
              'attackMs',
              'decayMs',
              'gain',
              'polyphony',
              'releaseMs',
              'sustain',
            ]),
          },
          {
            kind: 'sf2',
            config: levelOf(o, [
              'clearBankRig',
              'gain',
              'gsEfxRealization',
              'polyphony',
              'preferModelForModeledFamilies',
            ]),
            soundFontUri: 'urn:bank:missing',
          },
        ];
        for (const [destinationId, instrument] of instruments.entries()) {
          const trackId = p.addTrack({ kind: 1 });
          p.setTrackMidiDestination(trackId, destinationId);
          p.setDestinationInstrument(destinationId, instrument);
        }
        return p.destinationInstruments();
      }),
  },
  {
    jsName: 'setGraph',
    invoke: (o) =>
      withEngine((engine) => {
        engine.setGraph({
          ...levelOf(o, ['numChannels', 'parameterBindings']),
          nodes: [
            { ...levelOf(o, GRAPH_NODE_KEYS), id: 'in' },
            { ...levelOf(o, GRAPH_NODE_KEYS), id: 'out' },
          ],
          connections: [
            {
              ...levelOf(o, GRAPH_CONNECTION_KEYS),
              sourceNode: 'in',
              sourcePort: 0,
              destNode: 'out',
              destPort: 0,
            },
          ],
          inputNode: 'in',
          outputNode: 'out',
        });
        return [engine.graphNodeCount, engine.graphConnectionCount];
      }),
  },
  {
    jsName: 'setClips',
    invoke: (o) =>
      withEngine((engine) => {
        engine.setClips([
          { ...o, id: 1, startPpq: 0, channels: [new Float32Array(64).fill(0.25)] },
        ]);
        return engine.clipCount;
      }),
  },
  {
    jsName: 'upsertClip',
    invoke: (o) =>
      withEngine((engine) => {
        engine.upsertClip({
          ...o,
          id: 1,
          startPpq: 0,
          channels: [new Float32Array(64).fill(0.25)],
        });
        return engine.clipCount;
      }),
  },
  {
    jsName: 'setTrackLanes',
    invoke: (o) =>
      withEngine((engine) => {
        engine.setTrackBuses([{ busId: 1 }]);
        engine.setTrackLanes([
          {
            ...levelOf(o, ['outputBusId', 'sourceChannelLayout']),
            trackId: 1,
            sends: [{ ...levelOf(o, TRACK_SEND_KEYS), busId: 1 }],
          },
        ]);
        return 'accepted';
      }),
  },
  {
    jsName: 'setTrackSends',
    invoke: (o) =>
      withEngine((engine) => {
        engine.setTrackBuses([{ busId: 1 }]);
        engine.setTrackLanes([{ trackId: 1 }]);
        engine.setTrackSends(1, [{ ...levelOf(o, TRACK_SEND_KEYS), busId: 1 }]);
        return 'accepted';
      }),
  },
  {
    jsName: 'setTrackBuses',
    invoke: (o) =>
      withEngine((engine) => {
        engine.setTrackBuses([
          { ...levelOf(o, ['gainDb', 'channelLayout', 'outputBusId']), busId: 1 },
        ]);
        return 'accepted';
      }),
  },
  {
    jsName: 'setMidiEvents',
    invoke: (o) =>
      withProject((p) => {
        const { clipId } = p.addMidiClip(0, 4);
        p.setMidiEvents(clipId, [
          {
            ...o,
            ppq: 0,
            word0: midiNoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity: 100 }).word0,
          },
        ]);
        return p.clips().length;
      }),
  },
  {
    jsName: 'transcribe',
    invoke: (o) =>
      transcribe({
        ...facadeKeys(o, { fmin: 'f0MinHz', fmax: 'f0MaxHz' }),
        samples: sine(8192),
        sampleRate: SR,
      }),
  },
  {
    jsName: 'transcribeToClip',
    invoke: (o) =>
      withProject((p) => {
        const { clipId } = p.addMidiClip(0, 4);
        // The note count alone would hold between two transcriptions that both
        // found nothing, so the written events are compared too.
        const noteCount = p.transcribeToClip({ ...o, clipId, samples: sine(8192), sampleRate: SR });
        return [noteCount, p.toJson()];
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
        eq.setBand(0, { ...o, type: 'peak', frequencyHz: 1000, gainDb: 6, enabled: true });
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
  // The ObjectWrap CONSTRUCTORS. A constructor takes an options bag like any
  // method, and each of these reads one; the entry-point scan named none of them
  // until it learned the DefineClass spelling.
  {
    jsName: 'PolyphonicAnalysis',
    invoke: (o) => {
      const analysis = analyzePolyphonic({
        ...o,
        samples: sine(8192, 220),
        sampleRate: SR,
      });
      try {
        return [
          analysis.noteCount,
          analysis.frameCount,
          Array.from(analysis.render()).slice(0, 32),
        ];
      } finally {
        analysis.destroy();
      }
    },
  },
  {
    jsName: 'StreamAnalyzer',
    invoke: (o) => {
      const analyzer = new StreamAnalyzer({
        ...facadeKeys(o, {
          computeMagnitude: null,
          magnitudeDownsample: null,
          fmin: 'fminHz',
          fmax: 'fmaxHz',
          tuningRefHz: 'referenceHz',
        }),
        sampleRate: SR,
      });
      try {
        analyzer.process(sine(8192));
        analyzer.finalize();
        return [analyzer.frameCount, analyzer.sampleRate, analyzer.stats()];
      } finally {
        analyzer.destroy();
      }
    },
  },
  {
    jsName: 'StreamingEqualizer',
    invoke: (o) => {
      const eq = new StreamingEqualizer({ ...o, sampleRate: SR });
      try {
        eq.setBand(0, { type: 'peak', frequencyHz: 1000, gainDb: 6, enabled: true });
        const block = sine(512, 1000);
        const out = eq.processStereo(block, block);
        return [Array.from(eq.spectrum().bandGainDb), Array.from(out.left)];
      } finally {
        eq.destroy();
      }
    },
  },
  {
    jsName: 'StreamingMasteringChain',
    invoke: (o) => {
      const chain = new StreamingMasteringChain({
        sampleRate: SR,
        maxBlockSize: 512,
        numChannels: 2,
        ...o,
      });
      try {
        const block = sine(512, 1000);
        const out = chain.processStereo(block, block);
        return [chain.latencySamples, chain.stageNames(), Array.from(out.left)];
      } finally {
        chain.destroy();
      }
    },
  },
  {
    jsName: 'StreamingRetune',
    invoke: (o) => {
      const retune = new StreamingRetune({ ...o });
      try {
        retune.prepare(SR, 512);
        return [retune.latencySamples, retune.config(), Array.from(retune.processMono(sine(512)))];
      } finally {
        retune.destroy();
      }
    },
  },
  {
    // The strip count alone would hold between two adds that configured their
    // meters differently, so the serialized scene is compared too: the metering
    // object is where every key on this bag lands.
    jsName: 'addStrip',
    invoke: (o) =>
      withMixer((mixer) => {
        mixer.addStrip('probe', { ...o });
        return [mixer.stripCount, mixer.toJson()];
      }),
  },
  { jsName: 'detectClipping', invoke: (o) => meterAudio().detectClipping(o) },
  { jsName: 'dynamicRange', invoke: (o) => meterAudio().dynamicRange(o) },
  { jsName: 'spectrum', invoke: (o) => meterAudio().spectrum(o) },
  { jsName: 'spectrumFrame', invoke: (o) => meterAudio().spectrumFrame(0, o) },
];

function withEngine<T>(body: (engine: RealtimeEngine) => T): T {
  const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
  try {
    return body(engine);
  } finally {
    engine.destroy();
  }
}

function withSampleBank<T>(body: (bank: SampleBank) => T): T {
  const bank = new SampleBank();
  try {
    return body(bank);
  } finally {
    bank.destroy();
  }
}

function withMixer<T>(body: (mixer: Mixer) => T): T {
  const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('commentaryDucking')), {
    sampleRate: 48000,
    maxBlockSize: 8,
  });
  try {
    return body(mixer);
  } finally {
    mixer.destroy();
  }
}

function withProject<T>(body: (project: Project) => T): T {
  const project = new Project();
  try {
    return body(project);
  } finally {
    project.destroy();
  }
}

function withAddonProject<T>(body: (project: InstanceType<typeof addon.Project>) => T): T {
  const project = new addon.Project();
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
      ['addClip', 'Needs a project with a registered audio source.'],
      ['addLoopRecordingTakes', 'Needs a capture session with recorded takes.'],
      [
        'renderRefusal',
        'Reads a refusal record the addon itself attached to an error; driven through the respelled refusals in respell-converted-bounds.test.ts.',
      ],
      [
        'applyVocalEdit',
        'Vocal project apply requests combine required source identity fields with rendered buffers; the full lifecycle and malformed-input cases are covered by vocal_project.test.ts.',
      ],
      [
        'analyzeAsync',
        'Shares its reader with analyze, which the table drives; the table compares synchronous return values.',
      ],
      ['annotateChords', 'Needs a project carrying analysed chord data.'],
      ['annotateKeys', 'Needs a project carrying analysed key data.'],
      ['bounce', 'Covered by project.test.ts; needs a fully built project.'],
      ['bounceOffline', 'Needs a prepared engine graph.'],
      ['detectKey', 'Instance method on Audio; needs a decoded Audio handle.'],
      ['detectKeyCandidates', 'Instance method on Audio; needs a decoded Audio handle.'],
      ['detectOnsets', 'Positional-only options; no object bag on the public facade.'],
      [
        'createVocalEditSession',
        'Vocal session creation combines required samples/sampleRate with nested optional create and analysis settings; functional defaults and rejection cases are covered by vocal_edit.test.ts.',
      ],
      [
        'restoreVocalEditSession',
        'Vocal session restore combines required samples/sampleRate/state with optional limits; limit defaults and rejection cases are covered by vocal_edit.test.ts.',
      ],
      ['estimateRoom', 'Needs a measured impulse response.'],
      [
        'roomGeometryFromEstimate',
        'Reads an estimate record whose missing dimensions are refused rather than defaulted; covered by acoustic.test.ts.',
      ],
      ['freezeOffline', 'Needs a prepared engine graph.'],
      ['importExternalStems', 'Needs external stem buffers; covered by project-edit tests.'],
      [
        'masteringAssistantSuggest',
        'Reads through AssistantConfigFromParams, which the scanner does not follow; mastering-assistant-preset.test.ts drives preset: undefined against an omitted preset.',
      ],
      [
        'masteringAssistantSuggestStereo',
        'Reads through AssistantConfigFromParams, which the scanner does not follow; mastering-assistant-preset.test.ts drives preset: undefined against an omitted preset.',
      ],
      ['meteringSpectrum', 'Covered by metering-and-scale.test.ts.'],
      ['meteringSpectrumFrame', 'Covered by metering-and-scale.test.ts.'],
      ['midiCcLearn', 'Covered by public-input-conformance.test.ts.'],
      ['midiCcToBreakpoint', 'Reads a binding array rather than an options bag.'],
      ['midiParamToCc', 'Reads a binding array rather than an options bag.'],
      ['readFramesI16', 'Needs a live StreamAnalyzer session.'],
      ['readFramesU8', 'Needs a live StreamAnalyzer session.'],
      [
        'render',
        'PolyphonicAnalysis instance method; needs an open analysis, and its undefined-equivalence is asserted in polyphonic-analysis.test.ts.',
      ],
      ['setAssistSidecar', 'Needs a project with assist sidecar data.'],
      ['setClipCompSegments', 'Needs a project with comp takes.'],
      ['setClipFade', 'Needs a project with an existing clip.'],
      ['setClipTakes', 'Needs a project with an existing clip.'],
      [
        'setConfig',
        'StreamingRetune instance method; its undefined-equivalence is asserted in streaming-retune.test.ts.',
      ],
      ['setMarker', 'Project-side marker writer; covered by project.test.ts.'],
      [
        'setNoteEdit',
        'PolyphonicAnalysis instance method; needs an open analysis, and its undefined-equivalence is asserted in polyphonic-analysis.test.ts.',
      ],
      ['setSurroundPan', 'Needs a mixer strip handle.'],
      ['setWarpMap', 'Needs a project with a registered audio source; covered by project.test.ts.'],
      ['setSynthInstrument', 'Covered by synth-patch.test.ts and soundfont.test.ts.'],
      [
        'setTrackStripSurroundPan',
        'Needs a prepared engine with a track lane feeding a surround bus; covered by engine-mixing.test.ts.',
      ],
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
    // The shared family, and the two arities (with and without a leading
    // Napi::Env), have to keep resolving or the shape check is blind.
    for (const name of [
      'IntProperty',
      'StringProperty',
      'BoolProperty',
      'MidiByteProperty',
      'RequiredStringProperty',
    ]) {
      expect(inShared, `${name} should be seen as reader-shaped`).toContain(name);
    }
  });

  it('tells a member writer from a key reader by return type, reference and body', () => {
    // The writer family is a shape of its own: void, a mutable object reference, and a body that
    // only sets members. Each sample below breaks exactly one of the three and must stay a reader.
    const sample = (signature: string, body: string) =>
      classifyObjectKeyDefinitions('sample.h', `inline ${signature} {\n  ${body}\n}\n`);
    const writer = sample(
      'void Put(Napi::Object& out, const char* key, double v)',
      'out.Set(key, v);',
    );
    expect(writer.writers.map((site) => site.name)).toEqual(['Put']);
    for (const [signature, body] of [
      ['bool Put(Napi::Object& out, const char* key, double v)', 'out.Set(key, v); return true;'],
      ['void Put(const Napi::Object& out, const char* key, double v)', 'out.Set(key, v);'],
      ['void Put(Napi::Object out, const char* key, double v)', 'out.Set(key, v);'],
      ['void Put(Napi::Object& out, const char* key, double* v)', '*v = out.Get(key).ToNumber();'],
      [
        'void Put(Napi::Object& out, const char* key, double* v)',
        'if (out.Has(key)) out.Set(key, *v);',
      ],
    ] as const) {
      const split = sample(signature, body);
      expect(split.writers, `${signature} { ${body} }`).toEqual([]);
      expect(split.readers.map((site) => site.name)).toEqual(['Put']);
    }
    // And the live tree has the writer the level records go through.
    expect(writerShapedDefinitions().map((site) => site.id)).toContain(
      'sonare_wrap_utils.h:SetMeasured',
    );
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
