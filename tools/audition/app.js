/* audition — compare several renders of the same take from one transport.
 *
 * Every version of the selected take is decoded up front and played
 * simultaneously; only the gains differ, so a switch can be a gain change on a
 * sound that never stopped. That keeps the two versions sample-aligned, which
 * is the only way to compare a sustain or a decay.
 *
 * It is not what you want for an attack, though: switch four seconds into a
 * phrase and the new version's onset is already behind you, and a take is far
 * too long to wait out — the ear loses what it was holding. So "switch
 * restarts" (on by default) makes a switch seek back to the start instead, and
 * the sample-aligned form stays one keystroke away.
 *
 * WHAT IS SOUNDING IS ADDRESSABLE. The URL fragment is `#<set>/<take>/<version>`
 * and it is rewritten on every move, so the page can be pointed at one exact
 * render and the address bar always names the one being heard. A listening
 * report is worthless if the two people cannot be sure they heard the same
 * file, and with several sets of the same instrument open — each holding a
 * reference, an unmodified build, and a handful of candidate settings — being
 * sure is not something either of them can do from memory.
 *
 * The waveform and the spectrogram are drawn once into offscreen canvases and
 * blitted each frame with only the playhead on top. Recomputing either one per
 * frame is what turns a listening tool into a slideshow.
 */

'use strict';

const $ = (id) => document.getElementById(id);

const state = {
  sets: [],            // [{ id, title, takes, compare }]
  setId: null,         // the one being listened to
  base: '',            // URL prefix its audio is under
  compare: true,       // false once a set turns out to hold one version per take
  manifest: null,
  items: [],
  itemIndex: 0,
  versionIndex: 0,     // slot: an index into take.keys, or into blindOrder when blind
  display: [],         // slots in the order they are shown, which is what 1..9 count
  wantKey: '',         // version to re-select when the take or the set changes
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
  notes: {},
};

const SWITCH_RAMP = 0.006;   // seconds: instant to the ear, long enough not to click
const SPEC_FFT = 1024;
const SPEC_HOP = 512;
const SPEC_FLOOR_DB = -78;

//: Source roles, in the order their rows are shown. A manifest that declares
//: none puts every version in one unlabelled row, which is what a hand-made
//: directory gets and is the layout this page had before roles existed.
const ROLE_ORDER = ['model', 'reference'];

const layers = { wave: { sig: '', cv: null }, spec: { sig: '', cv: null } };

/* ------------------------------------------------------------------ audio */

function audio() {
  if (!state.ctx) {
    state.ctx = new (window.AudioContext || window.webkitAudioContext)();
    state.master = state.ctx.createGain();
    state.master.gain.value = 1;
    state.master.connect(state.ctx.destination);
  }
  return state.ctx;
}

async function loadTake(item) {
  const ctx = audio();
  const keys = Object.keys(item.tracks);
  const buffers = {};
  const rms = {};
  await Promise.all(keys.map(async (k) => {
    const url = state.base + item.tracks[k];
    const res = await fetch(url);
    if (!res.ok) throw new Error(`${url}: ${res.status}`);
    const buf = await ctx.decodeAudioData(await res.arrayBuffer());
    buffers[k] = buf;
    rms[k] = bufferRms(buf);
  }));
  const duration = Math.max(...keys.map((k) => buffers[k].duration));
  return { id: item.id, keys, buffers, rms, duration, specs: {}, peaks: {} };
}

function bufferRms(buf) {
  let sum = 0, n = 0;
  for (let c = 0; c < buf.numberOfChannels; c++) {
    const d = buf.getChannelData(c);
    // Every 7th sample: this estimate only has to be good enough to match two
    // renders in level, and reading every sample of every take would make
    // selecting one feel slow for a tenth of a decibel nobody hears.
    for (let i = 0; i < d.length; i += 7) { sum += d[i] * d[i]; n++; }
  }
  return Math.sqrt(sum / Math.max(n, 1));
}

function targetGain(key) {
  if (!$('matchRms').checked) return 1;
  const vals = state.take.keys.map((k) => state.take.rms[k]).filter((v) => v > 0);
  if (!vals.length) return 1;
  const ref = vals.reduce((a, b) => a + b, 0) / vals.length;
  const r = state.take.rms[key];
  return r > 0 ? Math.min(ref / r, 8) : 1;
}

function activeKey() {
  const idx = state.blind ? state.blindOrder[state.versionIndex] : state.versionIndex;
  return state.take.keys[idx];
}

function applyGains(immediate) {
  const ctx = audio();
  const now = ctx.currentTime;
  const active = activeKey();
  for (const k of state.take.keys) {
    const g = state.gains[k];
    if (!g) continue;
    const want = k === active ? targetGain(k) : 0;
    g.gain.cancelScheduledValues(now);
    if (immediate) {
      g.gain.setValueAtTime(want, now);
    } else {
      g.gain.setValueAtTime(g.gain.value, now);
      g.gain.linearRampToValueAtTime(want, now + SWITCH_RAMP);
    }
  }
  renderLevels();
}

function stopSources() {
  for (const s of state.sources) { try { s.stop(); } catch { /* already ended */ } }
  state.sources = [];
}

function startAt(offset) {
  const ctx = audio();
  stopSources();
  state.gains = {};
  const [a, b] = state.region || [0, state.take.duration];
  const looping = state.loop && b - a > 0.02;
  const from = looping ? Math.min(Math.max(offset, a), b - 0.001) : Math.max(0, offset);

  for (const k of state.take.keys) {
    const src = ctx.createBufferSource();
    src.buffer = state.take.buffers[k];
    const g = ctx.createGain();
    g.gain.value = 0;
    src.connect(g).connect(state.master);
    if (looping) { src.loop = true; src.loopStart = a; src.loopEnd = b; }
    src.start(0, from);
    state.sources.push(src);
    state.gains[k] = g;
  }
  state.startedAt = ctx.currentTime;
  state.startOffset = from;
  state.playing = true;
  applyGains(true);
  $('playBtn').textContent = 'pause';
  $('playBtn').setAttribute('aria-pressed', 'true');
}

function pause() {
  const at = playhead();
  stopSources();
  state.startOffset = at;
  state.playing = false;
  $('playBtn').textContent = 'play';
  $('playBtn').removeAttribute('aria-pressed');
}

function playhead() {
  if (!state.take) return 0;
  if (!state.playing) return state.startOffset;
  const t = state.startOffset + (audio().currentTime - state.startedAt);
  const [a, b] = state.region || [0, state.take.duration];
  if (state.loop && b - a > 0.02) return a + ((t - a) % (b - a));
  return Math.min(t, state.take.duration);
}

function togglePlay() {
  if (!state.take) return;
  audio().resume();
  if (state.playing) pause();
  else startAt(state.startOffset >= state.take.duration - 0.01 ? 0 : state.startOffset);
}

/* ----------------------------------------------------------------- route */

/* `#<set>/<take>/<version>`, with `?set=&take=&v=` accepted as well so a link
 * can be built by anything that finds a query string easier to write. The
 * fragment is authoritative and is what gets written back, since it costs no
 * request and leaves the page's own state the only thing that has to agree.
 *
 * The version is left off in blind mode: the whole point of that mode is that
 * the name is not visible, and an address bar is visible. */

