import { describe, expect, it } from 'vitest';
import { RESOURCES, setLang } from '../../src/i18n';
import {
  addressOf,
  aliasOf,
  methodOf,
  methodTag,
  oneDecimal,
  readShips,
  rerenderCommand,
  shipReasons,
  shipTag,
  slotKind,
  slotLong,
  slotTag,
  targetShort,
  targetState,
  targetWords,
  word,
} from '../../src/voice/identity';
import { bank } from './fixtures';

const voices = bank();
const byslug = (slug: string) => voices.find((v) => v.slug === slug) ?? voices[0];

describe('methodOf', () => {
  it('maps engines to the three methods', () => {
    expect(methodOf('fm')).toBe('fm');
    expect(methodOf('subtractive')).toBe('classic');
    expect(methodOf('additive')).toBe('classic');
    expect(methodOf('modal')).toBe('physical');
  });

  it('reads a missing engine as a waveform voice', () => {
    expect(methodOf(undefined)).toBe('classic');
    expect(methodOf(null)).toBe('classic');
    expect(methodOf('')).toBe('classic');
  });
});

describe('slotKind and addressOf', () => {
  it('tells a kit by its flag, never by a number', () => {
    expect(slotKind({ kit: true, program: 0, bank: 9 })).toBe('kit');
  });

  it('splits banks into GM, GM2 (LSB 1-7) and GS (MSB 8+)', () => {
    expect(slotKind({ program: 4, bank: 0 })).toBe('gm');
    expect(slotKind({ program: 4 })).toBe('gm');
    expect(slotKind({ program: 4, bank: 1 })).toBe('gm2');
    expect(slotKind({ program: 4, bank: 7 })).toBe('gm2');
    expect(slotKind({ program: 4, bank: 8 })).toBe('gs');
    expect(slotKind(null)).toBe('');
  });

  it('writes the address as program[:bank] or kit n', () => {
    expect(addressOf({ program: 5, bank: 0 })).toBe('5');
    expect(addressOf({ program: 5, bank: 16 })).toBe('5:16');
    expect(addressOf({ program: 9, kit: true })).toBe('kit 9');
    expect(addressOf({})).toBe('0');
  });
});

describe('aliasOf', () => {
  it('finds the other address of the same patch', () => {
    expect(aliasOf(byslug('p002b8'), voices)?.slug).toBe('p002b1');
    expect(aliasOf(byslug('p002b1'), voices)?.slug).toBe('p002b8');
  });

  it('has none for a capital tone, a kit, or a voice without a patch', () => {
    expect(aliasOf(byslug('p002'), voices)).toBeNull();
    expect(aliasOf(byslug('k000'), voices)).toBeNull();
    expect(aliasOf({ program: 2, bank: 8, patch: null }, voices)).toBeNull();
    expect(aliasOf(null, voices)).toBeNull();
  });

  it('does not match a voice of another program with the same patch name', () => {
    const other = [...voices, { program: 9, bank: 1, patch: 'rubber', slug: 'x9' }];
    expect(aliasOf(byslug('p002b8'), other)?.slug).toBe('p002b1');
  });
});

describe('slotLong and slotTag', () => {
  it('states the GM number counting from one', () => {
    expect(slotLong(byslug('p000'), voices)).toBe(
      'GM 1: a GM Level 1 sound, the GS capital tone at bank 0',
    );
  });

  it('names the alias for a GM2 address', () => {
    expect(slotLong(byslug('p002b1'), voices)).toContain('GS 2:8');
    expect(slotLong(byslug('p002b1'), voices)).toContain('Bank Select LSB 1');
  });

  it('shows a dash for an alias that does not exist', () => {
    expect(slotLong({ program: 2, bank: 2, patch: 'solo' }, voices)).toContain(
      'same patch as GS —',
    );
  });

  it('is empty for no voice', () => {
    expect(slotLong(null, voices)).toBe('');
  });

  it('tags with the long or the short word and carries the kind', () => {
    expect(slotTag(byslug('p002b8'), voices)).toMatchObject({ text: 'GS variation', key: 'gs' });
    expect(slotTag(byslug('p002b8'), voices, true)).toMatchObject({ text: 'GS', key: 'gs' });
    expect(slotTag(byslug('k000'), voices, true)).toMatchObject({ text: 'kit', key: 'kit' });
    expect(slotTag(null, voices)).toMatchObject({ text: '—', key: 'none' });
  });
});

