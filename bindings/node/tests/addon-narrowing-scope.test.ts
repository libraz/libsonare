/**
 * Every integer conversion of a caller's number lives in the shared header, and
 * every entry point can catch what refusing one throws.
 *
 * `Int32Value()`, `Uint32Value()` and `Int64Value()` are the ECMAScript modular
 * conversions, so `2**32` arrives as 0 — what most of these fields read as
 * "keep the default" — and `2**32 + 1` as 1. A wrapped value is always inside
 * the target type, so no guard downstream, in the C ABI or in the core, can
 * tell it from a setting the caller chose. Keeping the conversion in one
 * range-checked place is the only thing that closes it, and a site that spells
 * the accessor out again silently leaves that place.
 *
 * The second half is the precondition for the first. A reader refuses by
 * THROWING: the alternative, a pending JS exception plus a fallback, lets the
 * entry point keep running, and every N-API allocation after a pending
 * exception returns null for the result builders to memcpy into. So an entry
 * point without the catch harness turns a refusal into an uncaught exception
 * and takes the process down instead of reporting a RangeError.
 *
 * Every case drives `evaluateNarrowingScope` — the function the assertion below
 * calls — on synthetic sources, because a rule asserted through a
 * re-implementation of its scanner would only ever agree with itself. Each
 * class is reverted on its own and must produce exactly one finding, so a green
 * run means every class still fires for its own reason rather than one loud
 * class covering for the rest.
 */

import { describe, expect, it } from 'vitest';
import {
  type AddonSource,
  addonSources,
  entryPointGuards,
  evaluateNarrowingScope,
  integerNarrowingSites,
  isSharedNarrowingReader,
  SHARED_READER_FILE,
} from './_addon_sources.js';

/**
 * Sites whose conversion stays spelled out, each with the mechanism that makes
 * it harmless. An entry expires with its divergence.
 */
const NARROWING_ALLOWLIST: ReadonlyMap<string, string> = new Map([]);

const NO_FLOOR = { sources: 0, entryPoints: 0 };

/** A tree with one stray narrowing and nothing else wrong. */
const STRAY_NARROWING: AddonSource[] = [
  {
    file: 'fake.cpp',
    text: [
      'Napi::Value Fn(const Napi::CallbackInfo& info) {',
      '  Napi::Env env = info.Env();',
      '  SONARE_NODE_TRY',
      '  return Napi::Number::New(env, info[0].As<Napi::Number>().Int32Value());',
      '  SONARE_NODE_CATCH(env)',
      '}',
      'void Init(Napi::Env env, Napi::Object exports) {',
      '  exports.Set("fn", Napi::Function::New(env, &Fn));',
      '}',
    ].join('\n'),
  },
];

/** The same tree with the narrowing gone and the harness removed instead. */
const UNGUARDED_ENTRY_POINT: AddonSource[] = [
  {
    file: 'fake.cpp',
    text: [
      'Napi::Value Fn(const Napi::CallbackInfo& info) {',
      '  return info.Env().Undefined();',
      '}',
      'void Init(Napi::Env env, Napi::Object exports) {',
      '  exports.Set("fn", Napi::Function::New(env, &Fn));',
      '}',
    ].join('\n'),
  },
];

/**
 * The shared header's own shape: one narrowing inside the family, one beside it.
 *
 * Both sit in the file the rule used to trust by name, so the second is exactly
 * what that trust swallowed whole — and the header really has grown four
 * hand-written predicates next to the family since it was written. A run that
 * reports the second and not the first is the measurement that the trust now
 * belongs to the symbol rather than to the address.
 */
const SHARED_HEADER: AddonSource[] = [
  {
    file: SHARED_READER_FILE,
    text: [
      'inline int node_narrow_int(Napi::Env env, const Napi::Value& value, const char* name) {',
      '  return value.As<Napi::Number>().Int32Value();',
      '}',
      'inline uint8_t MidiByteProperty(Napi::Env env, const Napi::Object& obj, const char* key) {',
      '  return obj.Get(key).As<Napi::Number>().Uint32Value();',
      '}',
    ].join('\n'),
  },
];

/** A tree with neither defect, so any finding on it is a false positive. */
const CLEAN: AddonSource[] = [
  {
    file: 'fake.cpp',
    text: [
      'Napi::Value Fn(const Napi::CallbackInfo& info) {',
      '  Napi::Env env = info.Env();',
      '  SONARE_NODE_TRY',
      '  return Napi::Number::New(env, node_arg_int(info, 0, 0));',
      '  // info[0].As<Napi::Number>().Int32Value() described, not written',
      '  SONARE_NODE_CATCH(env)',
      '}',
      'void Init(Napi::Env env, Napi::Object exports) {',
      '  exports.Set("fn", Napi::Function::New(env, &Fn));',
      '}',
    ].join('\n'),
  },
];

