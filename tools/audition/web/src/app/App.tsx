/* The page: a header, two sheets, the bank, and the listening surface.
 *
 * Body classes carry what the stylesheets key whole regions on (which view is
 * up, blind, a set with nothing to compare, a run waiting on a render), so
 * they are derived from the store here and nowhere else.
 */

import { useEffect } from 'react';
import { BankView } from '../bank/BankView';
import { FeedbackPanel } from '../feedback/FeedbackPanel';
import { LANGS, langName, setLang, useT } from '../i18n';
import { blindRefusal, setBlind, setMatchRms, setRestartOnSwitch } from '../listen/actions';
import { Takes } from '../listen/Takes';
import { Transport } from '../listen/Transport';
import { NowLine, Versions } from '../listen/Versions';
import { PathLine } from '../path/PathLine';
import { RunPanel } from '../run/RunPanel';
import { useRun } from '../run/run';
import { Scopes } from '../scope/Scopes';
import { setState, useApp, type View } from '../state/store';
import { HeadStage } from '../voice/HeadStage';
import { VoicePalette, VoicePickButton } from '../voice/Palette';
import { Subject } from '../voice/Subject';
import { onHashChange, openVoice, setView } from './boot';
import { onKey, toggleHelp } from './keys';

const VIEW_WORDS = { run: 'view.run', bank: 'view.bank', listen: 'view.listen' } as const;
const VIEW_ORDER: readonly View[] = ['run', 'bank', 'listen'];

function useBodyClasses(): void {
  const view = useApp((x) => x.view);
  const blind = useApp((x) => x.blind);
  const compare = useApp((x) => x.compare);
  const wait = useRun((r) => r.wait);
  useEffect(() => {
    const c = document.body.classList;
    c.toggle('bank-view', view === 'bank');
    c.toggle('run-view', view === 'run');
    c.toggle('blind-on', blind);
    c.toggle('no-compare', !compare);
    c.toggle('run-pending', view === 'run' && Boolean(wait));
  }, [view, blind, compare, wait]);
}

function Header() {
  const { t, lang } = useT();
  const view = useApp((x) => x.view);
  const title = useApp((x) => x.title);
  const failed = useApp((x) => x.failed);
  const helpOpen = useApp((x) => x.helpOpen);
  const optionsOpen = useApp((x) => x.optionsOpen);
  // The manifest title names the set being listened to, which reads as a claim about the bank there.
  const shownTitle = view === 'bank' && !failed ? '' : title;
  return (
    <header>
      <div id="viewToggle" className="toggle views" role="tablist" aria-label={t('view.aria')}>
        {VIEW_ORDER.map((v) => (
          <button
            key={v}
            type="button"
            role="tab"
            data-view={v}
            aria-selected={v === view}
            onClick={() => void setView(v)}
          >
            {t(VIEW_WORDS[v])}
          </button>
        ))}
      </div>
      <VoicePickButton />
      <HeadStage />
      <h1 id="title">{shownTitle}</h1>
      <span className="grow" />
      {/* A list rather than a switch: it holds however many tables ship, each named in its own words. */}
      <select
        id="langPick"
        className="lang-pick"
        aria-label={t('head.lang')}
        value={lang}
        onChange={(ev) => setLang(ev.target.value)}
      >
        {LANGS.map((code) => (
          <option key={code} value={code} lang={code}>
            {langName(code)}
          </option>
        ))}
      </select>
      <button
        id="helpBtn"
        type="button"
        className="ghost"
        aria-expanded={helpOpen}
        aria-controls="help"
        onClick={() => toggleHelp()}
      >
        {t('head.help')}
      </button>
      <button
        id="optionsBtn"
        type="button"
        className="ghost listen-only"
        aria-expanded={optionsOpen}
        aria-controls="options"
        onClick={() => setState({ optionsOpen: !optionsOpen })}
      >
        {t('head.options')}
      </button>
    </header>
  );
}

