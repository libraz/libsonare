/**
 * One options field, one answer per surface for a value of the wrong type.
 *
 * `detectOnsets` was the demonstrated case: both surfaces took `threshold` and
 * `delta` off their presence-checked reader, and the two presence-checked
 * readers answer a wrong-typed value differently -- the addon refuses it by
 * name, embind coerces it and computes a different onset set. Both calls look
 * successful from the caller's side, which is why nothing reported it, and
 * parity cannot see it: it compares signatures, defaults and argument order, and
 * those agree.
 *
 * The one field is fixed. THIS FILE IS THE CLASS. It records every field both
 * surfaces read, which reader family each put it in, and -- for the ones that
 * still disagree -- the reader pair that explains the disagreement. A new field
 * put on mismatched readers is a finding rather than a quiet addition.
 *
 * WHAT A GREEN RUN DOES AND DOES NOT MEAN. Green says every cross-surface reader
 * disagreement in the scanned trees is one that was looked at and written down.
 * It does NOT say the surfaces agree -- 175 fields still diverge, in three
 * classes recorded below -- and it says nothing about fields the scan cannot
 * pair, for the reasons `_reader_family_sources.ts` records.
 *
 * Every assertion below names the production edit that breaks it.
 */

import { describe, expect, it } from 'vitest';
import {
  evaluateReaderFamilyScope,
  mismatchedFields,
  normalizeEntryPoint,
  type PairedField,
  pairedFields,
  READER_FAMILIES,
  readSites,
  unclassifiedReaders,
} from './_reader_family_sources.js';

/**
 * Every live cross-surface disagreement, grouped by the reader pair that causes
 * it, with what that pair does written once per class.
 *
 * KEYED ON THE READER PAIR, and that is the one place a reader may appear in a
 * reason. The rule these registers otherwise follow -- argue about the field,
 * never about the reader -- guards against a reason outliving the reader it
 * describes. Here the key IS the reader pair, so moving a field to a different
 * reader drops it out of its class and it must be accounted for again; the
 * per-field member lists are what make that a ratchet rather than a blanket.
 *
 * AN ENTRY EXPIRES WITH ITS DIVERGENCE. Bringing a field into agreement means
 * deleting its line here in the same change, or the register keeps asserting a
 * reviewed decision about a name the next field would inherit.
 *
 * ASK THIS REGISTER BY READER PAIR, NEVER BY FIELD NAME. A subsystem's fields
 * are not all spelled with its name: the reader pairs ending `repairFloatOption`
 * / `repairIntOption` / `repairBoolOption` hold 33 fields, of which 25 are
 * `denoiseconfig:*` / `dereverbconfig:*` / `trimsilenceconfig:*` and carry no
 * trace of the subsystem in their id. Counting by name returns 8 and looks like
 * an answer. The pair is what a migration actually moves, so the pair is what
 * says whether it moved.
 *
 * AN ID IS THE HELPER'S NAME, SO A RENAME ON ONE SURFACE UNPAIRS THE FIELD. The
 * two denoise readers were `read_denoise_config_c` and `readDenoiseConfig`, one
 * suffix apart after normalization, and nine fields left the comparison without
 * a finding -- only the six that had a line here showed up, and as expiry rather
 * than as loss. Moving an options read into a helper means giving it the name
 * the other surface's helper normalizes to.
 */
