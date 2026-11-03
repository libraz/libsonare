/* A take's label, note and group heading, resolved through the resource tables.
 *
 * The words are baked into every manifest in English, so the key is derived from
 * the sentence itself (lower-cased, runs of non-alphanumerics as one hyphen) and
 * the id is the address into `take.<kind>.*`. A sentence with no entry is shown
 * as it arrived. */

import { isKey, t } from '../i18n';

const KINDS = ['group', 'label', 'sub'] as const;

/** A musical take is transposed by whole octaves and says so at the end of its note. */
const OCTAVE = /^(.*) \(([-+]\d+) octaves?\)$/;

/** The stable id of a sentence: what its resource key ends with. */
export const takeSlug = (text: string): string =>
  text
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-|-$/g, '');

function lookup(text: string): string | null {
  const slug = takeSlug(text);
  for (const kind of KINDS) {
    const key = `take.${kind}.${slug}`;
    if (isKey(key)) {
      return t(key);
    }
  }
  return null;
}

function withOctave(text: string): string | null {
  const m = OCTAVE.exec(text);
  const base = m?.[1] ? lookup(m[1]) : null;
  if (base === null || !m?.[2]) {
    return null;
  }
  const shift = Number(m[2]);
  const key = `take.octave.${shift > 0 ? 'up' : 'down'}.${Math.abs(shift) === 1 ? 'one' : 'many'}`;
  return isKey(key) ? t(key, { base, n: Math.abs(shift) }) : null;
}

/** The text for one of the strings a manifest carries in the language in force, or the string itself. */
export function takeText(text: string | null | undefined): string {
  if (!text) {
    return '';
  }
  return lookup(text) ?? withOctave(text) ?? text;
}
