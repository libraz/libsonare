/* What every part of the page reads, and the two helpers that build its DOM.
 *
 * One object rather than module-private variables, because the listening
 * surface, the scopes, the feedback composer and the bank all answer questions
 * about the same take and there is no useful boundary to draw between them.
 * Keeping the bank's rows here too is what stops the listening surface and the
 * bank importing each other to read one field.
 */

'use strict';

export const $ = (id) => document.getElementById(id);

export const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text !== undefined) n.append(text);
  return n;
};

export const state = {
  sets: [],            // [{ id, title, takes, compare, group }]
  setId: null,         // the one being listened to
  base: '',            // URL prefix its audio is under
  compare: true,       // false once a set turns out to hold one version per take
  manifest: null,
  items: [],
  itemIndex: 0,
  versionIndex: 0,     // slot: an index into take.keys, or into blindOrder when blind
  display: [],         // slots in the order they are shown, which is what 1..9 count
  wantKey: '',         // version to re-select when the take or the set changes
  lastByRole: {},      // role -> the slot last chosen in it, for the swap key
  lastOracle: null,    // { role, key } -- the last reference/comparison version selected
  oracleOverride: null, // { role, key } -- a listener's correction to lastOracle, or null
  take: null,          // { id, keys[], buffers{}, rms{}, duration, specs{}, peaks{} }
  ctx: null,
  sources: [],
  gains: {},
  master: null,
  playing: false,
  startedAt: 0,        // ctx.currentTime when the sources started
  startOffset: 0,      // buffer seconds at that moment
  loop: false,
  region: null,        // [a, b] in seconds
  blind: false,
  failed: false,       // a load error is on the title and stays there
  blindOrder: [],      // slot -> real version index
  picks: {},           // itemId -> { slot, key, revealed }
  oldNotes: {},        // notes this browser holds from before the feedback log
  bank: { voices: [], loaded: false, shown: [], cursor: -1 },
};

export const SWITCH_RAMP = 0.006;   // seconds: instant to the ear, long enough not to click
export const SPEC_FFT = 1024;
export const SPEC_HOP = 512;
export const SPEC_FLOOR_DB = -78;

/* Strikes closer together than this are one audible event and share a number,
 * which is the rule `shape/hits.py` groups its rows by. */
export const FUSED_S = 0.035;

//: Source roles, in the order their rows are shown; a manifest that declares
//: none puts every version in one unlabelled row. `comparison` is a capture
//: heard beside the reference, never the target.
export const ROLE_ORDER = ['model', 'reference', 'comparison'];

/* Each role keeps one colour on its row, button, waveform trace, spectrogram
 * ramp and banner. Read from the stylesheet so the canvas cannot drift from
 * the CSS. */
const ROLE_RGB = {};

export function roleRgb(role) {
  const name = ROLE_ORDER.includes(role) ? role : 'other';
  if (!ROLE_RGB[name]) {
    const raw = getComputedStyle(document.documentElement)
      .getPropertyValue(`--role-${name}-rgb`).trim();
    const parts = raw.split(/[\s,]+/).map(Number).filter((n) => Number.isFinite(n));
    ROLE_RGB[name] = parts.length === 3 ? parts : [128, 136, 150];
  }
  return ROLE_RGB[name];
}

export const rgba = (rgb, a) => `rgba(${rgb[0]},${rgb[1]},${rgb[2]},${a})`;

export const sourceOf = (key) =>
  (state.manifest && state.manifest.sources && state.manifest.sources[key]) || {};

export const sourceLabel = (key) => sourceOf(key).label || key;

/// The class that carries a role's colour. Computed rather than interpolated
/// into a class literal, so there is one spelling of `role-model` in the source
/// and the stylesheet is the only other place it appears.
export const roleClass = (role) =>
  `role-${ROLE_ORDER.includes(role) ? role : 'other'}`;

export function roleOf(key) {
  const r = sourceOf(key).role;
  return ROLE_ORDER.includes(r) ? r : '';
}

/* Which signal path a render was taken down, and it is an AXIS rather than a
 * choice: `felt-worn` and `felt-worn` with the bank's rig cleared are one
 * setting heard two ways, not two candidates. Interleaved in one strip they
 * made a list of eighteen out of a question with six answers, and a pick out of
 * it could not be attributed to either question. */
export const pathOf = (key) => (sourceOf(key).path === 'direct' ? 'direct' : '');

/// The block a version belongs to: one question, one block.
export const blockOf = (key) => `${roleOf(key)}|${pathOf(key)}`;

/// Every source a feedback note could be judged against, reference before
/// comparison, in the order the manifest lists them within each role.
export function oracleSources() {
  const sources = (state.manifest && state.manifest.sources) || {};
  return ['reference', 'comparison'].flatMap((role) =>
    Object.keys(sources).filter((key) => sources[key].role === role)
      .map((key) => ({ role, key, label: sourceLabel(key) })));
}

export const notesKey = () => `audition:picks-note:${state.setId || 'untitled'}`;
export const picksKey = () => `audition:picks:${state.setId || 'untitled'}`;
export const SET_KEY = 'audition:set';
export const VIEW_KEY = 'audition:view';
