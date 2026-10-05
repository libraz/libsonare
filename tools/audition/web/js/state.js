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
  picks: {},           // itemId -> { slot, key, revealed }, for the selected comparison
  comparisonId: null,  // the manifest comparison being listened to, or null for its default
  setEpoch: 0,         // bumped per set load; an async reply about an older set commits nothing
  wantByComparison: {}, // comparison id -> the version last chosen in it
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

/* Which boundary a model render stops at — `instrument` with the bank's rig
 * cleared, `product` down the shipped path — and it is an AXIS rather than a
 * choice: `felt-worn` at both is one setting heard two ways, not two
 * candidates. Interleaved in one strip they made a list of eighteen out of a
 * question with six answers. References carry none. */
export const scopeOf = (key) => sourceOf(key).scope || '';

/// The block a version belongs to: one question, one block.
export const blockOf = (key) => `${roleOf(key)}|${scopeOf(key)}`;

/* The comparisons the manifest defines: the DI against a direct-input
 * reference, and the product against the GM/GS one. A page written before them
 * has none, and every reader falls back to the single comparison it had. */
export const comparisons = () =>
  ((state.manifest && state.manifest.comparisons) || []).filter((c) => c && c.id);

const STATUS_RANK = { matched: 0, context_only: 1, unverified: 2, unavailable: 3 };

/// The comparison being listened to: the one chosen, else the best-standing
/// one, the product before the instrument where they stand equal.
export function selectedComparison() {
  const all = comparisons();
  const held = all.find((c) => c.id === state.comparisonId);
  if (held || !all.length) return held || null;
  const rank = (c) => (STATUS_RANK[c.status] ?? 4) * 2 + (c.scope === 'product' ? 0 : 1);
  return all.reduce((best, c) => (rank(c) < rank(best) ? c : best));
}

/// The versions one comparison sets against each other: its model side and its oracle.
export const comparisonKeys = (c) =>
  new Set([...(c.model_sources || []), ...(c.oracle_sources || [])]);

/// Every source a feedback note could be judged against, reference before
/// comparison, in the order the manifest lists them within each role. With a
/// take open, only the ones that take holds.
export function oracleSources() {
  const sources = (state.manifest && state.manifest.sources) || {};
  return ['reference', 'comparison'].flatMap((role) =>
    Object.keys(sources)
      .filter((key) => sources[key].role === role && (!state.take || state.take.keys.includes(key)))
      .map((key) => ({ role, key, label: sourceLabel(key) })));
}

/// The manifest's generation digest, or null for a page written before it.
export const setGeneration = () => (state.manifest && state.manifest.set_generation) || null;

export const notesKey = () => `audition:picks-note:${state.setId || 'untitled'}`;
/// Picks are held per comparison and per generation; a page with neither keeps
/// the key it always had.
const picksBase = (c) => `audition:picks:${state.setId || 'untitled'}${c ? `:${c.id}` : ''}`;
export const picksKey = () => {
  const gen = setGeneration();
  return picksBase(selectedComparison()) + (gen ? `@${gen.slice(0, 16)}` : '');
};

/// Picks this browser holds for the same set and comparison under another
/// generation, or under a key written before generations or comparisons. Read
/// to be shown apart; never part of the current tally.
export function earlierPicksKeys() {
  const current = picksKey();
  const c = selectedComparison();
  const base = picksBase(c);
  const out = [];
  for (let i = 0; i < localStorage.length; i++) {
    const key = localStorage.key(i);
    if (key === current) continue;
    if (key === base || key.startsWith(`${base}@`)) out.push(key);
  }
  // The pre-comparison key held the product draw, so only that comparison inherits it.
  const bare = picksBase(null);
  if (c && c.scope === 'product' && localStorage.getItem(bare) !== null) out.push(bare);
  return out;
}
export const SET_KEY = 'audition:set';
export const VIEW_KEY = 'audition:view';
