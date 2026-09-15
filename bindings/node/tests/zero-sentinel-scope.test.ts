/**
 * Every integer key whose fallback is a literal 0 has answered what its zero
 * means.
 *
 * The addon's integer readers truncate, so `0.5` reaches the callee as 0. Where
 * 0 is a quantity that only changes a magnitude; where 0 means "keep the library
 * default" it is a category change reported as success, and the call returns
 * what a caller who asked for nothing would have got. `kZeroIsSentinel` marks
 * the second kind, and marking it made the distinction a property of the source
 * SPELLING — `node_int_option(opts, "nFft", kZeroIsSentinel)` and
 * `node_int_option(opts, "frameStart", 0)` are now different text where they
 * used to be identical.
 *
 * That is only worth anything if something reads it. This is that reader: a
 * literal-zero fallback is either tagged or carries a recorded reason, so the
 * population cannot quietly revert as keys are added. A reason is a STRING, not
 * an omission — the next reader has to be able to tell a decision from a gap.
 *
 * WHAT A GREEN RUN HERE DOES AND DOES NOT MEAN. Green says every literal-zero
 * read is accounted for, NOT that every read is benign:
 * {@link UNTAGGED_SENTINEL_READS} holds reads already established to select a
 * library default, which cannot be tagged without a C++ change, and while that
 * list is non-empty this file's green is conditional on it. It is empty, so
 * green stands on its own; the list is a ratchet — it may shrink, never grow —
 * so read it before treating a green run as a clean bill of health.
 *
 * A REASON IS WRITTEN ABOUT THE FIELD, NEVER ABOUT THE READER. A site is keyed
 * `file:key`, so a reason that argues from the reader ("this one refuses a
 * fraction") keeps applying verbatim after the site moves to a reader that does
 * not, laundering it. A reason that argues from what the field's zero MEANS
 * downstream survives that move intact, which is why reader-level exemptions
 * live in {@link UNSCANNED_SHARED_READERS} instead — see the note there.
 *
 * Every case drives `evaluateZeroSentinelScope` — the function the assertion
 * below calls — on synthetic sources, because a rule asserted through a
 * re-implementation of its scanner would only ever agree with itself. Each class
 * is reverted on its own and must produce exactly one finding.
 */

import { describe, expect, it } from 'vitest';
import {
  type AddonSource,
  addonSources,
  evaluateZeroSentinelScope,
  isScannedZeroFallbackReader,
  readerShapedDefinitions,
  SHARED_READER_FILE,
  zeroFallbackSites,
} from './_addon_sources.js';

/**
 * Keys whose fallback stays a literal 0, each with what their zero does. Most
 * are a quantity, an ordinal or an unset id the caller can mean; the rest say
 * plainly that the zero selects something else, or that nothing observable
 * settles it. An entry expires with its read.
 */
