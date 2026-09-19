/* audition — listen to one voice against the thing it is measured against.
 *
 * This file is the entry point and the wiring: which view is up, which language
 * the page is in, what every key does, and the frame loop that redraws the two
 * pictures. The transport is `player.js`, the pictures are `scope.js`, the
 * listening surface is `listen.js`, the bank is `bank.js`, and what a listener
 * says about what they heard is `feedback.js`.
 */

'use strict';

import { $, state, VIEW_KEY, SET_KEY } from './state.js';
import {
  t, applyStatic, initLang, onLang, setLang, currentLang, languages,
} from './i18n.js';
import {
  applyGains, pause, playhead, renderLevels, startAt, togglePlay,
} from './player.js';
import { drawSpec, drawWave, seekFromEvent } from './scope.js';
import {
  applyRoute, buildSetPicker, closePalette, copyConditions, loadFeedbackIndex,
  loadSet, openPalette, paletteKey, paletteOpen, readRoute, rebuildVersions,
  recordBlindResult, refreshListen, renderCaptions, renderScore, renderSubject,
  resetBlindReveal, reshuffleBlind, selectTake, setVersion, stepVersion, swapRole,
} from './listen.js';
import { bankKey, loadBank, refreshBank, renderBank, wireBank } from './bank.js';
import { refreshFeedback, wireFeedback } from './feedback.js';

function fail(msg) {
  // Held, because the bank view clears the title otherwise: the one failure a
  // fresh clone hits leaves it on the bank, which is the view worth being in.
  state.failed = true;
  $('title').textContent = msg;
  $('crumbs').replaceChildren();
  $('identPath').textContent = '';
}

/* ------------------------------------------------------------------ views */

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

/* --------------------------------------------------------------- language */

function buildLangToggle() {
  const box = $('langToggle');
  box.replaceChildren();
  for (const code of languages()) {
    const b = document.createElement('button');
    b.type = 'button';
    b.dataset.lang = code;
    b.textContent = code === 'ja' ? '日本語' : 'EN';
    b.setAttribute('aria-selected', String(code === currentLang()));
    b.addEventListener('click', () => setLang(code));
    box.append(b);
  }
}

/* ------------------------------------------------------------------- keys */

/* Two key maps, because the two views have different subjects: on the
 * listening surface every shortcut acts on what is sounding, and on the bank
 * nothing is sounding and the rows are what is navigated. */
function onKey(ev) {
  const tag = ev.target.tagName;
  // The find box is the one field with a way out: escape drops back to the
  // rows, which is where every other key does something.
  if (ev.target.id === 'voiceFind') {
    if (ev.key === 'Escape') { closePalette(); $('voicePick').focus(); return; }
    if (ev.key === 'ArrowDown' || ev.key === 'Enter') paletteKey(ev);
    return;
  }
  if (tag === 'INPUT' && ev.key === 'Escape') { ev.target.blur(); return; }
  if (tag === 'TEXTAREA' || tag === 'SELECT' || tag === 'INPUT'
      || ev.metaKey || ev.ctrlKey || ev.altKey) return;
  if (paletteOpen()) {
    if (ev.key === 'Escape') { closePalette(); $('voicePick').focus(); return; }
    paletteKey(ev);
    return;
  }
  if (ev.key === '?') { ev.preventDefault(); toggleHelp(); return; }
  if (ev.key === 'Escape' && !$('help').hidden) { toggleHelp(false); return; }
  if (document.body.classList.contains('bank-view')) { bankKey(ev); return; }

  const k = ev.key;
  if (k === 'v' || k === 'V') { ev.preventDefault(); openPalette(); return; }
  if (k === ' ') { ev.preventDefault(); togglePlay(); return; }
  if (!state.take) return;
  if (k === 'Tab') { ev.preventDefault(); swapRole(); return; }
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
      localStorage.setItem(`audition:picks:${state.setId}`, JSON.stringify(state.picks));
    } else {
      reshuffleBlind();
    }
    rebuildVersions();
  }
}

function toggleHelp(want) {
  const open = want === undefined ? $('help').hidden : want;
  $('help').hidden = !open;
  $('helpBtn').setAttribute('aria-expanded', String(open));
}

/* ------------------------------------------------------------------- wire */