function readRoute() {
  const q = new URLSearchParams(location.search);
  // The playhead rides in the query rather than in the fragment. It is a
  // starting position rather than live state, and writing it back the way the
  // fragment is written back would put a number in the address bar that changes
  // sixty times a second.
  const at = Number.parseFloat(q.get('t'));
  const t = Number.isFinite(at) ? at : null;
  const frag = location.hash.replace(/^#\/?/, '');
  if (frag) {
    const [set, take, ver] = frag.split('/');
    return {
      set: decodeURIComponent(set || ''),
      take: decodeURIComponent(take || ''),
      ver: decodeURIComponent(ver || ''),
      t,
    };
  }
  return { set: q.get('set') || '', take: q.get('take') || '', ver: q.get('v') || '', t };
}

function routeHash() {
  const parts = [state.setId];
  const item = state.items[state.itemIndex];
  if (item) parts.push(item.id);
  if (item && state.take && !state.blind) parts.push(activeKey());
  return '#' + parts.filter(Boolean).map(encodeURIComponent).join('/');
}

/// Rewritten rather than pushed: every arrow key is a move, and a hundred of
/// them in the back stack makes the browser's own back button useless.
function writeRoute() {
  const hash = routeHash();
  if (location.hash !== hash) history.replaceState(null, '', hash);
  renderIdent();
}

async function applyRoute(r) {
  if (r.set && r.set !== state.setId && state.sets.some((s) => s.id === r.set)) {
    await loadSet(r.set, r);
    return;
  }
  if (r.ver) state.wantKey = r.ver;
  const i = r.take ? state.items.findIndex((it) => it.id === r.take) : -1;
  if (i >= 0 && i !== state.itemIndex) { await selectTake(i); return; }
  if (r.ver) selectVersionByKey(r.ver);
}

/* ------------------------------------------------------------------- data */

const notesKey = () => `audition:notes:${state.setId || 'untitled'}`;
const picksKey = () => `audition:picks:${state.setId || 'untitled'}`;

const SET_KEY = 'audition:set';
const VIEW_KEY = 'audition:view';

function fail(msg) {
  // Held, because the bank view clears the title otherwise: the one failure a
  // fresh clone hits leaves it on the bank, which is the view worth being in.
  state.failed = true;
  $('title').textContent = msg;
  $('crumbs').replaceChildren();
  $('identPath').textContent = '';
}

async function boot() {
  state.sets = await (await fetch('sets.json')).json();
  wireBank();
  // Loaded up front rather than on the first switch to the bank: the listening
  // surface's own header line reads from it too.
  await loadBank();
  if (!state.sets.length) {
    fail('No renders found. Generate a set with tools/voicematch/make_audition.py '
       + '— --model-only needs no plugin — then reload.');
    // A fresh clone has no renders and that is the state the bank view is most
    // worth being in: it says which voices exist and which need an oracle, none
    // of which requires anything to have been rendered.
    await setView('bank');
    return;
  }
  buildSetPicker();
  wire();
  // An address wins over what was last listened to: a link is sent precisely
  // because the two ends are otherwise not looking at the same thing.
  const route = readRoute();
  const remembered = localStorage.getItem(SET_KEY);
  const start = state.sets.some((s) => s.id === route.set) ? route.set
    : state.sets.some((s) => s.id === remembered) ? remembered
      : state.sets[0].id;
  await loadSet(start, route);
  if (route.t !== null && state.take) {
    state.startOffset = Math.max(0, Math.min(route.t, state.take.duration));
  }
  // An address names a render and is therefore a request to listen to it; with
  // none, the view is whichever one the last session was left on.
  if (!route.set && localStorage.getItem(VIEW_KEY) === 'bank') await setView('bank');
}

/* The take's schedule as numbered strikes, which is what a listening note has
 * to be able to name. Strikes closer together than this are one audible event
 * and share a number, which is the rule `shape/hits.py` groups its rows by. The
 * number is not what carries the meaning: every readout beside it names the
 * note and the time it was struck, so the two ends agree even where a grouping
 * would not. */
const FUSED_S = 0.035;

function takeHits() {
  const item = state.items[state.itemIndex];
  const notes = item && item.meta && item.meta.notes;
  if (!notes || !notes.length) return [];
  const out = [];
  for (const n of [...notes].sort((a, b) => a.start - b.start || a.note - b.note)) {
    const last = out[out.length - 1];
    if (last && n.start - last.start <= FUSED_S) last.notes.push(n);
    else out.push({ n: out.length + 1, start: n.start, notes: [n] });
  }
  return out;
}

/// The strike the playhead is inside, or null before the first one.
function hitAt(t) {
  let cur = null;
  for (const hit of takeHits()) if (hit.start <= t + 0.005) cur = hit;
  return cur;
}

const specCaptionText = () => state.compare
  ? 'spectrogram — active version, log frequency'
  : 'spectrogram — log frequency';

/// Switch to a set: its manifest, its takes, and whether it can compare at all.
async function loadSet(id, want) {
  const entry = state.sets.find((s) => s.id === id) || state.sets[0];
  stopSources();
  state.playing = false;
  $('playBtn').textContent = 'play';
  state.setId = entry.id;
  state.base = `s/${entry.id}/`;
  localStorage.setItem(SET_KEY, entry.id);

  const m = await (await fetch(`${state.base}manifest.json`)).json();
  state.manifest = m;
  state.items = m.items || [];
  // A set whose takes hold one version each has nothing to switch between, so
  // the comparison controls come off rather than sitting there doing nothing.
  // That is the ordinary case for anyone without the reference plugin.
  state.compare = state.items.some((it) => Object.keys(it.tracks || {}).length > 1);
  document.body.classList.toggle('no-compare', !state.compare);
  if (!state.compare) {
    state.blind = false;
    $('blind').checked = false;
  }
  $('waveCaption').textContent = state.compare
    ? 'waveform — all versions overlaid, active one solid'
    : 'waveform';
  $('specCaption').textContent = specCaptionText();

  $('title').textContent = m.title || '';
  document.title = `${entry.id} — audition`;
  $('notes').textContent = m.notes || '';
  $('sharedNote').textContent = m.sources_note || '';
  // Keyed on the set, so notes taken on one instrument do not surface on another.
  state.notes = JSON.parse(localStorage.getItem(notesKey()) || '{}');
  state.picks = JSON.parse(localStorage.getItem(picksKey()) || '{}');
  $('setSelect').value = state.setId;
  renderHeadStage();

  const wanted = want || {};
  if (wanted.ver) state.wantKey = wanted.ver;
  const i = wanted.take ? state.items.findIndex((it) => it.id === wanted.take) : -1;
  state.itemIndex = i >= 0 ? i : 0;
  state.versionIndex = 0;
  buildTakeList();
  if (state.items.length) await selectTake(state.itemIndex);
  else { $('versions').replaceChildren(); writeRoute(); }
}

function buildSetPicker() {
  const sel = $('setSelect');
  sel.replaceChildren();
  // Grouped when the sets say what group they are in. A whole bank is a hundred
  // and thirty entries, and a flat list of those is scrolled past rather than
  // read; sets that declare no group stay in one ungrouped run at the top, so a
  // handful of hand-made pages is unaffected.
  const groups = new Map();
  state.sets.forEach((s) => {
    const key = s.group || '';
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push(s);
  });
  // The id is what a link carries and what the directory is called, so it leads;
  // the title is context and is routinely the same on several sets of one
  // instrument, which is exactly the case a title alone cannot tell apart.
  const option = (s) => {
    const o = document.createElement('option');
    o.value = s.id;
    const marks = [`${s.takes} takes`];
    if (!s.compare) marks.push('no reference');
    o.textContent = `${s.id}  ·  ${marks.join(', ')}`;
    return o;
  };
  groups.forEach((sets, name) => {
    if (!name) { sets.forEach((s) => sel.appendChild(option(s))); return; }
    const g = document.createElement('optgroup');
    g.label = name;
    sets.forEach((s) => g.appendChild(option(s)));
    sel.appendChild(g);
  });
  sel.addEventListener('change', () => loadSet(sel.value));
}

function buildTakeList() {
  const nav = $('takes');
  nav.replaceChildren();
  let group = null;
  state.items.forEach((item, i) => {
    if (item.group && item.group !== group) {
      group = item.group;
      const h = document.createElement('div');
      h.className = 'group';
      h.textContent = group;
      nav.appendChild(h);
    }
    const b = document.createElement('button');
    b.type = 'button';
    b.appendChild(document.createTextNode(item.label || item.id));
    const s = document.createElement('span');
    s.className = 'sub';
    // The id is what the URL carries, so it is shown rather than left to be
    // guessed from a prose label that does not have to resemble it.
    s.textContent = item.sub ? `${item.id} — ${item.sub}` : item.id;
    b.appendChild(s);
    b.addEventListener('click', () => selectTake(i));
    nav.appendChild(b);
  });
}

function markTakeList() {
  const buttons = [...$('takes').querySelectorAll('button')];
  buttons.forEach((b, i) => {
    if (i === state.itemIndex) {
      b.setAttribute('aria-current', 'true');
      b.scrollIntoView({ block: 'nearest' });
    } else {
      b.removeAttribute('aria-current');
    }
  });
}

async function selectTake(i) {
  if (i < 0 || i >= state.items.length) return;
  const wasPlaying = state.playing;
  pause();
  state.itemIndex = i;
  state.startOffset = 0;
  state.region = null;
  markTakeList();
  const item = state.items[i];
  $('specCaption').textContent = 'decoding…';
  $('specCaption').className = 'loading';
  try {
    state.take = await loadTake(item);
  } catch (err) {
    $('specCaption').textContent = `could not load: ${err.message}`;
    state.take = null;
    return;
  }
  $('specCaption').className = '';
  $('specCaption').textContent = specCaptionText();
  reshuffleBlind();
  // The version carries across takes by name rather than by position, so
  // stepping down the take list keeps auditioning the same candidate.
  const want = state.take.keys.indexOf(state.wantKey);
  state.versionIndex = state.blind
    ? Math.min(state.versionIndex, state.take.keys.length - 1)
    : (want >= 0 ? want : Math.min(state.versionIndex, state.take.keys.length - 1));
  buildVersionButtons();
  if (!state.blind) state.wantKey = activeKey();
  $('takeNotes').value = state.notes[item.id] || '';
  $('notePanel').open = Boolean(state.notes[item.id]);
  renderLevels();
  renderScore();
  writeRoute();
  if (wasPlaying) startAt(0);
}

function reshuffleBlind() {
  const n = state.take ? state.take.keys.length : 0;
  state.blindOrder = [...Array(n).keys()];
  for (let i = n - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [state.blindOrder[i], state.blindOrder[j]] = [state.blindOrder[j], state.blindOrder[i]];
  }
}

const sourceOf = (key) => (state.manifest.sources || {})[key] || {};

function sourceLabel(key) {
  return sourceOf(key).label || key;
}

function roleOf(key) {
  const r = sourceOf(key).role;
  return ROLE_ORDER.includes(r) ? r : '';
}

/// Slots in the order they are shown, which is the order `1`…`9` count in.
function displayOrder() {
  const slots = state.take.keys.map((_, i) => i);
  if (state.blind) return slots;
  const rank = (slot) => {
    const r = roleOf(state.take.keys[slot]);
    const at = ROLE_ORDER.indexOf(r);
    return at < 0 ? ROLE_ORDER.length : at;
  };
  // Stable, so a role's own versions keep the order the manifest gave them.
  return slots.map((s, i) => [s, i]).sort((a, b) => rank(a[0]) - rank(b[0]) || a[1] - b[1])
    .map(([s]) => s);
}

/// What goes on the button.
///
/// The source key, not the label: it is short enough to survive a segmented
/// control at seven across, and it is what the URL carries and what a listening
/// note has to name. A label is prose and there is no arranging for prose to be
/// both descriptive and four characters long -- the two references of one
/// instrument differ in the last three words of a forty-character label, which
/// is precisely the part a button ellipsises away. The label is not lost: the
/// selected version's is spelled out under the row, in full.
function versionLabel(slot) {
  const item = state.items[state.itemIndex];
  const key = state.take.keys[state.blind ? state.blindOrder[slot] : slot];
  if (!state.blind) return key;
  const letter = String.fromCharCode(65 + state.display.indexOf(slot));
  const pick = state.picks[item.id];
  return pick && pick.revealed ? `${letter} · ${key}` : letter;
}

function buildVersionButtons() {
  const box = $('versions');
  box.replaceChildren();
  state.display = displayOrder();

  // One row per role, in the role order, so the model side and the reference
  // side are not one undifferentiated strip of seven buttons. A manifest that
  // declares no roles gets a single unlabelled row, unchanged.
  let row = null;
  let rowRole = null;
  state.display.forEach((slot, pos) => {
    const role = state.blind ? '' : roleOf(state.take.keys[slot]);
    if (row === null || role !== rowRole) {
      rowRole = role;
      const wrap = document.createElement('div');
      wrap.className = role ? 'vrow' : 'vrow unlabelled';
      if (role) {
        const lab = document.createElement('span');
        lab.className = 'role';
        lab.textContent = role;
        wrap.appendChild(lab);
      }
      row = document.createElement('div');
      row.className = 'segmented';
      row.setAttribute('role', 'group');
      row.setAttribute('aria-label', role || 'versions');
      wrap.appendChild(row);
      box.appendChild(wrap);
    }
    const b = document.createElement('button');
    b.type = 'button';
    b.dataset.slot = String(slot);
    const k = document.createElement('span');
    k.className = 'key';
    k.textContent = String(pos + 1);
    b.appendChild(k);
    b.appendChild(document.createTextNode(versionLabel(slot)));
    b.title = versionTitle(slot);
    b.setAttribute('aria-pressed', String(slot === state.versionIndex));
    b.addEventListener('click', () => setVersion(slot));
    row.appendChild(b);
  });
  markVersion();
}

function versionTitle(slot) {
  if (state.blind) return '';
  const key = state.take.keys[slot];
  const src = sourceOf(key);
  return [key, src.label, src.detail].filter(Boolean).join('\n');
}

function markVersion() {
  [...$('versions').querySelectorAll('button')].forEach((b) =>
    b.setAttribute('aria-pressed', String(+b.dataset.slot === state.versionIndex)));
  // The selected version said in full, since the button can only carry its key.
  // Blind mode gets nothing here: this line is the answer it is withholding.
  const key = state.blind ? '' : activeKey();
  const src = key ? sourceOf(key) : {};
  $('versionDetail').textContent =
    [src.label, src.detail].filter((s) => s && s !== key).join('  ·  ');
  renderIdent();
}

function selectVersionByKey(key) {
  if (!state.take || state.blind) return;
  const slot = state.take.keys.indexOf(key);
  if (slot >= 0) setVersion(slot);
}

function setVersion(slot) {
  if (!state.take || slot < 0 || slot >= state.take.keys.length) return;
  state.versionIndex = slot;
  if (!state.blind) state.wantKey = activeKey();
  if (state.blind) {
    const id = state.items[state.itemIndex].id;
    state.picks[id] = { slot, key: state.take.keys[state.blindOrder[slot]], revealed: false };
    localStorage.setItem(picksKey(), JSON.stringify(state.picks));
    renderScore();
  }
  markVersion();
  // Two ways to switch, and they answer different questions. Crossfading in
  // place keeps the comparison sample-aligned, which is the only way to hear a
  // difference in a sustain or a decay. Restarting throws away that alignment
  // and gives back the attack: switching four seconds into a phrase otherwise
  // means the new version's onset has already gone by, and waiting out the take
  // to hear it is long enough for the ear to lose what it was holding.
  const restart = $('restartOnSwitch').checked;
  if (restart) {
    const from = (state.region || [0])[0];
    if (state.playing) startAt(from);
    else state.startOffset = from;  // the rAF loop redraws the playhead
  } else if (state.playing) {
    applyGains(false);
  }
  renderLevels();
  writeRoute();
}

/// Step through the versions in the order they are shown, not in manifest order.
function stepVersion(delta) {
  const at = state.display.indexOf(state.versionIndex);
  const n = state.display.length;
  if (!n) return;
  setVersion(state.display[((at < 0 ? 0 : at) + delta + n) % n]);
}

function renderIdent() {
  const crumbs = $('crumbs');
  crumbs.replaceChildren();
  const item = state.items[state.itemIndex];
  const parts = [state.setId, item && item.id];
  if (item && state.take) {
    parts.push(state.blind
      ? String.fromCharCode(65 + state.display.indexOf(state.versionIndex))
      : activeKey());
  }
  parts.filter(Boolean).forEach((p, i, all) => {
    const s = document.createElement('span');
    s.textContent = p;
    if (i === all.length - 1) s.className = 'now';
    crumbs.appendChild(s);
    if (i < all.length - 1) {
      crumbs.appendChild(Object.assign(document.createElement('span'),
        { className: 'sep', textContent: '›' }));
    }
  });
  // The path, so the file a listening note refers to is never inferred. Hidden
  // in blind mode, where it would name the version the letters are hiding.
  $('identPath').textContent = (item && state.take && !state.blind)
    ? state.base + item.tracks[activeKey()]
    : '';
}

function renderLevels() {
  if (!state.take) return;
  const key = activeKey();
  const db = (v) => (v > 0 ? (20 * Math.log10(v)).toFixed(1) : '-inf');
  const g = targetGain(key);
  const matched = $('matchRms').checked ? `  gain ${(20 * Math.log10(g)).toFixed(1)} dB` : '';
  $('levels').textContent = state.blind
    ? `rms ${db(state.take.rms[key] * g)} dBFS${matched}`
    : `rms ${db(state.take.rms[key])} dBFS${matched}`;
}

function renderScore() {
  if (!state.blind) { $('blindScore').textContent = ''; return; }
  const tally = {};
  let n = 0;
  for (const p of Object.values(state.picks)) {
    if (!p || !p.key) continue;
    tally[p.key] = (tally[p.key] || 0) + 1;
    n++;
  }
  const parts = Object.entries(tally)
    .sort((a, b) => b[1] - a[1])
    .map(([k, v]) => `${sourceLabel(k)} ${v}`);
  $('blindScore').textContent = n ? `preferred: ${parts.join('   ')}` : 'pick a version on each take';
}

/* ------------------------------------------------------------------ draw */

function fitCanvas(cv) {
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.round(cv.clientWidth * dpr));
  const h = Math.max(1, Math.round(cv.clientHeight * dpr));
  if (cv.width !== w) cv.width = w;
  if (cv.height !== h) cv.height = h;
  return { w, h };
}

