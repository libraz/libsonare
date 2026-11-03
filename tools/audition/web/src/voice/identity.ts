/* What a voice IS, in the facts a listener asks first: where it sits in the GM/GS map, how it is
 * synthesised, what it is aimed at, and whether the render being played is the voice that ships.
 *
 * Pure functions over a voice and explicit arguments, shared by the listening surface's strip and
 * the bank's rows and inspector so one fact is never worded three ways. Each returns data (text,
 * a data-attribute value, a tooltip); `chips.tsx` draws them.
 */

import type { Ships } from '../api/schemas';
import { ShipsView, type Target } from '../bank/schema';
import { isKey, t } from '../i18n';

/** The part of a voice that places it on the map; both a bank row and a manifest voice have it. */
export interface SlotRef {
  program?: number | null;
  bank?: number | null;
  kit?: boolean | null;
  patch?: string | null;
}

export type SlotKind = 'gm' | 'gs' | 'gm2' | 'kit';
export type Method = 'physical' | 'fm' | 'classic';

/** A table word where the key exists, else the data's own spelling; never a raw key. */
export function word(key: string, fallback: string): string {
  return isKey(key) ? t(key) : fallback;
}

/* Sixteen engines, three methods: "physical model, FM, or waveform" is all the eye is asked. */
const METHODS: Record<string, Method> = { fm: 'fm', subtractive: 'classic', additive: 'classic' };

export const methodOf = (engine: string | null | undefined): Method =>
  engine ? (METHODS[engine] ?? 'physical') : 'classic';

/** GS variation banks are Bank Select MSB numbers; GM2 addresses of the same patches are LSB 1-3. */
export function slotKind(v: SlotRef | null | undefined): SlotKind | '' {
  if (!v) {
    return '';
  }
  if (v.kit) {
    return 'kit';
  }
  if (!v.bank) {
    return 'gm';
  }
  return v.bank < 8 ? 'gm2' : 'gs';
}

/** The other address of the same patch: a verdict on either is a verdict on both. */
export function aliasOf<V extends SlotRef>(v: SlotRef | null | undefined, voices: V[]): V | null {
  if (!v || v.kit || !v.bank || !v.patch) {
    return null;
  }
  return (
    voices.find(
      (o) =>
        (o as SlotRef) !== v &&
        !o.kit &&
        o.program === v.program &&
        o.bank &&
        o.patch === v.patch &&
        slotKind(o) !== slotKind(v),
    ) ?? null
  );
}

export const addressOf = (v: SlotRef): string =>
  v.kit ? `kit ${v.program ?? 0}` : `${v.program ?? 0}${v.bank ? `:${v.bank}` : ''}`;

export function slotLong(v: SlotRef | null | undefined, voices: SlotRef[]): string {
  const kind = slotKind(v);
  if (!v || !kind) {
    return '';
  }
  const alias = aliasOf(v, voices);
  return t(`slot.${kind}.long`, {
    gm: (v.program ?? 0) + 1,
    bank: v.bank || 0,
    alias: alias ? addressOf(alias) : '—',
  });
}

export interface Tag {
  text: string;
  /** The value of the chip's data attribute. */
  key: string;
  title: string;
}

/** A small tag naming which part of the map a slot belongs to. */
export function slotTag(v: SlotRef | null | undefined, voices: SlotRef[], short = false): Tag {
  const kind = slotKind(v);
  return {
    text: kind ? t(short ? `slot.short.${kind}` : `slot.${kind}`) : '—',
    key: kind || 'none',
    title: slotLong(v, voices),
  };
}

/** The synthesis method in words, the engine as the identifier beside it. */
export function methodTag(engine: string | null | undefined, short = false): Tag {
  const m = methodOf(engine);
  return {
    text: t(short ? `method.short.${m}` : `method.${m}`),
    key: m,
    title: t(`method.${m}.long`, { engine: engine || t('bank.notReported') }),
  };
}

/** The policy branch in words: why a slot takes the module, which decides whether it can change. */
export const targetWords = (target: Target | null | undefined): string =>
  target?.branch ? word(`tgt.${target.branch}`, target.branch) : '';

export const targetShort = (target: Target | null | undefined): string =>
  target?.timbre ? word(`tgt.short.${target.timbre}`, target.timbre) : '';

/** How the captured reference stands against the aim, as one word; null with no state. */
export function targetState(target: Target | null | undefined): Tag | null {
  const s = target?.state;
  if (!s) {
    return null;
  }
  return { text: word(`tst.${s}`, s), key: s, title: word(`tst.${s}.long`, '') };
}

/** A set's ship record read with its detail; null when there is none or it is malformed. */
export function readShips(raw: unknown): ShipsView | null {
  if (!raw) {
    return null;
  }
  const got = ShipsView.safeParse(raw);
  return got.success ? got.data : null;
}

/** The reasons and every bump since the render, one per line, for a tooltip. */
export function shipReasons(ships: ShipsView | null): string {
  if (!ships) {
    return t('ship.none.long');
  }
  const lines = [...ships.reasons];
  for (const b of ships.since) {
    lines.push(`${b.unit} v${b.version}  ${b.date}  ${b.note}`);
  }
  return lines.join('\n') || word(`ship.${ships.state}.long`, ships.state);
}

/** One chip saying whether the page plays today's voice. */
export function shipTag(raw: Ships | ShipsView | null | undefined, short = false): Tag {
  const ships = readShips(raw);
  const st = ships?.state || 'none';
  return {
    text: word(short ? `ship.short.${st}` : `ship.${st}`, st),
    key: st,
    title: shipReasons(ships),
  };
}

/** The command that renders this voice again, so a stale page names its fix. */
export function rerenderCommand(v: SlotRef | null | undefined): string {
  if (!v) {
    return '';
  }
  const pick = v.kit
    ? `--kits ${v.program ?? 0}`
    : `--program ${v.program ?? 0} --banks ${v.bank || 0}`;
  return `tools/voicematch/make_audition.py ${pick}`;
}

/** One decimal place in the language in force: stage and rung values. */
export function oneDecimal(n: number, lang: string): string {
  return new Intl.NumberFormat(lang, { minimumFractionDigits: 1, maximumFractionDigits: 1 }).format(
    n,
  );
}