describe('caller integers are narrowed in one place', () => {
  it('reports nothing about the addon as it stands', () => {
    expect(evaluateNarrowingScope(addonSources(), NARROWING_ALLOWLIST)).toEqual([]);
  });

  it('clears the floor it is sized for, so the sweep swept something', () => {
    expect(addonSources().length).toBeGreaterThan(40);
    expect(entryPointGuards().length).toBeGreaterThan(400);
    // No floor on the narrowing population itself, and that is the point: the
    // whole addon narrows through node_narrow_number now, which reads a double,
    // so the modular accessors appear nowhere and the population is legitimately
    // zero. A floor cannot distinguish that from a scanner that stopped
    // matching, so the scanner's power is pinned on fixtures below instead.
    expect(integerNarrowingSites()).toEqual([]);
  });

  it('keeps every narrowing inside the shared narrowing family', () => {
    const outside = integerNarrowingSites().filter((site) => !isSharedNarrowingReader(site.owner));
    expect(
      outside.map(
        (site) => `${site.file}:${site.line} ${site.receiver}.${site.accessor}() in ${site.owner}`,
      ),
      'Read the value through the node_narrow_* family, or record the site in ' +
        'NARROWING_ALLOWLIST with the mechanism that makes it harmless.',
    ).toEqual([]);
  });
});

describe('each narrowing-scope failure class fires on its own', () => {
  const only = (findings: { heading: string; lines: string[] }[], fragment: string) => {
    expect(findings.map((finding) => finding.heading)).toHaveLength(1);
    expect(findings[0].heading).toContain(fragment);
    return findings[0].lines;
  };

  it('says nothing about a tree with neither defect', () => {
    expect(evaluateNarrowingScope(CLEAN, new Map(), NO_FLOOR)).toEqual([]);
  });

  it('reports a stray narrowing, and only that', () => {
    const lines = only(
      evaluateNarrowingScope(STRAY_NARROWING, new Map(), NO_FLOOR),
      'outside the shared narrowing family',
    );
    expect(lines).toEqual(['fake.cpp:4 info[0].Int32Value() in Fn']);
  });

  it('accepts a narrowing the shared family performs', () => {
    const inside = integerNarrowingSites(SHARED_HEADER).filter((site) =>
      isSharedNarrowingReader(site.owner),
    );
    expect(inside.map((site) => site.owner)).toEqual(['node_narrow_int']);
  });

  it('reports a narrowing written beside the family in the family own file', () => {
    const lines = only(
      evaluateNarrowingScope(SHARED_HEADER, new Map(), NO_FLOOR),
      'outside the shared narrowing family',
    );
    // The file is the one the rule used to trust outright, so this line existing
    // at all is the change: the address no longer launders the conversion.
    expect(lines).toEqual([
      `${SHARED_READER_FILE}:5 obj.Get(key).Uint32Value() in MidiByteProperty`,
    ]);
  });

  it('reports an entry point with nowhere to catch, and only that', () => {
    const lines = only(
      evaluateNarrowingScope(UNGUARDED_ENTRY_POINT, new Map(), NO_FLOOR),
      'nowhere to catch',
    );
    expect(lines).toEqual(['fake.cpp Fn (fn)']);
  });

  it('reports a stale allowlist entry, and only that', () => {
    const lines = only(
      evaluateNarrowingScope(CLEAN, new Map([['gone.cpp:x:Int32Value', 'why']]), NO_FLOOR),
      'matched nothing',
    );
    expect(lines).toEqual(['gone.cpp:x:Int32Value']);
  });

  it('reports a shrunken population, and only that', () => {
    const lines = only(
      evaluateNarrowingScope(CLEAN, new Map(), { sources: 2, entryPoints: 0 }),
      'no longer finds the population',
    );
    expect(lines).toEqual(['sources: found 1, floor is 2']);
  });

  it('suppresses a stray narrowing that carries a recorded mechanism', () => {
    const allowlist = new Map([['fake.cpp:info[0]:Int32Value', 'why it is harmless']]);
    expect(evaluateNarrowingScope(STRAY_NARROWING, allowlist, NO_FLOOR)).toEqual([]);
  });
});