function makeOffscreen(w, h) {
  if (typeof OffscreenCanvas === 'function') return new OffscreenCanvas(w, h);
  const cv = document.createElement('canvas');
  cv.width = w;
  cv.height = h;
  return cv;
}

function layer(name, w, h, sig, paint) {
  const l = layers[name];
  if (l.sig === sig && l.cv && l.cv.width === w && l.cv.height === h) return l.cv;
  const cv = l.cv && l.cv.width === w && l.cv.height === h ? l.cv : makeOffscreen(w, h);
  paint(cv.getContext('2d'), w, h);
  l.cv = cv;
  l.sig = sig;
  return cv;
}

function peaksFor(key, cols) {
  const cache = state.take.peaks;
  const id = `${key}:${cols}`;
  if (cache[id]) return cache[id];
  const buf = state.take.buffers[key];
  const ch = buf.getChannelData(0);
  const ch2 = buf.numberOfChannels > 1 ? buf.getChannelData(1) : null;
  const per = ch.length / cols;
  const lo = new Float32Array(cols);
  const hi = new Float32Array(cols);
  for (let x = 0; x < cols; x++) {
    const a = Math.floor(x * per);
    const b = Math.min(ch.length, Math.floor((x + 1) * per));
    let mn = 0, mx = 0;
    for (let i = a; i < b; i++) {
      const v = ch2 ? (ch[i] + ch2[i]) * 0.5 : ch[i];
      if (v < mn) mn = v;
      if (v > mx) mx = v;
    }
    lo[x] = mn; hi[x] = mx;
  }
  cache[id] = { lo, hi, cols };
  return cache[id];
}