const ACCOUNTED: ReadonlyMap<string, readonly string[]> = new Map([
  // THE ADDON REFUSES A WRONG TYPE WHERE EMBIND COERCES IT — 132 fields.
  //
  // The addon's presence-checked readers REFUSE a wrong-typed value by name; embind's read the
  // same field through `val::as<T>()`, which COERCES. A numeric string and a one-element array
  // arrive as the number they spell, a boolean as 0 or 1, so the WASM caller gets a result
  // computed from a value it did not write while the addon caller gets a TypeError. Not a
  // per-field decision: it is what the two presence-checked families do, and closing it means
  // giving the embind readers a type test, which is a contract change on the published package.
  [
    'FloatProperty>floatProperty',
    [
      'bindmidiccbinding:maxValue',
      'bindmidiccbinding:minValue',
      'decomposestems:beta',
      'decomposestems:maskPower',
      'estimateroom:aspectHintLh',
      'estimateroom:aspectHintLw',
      'estimateroom:minDecayDb',
      'estimateroom:noiseFloorMarginDb',
      'estimateroom:referenceAbsorption',
      'freezeoffline:gain',
      'notesegments:minNoteMs',
      'notesegments:referenceHz',
      'notesegments:segmentationThresholdCents',
      'notesegments:voicedThreshold',
      'pcen:bias',
      'pcen:eps',
      'pcen:gain',
      'pcen:power',
      'pcen:timeConstant',
      'pitchcorrecttimevarying:maxCorrectionSemitones',
      'pitchcorrecttimevarying:referenceMidi',
      'pitchcorrecttimevarying:retuneAmount',
      'pitchcorrecttimevarying:retuneSpeedMs',
      'pitchcorrecttimevarying:targetMidi',
      'pitchcorrecttimevarying:vibratoThresholdCents',
      'rendernotes:fadeMs',
      'rendernotes:frameRate',
      'rendernotes:vibratoCutoffHz',
      'roommorph:airHumidityPercent',
      'roommorph:airTemperatureC',
      'roommorph:crossfadeMs',
      'setclips:gain',
      'setmetronome:accentGain',
      'setmetronome:beatGain',
      'setsf2instrument:gain',
      'settrackbuses:gainDb',
      'settracklanes:levelDb',
      'synthesizerir:airHumidityPercent',
      'synthesizerir:airTemperatureC',
      'synthesizerir:crossfadeMs',
    ],
  ],
  // Same class as above, one reader along: the addon reads these six through the
  // FINITE float reader, so a non-finite is refused there as well as a wrong
  // type. Embind still coerces, so the divergence and its reason are unchanged.
  [
    'FiniteFloatProperty>floatProperty',
    [
      'roommorph:maxSeconds',
      'roommorph:mixingTimeMs',
      'roommorph:sourceTailSuppression',
      'roommorph:wet',
      'synthesizerir:maxSeconds',
      'synthesizerir:mixingTimeMs',
    ],
  ],
  [
    'IntProperty>intProperty',
    [
      'bounceoffline:blockSize',
      'bounceoffline:dither',
      'bounceoffline:ditherBits',
      'bounceoffline:numChannels',
      'bounceoffline:sourceSampleRate',
      'bounceoffline:targetSampleRate',
      'decomposestems:hopLength',
      'decomposestems:nComponents',
      'decomposestems:nFft',
      'decomposestems:nIter',
      'detectonsets:hopLength',
      'detectonsets:nFft',
      'estimateroom:mode',
      'estimateroom:nOctaveBands',
      'freezeoffline:blockSize',
      'freezeoffline:numChannels',
      'importexternalstems:sampleRate',
      'midirouteevents:filterChannel',
      'midirouteevents:filterGroup',
      'midirouteevents:remapChannel',
      'pcen:hopLength',
      'pcen:sampleRate',
      'pitchcorrecttimevarying:scaleModeMask',
      'pitchcorrecttimevarying:scaleRoot',
      'roommorph:ismOrder',
      'setautomationlane:curveToNext',
      'setgraph:mix',
      'setgraph:numChannels',
      'setgraph:numPorts',
      'setmarkerex:id',
      'setmarkerex:keyFifths',
      'setmarkerex:kind',
      'setmarkers:keyFifths',
      'setmarkers:kind',
      'setmetronome:clickSamples',
      'setsf2instrument:polyphony',
      'settracklanes:sendTiming',
      'synthesizerir:ismOrder',
      'synthesizerir:sampleRate',
    ],
  ],
  [
    'BoolProperty>boolProperty',
    [
      'bounceoffline:normalizeLufs',
      'estimateroom:preferEyring',
      'roommorph:airAbsorptionEnabled',
      'roommorph:preferEyring',
      'setclips:loop',
      'setmarkerex:keyMinor',
      'setmarkers:keyMinor',
      'setmetronome:enabled',
      'settracklanes:enabled',
      'synthesizerir:airAbsorptionEnabled',
      'synthesizerir:preferEyring',
      'tempooptionsfrom:adaptiveTempo',
      'tempooptionsfrom:includeOctaveCandidates',
    ],
  ],
  [
    'Int64Property>int64Property',
    [
      'bounceoffline:totalFrames',
      'freezeoffline:totalFrames',
      'importexternalstems:startFrame',
      'setclips:clipOffsetSamples',
      'setclips:fadeInSamples',
      'setclips:fadeOutSamples',
      'setclips:lengthSamples',
      'setmidiclips:lengthSamples',
      'setmidiclips:loopLengthSamples',
      'setmidiclips:startSample',
    ],
  ],
  [
    'IntProperty>uintProperty',
    [
      'addclip:trackId',
      'addlooprecordingtakes:trackId',
      'annotatechords:quality',
      'annotatechords:rootPc',
      'annotatechords:slashBassPc',
      'annotatekeys:mode',
      'annotatekeys:tonicPc',
    ],
  ],
  [
    'MidiByteProperty>byteProperty',
    [
      'bindmidiccbinding:ccLsbNumber',
      'bindmidiccbinding:kind',
      'bindmidiccbinding:selectorLsb',
      'bindmidiccbinding:selectorMsb',
    ],
  ],
  [
    'Uint32Property>intProperty',
    [
      'setclips:trackId',
      'settrackbuses:channelLayout',
      'settracklanes:outputBusId',
      'settracklanes:sourceChannelLayout',
    ],
  ],
  [
    'Uint32Property>uintProperty',
    ['setmidiclips:destinationId', 'setmidiclips:id', 'setmidiclips:trackId'],
  ],
  ['Int64Property>intProperty', ['bounceoffline:ditherSeed', 'freezeoffline:clipId']],
  ['DoubleProperty>doubleProperty', ['setmidiclips:startPpq', 'settemposegments:endBpm']],
  ['WordProperty>wordProperty', ['setmidievents:data1']],
  ['DoubleProperty>floatProperty', ['tempooptionsfrom:rampThreshold']],

  // THE ADDON REFUSES A WRONG TYPE WHERE EMBIND ANSWERS WITH THE DEFAULT — 33 fields.
  //
  // The repair readers were written to match the addon's options readers when THOSE substituted a
  // default for a wrong-typed value. The addon moved to refusing and these did not follow, so
  // the substitution now mirrors nothing: a wrong-typed repair field silently keeps the config
  // default on WASM and is refused by name on the addon.
  [
    'FloatProperty>repairFloatOption',
    [
      'denoiseconfig:ddAlpha',
      'denoiseconfig:noiseEstimationQuantile',
      'denoiseconfig:overSubtraction',
      'denoiseconfig:reductionDb',
      'denoiseconfig:spectralFloor',
      'dereverbconfig:attenuation',
      'dereverbconfig:lateDelayMs',
      'dereverbconfig:overSubtraction',
      'dereverbconfig:spectralFloor',
      'dereverbconfig:t60Sec',
      'dereverbconfig:threshold',
      'dereverbconfig:wpeStrength',
      'masteringrepairdeclick:neighborRatio',
      'masteringrepairdeclick:residualRatio',
      'masteringrepairdeclick:threshold',
      'masteringrepairdecrackle:threshold',
      'masteringrepairdehum:q',
      'trimsilenceconfig:gateLufs',
      'trimsilenceconfig:threshold',
      'trimsilenceconfig:windowMs',
    ],
  ],
  [
    'IntProperty>repairIntOption',
    [
      'denoiseconfig:hopLength',
      'denoiseconfig:nFft',
      'dereverbconfig:hopLength',
      'dereverbconfig:nFft',
      'dereverbconfig:wpeIterations',
      'dereverbconfig:wpeTaps',
      'masteringrepairdeclick:lpcOrder',
      'masteringrepairdeclick:maxClickSamples',
      'masteringrepairdeclip:lpcOrder',
    ],
  ],
  [
    'BoolProperty>repairBoolOption',
    [
      'denoiseconfig:gainSmoothing',
      'denoiseconfig:speechPresenceGain',
      'dereverbconfig:wpeEnabled',
    ],
  ],
  // Same class again, and the only field whose addon reader is size_t-wide. Both
  // surfaces refuse a NEGATIVE count -- the core field is a size_t, so -1 lands
  // past the validator's SIZE_MAX/2 bound rather than below zero -- and they
  // part company on a wrong TYPE: the addon refuses `'256'` by name, embind's
  // int reader substitutes the default for it.
  ['NonNegativeSizeTProperty>repairIntOption', ['trimsilenceconfig:paddingSamples']],

  // THE ADDON ANSWERS WITH THE DEFAULT WHERE EMBIND COERCES — 10 fields.
  //
  // The addon puts these on its substituting family and embind reads them through a coercing one,
  // so a numeric string is ignored on the addon and applied on WASM -- the divergence that is
  // two different results rather than a result against an error. `detectOnsets`' six frame
  // counts sit here, in the same bag as the two fields that were brought into agreement.
  [
    'node_int_option>onsetWindowFrames',
    [
      'detectonsets:backtrackRange',
      'detectonsets:postAvg',
      'detectonsets:postMax',
      'detectonsets:preAvg',
      'detectonsets:preMax',
      'detectonsets:wait',
    ],
  ],
  [
    'node_float_option>setNumberOption',
    [
      'estimatemeter:downbeatWeight',
      'estimatemeter:measureWeight',
      'estimatemeter:subdivisionWeight',
    ],
  ],
  ['node_int_option>setNumberOption', ['estimatemeter:denominator']],
]);

