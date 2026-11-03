/* The page's languages, through i18next.
 *
 * Nothing in code knows which languages exist: a table is a JSON file named
 * `<area>.<lang>.json` (or `<lang>.json`), picked up by the build, so adding a
 * language is adding its files. Detection, persistence, fallback and plural
 * choice are the library's, and a language's own name is a key in its table.
 *
 * Placeholders stay `{name}`. `t` is typed on the source language's keys, with
 * a plural family (`x_one`, `x_other`) typed as its base `x`; the tests hold
 * every other table to the same keys and to its own language's plural forms.
 */

import i18next from 'i18next';
import LanguageDetector from 'i18next-browser-languagedetector';
import { initReactI18next, useTranslation } from 'react-i18next';
import type { Phrase } from '../api/schemas';

/** The language the tables are written from: it types `t` and is the fallback. */
export const SOURCE_LANG = 'en';

type SourceTable = typeof import('./en.json') &
  typeof import('./extra/bank.en.json') &
  typeof import('./extra/feedback.en.json') &
  typeof import('./extra/listen.en.json') &
  typeof import('./extra/scope.en.json');

/** CLDR plural categories, the suffixes i18next resolves a counted key through. */
export const PLURAL_FORMS = ['zero', 'one', 'two', 'few', 'many', 'other'] as const;
type Base<K> = K extends `${infer B}_${(typeof PLURAL_FORMS)[number]}` ? B : K;

/** Every key the source table holds, plural forms spelled out. */
export type SourceKey = keyof SourceTable & string;
export type TKey = Base<SourceKey>;
/** A language's table: the source's keys, plural forms as that language needs them. */
export type Table = Record<SourceKey, string>;
export type Lang = string;

/** The language a table file belongs to: the last dotted part of its name. */
export const langOfPath = (path: string): string | undefined => /([^/.]+)\.json$/.exec(path)?.[1];

/** Merge table files into one table per language; a language is whatever the files name. */
export function assemble(files: Record<string, Record<string, string>>): Record<Lang, Table> {
  const out: Record<Lang, Record<string, string>> = {};
  for (const [path, table] of Object.entries(files).sort(([a], [b]) => a.localeCompare(b))) {
    const lang = langOfPath(path);
    if (lang) {
      out[lang] = { ...out[lang], ...table };
    }
  }
  // The tests hold every table to the source's keys; the type states that contract.
  return out as Record<Lang, Table>;
}

/** Every table the page ships, by language code. */
export const RESOURCES = assemble(
  import.meta.glob<Record<string, string>>(['./*.json', './extra/*.json'], {
    eager: true,
    import: 'default',
  }),
);

/** The shipped languages, the source first and the rest in code order. */
export const LANGS: Lang[] = [
  SOURCE_LANG,
  ...Object.keys(RESOURCES)
    .filter((l) => l !== SOURCE_LANG)
    .sort(),
];
export const LANG_KEY = 'audition:lang';

/** One language's table; asking for a language that ships none is a bug. */
export function tableOf(lang: Lang): Table {
  const table = RESOURCES[lang];
  if (!table) {
    throw new Error(`no string table for ${lang}`);
  }
  return table;
}

function reflect(lang: string): void {
  document.documentElement.lang = lang;
  document.title = t('app.title');
}

/** Start i18next once; a later call only moves the language. */
export function initI18n(lng?: Lang): void {
  if (i18next.isInitialized) {
    if (lng) {
      void i18next.changeLanguage(lng);
    }
    return;
  }
  i18next.on('languageChanged', reflect);
  void i18next
    .use(LanguageDetector)
    .use(initReactI18next)
    .init({
      resources: Object.fromEntries(LANGS.map((l) => [l, { translation: tableOf(l) }])),
      supportedLngs: LANGS,
      fallbackLng: SOURCE_LANG,
      load: 'languageOnly',
      nonExplicitSupportedLngs: true,
      ...(lng ? { lng } : {}),
      detection: {
        order: ['localStorage', 'navigator'],
        lookupLocalStorage: LANG_KEY,
        caches: ['localStorage'],
      },
      // The tables are flat: `role.model` and `role.model.long` are two keys, not a path.
      keySeparator: false,
      nsSeparator: false,
      interpolation: { prefix: '{', suffix: '}', escapeValue: false },
      returnNull: false,
      initAsync: false,
    });
  reflect(currentLang());
}

/** The language in force, as one of the shipped tables. */
export const currentLang = (): Lang => i18next.resolvedLanguage ?? SOURCE_LANG;

export function setLang(next: Lang): void {
  void i18next.changeLanguage(next);
}

/** A language's own name for itself, read from that language's table. */
export const langName = (code: Lang): string => i18next.getFixedT(code)('lang.self');

export type Vars = Record<string, string | number>;

/** The string for a key in the language in force, `{name}` filled from `vars`. */
export function t(key: TKey, vars?: Vars): string {
  return i18next.t(key as string, vars ?? {}) as string;
}

/** True when a dynamically built string names a key (or a plural family) the source table has. */
export function isKey(key: string): key is TKey {
  const source: Record<string, string> = tableOf(SOURCE_LANG);
  return (
    Object.hasOwn(source, key) || PLURAL_FORMS.some((f) => Object.hasOwn(source, `${key}_${f}`))
  );
}

/** Text carried in data per language, resolved in the library's own order (active, then fallbacks). */
export function phrase(obj: Phrase | undefined | null): string {
  if (!obj) {
    return '';
  }
  if (typeof obj === 'string') {
    return obj;
  }
  const byLang = obj as Record<string, string | undefined>;
  for (const code of i18next.languages ?? []) {
    const text = byLang[code];
    if (text) {
      return text;
    }
  }
  return '';
}

/** `t` and `phrase` for a component, re-rendered when the language changes. */
export function useT(): { t: typeof t; phrase: typeof phrase; lang: Lang } {
  const { i18n } = useTranslation();
  return { t, phrase, lang: i18n.resolvedLanguage ?? SOURCE_LANG };
}
