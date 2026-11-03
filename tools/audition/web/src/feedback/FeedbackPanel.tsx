/* The whole listening-note panel: the oracle chip, the narrowing, the comment box, the send row and the log. */

import { useEffect, useRef } from 'react';
import { z } from 'zod';
import type { FeedbackEntry } from '../api/schemas';
import { currentLang, isKey, type TKey, useT } from '../i18n';
import { reloadSet } from '../listen/actions';
import { comparedAgainst } from '../listen/note';
import { oracleSources, roleClass, roleOf, sourceLabel } from '../state/selectors';
import { getState, setState, useApp } from '../state/store';
import {
  answerAt,
  back,
  chooseOracle,
  clearStatus,
  composerDone,
  labelOf,
  resetComposer,
  send,
  setComment,
  undo,
} from './actions';
import { useFb } from './store';
import { questionOf, TREE } from './tree';

const GRADES: Record<string, TKey> = {
  ok: 'grade.ok',
  acceptable: 'grade.acceptable',
  'wrong-instrument': 'grade.wrongInstrument',
  broken: 'grade.broken',
  off: 'grade.off',
  'off/unsure': 'grade.unsure',
};

const roleName = (t: (k: TKey) => string, role: string): string => {
  const key = `role.${role}`;
  return isKey(key) ? t(key) : role;
};

/* What the log reads out of a stored note; the rest of it is the server's. */
const EntryConditions = z.looseObject({
  version: z.string().nullish(),
  take: z.string().nullish(),
  blind: z.boolean().nullish(),
});
const Oracle = z.looseObject({
  role: z.string(),
  label: z.string().nullish(),
  version: z.string().nullish(),
});
const Said = z.array(z.looseObject({ said: z.string().nullish() }));

function clock(iso: string | undefined): string {
  if (!iso) {
    return '';
  }
  const d = new Date(iso);
  return Number.isNaN(d.getTime())
    ? iso
    : d.toLocaleString(currentLang(), {
        month: 'short',
        day: 'numeric',
        hour: '2-digit',
        minute: '2-digit',
      });
}

function GradeChip({ name }: { name: string }) {
  const { t } = useT();
  const tone = name === 'ok' ? 'good' : name === 'acceptable' ? 'mid' : 'bad';
  const key = GRADES[name];
  return <span className={`fb-grade ${tone}`}>{key ? t(key) : name}</span>;
}

/** A saved note's oracle: recorded, explicitly none, or unknown (predates the field). */
function SavedOracle({ conditions }: { conditions: Record<string, unknown> }) {
  const { t } = useT();
  if (!Object.hasOwn(conditions, 'compared_against')) {
    return (
      <span className={`fb-cmp ${roleClass('')}`} title={t('cmp.unknownTitle')}>
        <span className="dot" />
        <span>{t('cmp.unknown')}</span>
      </span>
    );
  }
  const ca = Oracle.nullable().safeParse(conditions.compared_against);
  if (!ca.success || !ca.data) {
    return (
      <span className={`fb-cmp ${roleClass('')}`}>
        <span className="dot" />
        <span>{t('cmp.unplayed')}</span>
      </span>
    );
  }
  return (
    <span className={`fb-cmp ${roleClass(ca.data.role)}`} title={roleName(t, ca.data.role)}>
      <span className="dot" />
      <span>{ca.data.label || ca.data.version}</span>
    </span>
  );
}