/** Two surfaces reading one field two ways, the shape the register answers. */
const DISAGREEING: PairedField[] = [
  {
    id: 'fake:gain',
    node: ['refuse'],
    wasm: ['coerce'],
    nodeReaders: ['FloatProperty'],
    wasmReaders: ['floatProperty'],
    readerPair: 'FloatProperty>floatProperty',
  },
];

describe('a cross-surface reader-family disagreement is fixed or recorded', () => {
  it('reports nothing about the scanned trees as they stand', () => {
    // RED WHEN: a field is put on readers of different families on the two
    // surfaces without a line in ACCOUNTED, or a recorded field stops
    // disagreeing and its line is left behind.
    expect(evaluateReaderFamilyScope(pairedFields(), ACCOUNTED)).toEqual([]);
  });

  it('clears the floor it is sized for, so the scan scanned something', () => {
    // RED WHEN: the scan's regexes stop matching, or the entry-point
    // normalization stops joining the two surfaces. Without this a dead scan
    // would report an empty population and read as two surfaces in agreement.
    const paired = pairedFields();
    expect(paired.length).toBeGreaterThan(150);
    expect(new Set(paired.map((field) => field.id.split(':')[0])).size).toBeGreaterThan(30);
  });

  it('sees every reader family on both surfaces', () => {
    // RED WHEN: one surface's reader table stops matching. A scan that found
    // only `coerce` sites on WASM would clear the floor above while being blind
    // to the two families that make a disagreement visible at all.
    const families = (surface: 'node' | 'wasm') =>
      [...new Set(readSites(surface).map((site) => site.family))].sort();
    expect(families('node')).toEqual(['refuse', 'substitute']);
    expect(families('wasm')).toEqual(['coerce', 'refuse', 'substitute']);
  });

  it('demands a family for every name-shaped reader in use', () => {
    // RED WHEN: a `*Property` / `*Option` / `*_option` reader is added and used
    // with a key literal without being classified. This is what keeps the
    // register from silently under-counting as the helper set grows.
    expect(unclassifiedReaders('node')).toEqual([]);
    expect(unclassifiedReaders('wasm')).toEqual([]);
  });

  it('holds detectOnsets threshold and delta in agreement', () => {
    // RED WHEN: either field goes back onto a reader whose family the other
    // surface does not share -- floatProperty on the WASM side, or
    // node_float_option on the addon side. The per-site ratchet for the one
    // field this register was opened by, kept apart from ACCOUNTED because its
    // claim is agreement rather than a recorded divergence.
    const agreed = pairedFields().filter((field) =>
      ['detectonsets:threshold', 'detectonsets:delta'].includes(field.id),
    );
    expect(agreed.map((field) => field.id).sort()).toEqual([
      'detectonsets:delta',
      'detectonsets:threshold',
    ]);
    for (const field of agreed) {
      expect(field.node, field.id).toEqual(['refuse']);
      expect(field.wasm, field.id).toEqual(['refuse']);
    }
  });
});