const ZERO_FALLBACK_REASONS: ReadonlyMap<string, string> = new Map([
  [
    'engine/clips_capture.cpp:trackId',
    '0 is the unassigned track id: the clip player returns on it rather than substituting a track of its own.',
  ],
  [
    'engine/clips_capture.cpp:clipOffsetSamples',
    "A read offset into the clip's source; 0 is its first sample, a position the caller can mean.",
  ],
  [
    'engine/clips_capture.cpp:fadeInSamples',
    'A fade length in samples; 0 is no fade, a length the caller can mean.',
  ],
  ['engine/clips_capture.cpp:fadeOutSamples', 'Fade length, as fadeInSamples.'],
  [
    'addon.cpp:data1',
    'The optional second word of a packed MIDI 1.0 event; 0 is the empty word an absent one reads as.',
  ],
  [
    'project/midi_mir.cpp:data1',
    'The optional second word of a packed MIDI 1.0 event; 0 is the empty word an absent one reads as.',
  ],
  [
    'project/render.cpp:destinationId',
    'The C ABI documents the default MIDI destination as 0, so zero addresses a destination rather than standing in for an absent one.',
  ],
  [
    'sonare_wrap_sample_bank.cpp:loopStart',
    'A frame offset inside the sample; 0 is its first frame, and loopMode rather than this zero decides whether the region loops.',
  ],
  [
    'sonare_wrap_sample_bank.cpp:loopEnd',
    'A frame offset inside the sample; the core gates looping on loop_mode and loop_end > loop_start, so zero is a frame index rather than a selector.',
  ],
  [
    'sonare_wrap_sample_bank.cpp:sampleIndex',
    'An index into the bank; 0 is the first sample, a valid index the caller can mean.',
  ],
  [
    'engine/graph_offline.cpp:type',
    'Graph node kind ordinal, where 0 is pass-through — a named member rather than an absent value.',
  ],
  [
    'engine/graph_offline.cpp:totalFrames',
    'Both the bounce and the freeze reject total_frames <= 0 as an invalid parameter, so 0 selects nothing.',
  ],
  [
    'engine/graph_offline.cpp:dither',
    'Dither kind ordinal, where 0 is none — a named member the caller can ask for.',
  ],
  [
    'project/edit.cpp:takeId',
    "0 falls back to the clip's active take; the facade refuses a fractional value ahead of the addon, which keeps truncating.",
  ],
  [
    'project/edit.cpp:targetParamId',
    'The C ABI reserves 0 as the invalid/unset parameter id and refuses a lane carrying it.',
  ],
  [
    'project/edit.cpp:trackId',
    'A host-assigned track id, where 0 is the unset id rather than a selector for a default track.',
  ],
  [
    'project/edit.cpp:audioSampleRate',
    'Taken as a source-rate hint only above 0; without one the clip keeps the project rate, which is also what the omitted field gives.',
  ],
  [
    'project/external_stems.cpp:layout',
    'Channel-layout ordinals start at 1, so 0 matches none and the read site refuses the stem before the import.',
  ],
  [
    'project/external_stems.cpp:startFrame',
    'A placement offset in frames; 0 is the timeline origin, a position the caller can mean.',
  ],
  [
    'project/external_stems.cpp:sampleRate',
    "The import rejects sample_rate <= 0, and accepts only the project's own rate above it, so 0 selects nothing.",
  ],
  ['project/midi_mir.cpp:mode', 'Key-segment mode ordinal, where 0 is a named mode.'],
  ['project/midi_mir.cpp:quality', 'Chord-quality ordinal, where 0 is a named quality.'],
  [
    'project/midi_mir.cpp:schemaVersion',
    'A module-owned sidecar version the core stores verbatim; 0 is the unversioned payload.',
  ],
  [
    'project/midi_mir.cpp:targetTrackId',
    '0 is the unset track id, which scopes the sidecar to the project instead of to a track.',
  ],
  [
    'project/render.cpp:blockSize',
    'The C ABI reads <= 0 as 128, but the offline render is block-size independent, so no block size — the one 0 selects included — moves the samples.',
  ],
  [
    'project/render.cpp:sampleRate',
    "The C ABI reads <= 0 as the project's own rate and refuses any other, so the only accepted value is the one 0 selects.",
  ],
  [
    'project/render.cpp:instrumentLatencySamples',
    'Host-instrument PDC fed to the compiler. Unsettled rather than inert: a control that moves the render needs a latency-reporting host instrument, which the JS surface cannot install, so what its zero does here has not been observed.',
  ],
  [
    'sonare_wrap_acoustic.cpp:materialPreset',
    'Preset selectors start at 1; 0 is the no-preset branch, where the absorption and scattering fields decide the wall instead.',
  ],
  [
    'sonare_wrap_acoustic.cpp:mode',
    'Estimator mode ordinal, where 0 is the default arm of the switch — a named mode.',
  ],
  [
    'sonare_wrap_effects.cpp:onsetSample',
    'An onset position in samples; 0 is the first sample of the buffer.',
  ],
  [
    'sonare_wrap_effects.cpp:offsetSample',
    'An end position in samples; 0 is the first sample of the buffer.',
  ],
  ['sonare_wrap_effects.cpp:frameStart', 'An analysis frame index; 0 is the first frame.'],
  ['sonare_wrap_effects.cpp:frameEnd', 'An analysis frame index; 0 is the first frame.'],
  [
    'sonare_wrap_effects.cpp:timeOffsetSamples',
    'Zero is the identity shift; the facade refuses a fractional value ahead of the addon, which keeps truncating.',
  ],
  [
    'sonare_wrap_effects.cpp:startSample',
    'A spectral-edit op start position in samples; 0 is the first sample of the buffer.',
  ],
  [
    'sonare_wrap_engine.cpp:renderFrame',
    'The absolute frame an event is scheduled at; 0 is the first frame of the timeline.',
  ],
  [
    'sonare_wrap_engine.cpp:data0',
    'The compatibility alias word0 falls back to; 0 is the empty word, and wordCount decides how many words are read.',
  ],
  [
    'sonare_wrap_engine.cpp:data1',
    'The compatibility alias word1 falls back to; 0 is the empty word, and wordCount decides how many words are read.',
  ],
  [
    'sonare_wrap_engine.cpp:word2',
    'A trailing UMP word; 0 is the empty word, and wordCount rather than this zero decides how many are read.',
  ],
  [
    'sonare_wrap_engine.cpp:word3',
    'A trailing UMP word; 0 is the empty word, and wordCount rather than this zero decides how many are read.',
  ],
  [
    'sonare_wrap_engine.cpp:wordCount',
    'Documented as tolerant — anything outside [1,4] lets the C bridge infer the word form. Unsettled: an inferred form and a requested one are not distinguishable from the JS surface, so no control separates them.',
  ],
  [
    'sonare_wrap_engine.cpp:sysexHandle',
    'Unsettled: 0 is the absent-handle spelling, and a control needs a live registered handle, which this entry point cannot produce, so what a truncated handle does has not been observed.',
  ],
  ['sonare_wrap_engine.cpp:curveToNext', 'Automation curve ordinal, where 0 is a named curve.'],
  [
    'sonare_wrap_engine.cpp:keyFifths',
    'A signed count of sharps or flats; 0 is C major, a quantity the caller can mean.',
  ],
  [
    'sonare_wrap_engine.cpp:id',
    'A host-assigned MIDI clip id, where 0 is the unset id rather than a selector for a default clip.',
  ],
  [
    'sonare_wrap_engine.cpp:trackId',
    'The unassigned track id, and also what destinationId falls back to; the clip player returns on it rather than substituting a track.',
  ],
  [
    'sonare_wrap_engine.cpp:startSample',
    'A clip start position on the timeline; 0 is the first frame.',
  ],
  [
    'sonare_wrap_note_objects.h:timeOffsetSamples',
    'Zero is the identity shift; the facade refuses a fractional value ahead of the addon, which keeps truncating.',
  ],
  [
    'sonare_wrap_project.cpp:id',
    'A marker id chosen by the caller, where 0 is the unset id rather than a selector for a default marker.',
  ],
  [
    'sonare_wrap_project.cpp:keyFifths',
    'A signed count of sharps or flats; 0 is C major, a quantity the caller can mean.',
  ],
  [
    'sonare_wrap_synth_patch.h:sampleSet',
    'A keymap set index that is addressable as a plain zero by design: the sample block carries no presence bits precisely so set 0 stays reachable.',
  ],
]);