function Entry({ entry }: { entry: FeedbackEntry }) {
  const { t } = useT();
  const c = EntryConditions.safeParse(entry.conditions ?? {});
  const cond = c.success ? c.data : {};
  const said = (Said.safeParse(entry.answers ?? []).data ?? []).map((a) => a.said).filter(Boolean);
  const s = getState();
  return (
    <div className="fb-entry">
      <div className="fb-entry-head">
        {entry.grade ? <GradeChip name={entry.grade} /> : null}
        {cond.version ? (
          <span
            className={`fb-who ${roleClass(roleOf(s, cond.version))}`}
            title={sourceLabel(s, cond.version)}
          >
            <span className="dot" />
            <span>{cond.version}</span>
          </span>
        ) : cond.blind ? (
          <span className={`fb-who ${roleClass('')}`}>{t('now.hidden')}</span>
        ) : null}
        <SavedOracle conditions={entry.conditions ?? {}} />
        {cond.take ? <span className="fb-take">{cond.take}</span> : null}
        <span className="grow" />
        <span className="fb-when">{clock(entry.at)}</span>
      </div>
      {said.length ? <p className="fb-said">{said.join('  ›  ')}</p> : null}
      {entry.text ? <p className="fb-text">{entry.text}</p> : null}
      {entry.tag ? <code className="fb-tag">{entry.tag}</code> : null}
    </div>
  );
}

function ComparedAgainst() {
  const { t } = useT();
  const override = useApp((s) => s.oracleOverride);
  // Read so the chip and the select follow the take, the manifest and the last oracle.
  useApp((s) => s.lastOracle);
  useApp((s) => s.manifest);
  useApp((s) => s.take);
  const s = getState();
  const ca = comparedAgainst(s);
  return (
    <div className="fb-cmp-row">
      <span className="lab">{t('cmp.label')}</span>
      <span
        id="cmpChip"
        className={`cmp-chip ${roleClass(ca?.role ?? '')}${ca ? '' : ' warn'}`}
        title={
          ca
            ? ca.chosen === 'manual'
              ? t('cmp.manualTitle')
              : t('cmp.autoTitle')
            : t('cmp.unplayedTitle')
        }
      >
        <span className="dot" />
        {ca ? (
          <>
            <span className="cmp-name">{ca.label}</span>
            <span className="cmp-role">({roleName(t, ca.role)})</span>
          </>
        ) : (
          <span>{t('cmp.unplayed')}</span>
        )}
      </span>
      <select
        id="cmpOverride"
        className="cmp-override"
        title={t('cmp.overrideTitle')}
        value={override?.key ?? ''}
        onChange={(ev) => chooseOracle(ev.target.value)}
      >
        <option value="">{t('cmp.overrideAuto')}</option>
        {oracleSources(s).map((src) => (
          <option key={src.key} value={src.key}>
            {`${roleName(t, src.role)}: ${src.label}`}
          </option>
        ))}
      </select>
    </div>
  );
}

function Narrowing() {
  const { t } = useT();
  const node = useFb((f) => f.node);
  const trail = useFb((f) => f.trail);
  const solo = useApp((s) => !s.compare);
  const question = useRef<HTMLParagraphElement>(null);
  const answered = useRef(0);
  // The panel grows as it is answered; a question that scrolls off under the console goes unnoticed.
  useEffect(() => {
    if (trail.length > answered.current) {
      question.current?.scrollIntoView?.({ block: 'nearest' });
    }
    answered.current = trail.length;
  }, [trail.length]);
  const choices = node ? (TREE.nodes[node]?.a ?? []) : [];
  return (
    <div className="fb-ask">
      <div className="fb-trail-row">
        <span id="fbTrail" className="fb-trail">
          {trail.map((step, i) => (
            // biome-ignore lint/suspicious/noArrayIndexKey: a trail may repeat a node and tag; its order is its identity
            <span key={i} style={{ display: 'contents' }}>
              {i ? <span className="fb-sep">›</span> : null}
              <span className="fb-step">{labelOf(step)}</span>
            </span>
          ))}
        </span>
        <button id="fbBack" type="button" className="ghost" hidden={!trail.length} onClick={back}>
          {t('fb.back')}
        </button>
        <button
          id="fbRestart"
          type="button"
          className="ghost"
          hidden={!trail.length}
          onClick={resetComposer}
        >
          {t('fb.restart')}
        </button>
      </div>
      <p id="fbQuestion" ref={question} className="fb-q" hidden={!node}>
        {node ? questionOf(node, solo) : ''}
      </p>
      <div id="fbChoices" className="fb-choices">
        {node
          ? [
              ...choices.map((c, i) => (
                <button key={c.tag} type="button" className="fb-choice" onClick={() => answerAt(i)}>
                  <kbd className="fb-key">{i + 1}</kbd>
                  <span>{labelOf({ node, tag: c.tag })}</span>
                </button>
              )),
              <button
                key="unsure"
                type="button"
                className="fb-choice fb-unsure"
                onClick={() => answerAt(choices.length)}
              >
                <kbd className="fb-key">{choices.length + 1}</kbd>
                <span>{t('fb.notSure')}</span>
              </button>,
            ]
          : null}
      </div>
    </div>
  );
}

