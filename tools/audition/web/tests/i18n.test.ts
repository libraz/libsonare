import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { TREE } from '../src/feedback/tree';
import {
  assemble,
  currentLang,
  isKey,
  LANGS,
  langName,
  PLURAL_FORMS,
  phrase,
  SOURCE_LANG,
  setLang,
  t,
  tableOf,
} from '../src/i18n';

const SRC = join(__dirname, '..', 'src');

function sources(dir: string): string[] {
  return readdirSync(dir).flatMap((name) => {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) {
      return sources(p);
    }
    return /\.(ts|tsx)$/.test(name) ? [p] : [];
  });
}

/** A key with its plural suffix taken off, and the suffix (or null). */
function split(key: string): [string, string | null] {
  const m = new RegExp(`^(.*)_(${PLURAL_FORMS.join('|')})$`).exec(key);
  return m?.[1] && m[2] ? [m[1], m[2]] : [key, null];
}

/** A table as its keys with plural families folded, each with the forms it spells out. */
function families(table: Record<string, string>): Map<string, Set<string>> {
  const out = new Map<string, Set<string>>();
  for (const key of Object.keys(table)) {
    const [base, form] = split(key);
    const forms = out.get(base) ?? new Set<string>();
    if (form) {
      forms.add(form);
    }
    out.set(base, forms);
  }
  return out;
}

const pluralForms = (lang: string) => new Intl.PluralRules(lang).resolvedOptions().pluralCategories;

describe('the string tables', () => {
  it('are found by file name, so a language is its files', () => {
    const found = assemble({
      './en.json': { a: 'A' },
      './extra/x.en.json': { b: 'B' },
      './fr.json': { a: 'Á' },
      './extra/x.fr.json': { b: 'É' },
    });
    expect(Object.keys(found).sort()).toEqual(['en', 'fr']);
    expect(found.fr).toEqual({ a: 'Á', b: 'É' });
    expect(LANGS[0]).toBe(SOURCE_LANG);
    expect(LANGS.length).toBe(new Set(LANGS).size);
  });

  it('declare the same keys in every language, plural families folded', () => {
    const want = [...families(tableOf(SOURCE_LANG)).keys()].sort();
    for (const lang of LANGS) {
      expect([...families(tableOf(lang)).keys()].sort(), lang).toEqual(want);
    }
  });

  it('spell each counted string in exactly the plural forms its language has', () => {
    const counted = [...families(tableOf(SOURCE_LANG))].filter(([, forms]) => forms.size > 0);
    expect(counted.length).toBeGreaterThan(0);
    for (const lang of LANGS) {
      const have = families(tableOf(lang));
      for (const [base] of counted) {
        expect([...(have.get(base) ?? [])].sort(), `${lang} ${base}`).toEqual(
          [...pluralForms(lang)].sort(),
        );
      }
    }
  });

  it('fill every placeholder with the same names in every language', () => {
    const names = (s: string) => [...s.matchAll(/\{(\w+)\}/g)].map((m) => m[1]).sort();
    const source: Record<string, string> = tableOf(SOURCE_LANG);
    const ofBase = (table: Record<string, string>, base: string) =>
      Object.entries(table).filter(([k]) => split(k)[0] === base);
    for (const base of families(source).keys()) {
      const want = names(ofBase(source, base)[0]?.[1] ?? '');
      for (const lang of LANGS) {
        for (const [key, value] of ofBase(tableOf(lang), base)) {
          expect(names(value), `${lang} ${key}`).toEqual(want);
        }
      }
    }
  });

  it('count through the count variable', () => {
    for (const [key, value] of Object.entries(tableOf(SOURCE_LANG))) {
      if (split(key)[1]) {
        expect(value, key).toContain('{count}');
      }
    }
  });

  it('leave no value empty', () => {
    for (const lang of LANGS) {
      for (const [key, value] of Object.entries(tableOf(lang))) {
        expect(value.trim(), `${lang} ${key}`).not.toBe('');
      }
    }
  });

  it('choose the plural form by the language, not by the code', () => {
    setLang('en');
    expect(t('pick.notes', { count: 1 })).toBe('1 listening note');
    expect(t('pick.notes', { count: 2 })).toBe('2 listening notes');
    setLang('ja');
    expect(t('pick.notes', { count: 1 })).toBe(
      tableOf('ja')['pick.notes_other'].replace('{count}', '1'),
    );
    setLang('en');
  });

  it('read flat keys rather than paths', () => {
    expect(t('role.model')).not.toBe(t('role.model.long'));
    expect(t('role.model')).not.toBe('role.model');
  });

  it('fill placeholders written as {name}', () => {
    expect(t('run.count', { at: 3, of: 188 })).toBe('3 / 188');
  });
});

