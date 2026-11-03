import { describe, expect, it } from 'vitest';
import { LANGS, SOURCE_LANG, type SourceKey, setLang, tableOf } from '../../src/i18n';
import { takeSlug, takeText } from '../../src/takes/text';

const takeKeys = (lang: string) =>
  Object.keys(tableOf(lang)).filter((k) => /^take\.(group|label|sub)\./.test(k));

describe('takeText', () => {
  it('carries every converted sentence', () => {
    for (const lang of LANGS) {
      expect(takeKeys(lang).length, lang).toBe(takeKeys(SOURCE_LANG).length);
    }
    expect(takeKeys('en').length).toBeGreaterThan(100);
  });

  it('keeps ids stable: the key is the slug of the English sentence', () => {
    for (const key of takeKeys('en')) {
      const english = tableOf('en')[key as SourceKey];
      expect(key.endsWith(`.${takeSlug(english)}`)).toBe(true);
    }
  });

  it('returns every sentence unchanged in English', () => {
    setLang('en');
    for (const key of takeKeys('en')) {
      const english = tableOf('en')[key as SourceKey];
      expect(takeText(english)).toBe(english);
    }
  });

  it('translates every sentence in Japanese', () => {
    setLang('ja');
    for (const key of takeKeys('ja')) {
      const english = tableOf('en')[key as SourceKey];
      expect(takeText(english)).toBe(tableOf('ja')[key as SourceKey]);
    }
  });

  it('translates known samples of each kind', () => {
    setLang('ja');
    expect(takeText('one note at a time')).toBe('1音ずつ');
    expect(takeText('Single note — held four seconds')).toBe('単音 — 4秒保持');
    expect(takeText('attack, free decay, damper')).toBe('アタック、自由減衰、ダンパー');
  });

  it('reads either dash spelling of a label', () => {
    setLang('ja');
    expect(takeText('Single note - C4, mf')).toBe('単音 — C4、mf');
    expect(takeText('Single note — C4, mf')).toBe('単音 — C4、mf');
    expect(takeText('Dynamics - C4 at five velocities')).toBe(
      takeText('Dynamics — C4 at five velocities'),
    );
  });

  it('puts the octave suffix back in words', () => {
    setLang('ja');
    expect(takeText('struck at once, let ring (+1 octave)')).toBe(
      '同時に打って、鳴らしたまま（1オクターブ上）',
    );
    expect(takeText('struck at once, let ring (-2 octaves)')).toBe(
      '同時に打って、鳴らしたまま（2オクターブ下）',
    );
    setLang('en');
    expect(takeText('struck at once, let ring (+1 octave)')).toBe(
      'struck at once, let ring (+1 octave)',
    );
    expect(takeText('struck at once, let ring (-2 octaves)')).toBe(
      'struck at once, let ring (-2 octaves)',
    );
  });

  it('passes unknown text through, in both languages', () => {
    for (const lang of ['en', 'ja'] as const) {
      setLang(lang);
      expect(takeText('A take nobody translated')).toBe('A take nobody translated');
      expect(takeText('unknown sentence (+1 octave)')).toBe('unknown sentence (+1 octave)');
    }
  });

  it('answers empty for nothing', () => {
    expect(takeText(undefined)).toBe('');
    expect(takeText('')).toBe('');
  });
});