function drawWave() {
  const cv = $('wave');
  const { w, h } = fitCanvas(cv);
  const g = cv.getContext('2d');
  if (!state.take) { g.clearRect(0, 0, w, h); return; }
  const active = activeKey();
  const sig = `${state.take.id}|${active}|${w}x${h}|${state.region ? state.region.join(',') : ''}`;
  const off = layer('wave', w, h, sig, (c) => {
    c.clearRect(0, 0, w, h);
    const mid = h / 2;
    const cols = Math.min(w, 3000);
    if (state.region) {
      const [a, b] = state.region;
      c.fillStyle = 'rgba(111,179,160,0.13)';
      c.fillRect((a / state.take.duration) * w, 0, ((b - a) / state.take.duration) * w, h);
    }
    // The inactive versions first and dimmed, so the active one is never hidden
    // behind a louder take that happens to be selected somewhere else.
    const order = [...state.take.keys.filter((k) => k !== active), active];
    for (const key of order) {
      const p = peaksFor(key, cols);
      c.fillStyle = key === active ? 'rgba(111,179,160,0.95)' : 'rgba(133,141,155,0.30)';
      const bw = Math.max(1, w / cols);
      for (let x = 0; x < cols; x++) {
        const y0 = mid - p.hi[x] * mid * 0.94;
        const y1 = mid - p.lo[x] * mid * 0.94;
        c.fillRect((x / cols) * w, y0, bw, Math.max(1, y1 - y0));
      }
    }
    c.strokeStyle = 'rgba(215,219,226,0.18)';
    c.beginPath(); c.moveTo(0, mid); c.lineTo(w, mid); c.stroke();
    // Every strike, numbered where it lands and labelled with the note it
    // plays. Without it the only way to say which one sounded wrong is to
    // count, and a count has to be reconciled against the phrase set by hand at
    // the other end -- which is a step where "the second one" silently becomes
    // a different instrument.
    // Every strike gets a tick; a label only where the last one has cleared.
    // The musical take is seventy-six strikes in thirteen seconds and labelling
    // all of them writes one illegible band across the top of the waveform,
    // which loses the sparse takes' labels as well as its own.
    c.font = '10px ui-monospace, SFMono-Regular, monospace';
    c.textBaseline = 'top';
    let freeAt = 0;
    for (const hit of takeHits()) {
      const x = (hit.start / state.take.duration) * w;
      c.strokeStyle = 'rgba(232,226,212,0.30)';
      c.beginPath(); c.moveTo(x, 0); c.lineTo(x, 14); c.stroke();
      const text = `#${hit.n} ${hit.notes.map((n) => n.note).join('+')}`;
      if (x < freeAt) continue;
      c.fillStyle = 'rgba(232,226,212,0.72)';
      c.fillText(text, x + 3, 2);
      freeAt = x + 3 + c.measureText(text).width + 8;
    }
  });
  g.clearRect(0, 0, w, h);
  g.drawImage(off, 0, 0);
  drawPlayhead(g, w, h, 'rgba(232,226,212,0.9)');
}

function drawPlayhead(g, w, h, color) {
  const x = (playhead() / state.take.duration) * w;
  g.strokeStyle = color;
  g.lineWidth = Math.max(1, window.devicePixelRatio || 1);
  g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke();
}

/* --- FFT: iterative radix-2, in place on split real/imaginary arrays --- */

function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      [re[i], re[j]] = [re[j], re[i]];
      [im[i], im[j]] = [im[j], im[i]];
    }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = -2 * Math.PI / len;
    const wr = Math.cos(ang), wi = Math.sin(ang);
    const half = len >> 1;
    for (let i = 0; i < n; i += len) {
      let cr = 1, ci = 0;
      for (let k = 0; k < half; k++) {
        const ur = re[i + k], ui = im[i + k];
        const xr = re[i + k + half], xi = im[i + k + half];
        const vr = xr * cr - xi * ci;
        const vi = xr * ci + xi * cr;
        re[i + k] = ur + vr; im[i + k] = ui + vi;
        re[i + k + half] = ur - vr; im[i + k + half] = ui - vi;
        const nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr; cr = nr;
      }
    }
  }
}