/**
 * A site's owner is the function whose BODY encloses it, not the definition that
 * happens to sit above it.
 *
 * Both spellings below broke an owner taken from the nearest preceding
 * definition, and they break it in opposite directions: one credits a site to a
 * function that had already closed, the other credits it to a control-flow
 * keyword. The first is the one that matters — a site credited to the shared
 * family is accepted in silence, which is worse than the filename rule this
 * replaced, because a filename cannot misattribute a site to a function.
 */
describe('a narrowing is owned by the body that encloses it', () => {
  /** A narrowing at FILE SCOPE, written after the shared reader has closed. */
  const AFTER_A_CLOSED_BODY: AddonSource[] = [
    {
      file: SHARED_READER_FILE,
      text: [
        'inline int node_narrow_int(Napi::Env env, const Napi::Value& v, const char* n) {',
        '  return 0;',
        '}',
        'const int kSneak = Fallback().As<Napi::Number>().Int32Value();',
      ].join('\n'),
    },
  ];

  /** A narrowing the shared reader performs inside one of its own branches. */
  const INSIDE_A_BRANCH: AddonSource[] = [
    {
      file: SHARED_READER_FILE,
      text: [
        'inline int node_narrow_int(Napi::Env env, const Napi::Value& v, const char* n) {',
        '  if (v.IsNumber()) {',
        '    return v.As<Napi::Number>().Int32Value();',
        '  }',
        '  return 0;',
        '}',
      ].join('\n'),
    },
  ];

  it('does not credit a file-scope narrowing to the definition above it', () => {
    expect(integerNarrowingSites(AFTER_A_CLOSED_BODY)[0].owner).toBe('');
  });

  it('reports that file-scope narrowing rather than accepting it', () => {
    const findings = evaluateNarrowingScope(AFTER_A_CLOSED_BODY, new Map(), NO_FLOOR);
    expect(findings).toHaveLength(1);
    expect(findings[0].lines).toEqual([
      `${SHARED_READER_FILE}:4 Fallback().Int32Value() in (file scope)`,
    ]);
  });

  it('credits a branch inside the shared reader to the reader, not to the branch', () => {
    expect(integerNarrowingSites(INSIDE_A_BRANCH)[0].owner).toBe('node_narrow_int');
  });

  it('accepts that branch, so a guarded reader is not reported against itself', () => {
    expect(evaluateNarrowingScope(INSIDE_A_BRANCH, new Map(), NO_FLOOR)).toEqual([]);
  });
});

describe('the narrowing scanner sees what it claims to', () => {
  const widths: AddonSource[] = [
    {
      file: 'fake.cpp',
      text: [
        'int A(const Napi::Value& v) { return v.As<Napi::Number>().Int32Value(); }',
        'uint32_t B(const Napi::Value& v) { return v.As<Napi::Number>().Uint32Value(); }',
        'int64_t C(const Napi::Value& v) { return v.As<Napi::Number>().Int64Value(); }',
        'float D(const Napi::Value& v) { return v.As<Napi::Number>().FloatValue(); }',
      ].join('\n'),
    },
  ];

  it('finds each modular width and ignores the accessors that cannot wrap', () => {
    expect(integerNarrowingSites(widths).map((site) => site.accessor)).toEqual([
      'Int32Value',
      'Uint32Value',
      'Int64Value',
    ]);
  });

  it('does not read a narrowing written in a comment', () => {
    expect(
      integerNarrowingSites([{ file: 'fake.cpp', text: '// v.As<Napi::Number>().Int32Value()\n' }]),
    ).toEqual([]);
  });

  it('keys a site on its receiver, so an allowlist entry survives a line move', () => {
    expect(integerNarrowingSites(STRAY_NARROWING)[0].id).toBe('fake.cpp:info[0]:Int32Value');
  });

  it('attributes a narrowing to the function it is written inside', () => {
    expect(integerNarrowingSites(SHARED_HEADER).map((site) => site.owner)).toEqual([
      'node_narrow_int',
      'MidiByteProperty',
    ]);
  });

  it('gives a narrowing above every definition no owner, so nothing launders it', () => {
    const fileScope: AddonSource[] = [
      {
        file: SHARED_READER_FILE,
        text: 'const int kD = Fallback().As<Napi::Number>().Int32Value();\n',
      },
    ];
    expect(integerNarrowingSites(fileScope)[0].owner).toBe('');
    const findings = evaluateNarrowingScope(fileScope, new Map(), NO_FLOOR);
    expect(findings).toHaveLength(1);
    expect(findings[0].lines).toEqual([
      `${SHARED_READER_FILE}:1 Fallback().Int32Value() in (file scope)`,
    ]);
  });
});
