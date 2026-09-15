/**
 * The matcher reads every registration the addon actually writes.
 *
 * Every guard over the addon sources — the narrowing sweep, the catch-harness
 * check, the positional-reader table, the options-key coverage register — is
 * built on one matcher that recognises registrations by their SHAPE. That makes
 * a shape it cannot read a single point of failure: the entry point behind it is
 * absent from all of them at once, and each reports a clean sweep of a
 * population one smaller than it thinks. Nothing in those guards can notice,
 * because they agree about the population by construction.
 *
 * So this file counts the registration SPELLINGS by their bare tokens and
 * reconciles that against what the matcher reads. The census is deliberately not
 * built on the matcher: a check sharing its derivation with what it checks can
 * only agree with itself.
 *
 * The absences are asserted for the same reason and are the other half. An
 * accounting identity says a number moved; it cannot say which form appeared, and
 * a form that changes no count at all — an accessor pair, say — would not move
 * one. Each spelling is named instead, with what it does to the matcher rather
 * than with whether the tree uses it today.
 *
 * Every case drives `evaluateRegistrationCensus` — the function the assertions
 * call — on synthetic sources, and each class is reverted on its own, so a green
 * run means each class still fires for its own reason rather than one loud class
 * covering for the rest.
 */

import { describe, expect, it } from 'vitest';
import {
  type AddonSource,
  entryPointGuards,
  evaluateRegistrationCensus,
  registrationCensus,
  unrecognisedRegistrationSpellings,
} from './_addon_sources.js';

const NO_FLOOR = { registrations: 0 };

/** One free function and nothing the matcher cannot read. */
const CLEAN: AddonSource[] = [
  {
    file: 'fake.cpp',
    text: [
      'Napi::Value Fn(const Napi::CallbackInfo& info) {',
      '  Napi::Env env = info.Env();',
      '  SONARE_NODE_TRY',
      '  return env.Undefined();',
      '  SONARE_NODE_CATCH(env)',
      '}',
      'void Init(Napi::Env env, Napi::Object exports) {',
      '  exports.Set("fn", Napi::Function::New(env, &Fn));',
      '}',
    ].join('\n'),
  },
];

/** The same tree with one more line inside `Init`. */
const withInInit = (line: string): AddonSource[] => [
  { file: 'fake.cpp', text: CLEAN[0].text.replace(/\n}$/, `\n${line}\n}`) },
];

/** An accessor pair: an entry point that moves none of the census counts. */
const ACCESSOR = withInInit('  InstanceAccessor("gain", &FakeWrap::GetGain, &FakeWrap::SetGain);');

/** A method registered by argument rather than by template parameter. */
const ARGUMENT_METHOD = withInInit('  InstanceMethod("fn", &FakeWrap::Fn);');

/** An export whose name is built rather than spelled as a literal. */
const BUILT_NAME = withInInit(
  '  exports.Set(Napi::String::New(env, "built"), Napi::Function::New(env, &Fn));',
);

/** A registration whose JS name is a constant, so the matcher's literal misses it. */
const CONSTANT_NAME = withInInit('  InstanceMethod<&FakeWrap::Fn>(kName),');

/** An export that is a plain value, so it is neither function nor class. */
const VALUE_EXPORT = withInInit('  exports.Set("version", Napi::Number::New(env, 1));');

/** An ObjectWrap class with a constructor, one method, and both bodies present. */
const WRAPPED_CLASS: AddonSource[] = [
  {
    file: 'fake.cpp',
    text: [
      'Napi::Object FakeWrap::Init(Napi::Env env, Napi::Object exports) {',
      '  Napi::Function func = DefineClass(env, "Fake", {',
      '    InstanceMethod<&FakeWrap::Fn>("fn"),',
      '  });',
      '  exports.Set("Fake", func);',
      '  return exports;',
      '}',
      'FakeWrap::FakeWrap(const Napi::CallbackInfo& info) {',
      '  SONARE_NODE_TRY',
      '  SONARE_NODE_CATCH(info.Env())',
      '}',
      'Napi::Value FakeWrap::Fn(const Napi::CallbackInfo& info) {',
      '  SONARE_NODE_TRY',
      '  SONARE_NODE_CATCH(info.Env())',
      '}',
    ].join('\n'),
  },
];

/** The same class with the method's DEFINITION gone, the registration left behind. */
const REGISTERED_WITHOUT_BODY: AddonSource[] = [
  {
    file: 'fake.cpp',
    text: WRAPPED_CLASS[0].text.split('Napi::Value FakeWrap::Fn')[0].trimEnd(),
  },
];