/**
 * Reads ESTABLISHED to select a library default, still spelled with a literal 0
 * because tagging them is a C++ change. Empty: every established sentinel read
 * carries `kZeroIsSentinel`.
 *
 * These are not reasoned-away sites and must not be merged back into
 * {@link ZERO_FALLBACK_REASONS}: that map answers "this zero is a quantity", and
 * routing a known sentinel through the same door makes one green mean two
 * different things. An entry states what the zero actually selects, cites the
 * line that settles it, and says whether a control that would demonstrate the
 * substitution can be built from the JS surface today — which is where anyone
 * picking one up should start.
 *
 * THIS LIST IS A RATCHET: it may shrink, never grow. An entry leaves when the
 * read is tagged; the assertions below fail both on an entry that
 * {@link EXPECTED_UNTAGGED_SENTINELS} does not also name and on an entry whose
 * read has already been tagged. A non-empty list deliberately does NOT fail on
 * its own — the tag needs a native build — so the cost of leaving one here is
 * that this file's green is conditional, which the header says.
 */
const UNTAGGED_SENTINEL_READS: ReadonlyMap<string, string> = new Map<string, string>();

/**
 * Readers the shared header defines that the zero-fallback scan does NOT cover,
 * each with why a call to one owes no reason.
 *
 * This is the half that keeps the population from reverting as the header grows:
 * the reason register above can only speak about reads the scan can see, so a
 * new integer reader added to the header would escape it silently. Here a new
 * name fails until someone either teaches the scan or records why it is exempt.
 */
