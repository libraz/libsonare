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

  it('keeps every narrowing in the shared header', () => {
    const outside = integerNarrowingSites().filter((site) => site.file !== SHARED_READER_FILE);
    expect(
      outside.map((site) => `${site.file}:${site.line} ${site.receiver}.${site.accessor}()`),
      `Read the value through the node_narrow_* family in ${SHARED_READER_FILE}, or record the ` +
        'site in NARROWING_ALLOWLIST with the mechanism that makes it harmless.',
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
      'outside the shared header',
    );
    expect(lines).toEqual(['fake.cpp:4 info[0].Int32Value()']);
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
});