function specFor(key, cols) {
  const cache = state.take.specs;
  const id = `${key}:${cols}`;
  if (cache[id]) return cache[id];
  const buf = state.take.buffers[key];
  const ch = buf.getChannelData(0);
  const ch2 = buf.numberOfChannels > 1 ? buf.getChannelData(1) : null;
  const bins = SPEC_FFT / 2;
  const win = new Float32Array(SPEC_FFT);
  for (let i = 0; i < SPEC_FFT; i++) win[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / SPEC_FFT);

  const frames = Math.max(1, Math.floor((ch.length - SPEC_FFT) / SPEC_HOP) + 1);
  const step = Math.max(1, Math.floor(frames / cols));
  const used = Math.max(1, Math.floor(frames / step));
  const mag = new Float32Array(used * bins);
  const re = new Float32Array(SPEC_FFT), im = new Float32Array(SPEC_FFT);

  for (let f = 0; f < used; f++) {
    const base = f * step * SPEC_HOP;
    for (let i = 0; i < SPEC_FFT; i++) {
      const s = base + i;
      const v = s < ch.length ? (ch2 ? (ch[s] + ch2[s]) * 0.5 : ch[s]) : 0;
      re[i] = v * win[i]; im[i] = 0;
    }
    fft(re, im);
    for (let b = 0; b < bins; b++) mag[f * bins + b] = Math.hypot(re[b], im[b]);
  }
  cache[id] = { frames: used, bins, mag, sampleRate: buf.sampleRate };
  return cache[id];
}

function specColor(t) {
  // Dark to teal to sand: an ordered two-hue ramp rather than a rainbow, so a
  // level reads as a level instead of as a colour that has to be decoded.
  const stops = [
    [0.00, 20, 22, 26],
    [0.35, 26, 66, 68],
    [0.62, 111, 179, 160],
    [0.85, 208, 154, 90],
    [1.00, 246, 240, 228],
  ];
  for (let i = 1; i < stops.length; i++) {
    if (t <= stops[i][0]) {
      const [p0, r0, g0, b0] = stops[i - 1];
      const [p1, r1, g1, b1] = stops[i];
      const u = (t - p0) / (p1 - p0);
      return [r0 + (r1 - r0) * u, g0 + (g1 - g0) * u, b0 + (b1 - b0) * u];
    }
  }
  return [246, 240, 228];
}

function drawSpec() {
  const cv = $('spec');
  const { w, h } = fitCanvas(cv);
  const g = cv.getContext('2d');
  if (!state.take) { g.clearRect(0, 0, w, h); return; }
  const key = activeKey();
  const sig = `${state.take.id}|${key}|${w}x${h}`;
  const off = layer('spec', w, h, sig, (c) => {
    const s = specFor(key, Math.min(w, 1400));
    const nyq = s.sampleRate / 2;
    const lmin = Math.log(40), lmax = Math.log(nyq);
    const img = c.createImageData(w, h);
    // One image row per canvas row, taking the loudest bin in that row's band:
    // low on a log axis a row spans many bins, and averaging there smears a
    // partial into the gap beside it.
    const rowBin = new Int32Array(h + 1);
    for (let y = 0; y <= h; y++) {
      const f = Math.exp(lmax - (y / h) * (lmax - lmin));
      rowBin[y] = Math.min(s.bins - 1, Math.max(0, Math.round(f / nyq * s.bins)));
    }
    for (let x = 0; x < w; x++) {
      const f = Math.min(s.frames - 1, Math.floor(x / w * s.frames));
      const row = f * s.bins;
      for (let y = 0; y < h; y++) {
        const b0 = Math.min(rowBin[y], rowBin[y + 1]);
        const b1 = Math.max(rowBin[y], rowBin[y + 1]);
        let m = 0;
        for (let b = b0; b <= b1; b++) { const v = s.mag[row + b]; if (v > m) m = v; }
        const db = 20 * Math.log10(m / (SPEC_FFT / 4) + 1e-9);
        const t = Math.max(0, Math.min(1, (db - SPEC_FLOOR_DB) / -SPEC_FLOOR_DB));
        const [r, gg, bb] = specColor(t);
        const o = (y * w + x) * 4;
        img.data[o] = r; img.data[o + 1] = gg; img.data[o + 2] = bb; img.data[o + 3] = 255;
      }
    }
    c.putImageData(img, 0, 0);
    c.strokeStyle = 'rgba(215,219,226,0.10)';
    c.fillStyle = 'rgba(215,219,226,0.5)';
    c.font = `${11 * (window.devicePixelRatio || 1)}px ui-monospace, monospace`;
    for (const f of [100, 1000, 10000]) {
      if (f >= nyq) continue;
      const y = h - ((Math.log(f) - lmin) / (lmax - lmin)) * h;
      c.beginPath(); c.moveTo(0, y); c.lineTo(w, y); c.stroke();
      c.fillText(f >= 1000 ? `${f / 1000}k` : String(f), 4, y - 3);
    }
  });
  g.clearRect(0, 0, w, h);
  g.drawImage(off, 0, 0);
  drawPlayhead(g, w, h, 'rgba(232,226,212,0.85)');
}

/* ------------------------------------------------------------------ wire */

function seekFromEvent(cv, ev) {
  const r = cv.getBoundingClientRect();
  return Math.max(0, Math.min(1, (ev.clientX - r.left) / r.width)) * state.take.duration;
}

/* What was heard, in a form that can be pasted somewhere else.
 *
 * An address names the set, the take and the version and stops there, so
 * everything that decides what the ear actually met -- where the playhead was,
 * which strike that is, whether the levels were matched, whether the names were
 * hidden -- had to be described from memory at the other end or reconstructed
 * by counting strikes against a phrase set. Both of those are a way of getting
 * it wrong quietly: "the second one" is a different note in every take, and the
 * two ends do not find out that they disagreed. */
function conditionsText() {
  const item = state.items[state.itemIndex];
  const url = location.origin + location.pathname
    + `?t=${playhead().toFixed(2)}` + routeHash();
  const lines = [
    `set:      ${state.setId}`,
    `take:     ${item ? item.id : '-'}${item && item.label ? ` — ${item.label}` : ''}`,
    `version:  ${state.blind ? 'hidden (blind)' : activeKey()}`,
    `playhead: ${playhead().toFixed(2)} s of `
      + `${state.take ? state.take.duration.toFixed(2) : '?'} s`,
  ];
  const hit = hitAt(playhead());
  if (hit) {
    const plays = hit.notes.map((n) => `note ${n.note} v${n.velocity}`).join(' + ');
    lines.push(`hit:      #${hit.n} — ${plays}, struck at ${hit.start.toFixed(2)} s`
      + ` (${(playhead() - hit.start).toFixed(2)} s in)`);
  }
  if (state.region) {
    lines.push(`region:   ${state.region[0].toFixed(2)}–${state.region[1].toFixed(2)} s`);
  }
  lines.push(`options:  gain-match ${$('matchRms').checked ? 'on' : 'off'},`
    + ` loop ${state.loop ? 'on' : 'off'}, blind ${state.blind ? 'on' : 'off'}`);
  lines.push(url);
  return lines.join('\n');
}

async function copyConditions() {
  const text = conditionsText();
  const btn = $('copyLink');
  try {
    await navigator.clipboard.writeText(text);
    btn.textContent = 'copied';
  } catch {
    // A page served over plain http from another host has no clipboard, and a
    // block this size does not fit on a button. A prompt is selectable, and it
    // is the one fallback that does not overwrite the take's own notes.
    window.prompt('Listening conditions — copy this', text);
    btn.textContent = 'shown above';
  }
  setTimeout(() => { btn.textContent = 'copy what I hear'; }, 1600);
}