describe('each failure class fires on its own', () => {
  const only = (findings: { heading: string; lines: string[] }[], fragment: string) => {
    expect(findings.map((finding) => finding.heading)).toHaveLength(1);
    expect(findings[0].heading).toContain(fragment);
    return findings[0].lines;
  };

  it('says nothing about a disagreement that is recorded', () => {
    const reasons = new Map([['FloatProperty>floatProperty', ['fake:gain']]]);
    expect(evaluateReaderFamilyScope(DISAGREEING, reasons, 0)).toEqual([]);
  });

  it('reports an unrecorded disagreement, and only that', () => {
    const lines = only(
      evaluateReaderFamilyScope(DISAGREEING, new Map(), 0),
      'different reader families',
    );
    expect(lines).toEqual([
      'fake:gain — node FloatProperty (refuse) vs wasm floatProperty (coerce)',
    ]);
  });

  it('reports a record that matches no live disagreement, and only that', () => {
    const reasons = new Map([['FloatProperty>floatProperty', ['fake:gain', 'gone:hopLength']]]);
    const lines = only(evaluateReaderFamilyScope(DISAGREEING, reasons, 0), 'no longer live');
    expect(lines).toEqual(['gone:hopLength (recorded under FloatProperty>floatProperty)']);
  });

  it('reports a record filed under the wrong class, and only that', () => {
    // A field moved from one reader to another keeps disagreeing, so the
    // unrecorded check alone would still pass it under its old entry. Filing is
    // part of the identity: the class carries the reason, so a field under the
    // wrong one is excused by an argument that does not describe it.
    const reasons = new Map([['IntProperty>intProperty', ['fake:gain']]]);
    const findings = evaluateReaderFamilyScope(DISAGREEING, reasons, 0);
    expect(findings.map((finding) => finding.heading)).toHaveLength(2);
    expect(findings[1].lines).toEqual(['fake:gain (recorded under IntProperty>intProperty)']);
  });

  it('reports a shrunken population, and only that', () => {
    const reasons = new Map([['FloatProperty>floatProperty', ['fake:gain']]]);
    const lines = only(
      evaluateReaderFamilyScope(DISAGREEING, reasons, 2),
      'population it is sized for',
    );
    expect(lines).toEqual(['paired fields: found 1, floor is 2']);
  });
});

