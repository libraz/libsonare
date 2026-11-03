import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { TREE } from '../src/feedback/tree';
import { currentLang, isKey, LANGS, langName, phrase, RESOURCES, setLang, t } from '../src/i18n';

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

describe('the string tables', () => {
  it('declare the same keys in every language', () => {
    const [first, ...rest] = LANGS;
    const want = Object.keys(RESOURCES[first as keyof typeof RESOURCES]).sort();
    for (const lang of rest) {
      expect(Object.keys(RESOURCES[lang]).sort(), lang).toEqual(want);
    }
  });

  it('fill every placeholder with the same names in every language', () => {
    const names = (s: string) => [...s.matchAll(/\{(\w+)\}/g)].map((m) => m[1]).sort();
    for (const key of Object.keys(RESOURCES.en) as (keyof typeof RESOURCES.en)[]) {
      for (const lang of LANGS) {
        expect(names(RESOURCES[lang][key]), `${lang} ${key}`).toEqual(names(RESOURCES.en[key]));
      }
    }
  });

  it('leave no value empty', () => {
    for (const lang of LANGS) {
      for (const [key, value] of Object.entries(RESOURCES[lang])) {
        expect(value.trim(), `${lang} ${key}`).not.toBe('');
      }
    }
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
    expect(t('view.run')).toBe(RESOURCES.ja['view.run']);
    setLang('en');
    expect(t('view.run')).toBe(RESOURCES.en['view.run']);
  });

  it('names each language in its own words', () => {
    expect(langName('ja')).toBe(RESOURCES.ja['lang.self']);
    expect(langName('en')).toBe(RESOURCES.en['lang.self']);
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
