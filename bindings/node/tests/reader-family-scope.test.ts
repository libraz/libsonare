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
 * surfaces read and which reader family each put it in, and any field still
 * disagreeing has to be written down in ACCOUNTED with the reader pair that
 * explains it. A new field put on mismatched readers is a finding rather than a
 * quiet addition.
 *
 * WHAT A GREEN RUN DOES AND DOES NOT MEAN. Green says no paired field is read
 * under different families on the two surfaces, that no reader in use on either
 * surface coerces or substitutes, and that every reader classified as refusing
 * carries a type test in its body. It says nothing about fields the scan cannot
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
  readSites,
  unclassifiedReaders,
  ungatedRefusers,
  ungatedTypeGates,
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
 * are not all spelled with its name: a pair's members carry the id of the
 * enclosing helper, so counting by a subsystem's name undercounts. The pair is
 * what a migration actually moves, so the pair is what says whether it moved.
 *
 * AN ID IS THE HELPER'S NAME, SO A RENAME ON ONE SURFACE UNPAIRS THE FIELD. The
 * two denoise readers were `read_denoise_config_c` and `readDenoiseConfig`, one
 * suffix apart after normalization, and nine fields left the comparison without
 * a finding -- only the six that had a line here showed up, and as expiry rather
 * than as loss. Moving an options read into a helper means giving it the name
 * the other surface's helper normalizes to.
 */
const ACCOUNTED: ReadonlyMap<string, readonly string[]> = new Map();

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

  it('finds no coercing or substituting reader in use on either surface', () => {
    // RED WHEN: a field is read through a reader classified `coerce` or
    // `substitute`, on either surface -- including a pair that would agree with
    // each other, which the mismatch check above cannot see.
    const families = (surface: 'node' | 'wasm') =>
      [...new Set(readSites(surface).map((site) => site.family))].sort();
    expect(families('node')).toEqual(['refuse']);
    expect(families('wasm')).toEqual(['refuse']);
  });

  it('holds every refusing reader to a type test in its own body', () => {
    // RED WHEN: a reader stays classified `refuse` after its body stops testing
    // the value's type (the table is hand-written; the body is the fact), or a
    // classified reader no longer exists, or a gate helper stops testing.
    expect(ungatedRefusers('node')).toEqual([]);
    expect(ungatedRefusers('wasm')).toEqual([]);
    expect(ungatedTypeGates('node')).toEqual([]);
    expect(ungatedTypeGates('wasm')).toEqual([]);
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
    // surface does not share -- a coercing reader on the WASM side, or a
    // substituting reader on the addon side. The per-site ratchet for the one
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
    expect(wasm.map((site) => [site.reader, site.family])).toEqual([['floatProperty', 'refuse']]);
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
    // A body that converts without testing is reported however it is filed.
    const coercing = [
      {
        file: 'fake.cpp',
        text: [
          'float floatProperty(val object, const char* key, float fallback) {',
          '  val value = object[key];',
          '  return value.isUndefined() ? fallback : value.as<float>();',
          '}',
        ].join('\n'),
      },
    ];
    expect(ungatedRefusers('wasm', coercing, { floatProperty: 'refuse' })).toEqual([
      'floatProperty: no type test in its body',
    ]);
  });

  it('accepts a body that tests the type directly or through a gate helper', () => {
    const gated = [
      {
        file: 'fake.cpp',
        text: [
          'float floatProperty(val object, const char* key, float fallback) {',
          '  val value = typedPropertyValue(object, key, "number");',
          '  return value.isUndefined() ? fallback : value.as<float>();',
          '}',
          'bool boolProperty(val object, const char* key, bool fallback) {',
          '  val value = object[key];',
          '  if (value.typeOf().as<std::string>() != "boolean") throw 1;',
          '  return value.as<bool>();',
          '}',
          'int windowFrames(val object, const char* key, int fallback) {',
          '  return floatProperty(object, key, fallback);',
          '}',
        ].join('\n'),
      },
    ];
    expect(
      ungatedRefusers('wasm', gated, {
        floatProperty: 'refuse',
        boolProperty: 'refuse',
        windowFrames: 'refuse',
      }),
    ).toEqual([]);
  });

  it('reports a refusing reader with no definition, so a stale entry cannot pass by absence', () => {
    expect(ungatedRefusers('wasm', [], { goneProperty: 'refuse' })).toEqual([
      'goneProperty: no definition found',
    ]);
  });

  it('counts the live divergence classes it is recording', () => {
    // RED WHEN: a divergence class appears. The number a reader of this file is
    // being asked to believe is zero, and ACCOUNTED is empty to match.
    expect(mismatchedFields()).toEqual([]);
    expect(ACCOUNTED.size).toBe(0);
  });
});