/** Everything decided once and then left alone; on screen it is one button. */
function OptionsSheet() {
  const { t } = useT();
  const s = useApp((x) => x);
  const refused = blindRefusal(s);
  return (
    <div id="options" className="sheet" hidden={!s.optionsOpen}>
      <div className="opt-col">
        <h2>{t('opt.playback')}</h2>
        <label className="compare-only">
          <input
            type="checkbox"
            id="matchRms"
            autoComplete="off"
            checked={s.matchRms}
            onChange={(e) => setMatchRms(e.target.checked)}
          />
          <span>{t('opt.matchLoudness')}</span> <kbd>M</kbd>
        </label>
        <label className="compare-only">
          <input
            type="checkbox"
            id="restartOnSwitch"
            autoComplete="off"
            checked={s.restartOnSwitch}
            onChange={(e) => setRestartOnSwitch(e.target.checked)}
          />
          <span>{t('opt.restart')}</span> <kbd>S</kbd>
        </label>
        <label className="compare-only" title={refused}>
          <input
            type="checkbox"
            id="blind"
            autoComplete="off"
            checked={s.blind}
            disabled={Boolean(refused)}
            onChange={(e) => setBlind(e.target.checked)}
          />
          <span>{t('opt.blind')}</span> <kbd>B</kbd>
        </label>
      </div>
      <div className="opt-col wide">
        <h2>{t('opt.about')}</h2>
        <p id="notes" className="hint">
          {s.manifest?.notes ?? ''}
        </p>
        <p id="sharedNote" className="hint">
          {s.manifest?.sources_note ?? ''}
        </p>
      </div>
      <div className="sheet-close">
        <button
          id="optionsClose"
          type="button"
          className="ghost"
          onClick={() => setState({ optionsOpen: false })}
        >
          {t('opt.close')}
        </button>
      </div>
    </div>
  );
}

function Keys({ rows }: { rows: [React.ReactNode, string, boolean?][] }) {
  return (
    <dl className="keys">
      {rows.map(([keys, words, compareOnly]) => (
        <div key={words} style={{ display: 'contents' }}>
          <dt className={compareOnly ? 'compare-only' : undefined}>{keys}</dt>
          <dd className={compareOnly ? 'compare-only' : undefined}>{words}</dd>
        </div>
      ))}
    </dl>
  );
}

/** The keys, on every view, reachable from anywhere with `?`. */
function HelpSheet() {
  const { t } = useT();
  const open = useApp((x) => x.helpOpen);
  const k = (...names: string[]) => names.map((n) => <kbd key={n}>{n}</kbd>);
  return (
    <div id="help" className="sheet" hidden={!open}>
      <div className="opt-col">
        <h2>{t('keys.title')}</h2>
        <Keys
          rows={[
            [k('V'), t('keys.voice')],
            [k('space'), t('keys.playPause')],
            [k('↑', '↓'), t('keys.take')],
            [k('tab'), t('keys.swap'), true],
            [
              <>
                <kbd>1</kbd>…<kbd>9</kbd> <kbd>←</kbd>
                <kbd>→</kbd>
              </>,
              t('keys.version'),
              true,
            ],
            [k('home'), t('keys.rewind')],
            [k('L'), t('keys.loop')],
            [k('M'), t('keys.match'), true],
            [k('S'), t('keys.restart'), true],
            [k('B'), t('keys.blind'), true],
            [k('R'), t('keys.reveal'), true],
          ]}
        />
      </div>
      <div className="opt-col">
        <h2>{t('view.bank')}</h2>
        <Keys
          rows={[
            [k('↑', '↓', 'J', 'K'), t('keys.bankMove')],
            [k('home', 'end'), t('keys.bankEnds')],
            [k('enter'), t('keys.bankOpen')],
            [k('/'), t('keys.bankFind')],
          ]}
        />
        <p className="hint">{t('keys.drag')}</p>
      </div>
      <div className="opt-col">
        <h2>{t('view.run')}</h2>
        <Keys
          rows={[
            [k('tab'), t('run.keySwap')],
            [k('1', '2', '3'), t('run.keyAnswer')],
            [k('enter'), t('run.keySend')],
            [k('N'), t('run.skip')],
          ]}
        />
      </div>
      <div className="sheet-close">
        <button id="helpClose" type="button" className="ghost" onClick={() => toggleHelp(false)}>
          {t('opt.close')}
        </button>
      </div>
    </div>
  );
}

export function App() {
  useBodyClasses();
  const view = useApp((x) => x.view);
  useEffect(() => {
    document.addEventListener('keydown', onKey);
    const hash = () => void onHashChange();
    window.addEventListener('hashchange', hash);
    return () => {
      document.removeEventListener('keydown', onKey);
      window.removeEventListener('hashchange', hash);
    };
  }, []);
  return (
    <>
      <Header />
      <VoicePalette onPick={(id) => void openVoice(id)} />
      <OptionsSheet />
      <HelpSheet />
      {view === 'bank' && <BankView onOpen={(slug) => void openVoice(slug)} />}
      <main>
        <Takes />
        <section id="stage">
          <div className="console">
            {view === 'run' && <RunPanel />}
            <Subject />
            <Versions />
            <NowLine />
            <PathLine />
            <Transport />
          </div>
          <div className="stage-body">
            <Scopes />
            <FeedbackPanel />
          </div>
        </section>
      </main>
    </>
  );
}
