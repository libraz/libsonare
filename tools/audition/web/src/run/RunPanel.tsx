/* The run bar: which voice this is in the working order, what it is set
 * against, and the three keys the run needs. Everything the listener would
 * otherwise have chosen is decided by the run. */

import { useEffect, useState } from 'react';
import type { QueueStep } from '../api/schemas';
import { isKey, t, useT } from '../i18n';
import { setVersion } from '../listen/actions';
import { activeKey, roleClass, roleOf, selectedComparison, sourceLabel } from '../state/selectors';
import { useApp } from '../state/store';
import { advance, modelKey, pending, useRun } from './run';

/** A word from the table where it has one, the server's own name where it does not. */
const word = (key: string, raw: string): string => (isKey(key) ? t(key) : raw);

export function placeLabel(step: QueueStep): string {
  const tier = word(`run.tier.${step.tier}`, step.tier);
  return step.group ? `${tier} › ${word(`run.group.${step.group}`, step.group)}` : tier;
}

export function addressLabel(step: QueueStep): string {
  if (step.kit) {
    return t('run.addrKit', { n: step.program });
  }
  if (step.bank) {
    return t('run.addrVariation', { n: step.program + 1, bank: step.bank });
  }
  return t('run.addrProgram', { n: step.program + 1 });
}

const ROLE_WORD = {
  model: 'role.model',
  reference: 'role.reference',
  comparison: 'role.comparison',
} as const;

/** What the library side is set against, said once so it never has to be chosen; each side is its own switch. */
function Pair({ step }: { step: QueueStep }) {
  useT();
  const s = useApp((x) => x);
  if (!s.take || s.setId !== step.set) {
    return <div id="runPair" className="run-pair" />;
  }
  const keys = s.take.keys;
  const oracle = selectedComparison(s)?.oracle_sources.find((k) => keys.includes(k));
  const sounding = activeKey(s);
  const side = (key: string, role: 'model' | 'reference' | 'comparison') => (
    <button
      type="button"
      className={`run-side ${roleClass(role)}`}
      aria-pressed={key === sounding}
      onClick={() => setVersion(keys.indexOf(key), { play: true })}
    >
      <span className="dot" />
      <span>{t(ROLE_WORD[role])}</span>
    </button>
  );
  const oracleRole = oracle ? roleOf(s, oracle) || 'reference' : 'reference';
  return (
    <div id="runPair" className="run-pair">
      {side(modelKey(s), 'model')}
      {s.compare && oracle ? (
        <>
          <span className="run-vs">⇄</span>
          {side(oracle, oracleRole)}
          <span className="run-oracle">{sourceLabel(s, oracle)}</span>
        </>
      ) : (
        <span className="run-solo">{t('run.solo')}</span>
      )}
    </div>
  );
}

const elapsed = (since: number, now: number): string => {
  const sec = Math.max(0, Math.floor((now - since) / 1000));
  return `${Math.floor(sec / 60)}:${String(sec % 60).padStart(2, '0')}`;
};

const WAIT_TITLE = {
  rendering: 'run.wait.rendering',
  failed: 'run.wait.failed',
  done: 'run.wait.done',
} as const;

function WaitPanel() {
  useT();
  const wait = useRun((r) => r.wait);
  const error = useRun((r) => r.error);
  const since = useRun((r) => r.waitSince);
  const step = useRun((r) => r.step);
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    if (wait !== 'rendering') {
      return;
    }
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, [wait]);
  if (!wait) {
    return <div id="runWait" className="run-wait" hidden />;
  }
  let why = '';
  let log = '';
  if (wait === 'failed') {
    why = error;
    log = step?.log ?? '';
  } else if (wait === 'rendering') {
    why = t('run.why.rendering');
    log = t('run.elapsed', { t: elapsed(since, now) });
  } else {
    why = t('run.why.done');
  }
  const kind = wait === 'rendering' ? '' : ` ${wait}`;
  return (
    <div id="runWait" className={`run-wait${kind}`}>
      <span id="runSpin" className="run-spin" aria-hidden="true" />
      <div className="run-wait-text">
        <p id="runWaitTitle" className="run-wait-title">
          {t(WAIT_TITLE[wait])}
        </p>
        <p id="runWaitWhy" className="run-wait-why">
          {why}
        </p>
        <code id="runWaitLog" className="run-wait-log">
          {log}
        </code>
      </div>
    </div>
  );
}

export function RunPanel() {
  useT();
  const step = useRun((r) => r.step);
  const total = useRun((r) => r.total);
  const wait = useRun((r) => r.wait);
  const queue = useApp((x) => x.queue);
  const left = pending(queue);
  const at = Math.max(1, total - left.length + (step ? 1 : 0));
  const fill = total ? `${Math.min(100, ((at - 1) / total) * 100)}%` : '0';
  const next = step ? left.find((x) => x.set !== step.set) : undefined;
  let nextLine = '';
  if (step) {
    nextLine = next ? t('run.next', { name: next.name }) : t('run.last');
  }
  return (
    <section id="run" className="run run-only" aria-live="polite">
      <div className="run-rail" aria-hidden="true">
        <span id="runFill" className="run-fill" style={{ width: fill }} />
      </div>
      <div className="run-head">
        <div className="run-who">
          <span id="runPlace" className="run-place">
            {step ? `${placeLabel(step)}  ·  ${addressLabel(step)}` : ''}
          </span>
          <h2 id="runName" className="run-name">
            {step ? step.name : t('run.doneTitle')}
          </h2>
        </div>
        <span className="grow" />
        <span id="runCount" className="run-count num">
          {step ? t('run.count', { at, of: total }) : ''}
        </span>
        <button
          id="runSkip"
          type="button"
          className="ghost run-skip"
          title={t('run.skipTitle')}
          hidden={!step}
          onClick={() => void advance()}
        >
          {t('run.skip')}
        </button>
      </div>
      {step && <Pair step={step} />}
      <div id="runKeys" className="run-foot" hidden={Boolean(wait)}>
        <ol className="run-keys">
          <li>
            <kbd>Tab</kbd>
            <span>{t('run.keySwap')}</span>
          </li>
          <li>
            <kbd>1</kbd>
            <kbd>2</kbd>
            <kbd>3</kbd>
            <span>{t('run.keyAnswer')}</span>
          </li>
          <li>
            <kbd>Enter</kbd>
            <span>{t('run.keySend')}</span>
          </li>
        </ol>
        <p id="runNext" className="run-next">
          {nextLine}
        </p>
      </div>
      <WaitPanel />
    </section>
  );
}