describe('every registration the addon writes is one the matcher reads', () => {
  it('reports nothing about the addon as it stands', () => {
    expect(evaluateRegistrationCensus()).toEqual([]);
  });

  it('clears the floor it is sized for, so the reconciliation reconciled something', () => {
    const census = registrationCensus();
    expect(entryPointGuards().length).toBeGreaterThan(400);
    expect(census.methods).toBeGreaterThan(0);
    expect(census.functionNews).toBeGreaterThan(0);
    expect(census.defineClasses).toBeGreaterThan(0);
  });

  it('accounts for every exports.Set as a function registration or a class export', () => {
    const census = registrationCensus();
    expect(census.exportSets).toBe(census.functionNews + census.defineClasses);
  });

  it('reads as many registrations as there are spellings to read', () => {
    const census = registrationCensus();
    expect(census.methods + census.functionNews + census.defineClasses).toBe(
      entryPointGuards().length,
    );
  });

  it('finds no registration written in a form it cannot read', () => {
    expect(
      unrecognisedRegistrationSpellings().map(
        (site) => `${site.file}:${site.line} ${site.spelling}`,
      ),
      'This form registers an entry point the matcher cannot see, which drops it out of every ' +
        'guard built on the matcher at once. Teach the matcher the spelling rather than ' +
        'excusing the site.',
    ).toEqual([]);
  });
});

describe('each census failure class fires on its own', () => {
  const only = (findings: { heading: string; lines: string[] }[], fragment: string) => {
    expect(findings.map((finding) => finding.heading)).toHaveLength(1);
    expect(findings[0].heading).toContain(fragment);
    return findings[0].lines;
  };

  it('says nothing about a tree with none of the defects', () => {
    expect(evaluateRegistrationCensus(CLEAN, NO_FLOOR)).toEqual([]);
  });

  it('reports a shrunken population, and only that', () => {
    const lines = only(
      evaluateRegistrationCensus(CLEAN, { registrations: 2 }),
      'no longer finds the population',
    );
    expect(lines).toEqual(['registrations: found 1, floor is 2']);
  });

  it('reports an accessor pair, and only that, since it moves no count', () => {
    const lines = only(evaluateRegistrationCensus(ACCESSOR, NO_FLOOR), 'cannot read');
    expect(lines).toHaveLength(1);
    expect(lines[0]).toContain('InstanceAccessor / StaticAccessor');
  });

  it('reports an exports.Set that is neither a function nor a class, and only that', () => {
    const lines = only(evaluateRegistrationCensus(VALUE_EXPORT, NO_FLOOR), 'neither a function');
    expect(lines).toEqual(['exports.Set(: 2', 'Napi::Function::New: 1', 'DefineClass(: 0']);
  });

  it('reports a registration whose name is a constant as a count the matcher lost', () => {
    const lines = only(evaluateRegistrationCensus(CONSTANT_NAME, NO_FLOOR), 'disagree');
    expect(lines).toContain('counted 2, matcher reads 1');
  });

  it('reports a registration whose definition is missing, and only that', () => {
    // The matcher skips a registration whose body it cannot locate, so it drops
    // out of the guard population in silence and only the count sees it.
    const lines = only(evaluateRegistrationCensus(REGISTERED_WITHOUT_BODY, NO_FLOOR), 'disagree');
    expect(lines).toContain('counted 2, matcher reads 1');
  });

  it('reports a method registered by argument, by both the count and the spelling', () => {
    const findings = evaluateRegistrationCensus(ARGUMENT_METHOD, NO_FLOOR);
    const headings = findings.map((finding) => finding.heading);
    expect(headings).toHaveLength(2);
    expect(headings.some((heading) => heading.includes('disagree'))).toBe(true);
    const named = findings.find((finding) => finding.heading.includes('cannot read'));
    expect(named?.lines.join('\n')).toContain('InstanceMethod("name", &Class::Method)');
  });

  it('reports a built export name, by both the count and the spelling', () => {
    const findings = evaluateRegistrationCensus(BUILT_NAME, NO_FLOOR);
    const headings = findings.map((finding) => finding.heading);
    expect(headings).toHaveLength(2);
    expect(headings.some((heading) => heading.includes('disagree'))).toBe(true);
    const named = findings.find((finding) => finding.heading.includes('cannot read'));
    expect(named?.lines.join('\n')).toContain('exports.Set(Napi::String::New(env, "name"), ...)');
  });
});

describe('the census sees what it claims to', () => {
  it('counts each spelling in the tree it is given', () => {
    expect(registrationCensus(CLEAN)).toEqual({
      methods: 0,
      functionNews: 1,
      exportSets: 1,
      defineClasses: 0,
    });
  });

  it('does not count a spelling written in a comment', () => {
    const commented: AddonSource[] = [
      {
        file: 'fake.cpp',
        text: [
          '// exports.Set("fn", Napi::Function::New(env, &Fn)); described, not written',
          '/* InstanceAccessor("gain", &W::G, &W::S); described, not written */',
        ].join('\n'),
      },
    ];
    expect(registrationCensus(commented)).toEqual({
      methods: 0,
      functionNews: 0,
      exportSets: 0,
      defineClasses: 0,
    });
    expect(unrecognisedRegistrationSpellings(commented)).toEqual([]);
  });

  it('counts a template method registration and the class export beside it', () => {
    expect(registrationCensus(WRAPPED_CLASS)).toEqual({
      methods: 1,
      functionNews: 0,
      exportSets: 1,
      defineClasses: 1,
    });
    expect(evaluateRegistrationCensus(WRAPPED_CLASS, NO_FLOOR)).toEqual([]);
  });
});