const UNSCANNED_SHARED_READERS: ReadonlyMap<string, string> = new Map([
  ['BoolProperty', 'Reads a boolean, so it has no integer zero to land on.'],
  ['DoubleProperty', 'Reads a double; truncation is not in play.'],
  ['FloatProperty', 'Reads a float; truncation is not in play.'],
  ['FiniteFloatProperty', 'Reads a float; truncation is not in play.'],
  ['StringProperty', 'Reads a string, so it has no numeric fallback at all.'],
  ['node_bool_option', 'Reads a boolean, so it has no integer zero to land on.'],
  ['node_double_option', 'Reads a double; truncation is not in play.'],
  ['node_float_option', 'Reads a float; truncation is not in play.'],
  ['node_string_option', 'Reads a string, so it has no numeric fallback at all.'],
  [
    'FloatArrayProperty',
    'Reads a float array off a record and returns an empty vector when absent; there is no scalar fallback.',
  ],
  [
    'MidiByteProperty',
    'Refuses a non-integer outright, so no fractional value can truncate onto its zero. The exemption belongs to the READER rather than to any field, which is why it is recorded here instead of once per call site: a site moving off it onto a truncating reader must start owing a reason.',
  ],
  [
    'node_uint32_option',
    'Carries only the ZeroIsSentinel overload, so no call can spell a literal-zero fallback, and it refuses a fraction besides. Both are properties of the READER, so a site that moves to a plain-fallback uint32 reader starts owing a reason.',
  ],
  [
    'Int32Property',
    'Refuses a non-integer outright, as MidiByteProperty does, so no fractional value can truncate onto its zero.',
  ],
  [
    'NonNegativeSizeTProperty',
    'Refuses a non-integer outright, and takes an out-pointer besides, so no fractional value can truncate onto its zero.',
  ],
  [
    'RequiredIntProperty',
    'Takes an out-pointer instead of a fallback: a missing or wrong-typed value is an error, so there is no zero to land on.',
  ],
  ['RequiredUint32Property', 'Out-pointer rather than a fallback, as RequiredIntProperty.'],
  ['RequiredDoubleProperty', 'Reads a double, and takes an out-pointer rather than a fallback.'],
  ['RequiredFloatProperty', 'Reads a float, and takes an out-pointer rather than a fallback.'],
  ['RequiredStringProperty', 'Reads a string, and takes an out-pointer rather than a fallback.'],
]);

const NO_FLOOR = { tagged: 0, sites: 0 };

/** A tree with one untagged literal-zero read and nothing else. */
const UNTAGGED: AddonSource[] = [
  {
    file: 'fake.cpp',
    text: 'void Read(const Napi::Object& obj) { x = IntProperty(obj, "frameStart", 0); }\n',
  },
];

/** The same read, tagged. */
const TAGGED: AddonSource[] = [
  {
    file: 'fake.cpp',
    text: 'void Read(const Napi::Object& obj) { x = IntProperty(obj, "nFft", kZeroIsSentinel); }\n',
  },
];

/** Both registers, which is what the scan's "tagged or accounted for" reads. */
const ACCOUNTED: ReadonlyMap<string, string> = new Map([
  ...ZERO_FALLBACK_REASONS,
  ...UNTAGGED_SENTINEL_READS,
]);

/**
 * The ratchet. Written out rather than derived from the map, so that adding a
 * sentinel is a conscious edit in two places instead of a quiet append.
 */
const EXPECTED_UNTAGGED_SENTINELS: readonly string[] = [];

