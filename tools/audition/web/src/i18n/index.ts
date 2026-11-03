/* The page's languages, through i18next.
 *
 * Nothing outside the resource files knows which languages exist: detection,
 * persistence and fallback are the library's, a language's own name is a key
 * in its own table, and text that arrives in data per language is resolved
 * through the library's resolution order. Adding a language is a resource file.
 *
 * Placeholders stay `{name}` as the tables have always written them. `t` is
 * typed on the English table's keys, so a key not in it does not compile and
 * a key built from a union (`role.${Role}`) is checked for every member.
 */

import i18next from 'i18next';
import LanguageDetector from 'i18next-browser-languagedetector';
import { initReactI18next, useTranslation } from 'react-i18next';
import type { Phrase } from '../api/schemas';
import baseEn from './en.json';
import bankEn from './extra/bank.en.json';
import bankJa from './extra/bank.ja.json';
import feedbackEn from './extra/feedback.en.json';
import feedbackJa from './extra/feedback.ja.json';
import listenEn from './extra/listen.en.json';
import listenJa from './extra/listen.ja.json';
import scopeEn from './extra/scope.en.json';
import scopeJa from './extra/scope.ja.json';
import baseJa from './ja.json';

/* One table per language, assembled from the shared one and one per area of
 * the page, so each area adds its words without touching another's file. */
const en = { ...baseEn, ...bankEn, ...feedbackEn, ...listenEn, ...scopeEn };
const ja = { ...baseJa, ...bankJa, ...feedbackJa, ...listenJa, ...scopeJa };

export type TKey = keyof typeof en;

/** Every table the page ships, by language code. The first is the fallback. */
export const RESOURCES = { en, ja } satisfies Record<string, Record<TKey, string>>;
export type Lang = keyof typeof RESOURCES;
export const LANGS = Object.keys(RESOURCES) as Lang[];
export const LANG_KEY = 'audition:lang';

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
      resources: Object.fromEntries(LANGS.map((l) => [l, { translation: RESOURCES[l] }])),
      supportedLngs: LANGS,
      fallbackLng: LANGS[0],
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
export const currentLang = (): Lang => (i18next.resolvedLanguage ?? LANGS[0]) as Lang;

export function setLang(next: Lang): void {
  void i18next.changeLanguage(next);
}

/** A language's own name for itself, read from that language's table. */
export const langName = (code: Lang): string => i18next.getFixedT(code)('lang.self');

export type Vars = Record<string, string | number>;

/** The string for a key in the language in force, `{name}` filled from `vars`. */
export function t(key: TKey, vars?: Vars): string {
  return i18next.t(key, vars ?? {}) as string;
}

/** True when a dynamically built string names a key the table has. */
export const isKey = (key: string): key is TKey => Object.hasOwn(en, key);

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
  return { t, phrase, lang: (i18n.resolvedLanguage ?? LANGS[0]) as Lang };
}