function wire() {
  $('playBtn').addEventListener('click', togglePlay);

  $('loopBtn').addEventListener('click', () => {
    state.loop = !state.loop;
    $('loopBtn').setAttribute('aria-pressed', String(state.loop));
    if (state.playing) startAt(playhead());
  });

  $('optionsBtn').addEventListener('click', () => {
    const open = $('options').hidden;
    $('options').hidden = !open;
    $('optionsBtn').setAttribute('aria-expanded', String(open));
  });

  $('copyLink').addEventListener('click', copyConditions);

  $('matchRms').addEventListener('change', () => { applyGains(false); renderLevels(); });

  $('blind').addEventListener('change', () => {
    state.blind = $('blind').checked;
    reshuffleBlind();
    if (state.take) { buildVersionButtons(); applyGains(false); renderLevels(); }
    renderScore();
    writeRoute();
  });

  $('exportBtn').addEventListener('click', exportNotes);

  $('takeNotes').addEventListener('input', () => {
    state.notes[state.items[state.itemIndex].id] = $('takeNotes').value;
    localStorage.setItem(notesKey(), JSON.stringify(state.notes));
  });

  window.addEventListener('hashchange', () => { applyRoute(readRoute()); });

  for (const cv of [$('wave'), $('spec')]) {
    let dragFrom = null;
    cv.addEventListener('pointerdown', (ev) => {
      if (!state.take) return;
      cv.setPointerCapture(ev.pointerId);
      dragFrom = seekFromEvent(cv, ev);
    });
    cv.addEventListener('pointermove', (ev) => {
      if (dragFrom === null) return;
      const to = seekFromEvent(cv, ev);
      if (Math.abs(to - dragFrom) > 0.02) {
        state.region = [Math.min(dragFrom, to), Math.max(dragFrom, to)];
      }
    });
    cv.addEventListener('pointerup', (ev) => {
      if (dragFrom === null) return;
      const to = seekFromEvent(cv, ev);
      if (Math.abs(to - dragFrom) <= 0.02) {
        // A click with no drag is a seek, and drops whatever region it lands in.
        state.region = null;
        state.startOffset = to;
        if (state.playing) startAt(to);
      } else if (state.playing) {
        startAt(state.region[0]);
      }
      dragFrom = null;
    });
  }

  document.addEventListener('keydown', (ev) => {
    const tag = ev.target.tagName;
    // The find box is the one field with a way out: escape drops back to the
    // rows, which is where every other key does something.
    if (tag === 'INPUT' && ev.key === 'Escape') { ev.target.blur(); return; }
    if (tag === 'TEXTAREA' || tag === 'SELECT' || tag === 'INPUT'
        || ev.metaKey || ev.ctrlKey || ev.altKey) return;
    // Every shortcut below acts on what is sounding, and on the bank there is
    // nothing sounding. It has its own set: the rows are what is navigated.
    if (document.body.classList.contains('bank-view')) { bankKey(ev); return; }
    const k = ev.key;
    if (k === ' ') { ev.preventDefault(); togglePlay(); return; }
    if (!state.take) return;
    if (k >= '1' && k <= '9') { setVersion(state.display[+k - 1]); return; }
    if (k === 'ArrowRight') { ev.preventDefault(); stepVersion(1); return; }
    if (k === 'ArrowLeft') { ev.preventDefault(); stepVersion(-1); return; }
    if (k === 'ArrowDown') { ev.preventDefault(); selectTake(state.itemIndex + 1); return; }
    if (k === 'ArrowUp') { ev.preventDefault(); selectTake(state.itemIndex - 1); return; }
    if (k === 'l' || k === 'L') { $('loopBtn').click(); return; }
    if (k === 'm' || k === 'M') {
      $('matchRms').checked = !$('matchRms').checked;
      $('matchRms').dispatchEvent(new Event('change'));
      return;
    }
    if (k === 's' || k === 'S') {
      $('restartOnSwitch').checked = !$('restartOnSwitch').checked;
      return;
    }
    if (k === 'b' || k === 'B') {
      $('blind').checked = !$('blind').checked;
      $('blind').dispatchEvent(new Event('change'));
      return;
    }
    if (k === 'r' || k === 'R') {
      const id = state.items[state.itemIndex].id;
      if (state.blind && state.picks[id] && !state.picks[id].revealed) {
        state.picks[id].revealed = true;
        localStorage.setItem(picksKey(), JSON.stringify(state.picks));
      } else {
        reshuffleBlind();
      }
      buildVersionButtons();
      applyGains(false);
    }
  });

  const tick = () => {
    if (state.take) {
      $('clock').textContent = `${playhead().toFixed(2)} / ${state.take.duration.toFixed(2)}`;
      if (state.playing && !state.loop && playhead() >= state.take.duration - 0.02) pause();
      drawWave();
      drawSpec();
    }
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}

function exportNotes() {
  const payload = {
    set: state.setId,
    title: state.manifest.title,
    exported: new Date().toISOString(),
    notes: state.notes,
    blind_picks: Object.fromEntries(
      Object.entries(state.picks).map(([id, p]) => [id, p && p.key]),
    ),
  };
  const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `audition-notes-${state.setId || 'takes'}.json`;
  a.click();
  URL.revokeObjectURL(url);
}

/* ------------------------------------------------------------------- bank */

/* The bank is the master: every GM program and GS variation the library voices,
 * whatever has been rendered. The listening surface shows one voice; this shows
 * where all of them stand, which is the half of the work that is otherwise only
 * visible to whoever remembers what has been captured.
 *
 * Three bands, because one list of 188 rows answers only the question you were
 * already on. The masthead is the overview — totals and the distribution across
 * the six stages — the toolbar narrows, and the table lists beside an inspector
 * that holds what a row cannot: the coverage denominator, which dimensions sit
 * outside the reference spread and by how much, and the ladder in full.
 *
 * Sixteen engines, three methods. What the eye is being asked here is "physical
 * model, FM, or neither" — sixteen hues would answer nothing, and the engine's
 * own name is on the row. */
const METHODS = {
  fm: 'fm',
  subtractive: 'classic',
  additive: 'classic',
};

const methodOf = (engine) => (engine ? (METHODS[engine] || 'physical') : 'classic');

const METHOD_PICKS = [
  ['', 'any'],
  ['physical', 'physical'],
  ['fm', 'FM'],
  ['classic', 'subtractive'],
];

const FILTER_PICKS = [
  ['oracle', 'has an oracle'],
  ['no-oracle', 'needs one'],
  ['unwritten', 'unwritten'],
  ['page', 'rendered'],
];

/* The ladder, with each step's predicate. The same wording as
 * `tools/voicematch/docs/status.md`, which is where it is argued. */
const LADDER = [
  ['0.0', 'untouched', 'no deliberate patch: a famN family fallback on subtractive'],
  ['0.2', 'voiced', 'a deliberate engine and patch answer it'],
  ['0.4', 'measured', 'two or more reference timbres, a profile, a current gate'],
  ['0.6', 'covered', 'every canonical dimension gated, or excused with a reason'],
  ['0.8', 'agreeing', 'most gated dimensions sit inside the reference spread'],
  ['1.0', 'settled', 'no structural residual, and the musical take signed off'],
];

const stageIndex = (v) => Math.max(0, Math.min(5, Math.round(v.stage * 5)));
const stageColor = (i) => `var(--s${i})`;

const bank = {
  voices: [],
  loaded: false,
  method: '',
  filter: '',
  stage: null,     // a rung index, or null for every rung
  sort: 'address',
  shown: [],
  cursor: -1,      // index into `shown`; the selection and the keyboard cursor
};

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text !== undefined) n.append(text);
  return n;
};

const hasPage = (slug) => state.sets.some((s) => s.id === slug);

async function loadBank() {
  if (bank.loaded) return;
  try {
    const got = await (await fetch('bank.json')).json();
    bank.voices = got.voices || [];
  } catch {
    bank.voices = [];
  }
  bank.loaded = true;
}

function bankMatches(v) {
  const find = $('bankFind').value.trim().toLowerCase();
  if (bank.method && methodOf(v.engine) !== bank.method) return false;
  if (bank.stage !== null && stageIndex(v) !== bank.stage) return false;
  if (bank.filter === 'oracle' && !v.capture) return false;
  if (bank.filter === 'no-oracle' && v.capture) return false;
  if (bank.filter === 'unwritten' && !(v.open_candidates || []).length) return false;
  if (bank.filter === 'page' && !hasPage(v.slug)) return false;
  if (find) {
    const hay = [v.name, v.engine, v.patch, v.slug, v.tone_class].join(' ').toLowerCase();
    if (!hay.includes(find)) return false;
  }
  return true;
}

const addressOf = (v) =>
  (v.kit ? `kit ${v.program}` : `${v.program}${v.bank ? `:${v.bank}` : ''}`);