function wire() {
  $('playBtn').addEventListener('click', togglePlay);

  $('loopBtn').addEventListener('click', () => {
    state.loop = !state.loop;
    $('loopBtn').setAttribute('aria-pressed', String(state.loop));
    if (state.playing) startAt(playhead());
  });

  $('clearRegion').addEventListener('click', () => {
    state.region = null;
    markRegion();
    if (state.playing) startAt(playhead());
  });

  $('optionsBtn').addEventListener('click', () => {
    const open = $('options').hidden;
    $('options').hidden = !open;
    $('optionsBtn').setAttribute('aria-expanded', String(open));
  });
  $('optionsClose').addEventListener('click', () => {
    $('options').hidden = true;
    $('optionsBtn').setAttribute('aria-expanded', 'false');
  });

  $('helpBtn').addEventListener('click', () => toggleHelp());
  $('helpClose').addEventListener('click', () => toggleHelp(false));

  $('copyLink').addEventListener('click', copyConditions);
  $('blindRecord').addEventListener('click', recordBlindResult);
  $('swapBtn').addEventListener('click', swapRole);
  $('voicePick').addEventListener('click', () => {
    if (paletteOpen()) closePalette(); else openPalette();
  });
  $('voiceFind').addEventListener('input', buildSetPicker);
  // Click-away rather than a scrim: the palette is a control on the header and
  // the page behind it stays live, so a click meant for the transport should
  // reach the transport.
  document.addEventListener('pointerdown', (ev) => {
    if (!paletteOpen()) return;
    if (!$('voicePanel').contains(ev.target) && !$('voicePick').contains(ev.target)) {
      closePalette();
    }
  });

  $('matchRms').addEventListener('change', () => {
    if (state.take) { applyGains(false); renderLevels(); }
  });

  $('blind').addEventListener('change', () => {
    state.blind = $('blind').checked;
    document.body.classList.toggle('blind-on', state.blind);
    // Entering blind mode starts a run, and a run starts with its own result
    // withheld: the tally is what would bias the takes still to come.
    resetBlindReveal();
    reshuffleBlind();
    rebuildVersions();
    renderScore();
    // The reference half of the subject line names the product, which is the
    // one thing blind mode exists to withhold.
    renderSubject();
  });

  for (const b of $('viewToggle').querySelectorAll('button')) {
    b.addEventListener('click', () => setView(b.dataset.view));
  }

  $('bankSort').addEventListener('change', renderBank);
  $('bankFind').addEventListener('input', renderBank);

  // An address names a render and is therefore a request to listen to it —
  // which has to hold for an address arriving at a page that is already open,
  // not only for one the page booted on. A link pasted into a tab sitting on
  // the bank otherwise loaded the set behind the bank and looked like nothing
  // happening at all.
  window.addEventListener('hashchange', async () => {
    const route = readRoute();
    if (route.set) await setView('listen');
    applyRoute(route);
  });

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
        markRegion();
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
      markRegion();
      dragFrom = null;
    });
  }

  document.addEventListener('keydown', onKey);

  const tick = () => {
    if (state.take) {
      $('clock').textContent =
        `${playhead().toFixed(2)} / ${state.take.duration.toFixed(2)}`;
      if (state.playing && !state.loop && playhead() >= state.take.duration - 0.02) pause();
      drawWave();
      drawSpec();
    }
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}

function markRegion() {
  const on = Boolean(state.region);
  $('clearRegion').hidden = !on;
  $('regionSpan').textContent = on
    ? `${state.region[0].toFixed(2)}–${state.region[1].toFixed(2)} s` : '';
}

/* ------------------------------------------------------------------- boot */

async function boot() {
  initLang();
  buildLangToggle();
  state.sets = await (await fetch('sets.json')).json();
  await loadFeedbackIndex();
  wireBank(async (slug) => {
    await setView('listen');
    await loadSet(slug, { set: slug, take: '', ver: '', t: null });
  });
  wireFeedback();
  // Loaded up front rather than on the first switch to the bank: the listening
  // surface's own header line reads from it too.
  await loadBank();

  onLang(() => {
    buildLangToggle();
    refreshListen();
    refreshBank();
    refreshFeedback();
    markRegion();
  });

  if (!state.sets.length) {
    applyStatic();
    fail(t('empty.noRenders'));
    // A fresh clone has no renders and that is the state the bank view is most
    // worth being in: it says which voices exist and which need a reference,
    // none of which requires anything to have been rendered.
    wire();
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
  renderCaptions();
  markRegion();
  // An address names a render and is therefore a request to listen to it; with
  // none, the view is whichever one the last session was left on.
  if (!route.set && localStorage.getItem(VIEW_KEY) === 'bank') await setView('bank');
}

boot().catch((err) => {
  applyStatic();
  fail(t('empty.loadFailed', { msg: err.message }));
});