function SendRow() {
  const { t } = useT();
  const attach = useApp((s) => s.attach);
  const canSend = useApp((s) => s.canSend);
  const running = useApp((s) => s.view === 'run');
  const status = useFb((f) => f.status);
  const stale = useFb((f) => f.stale);
  const done = useFb((f) => composerDone(f));
  return (
    <div className="fb-send-row">
      <label className="fb-attach" title={t('fb.attachWhat')}>
        <input
          type="checkbox"
          id="fbAttach"
          autoComplete="off"
          checked={attach}
          onChange={(ev) => setState({ attach: ev.target.checked })}
        />
        <span>{t('fb.attach')}</span>
      </label>
      <span className="grow" />
      <span id="fbStatus" className={`fb-status${status?.bad ? ' bad' : ''}`}>
        {status ? t(status.key, status.vars) : ''}
      </span>
      <button
        id="fbReload"
        type="button"
        className="ghost"
        hidden={stale === null}
        onClick={() => {
          clearStatus();
          void reloadSet();
        }}
      >
        {t('fb.reload')}
      </button>
      <button
        id="fbSend"
        type="button"
        className={`send${done ? ' ready' : ''}`}
        disabled={!canSend}
        onClick={() => void send()}
      >
        {running ? t('run.keySend') : t('fb.send')}
      </button>
    </div>
  );
}

function Log() {
  const { t } = useT();
  const entries = useFb((f) => f.entries);
  const path = useFb((f) => f.path);
  const setId = useApp((s) => s.setId);
  const owned = useFb((f) => (f.mine[setId ?? ''] ?? []).length > 0);
  return (
    <div className="fb-log">
      <div className="fb-log-head">
        <span className="lab">{t('fb.recent')}</span>
        <span className="grow" />
        <button
          id="fbUndo"
          type="button"
          className="ghost"
          disabled={!owned}
          onClick={() => void undo()}
        >
          {t('fb.undo')}
        </button>
      </div>
      <div id="fbRecent">
        {entries.length ? (
          [...entries]
            .reverse()
            .map((entry, i) => <Entry key={entry.id ?? `n${i}`} entry={entry} />)
        ) : (
          <p className="hint">{t('fb.none')}</p>
        )}
      </div>
      <p id="fbWhere" className="fb-where">
        {path ? t('fb.where', { path }) : ''}
      </p>
    </div>
  );
}

export function FeedbackPanel() {
  const { t } = useT();
  const comment = useFb((f) => f.comment);
  return (
    <section className="fb">
      <div className="fb-head">
        <h2>{t('fb.title')}</h2>
        <p className="hint">{t('fb.intro')}</p>
      </div>
      <ComparedAgainst />
      <Narrowing />
      <label className="fb-comment">
        <span className="lab">{t('fb.comment')}</span>
        <textarea
          id="fbComment"
          rows={3}
          placeholder={t('fb.commentPlaceholder')}
          value={comment}
          onChange={(ev) => setComment(ev.target.value)}
          onKeyDown={(ev) => {
            // Sending is the one shortcut worth having, and it has to be modified: the box is where a sentence gets typed.
            if ((ev.metaKey || ev.ctrlKey) && ev.key === 'Enter') {
              ev.preventDefault();
              void send();
            }
          }}
        />
      </label>
      <SendRow />
      <Log />
    </section>
  );
}