/* ---- the small repeated readouts ---- */

const dotEl = (engine) => el('span', `dot ${methodOf(engine)}`);

function stageBar(v) {
  const bar = el('span', 'bar');
  const filled = stageIndex(v);
  bar.style.setProperty('--sc', stageColor(filled));
  for (let i = 0; i < 5; i += 1) bar.append(el('span', i < filled ? 'seg on' : 'seg'));
  return bar;
}

function stageEl(v) {
  const wrap = el('span', 'stage');
  wrap.append(stageBar(v), el('span', '', v.stage.toFixed(1)));
  wrap.title = `${v.stage_name} — ${v.next}`;
  return wrap;
}

/// A proportional bar over a set of voices. One count in six is 1 of 188, so the
/// segments carry a floor width and the countable numbers live in the chips.
function distBar(into, voices) {
  into.replaceChildren();
  const total = voices.length || 1;
  for (let i = 0; i < 6; i += 1) {
    const n = voices.filter((v) => stageIndex(v) === i).length;
    if (!n) continue;
    const seg = el('span');
    seg.style.flex = `${n} 1 0`;
    seg.style.background = stageColor(i);
    seg.title = `${n} ${LADDER[i][1]} (${Math.round((n / total) * 100)}%)`;
    into.append(seg);
  }
}

/* ---- masthead ---- */

function figure(label, value, onClick, warn) {
  const b = el('button', `fig${onClick ? ' act' : ''}${warn ? ' warn' : ''}`);
  b.type = 'button';
  b.append(el('span', 'num', String(value)), el('span', 'lab', label));
  if (onClick) b.addEventListener('click', onClick);
  else b.disabled = true;
  return b;
}

function renderMasthead() {
  const vs = bank.voices;
  const withOracle = vs.filter((v) => v.capture).length;
  const unwritten = vs.reduce((n, v) => n + (v.open_candidates || []).length, 0);
  const pages = vs.filter((v) => hasPage(v.slug)).length;
  const pick = (name) => () => {
    bank.filter = bank.filter === name ? '' : name;
    renderBank();
  };

  $('bankFigures').replaceChildren(
    figure('voices', vs.length, null),
    figure('with an oracle', withOracle, pick('oracle')),
    figure('unwritten', unwritten, unwritten ? pick('unwritten') : null, unwritten > 0),
    figure('rendered', pages, pages ? pick('page') : null),
  );

  distBar($('bankDist'), vs);

  const keys = $('bankStageKeys');
  keys.replaceChildren();
  LADDER.forEach(([at, name, of], i) => {
    const n = vs.filter((v) => stageIndex(v) === i).length;
    const b = el('button', 'skey');
    b.type = 'button';
    b.setAttribute('aria-pressed', String(bank.stage === i));
    b.title = `${at} — ${of}`;
    const dot = el('span', 'dot');
    dot.style.background = stageColor(i);
    b.append(dot, el('span', '', name), el('span', n ? 'n' : 'n z', String(n)));
    b.addEventListener('click', () => {
      bank.stage = bank.stage === i ? null : i;
      renderBank();
    });
    keys.append(b);
  });
}

/* ---- rows ---- */

function bankRow(v, index) {
  const row = el('div', 'bank-row');
  row.id = `v-${v.slug}`;
  row.setAttribute('role', 'option');
  row.setAttribute('aria-selected', 'false');
  row.addEventListener('click', () => { $('bankRows').focus(); selectRow(index); });
  row.addEventListener('dblclick', () => openVoice(v));

  const who = el('span', 'who', v.name);
  if (v.patch) who.append(el('span', 'patch', `  ${v.patch}`));

  const engine = el('span', 'engine');
  engine.append(dotEl(v.engine), el('span', '', v.engine || 'not reported'));
  engine.title = v.engine
    ? `${methodOf(v.engine)} — ${v.tone_class}`
    : 'no tuning build was available when the bank view was generated';

  const oracle = el('span', `oracle${v.capture ? '' : ' absent'}`, v.capture || 'no oracle');

  const tail = el('span', 'bank-tail');
  const open = v.open_candidates || [];
  if (open.length) {
    const badge = el('span', 'badge warn', `${open.length} unwritten`);
    badge.title = open.join(', ');
    tail.append(badge);
  }
  // Only past the oracle step: below it every voice says the same sentence, and
  // 150 copies of it bury the four that say something.
  if (v.stage > 0.2 && v.stage < 1) tail.append(el('span', 'next', v.next));

  row.append(el('span', 'addr', addressOf(v)), who, engine, stageEl(v), oracle, tail);
  return row;
}

const SORTS = {
  address: null,   // the generated order, which is already program order
  stage: (a, b) => b.stage - a.stage || a.program - b.program,
  name: (a, b) => a.name.localeCompare(b.name),
};

function renderBank() {
  const rows = $('bankRows');
  rows.replaceChildren();
  if (!bank.voices.length) {
    rows.append(el('p', 'bank-empty',
      'No bank view generated. Run `make voice-status-refresh` '
      + '(it needs a -DBUILD_TUNING=ON build) and reload.'));
    $('bankCount').textContent = '';
    $('bankFigures').replaceChildren();
    $('bankStageKeys').replaceChildren();
    bank.shown = [];
    bank.cursor = -1;
    renderInspect();
    return;
  }
  renderMasthead();
  markToolbar();

  const held = bank.shown[bank.cursor] ? bank.shown[bank.cursor].slug : null;
  const cmp = SORTS[bank.sort];
  bank.shown = bank.voices.filter(bankMatches);
  if (cmp) bank.shown = [...bank.shown].sort(cmp);

  // The GM families are the address order's own headings; under any other sort
  // they would be sixteen headings interleaved at random.
  const grouped = bank.sort === 'address';
  let group = null;
  bank.shown.forEach((v, i) => {
    if (grouped && v.group !== group) {
      group = v.group;
      const mine = bank.shown.filter((x) => x.group === group);
      const mini = el('span', 'mini');
      distBar(mini, mine);
      const h = el('h2', 'bank-group');
      h.append(el('span', '', group), el('span', 'n', String(mine.length)), mini);
      rows.append(h);
    }
    rows.append(bankRow(v, i));
  });
  if (!bank.shown.length) rows.append(el('p', 'bank-empty', 'Nothing matches.'));

  $('bankCount').textContent = `${bank.shown.length} / ${bank.voices.length}`;
  // The selection follows the voice rather than the row number: a filter change
  // that leaves it on screen should not move the inspector to whatever slid
  // into its place.
  const back = held ? bank.shown.findIndex((v) => v.slug === held) : -1;
  selectRow(back >= 0 ? back : (bank.shown.length ? 0 : -1), true);
}

function markToolbar() {
  for (const b of $('bankMethod').querySelectorAll('button')) {
    b.setAttribute('aria-pressed', String(b.dataset.method === bank.method));
  }
  for (const b of $('bankFilter').querySelectorAll('button')) {
    b.setAttribute('aria-pressed', String(b.dataset.filter === bank.filter));
  }
}

/// Move the cursor, which is also the selection: one row is focused, inspected
/// and openable, so there is never a question of which one an action lands on.
function selectRow(i, quiet) {
  bank.cursor = i;
  const rows = [...$('bankRows').querySelectorAll('.bank-row')];
  rows.forEach((r, at) => {
    const on = at === i;
    r.setAttribute('aria-selected', String(on));
    if (on) r.setAttribute('aria-current', 'true');
    else r.removeAttribute('aria-current');
  });
  const cur = rows[i];
  $('bankRows').setAttribute('aria-activedescendant', cur ? cur.id : '');
  if (cur && !quiet) cur.scrollIntoView({ block: 'nearest' });
  renderInspect();
}

function moveCursor(delta) {
  if (!bank.shown.length) return;
  const n = bank.shown.length;
  selectRow((bank.cursor + delta + n) % n);
}

function openVoice(v) {
  if (!v || !hasPage(v.slug)) return;
  setView('listen');
  loadSet(v.slug, { set: v.slug, take: '', ver: '', t: null });
}