describe('a literal-zero fallback is tagged or reasoned', () => {
  it('reports nothing about the addon as it stands', () => {
    expect(evaluateZeroSentinelScope(addonSources(), ACCOUNTED)).toEqual([]);
  });

  it('clears the floor it is sized for, so the sweep swept something', () => {
    const sites = zeroFallbackSites();
    expect(sites.filter((site) => site.tagged).length).toBeGreaterThan(20);
    expect(sites.length).toBeGreaterThan(50);
  });

  it('keeps the recorded reasons free of entries that no longer apply', () => {
    const open = new Set(
      zeroFallbackSites()
        .filter((site) => !site.tagged)
        .map((site) => site.id),
    );
    expect([...ACCOUNTED.keys()].filter((id) => !open.has(id))).toEqual([]);
  });

  it('holds the untagged-sentinel list to exactly the reads already established', () => {
    // A ratchet in both directions. An unlisted entry fails here, so a newly
    // found sentinel cannot be filed away quietly; and the list must shrink to
    // nothing as the tags land, so the frozen list has to be edited to let one
    // go.
    expect(
      [...UNTAGGED_SENTINEL_READS.keys()].sort(),
      'A read newly established to select a library default is not a bookkeeping entry. Add it ' +
        'here AND to EXPECTED_UNTAGGED_SENTINELS, or tag it in C++ and remove it from both.',
    ).toEqual(EXPECTED_UNTAGGED_SENTINELS);
  });

  it('fails an untagged-sentinel entry whose read has since been tagged', () => {
    // The other end of the ratchet: once the C++ tag lands, the entry stops
    // describing anything and has to go, or it keeps asserting a decision about
    // a spelling that no longer exists.
    const untagged = new Set(
      zeroFallbackSites()
        .filter((site) => !site.tagged)
        .map((site) => site.id),
    );
    const settled = [...UNTAGGED_SENTINEL_READS.keys()].filter((id) => !untagged.has(id));
    expect(
      settled,
      'These reads are no longer untagged. Delete their UNTAGGED_SENTINEL_READS and ' +
        'EXPECTED_UNTAGGED_SENTINELS entries — the divergence they describe is over.',
    ).toEqual([]);
  });

  it('covers every integer reader the shared header defines, or says why not', () => {
    const shared = [
      ...new Set(
        readerShapedDefinitions()
          .filter((site) => site.file === SHARED_READER_FILE)
          .map((site) => site.name),
      ),
    ].sort();
    // A floor, because an empty header list would make the check below pass
    // while comparing nothing.
    expect(shared.length).toBeGreaterThanOrEqual(15);
    const unaccounted = shared.filter(
      (name) => !isScannedZeroFallbackReader(name) && !UNSCANNED_SHARED_READERS.has(name),
    );
    expect(
      unaccounted,
      'A new reader in the shared header must be added to ZERO_FALLBACK_READER_NAMES in ' +
        '_addon_sources.ts, or listed in UNSCANNED_SHARED_READERS with the reason a literal-zero ' +
        'fallback on it cannot be reached by truncation.',
    ).toEqual([]);
  });

  it('keeps the unscanned-reader register free of names the header no longer defines', () => {
    const shared = new Set(
      readerShapedDefinitions()
        .filter((site) => site.file === SHARED_READER_FILE)
        .map((site) => site.name),
    );
    expect([...UNSCANNED_SHARED_READERS.keys()].filter((name) => !shared.has(name))).toEqual([]);
  });
});