describe('methodTag', () => {
  it('names the method and carries the engine in the tooltip', () => {
    const tag = methodTag('fm');
    expect(tag).toMatchObject({ text: 'FM', key: 'fm' });
    expect(tag.title).toContain('engine fm');
  });

  it('uses the short word, and says the engine is not reported when it is missing', () => {
    expect(methodTag('modal', true).text).toBe('physical');
    const none = methodTag(undefined, true);
    expect(none).toMatchObject({ text: 'wave', key: 'classic' });
    expect(none.title).toContain('not reported');
  });
});

describe('target words', () => {
  it('words the policy branch and the timbre', () => {
    expect(targetWords({ branch: 'kits' })).toBe('the module’s kit');
    expect(targetShort({ timbre: 'machine' })).toBe('module');
    expect(targetWords(null)).toBe('');
    expect(targetShort({})).toBe('');
  });

  it('falls back to the data spelling for a branch the table lacks, never a raw key', () => {
    expect(targetWords({ branch: 'brand_new' })).toBe('brand_new');
  });

  it('returns the state as a chip, or null without one', () => {
    expect(targetState({ state: 'off-target' })).toEqual({
      text: 'off target',
      key: 'off-target',
      title: RESOURCES.en['tst.off-target.long'],
    });
    expect(targetState({})).toBeNull();
    expect(targetState(null)).toBeNull();
    expect(targetState({ state: 'novel' })?.text).toBe('novel');
  });
});

describe('ships', () => {
  const stale = {
    state: 'stale',
    reasons: ['brass moved'],
    since: [{ unit: 'brass', version: 3, date: '2026-10-01', note: 'lip retune' }],
  };

  it('reads detail and refuses nothing it cannot parse into a crash', () => {
    expect(readShips(stale)?.since).toHaveLength(1);
    expect(readShips(null)).toBeNull();
    expect(readShips({ state: 5 })).toBeNull();
  });

  it('lists reasons then every bump, one per line', () => {
    expect(shipReasons(readShips(stale))).toBe('brass moved\nbrass v3  2026-10-01  lip retune');
  });

  it('explains the state when there is nothing to list', () => {
    expect(shipReasons(readShips({ state: 'current' }))).toBe(RESOURCES.en['ship.current.long']);
    expect(shipReasons(null)).toBe(RESOURCES.en['ship.none.long']);
  });

  it('builds the chip long or short, and none for no record', () => {
    expect(shipTag({ state: 'stale' })).toMatchObject({
      text: 'older than what ships',
      key: 'stale',
    });
    expect(shipTag({ state: 'stale' }, true).text).toBe('old');
    expect(shipTag(null)).toMatchObject({ text: 'no record', key: 'none' });
    expect(shipTag({ state: 'mystery' }).text).toBe('mystery');
  });
});

describe('rerenderCommand', () => {
  it('names a program and bank, or a kit', () => {
    expect(rerenderCommand({ program: 40, bank: 8 })).toBe(
      'tools/voicematch/make_audition.py --program 40 --banks 8',
    );
    expect(rerenderCommand({ program: 40 })).toContain('--banks 0');
    expect(rerenderCommand({ program: 3, kit: true })).toBe(
      'tools/voicematch/make_audition.py --kits 3',
    );
    expect(rerenderCommand(null)).toBe('');
  });
});

describe('words in both languages', () => {
  it('never answers with a raw key', () => {
    for (const lang of ['en', 'ja'] as const) {
      setLang(lang);
      for (const k of ['gm', 'gs', 'gm2', 'kit'] as const) {
        expect(
          slotTag({ kit: k === 'kit', bank: { gm: 0, gm2: 1, gs: 8, kit: 0 }[k] }, [], true).text,
        ).not.toMatch(/slot\./);
      }
      for (const e of ['fm', 'modal', undefined]) {
        expect(methodTag(e).text).not.toMatch(/method\./);
      }
      expect(word('nope.key', 'plain')).toBe('plain');
    }
  });

  it('writes numbers through Intl in the language in force', () => {
    expect(oneDecimal(0.6, 'en')).toBe('0.6');
    expect(oneDecimal(1, 'ja')).toBe('1.0');
  });
});