/* ---- inspector ---- */

/* Always present, so selecting a row never resizes the table beside it, and it
 * holds the legend when nothing is selected rather than sitting blank. What is
 * in it is what a 188-row table has no width for: the coverage denominator, the
 * dimensions outside the references' own spread and by how much, and the rung
 * this voice is on with the predicate that would raise it. */

function fact(dl, term, value, aside, warn) {
  dl.append(el('dt', '', term));
  const dd = el('dd', warn ? 'warn' : '', value);
  if (aside) dd.append(el('span', 'aside', `  ${aside}`));
  dl.append(dd);
}

function methodLegend() {
  const legend = el('div', 'legend');
  for (const [m, text] of [['physical', 'physical model'], ['fm', 'FM'],
    ['classic', 'subtractive / additive']]) {
    const row = el('div');
    row.append(el('span', `dot ${m}`), el('span', '', text));
    legend.append(row);
  }
  return legend;
}

function inspectLegend(box) {
  box.append(el('span', 'lab', 'the ladder'));
  const ladder = el('div', 'ladder');
  LADDER.forEach(([at, name, of]) => {
    const r = el('div', 'rung done');
    r.append(el('span', 'mark', '·'), el('span', 'at', at), el('span', '', name));
    r.title = of;
    ladder.append(r);
  });
  box.append(ladder, el('span', 'lab', 'method'), methodLegend());
  box.append(el('p', 'hint', 'Select a voice for its coverage and where it sits '
    + 'against the references’ own spread.'), keyHint());
}

/// Kept on the panel rather than behind `options`, which is the listening
/// surface's and is off in this view.
const keyHint = () => el('p', 'key-hint', '↑ ↓ move   enter listen   / find');

function inspectVoice(box, v) {
  box.append(el('h2', '', v.name));
  box.append(el('p', 'addr-line',
    [addressOf(v), v.patch, v.tone_class].filter(Boolean).join('  ·  ')));

  const engine = el('p', 'addr-line engine');
  engine.append(dotEl(v.engine),
    el('span', '', `${v.engine || 'not reported'}  ·  ${methodOf(v.engine)}`));
  box.append(engine);

  box.append(el('span', 'lab', `stage ${v.stage.toFixed(1)} — ${v.stage_name}`));
  // Names only, with the rung marked. The predicate is the tooltip, and the one
  // move that would raise it is spelled out below — six wrapped predicates here
  // would be the tallest thing in the panel and the least actionable.
  const ladder = el('div', 'ladder');
  const here = stageIndex(v);
  LADDER.forEach(([at, name, of], i) => {
    const r = el('div', `rung${i === here ? ' here' : i < here ? ' done' : ''}`);
    r.append(el('span', 'mark', i === here ? '▸' : ' '),
      el('span', 'at', at), el('span', '', name));
    r.title = of;
    ladder.append(r);
  });
  box.append(ladder);

  box.append(el('span', 'lab', 'measurement'));
  const dl = el('dl', 'facts');
  const ax = v.axes || {};
  if (v.capture) {
    fact(dl, 'oracle', v.capture, `${ax.timbres} timbre(s), ${ax.profile_rows} rows, `
      + `gate ${ax.gate_state || 'not recorded'}`);
  } else {
    fact(dl, 'oracle', 'not captured', '', true);
  }

  const cov = ax.coverage || {};
  if (cov.canonical) {
    const excused = cov.excused || [];
    fact(dl, 'coverage', `${cov.gated} of ${cov.canonical} gated`,
      excused.length ? `${excused.length} excused: ${excused.join(', ')}` : '',
      !cov.complete);
    if ((cov.gaps || []).length) {
      fact(dl, 'gaps', String(cov.gaps.length), cov.gaps.join(', '), true);
    }
  }

  const ag = ax.agreement || {};
  if (ag.total) {
    const outside = Object.entries(ag.outside || {}).map(([d, m]) => `${d} ${m}×`);
    fact(dl, 'agreement', `${ag.inside} of ${ag.total} inside the spread`,
      outside.join(', '));
  } else if ((ag.unjudgeable || []).length) {
    fact(dl, 'agreement', 'no spread',
      `${ag.unjudgeable.length} dimension(s) unjudgeable — one reference timbre`, true);
  }

  const open = v.open_candidates || [];
  if (open.length) fact(dl, 'unwritten', String(open.length), open.join(', '), true);
  box.append(dl);

  box.append(el('span', 'lab', 'next'), el('p', 'next-text', v.next));

  if (hasPage(v.slug)) {
    const go = el('button', 'go', 'listen →');
    go.type = 'button';
    go.addEventListener('click', () => openVoice(v));
    box.append(go);
  }
  box.append(keyHint());
}

function renderInspect() {
  const box = $('bankInspect');
  box.replaceChildren();
  const v = bank.shown[bank.cursor];
  if (v) inspectVoice(box, v);
  else inspectLegend(box);
}

/* ---- the listening surface's own bank line ---- */

/// Without it a page says what a voice sounds like and nothing about why it is
/// open: which rung it is on, whether it has an oracle, what is unadopted.
function renderHeadStage() {
  const box = $('headStage');
  box.replaceChildren();
  const v = bank.voices.find((x) => x.slug === state.setId);
  if (!v) return;
  box.append(dotEl(v.engine), el('span', '', v.engine || 'not reported'),
    stageBar(v), el('span', '', `${v.stage.toFixed(1)} ${v.stage_name}`));
  const open = v.open_candidates || [];
  if (open.length) {
    const badge = el('span', 'badge warn', `${open.length} unwritten`);
    badge.title = open.join(', ');
    box.append(badge);
  }
  box.title = v.next;
}

/* ---- views and wiring ---- */

async function setView(view) {
  const wantBank = view === 'bank';
  document.body.classList.toggle('bank-view', wantBank);
  $('bank').hidden = !wantBank;
  localStorage.setItem(VIEW_KEY, view);
  // The manifest title names the set being listened to, which says nothing here
  // and reads as a claim about the bank. `loadSet` puts it back.
  if (wantBank && !state.failed) $('title').textContent = '';
  for (const b of $('viewToggle').querySelectorAll('button')) {
    b.setAttribute('aria-selected', String(b.dataset.view === view));
  }
  if (wantBank) {
    if (state.playing) pause();
    await loadBank();
    renderBank();
    $('bankRows').focus();
  }
}

function bankKey(ev) {
  const k = ev.key;
  if (k === '/') { ev.preventDefault(); $('bankFind').focus(); return; }
  if (k === 'ArrowDown' || k === 'j') { ev.preventDefault(); moveCursor(1); return; }
  if (k === 'ArrowUp' || k === 'k') { ev.preventDefault(); moveCursor(-1); return; }
  if (k === 'Home') { ev.preventDefault(); selectRow(0); return; }
  if (k === 'End') { ev.preventDefault(); selectRow(bank.shown.length - 1); return; }
  if (k === 'Enter') { ev.preventDefault(); openVoice(bank.shown[bank.cursor]); }
}

function wireBank() {
  for (const b of $('viewToggle').querySelectorAll('button')) {
    b.addEventListener('click', () => setView(b.dataset.view));
  }

  const method = $('bankMethod');
  for (const [value, label] of METHOD_PICKS) {
    const b = el('button', '', label);
    b.type = 'button';
    b.dataset.method = value;
    b.addEventListener('click', () => { bank.method = value; renderBank(); });
    method.append(b);
  }

  const filter = $('bankFilter');
  for (const [value, label] of FILTER_PICKS) {
    const b = el('button', 'chip', label);
    b.type = 'button';
    b.dataset.filter = value;
    b.addEventListener('click', () => {
      bank.filter = bank.filter === value ? '' : value;
      renderBank();
    });
    filter.append(b);
  }

  $('bankSort').addEventListener('change', () => {
    bank.sort = $('bankSort').value;
    renderBank();
  });
  $('bankFind').addEventListener('input', renderBank);
}

boot().catch((err) => {
  fail(`could not load the renders: ${err.message}`);
});
