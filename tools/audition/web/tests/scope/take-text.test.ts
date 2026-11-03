import { describe, expect, it } from 'vitest';
import { RESOURCES, setLang } from '../../src/i18n';
import { takeSlug, takeText } from '../../src/takes/text';

const takeKeys = (lang: 'en' | 'ja') =>
  Object.keys(RESOURCES[lang]).filter((k) => /^take\.(group|label|sub)\./.test(k));

describe('takeText', () => {
  it('carries every converted sentence', () => {
    expect(takeKeys('en').length).toBe(takeKeys('ja').length);
    expect(takeKeys('en').length).toBeGreaterThan(100);
  });

  it('keeps ids stable: the key is the slug of the English sentence', () => {
    for (const key of takeKeys('en')) {
      const english = RESOURCES.en[key as keyof typeof RESOURCES.en];
      expect(key.endsWith(`.${takeSlug(english)}`)).toBe(true);
    }
  });

  it('returns every sentence unchanged in English', () => {
    setLang('en');
    for (const key of takeKeys('en')) {
      const english = RESOURCES.en[key as keyof typeof RESOURCES.en];
      expect(takeText(english)).toBe(english);
    }
  });

  it('translates every sentence in Japanese', () => {
    setLang('ja');
    for (const key of takeKeys('ja')) {
      const english = RESOURCES.en[key as keyof typeof RESOURCES.en];
      expect(takeText(english)).toBe(RESOURCES.ja[key as keyof typeof RESOURCES.ja]);
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

  it('keeps placeholders identical between languages', () => {
    const vars = (s: string) => (s.match(/\{\w+\}/g) ?? []).sort().join();
    for (const key of Object.keys(RESOURCES.en).filter((k) => /^(take|path)\./.test(k))) {
      const k = key as keyof typeof RESOURCES.en;
      expect(vars(RESOURCES.ja[k])).toBe(vars(RESOURCES.en[k]));
    }
  });
});