describe('the scanner sees what it claims to', () => {
  it('joins the two surfaces spelling of one entry point', () => {
    expect(normalizeEntryPoint('js_detect_onsets')).toBe('detectonsets');
    expect(normalizeEntryPoint('DetectOnsets')).toBe('detectonsets');
    expect(normalizeEntryPoint('readDereverbConfig')).toBe('dereverbconfig');
    expect(normalizeEntryPoint('ReadDereverbConfig')).toBe('dereverbconfig');
  });

  it('reads one field per surface off the sources rather than off a name', () => {
    const node = readSites('node').filter((site) => site.id === 'detectonsets:delta');
    const wasm = readSites('wasm').filter((site) => site.id === 'detectonsets:delta');
    expect(node.map((site) => [site.reader, site.family])).toEqual([['FloatProperty', 'refuse']]);
    expect(wasm.map((site) => [site.reader, site.family])).toEqual([
      ['typedFloatProperty', 'refuse'],
    ]);
  });

  it('keeps the two spellings of one key in different bags apart', () => {
    // `threshold` is an onset picking level in one bag and a declick detector
    // level in another, deliberately read under different families. Joining on
    // the bare key would call that one field and report a mismatch that is not
    // one, which is why the entry point is half the identity.
    const ids = readSites('wasm')
      .filter((site) => site.key === 'threshold')
      .map((site) => site.id);
    expect(new Set(ids).size).toBeGreaterThan(1);
    expect(ids).toContain('detectonsets:threshold');
  });

  it('classifies a reader by what its body does, not by its suffix', () => {
    // `floatOption` and `floatProperty` are named for the opposite families to
    // the ones they belong to: the Option suffix reads as a fallback and that
    // reader refuses, the Property suffix reads as checked and that one coerces.
    expect(READER_FAMILIES.wasm.floatOption).toBe('refuse');
    expect(READER_FAMILIES.wasm.floatProperty).toBe('coerce');
    expect(READER_FAMILIES.node.node_float_option).toBe('substitute');
    expect(READER_FAMILIES.node.FloatProperty).toBe('refuse');
  });

  it('counts the live divergence classes it is recording', () => {
    // RED WHEN: a class empties out or a new one appears. ACCOUNTED's per-field
    // lists already catch both, but this states the number a reader of this file
    // is being asked to believe.
    const live = new Set(mismatchedFields().map((field) => field.readerPair));
    expect(live.size).toBe(20);
    expect([...live].every((pair) => ACCOUNTED.has(pair))).toBe(true);
  });
});