describe('the language switch', () => {
  it('moves every string and the document language together', () => {
    setLang('ja');
    expect(currentLang()).toBe('ja');
    expect(document.documentElement.lang).toBe('ja');
    expect(t('view.run')).toBe(tableOf('ja')['view.run']);
    setLang('en');
    expect(t('view.run')).toBe(tableOf('en')['view.run']);
  });

  it('names each language in its own words', () => {
    expect(langName('ja')).toBe(tableOf('ja')['lang.self']);
    expect(langName('en')).toBe(tableOf('en')['lang.self']);
  });

  it('resolves per-language data in the library order, falling back to the first table', () => {
    setLang('ja');
    expect(phrase({ en: 'bright', ja: '明るい' })).toBe('明るい');
    expect(phrase({ en: 'bright' })).toBe('bright');
    expect(phrase('as is')).toBe('as is');
    expect(phrase(undefined)).toBe('');
  });

  it('is persisted where the detector looks first', () => {
    setLang('ja');
    expect(localStorage.getItem('audition:lang')).toBe('ja');
  });
});

describe('the narrowing', () => {
  it('words every question and answer in every table', () => {
    for (const [id, node] of Object.entries(TREE.nodes)) {
      expect(isKey(`tree.${id}.q`), id).toBe(true);
      expect(isKey(`tree.${id}.qSolo`), id).toBe(node.solo);
      for (const c of node.a) {
        expect(isKey(`tree.${id}.${c.tag}`), `${id} ${c.tag}`).toBe(true);
      }
    }
  });

  it('leads only to nodes it has, and every path ends', () => {
    for (const [id, node] of Object.entries(TREE.nodes)) {
      expect(node.a.length, id).toBe(2);
      for (const c of node.a) {
        expect(Boolean(c.leaf) !== Boolean(c.to), `${id} ${c.tag}`).toBe(true);
        if (c.to) {
          expect(TREE.nodes[c.to], `${id} -> ${c.to}`).toBeDefined();
        }
      }
    }
    const seen = new Set<string>();
    const walk = (id: string, path: string[]) => {
      expect(path.includes(id), `cycle through ${id}`).toBe(false);
      seen.add(id);
      for (const c of TREE.nodes[id]?.a ?? []) {
        if (c.to) {
          walk(c.to, [...path, id]);
        }
      }
    };
    walk(TREE.start, []);
    expect([...seen].sort()).toEqual(Object.keys(TREE.nodes).sort());
  });
});

describe('the source', () => {
  // A branch on the language code answers a new language with one of the old two.
  it('never chooses by language code', () => {
    const codes = LANGS.map((l) => `['"]${l}['"]`).join('|');
    const branch = new RegExp(
      `[!=]==?\\s*(${codes})|(${codes})\\s*[!=]==?|\\?\\s*(${codes})\\s*:|\\.(${LANGS.join('|')})\\b(?!\\.json)`,
    );
    const offenders = sources(SRC).flatMap((file) =>
      readFileSync(file, 'utf8')
        .split('\n')
        .map((line, i) => ({ line, at: `${file.slice(SRC.length + 1)}:${i + 1}` }))
        .filter(({ line }) => branch.test(line) && !/^\s*(\/\/|\*|\/\*)/.test(line))
        .map(({ at, line }) => `${at}: ${line.trim()}`),
    );
    expect(offenders).toEqual([]);
  });

  it('names only keys the tables carry', () => {
    const missing = sources(SRC).flatMap((file) =>
      [...readFileSync(file, 'utf8').matchAll(/\bt\(\s*'([^'`$]+)'/g)]
        .map((m) => m[1] ?? '')
        .filter((key) => !isKey(key))
        .map((key) => `${file.slice(SRC.length + 1)}: ${key}`),
    );
    expect(missing).toEqual([]);
  });
});