describe('each zero-fallback failure class fires on its own', () => {
  const only = (findings: { heading: string; lines: string[] }[], fragment: string) => {
    expect(findings.map((finding) => finding.heading)).toHaveLength(1);
    expect(findings[0].heading).toContain(fragment);
    return findings[0].lines;
  };

  it('says nothing about a tree whose only read is tagged', () => {
    expect(evaluateZeroSentinelScope(TAGGED, new Map(), NO_FLOOR)).toEqual([]);
  });

  it('reports an untagged read, and only that', () => {
    const lines = only(
      evaluateZeroSentinelScope(UNTAGGED, new Map(), NO_FLOOR),
      'neither the sentinel tag nor a recorded reason',
    );
    expect(lines).toEqual(['fake.cpp:frameStart (fake.cpp:1 IntProperty)']);
  });

  it('suppresses an untagged read that carries a recorded reason', () => {
    const reasons = new Map([['fake.cpp:frameStart', 'why its zero is a quantity']]);
    expect(evaluateZeroSentinelScope(UNTAGGED, reasons, NO_FLOOR)).toEqual([]);
  });

  it('reports a reason that excuses nothing, and only that', () => {
    // The live read is reasoned so only the dead entry is left to fire.
    const reasons = new Map([
      ['fake.cpp:frameStart', 'why its zero is a quantity'],
      ['gone.cpp:x', 'why'],
    ]);
    const lines = only(
      evaluateZeroSentinelScope(UNTAGGED, reasons, NO_FLOOR),
      'matched no untagged read',
    );
    expect(lines).toEqual(['gone.cpp:x']);
  });

  it('treats a reason on a now-tagged read as expired', () => {
    const lines = only(
      evaluateZeroSentinelScope(TAGGED, new Map([['fake.cpp:nFft', 'why']]), NO_FLOOR),
      'matched no untagged read',
    );
    expect(lines).toEqual(['fake.cpp:nFft']);
  });

  it('reports a shrunken population, and only that', () => {
    const lines = only(
      evaluateZeroSentinelScope(TAGGED, new Map(), { tagged: 2, sites: 0 }),
      'no longer finds the population',
    );
    expect(lines).toEqual(['tagged reads: found 1, floor is 2']);
  });
});

describe('the zero-fallback scanner sees what it claims to', () => {
  const spellings: AddonSource[] = [
    {
      file: 'fake.cpp',
      text: [
        'void A(const Napi::Object& o) { a = node_int_option(o, "a", 0); }',
        'void B(const Napi::Object& o) { b = node_int64_option(o, "b", 0); }',
        'void C(const Napi::Object& o) { c = Int64Property(o, "c", kZeroIsSentinel); }',
        'void D(const Napi::Object& o) { d = Uint32Property(o, "d", 0); }',
        'void E(const Napi::Object& o) { e = node_float_option(o, "e", 0); }',
        'void F(const Napi::Object& o) { f = IntProperty(o, "f", 8); }',
      ].join('\n'),
    },
  ];

  it('finds each integer reader and ignores a float one or a non-zero fallback', () => {
    expect(zeroFallbackSites(spellings).map((site) => `${site.key}:${site.tagged}`)).toEqual([
      'a:false',
      'b:false',
      'c:true',
      'd:false',
    ]);
  });

  it('reads a reader that takes a leading env, and a 0u fallback', () => {
    // Both spellings were invisible to the first version of this scan, and an
    // invisible site owes no reason, so nothing reported the gap.
    const arities: AddonSource[] = [
      {
        file: 'fake.cpp',
        text: [
          'void A(Napi::Env env, const Napi::Object& o) { a = IntProperty(env, o, "a", 0); }',
          'void B(const Napi::Object& o) { b = Uint32Property(o, "b", 0u); }',
        ].join('\n'),
      },
    ];
    expect(zeroFallbackSites(arities).map((site) => site.key)).toEqual(['a', 'b']);
  });

  it('does not read a MidiByteProperty fallback, whose reader refuses a fraction', () => {
    const midi: AddonSource[] = [
      {
        file: 'fake.cpp',
        text: 'void A(Napi::Env env, const Napi::Object& o) { a = MidiByteProperty(env, o, "a", 0); }\n',
      },
    ];
    expect(zeroFallbackSites(midi)).toEqual([]);
  });

  it('does not read a fallback written in a comment', () => {
    expect(
      zeroFallbackSites([{ file: 'fake.cpp', text: '// IntProperty(obj, "x", 0)\n' }]),
    ).toEqual([]);
  });

  it('reads the inner call of a nested fallback, where the zero actually is', () => {
    const nested: AddonSource[] = [
      {
        file: 'fake.cpp',
        text: 'void A(const Napi::Object& o) { a = Uint32Property(o, "outer", Uint32Property(o, "inner", 0)); }\n',
      },
    ];
    expect(zeroFallbackSites(nested).map((site) => site.key)).toEqual(['inner']);
  });
});
