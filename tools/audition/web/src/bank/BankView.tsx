/* The bank: every GM program and GS variation the library voices, whatever has been rendered.
 *
 * Three bands: the masthead is the overview (totals and the distribution across the six stages),
 * the toolbar narrows, and the table lists beside an inspector that holds what a row cannot --
 * the coverage denominator, the dimensions outside the reference spread, the ladder in full.
 * Opening a voice is handed back through `onOpen`: this file is the bank and knows nothing of the
 * transport. Data is `tools/voice-status.json` as served in `bank.json`.
 */

import { type KeyboardEvent, useEffect, useMemo, useRef } from 'react';
import { useT } from '../i18n';
import { shipsOf } from '../state/selectors';
import { getState as appState, useApp } from '../state/store';
import {
  HeardChip,
  MethodTag,
  ShipChip,
  SignoffChip,
  SlotTag,
  StageBar,
  stageColor,
} from '../voice/chips';
import {
  addressOf,
  type Method,
  oneDecimal,
  readShips,
  type SlotKind,
  slotLong,
  targetShort,
  targetState,
  targetWords,
  word,
} from '../voice/identity';
import {
  type BankFilter,
  bankStats,
  cursorIndex,
  type PageContext,
  pageContext,
  RUNGS,
  type Rung,
  type SortKey,
  shownVoices,
  stageIndex,
} from './logic';
import type { BankVoiceRow } from './schema';
import { currentRow, currentShown, getBank, setCursor, setQuery, useBank } from './store';

const METHOD_PICKS: [Method | '', 'bank.any' | 'bank.physical' | 'bank.fm' | 'bank.classic'][] = [
  ['', 'bank.any'],
  ['physical', 'bank.physical'],
  ['fm', 'bank.fm'],
  ['classic', 'bank.classic'],
];

/** Where on the map a slot sits. One axis, so a segmented control. */
const SLOT_PICKS: [
  SlotKind | '',
  'bank.any' | 'slot.short.gm' | 'slot.short.gs' | 'slot.short.gm2' | 'slot.short.kit',
][] = [
  ['', 'bank.any'],
  ['gm', 'slot.short.gm'],
  ['gs', 'slot.short.gs'],
  ['gm2', 'slot.short.gm2'],
  ['kit', 'slot.short.kit'],
];

const FILTER_PICKS: [
  BankFilter,
  'bank.fHasOracle' | 'bank.fNeedsOracle' | 'bank.fUnwritten' | 'bank.fRendered',
][] = [
  ['oracle', 'bank.fHasOracle'],
  ['no-oracle', 'bank.fNeedsOracle'],
  ['unwritten', 'bank.fUnwritten'],
  ['page', 'bank.fRendered'],
];

const SORT_PICKS: [SortKey, 'bank.sortAddress' | 'bank.sortStage' | 'bank.sortName'][] = [
  ['address', 'bank.sortAddress'],
  ['stage', 'bank.sortStage'],
  ['name', 'bank.sortName'],
];

const NONE = '—';

/** Hands a row back to the caller; set by the mounted view so `bankKey` can open from anywhere. */
let openVoiceFn: (slug: string) => void = () => {};

function openRow(v: BankVoiceRow | null | undefined): void {
  if (v && pageContext(appState().sets).hasPage(v.slug)) {
    openVoiceFn(v.slug);
  }
}

function moveCursor(delta: number): void {
  const shown = currentShown();
  if (!shown.length) {
    return;
  }
  const at = cursorIndex(shown, getBank().cursor);
  setCursor(shown[(at + delta + shown.length) % shown.length]?.slug ?? null);
}

const isTyping = (target: EventTarget | null): boolean =>
  target instanceof HTMLInputElement ||
  target instanceof HTMLTextAreaElement ||
  target instanceof HTMLSelectElement;

/** Handles ↑ ↓ J K, Home/End, Enter and `/` for the bank; true when the key was taken. */
export function bankKey(ev: globalThis.KeyboardEvent): boolean {
  if (ev.defaultPrevented || ev.ctrlKey || ev.metaKey || ev.altKey || isTyping(ev.target)) {
    return false;
  }
  const take = (fn: () => void) => {
    ev.preventDefault();
    fn();
    return true;
  };
  switch (ev.key) {
    case '/':
      return take(() => document.getElementById('bankFind')?.focus());
    case 'ArrowDown':
    case 'j':
      return take(() => moveCursor(1));
    case 'ArrowUp':
    case 'k':
      return take(() => moveCursor(-1));
    case 'Home':
      return take(() => setCursor(currentShown()[0]?.slug ?? null));
    case 'End':
      return take(() => setCursor(currentShown().at(-1)?.slug ?? null));
    case 'Enter':
      return take(() => openRow(currentRow()));
    default:
      return false;
  }
}

/** A proportional bar over a set of voices; segments carry a floor width so one in 188 shows. */
function DistBar({
  voices,
  className,
  ariaHidden,
}: {
  voices: BankVoiceRow[];
  className?: string;
  ariaHidden?: boolean;
}) {
  const { t } = useT();
  const total = voices.length || 1;
  return (
    <span className={className} aria-hidden={ariaHidden}>
      {RUNGS.map((i) => {
        const n = voices.filter((v) => stageIndex(v) === i).length;
        if (!n) {
          return null;
        }
        return (
          <span
            key={i}
            style={{ flex: `${n} 1 0`, background: stageColor(i) }}
            title={`${n} ${t(`stage.${i}`)} (${Math.round((n / total) * 100)}%)`}
          />
        );
      })}
    </span>
  );
}

function Figure({
  label,
  value,
  onClick,
  warn,
}: {
  label: string;
  value: number;
  onClick?: () => void;
  warn?: boolean;
}) {
  return (
    <button
      type="button"
      className={`fig${onClick ? ' act' : ''}${warn ? ' warn' : ''}`}
      disabled={!onClick}
      onClick={onClick}
    >
      <span className="num">{value}</span>
      <span className="lab">{label}</span>
    </button>
  );
}

function Masthead({ ctx }: { ctx: PageContext }) {
  const { t, lang } = useT();
  const voices = useBank((s) => s.voices);
  const filter = useBank((s) => s.query.filter);
  const stage = useBank((s) => s.query.stage);
  const st = bankStats(voices, ctx);
  const pick = (name: BankFilter, n: number) =>
    n ? () => setQuery({ filter: filter === name ? '' : name }) : undefined;
  return (
    <div className="bank-head">
      <div id="bankFigures" className="figures">
        <Figure label={t('bank.voices')} value={st.voices} />
        <Figure label={t('bank.withOracle')} value={st.withOracle} onClick={pick('oracle', 1)} />
        <Figure
          label={t('bank.unwritten')}
          value={st.unwritten}
          onClick={pick('unwritten', st.unwritten)}
          warn={st.unwritten > 0}
        />
        <Figure label={t('bank.rendered')} value={st.pages} onClick={pick('page', st.pages)} />
        <Figure
          label={t('bank.pagesCurrent')}
          value={st.current}
          onClick={pick('current', st.current)}
        />
        <Figure
          label={t('bank.pagesStale')}
          value={st.stale}
          onClick={pick('stale', st.stale)}
          warn={st.stale > 0}
        />
        <Figure
          label={t('bank.aimInstrument')}
          value={st.aimInstrument}
          onClick={pick('t-instrument', st.aimInstrument)}
        />
        <Figure
          label={t('bank.aimMachine')}
          value={st.aimMachine}
          onClick={pick('t-machine', st.aimMachine)}
        />
      </div>
      <div className="dist">
        <DistBar voices={voices} className="dist-bar" ariaHidden />
        {/* biome-ignore lint/a11y/useSemanticElements: the stylesheet styles these groups by class on a div */}
        <div id="bankStageKeys" className="dist-keys" role="group" aria-label={t('bank.colStage')}>
          {RUNGS.map((i) => {
            const n = st.perStage[i] ?? 0;
            return (
              <button
                key={i}
                type="button"
                className="skey"
                aria-pressed={stage === i}
                title={`${oneDecimal(i / 5, lang)} — ${t(`stage.${i}.of`)}`}
                onClick={() => setQuery({ stage: stage === i ? null : (i as Rung) })}
              >
                <span className="dot" style={{ background: stageColor(i) }} />
                <span>{t(`stage.${i}`)}</span>
                <span className={n ? 'n' : 'n z'}>{n}</span>
              </button>
            );
          })}
        </div>
      </div>
    </div>
  );
}

function Toolbar({ shown, total }: { shown: number; total: number }) {
  const { t } = useT();
  const q = useBank((s) => s.query);
  return (
    <div className="bank-bar">
      {/* biome-ignore lint/a11y/useSemanticElements: the stylesheet styles these groups by class on a div */}
      <div id="bankSlot" className="seg4" role="group" aria-label={t('bank.slot')}>
        {SLOT_PICKS.map(([value, key]) => (
          <button
            key={value}
            type="button"
            data-slot={value}
            title={value ? t(`slot.${value}`) : undefined}
            aria-pressed={q.slot === value}
            onClick={() => setQuery({ slot: value })}
          >
            {t(key)}
          </button>
        ))}
      </div>
      {/* biome-ignore lint/a11y/useSemanticElements: the stylesheet styles these groups by class on a div */}
      <div id="bankMethod" className="seg4" role="group" aria-label={t('bank.method')}>
        {METHOD_PICKS.map(([value, key]) => (
          <button
            key={value}
            type="button"
            data-method={value}
            aria-pressed={q.method === value}
            onClick={() => setQuery({ method: value })}
          >
            {t(key)}
          </button>
        ))}
      </div>
      {/* biome-ignore lint/a11y/useSemanticElements: the stylesheet styles these groups by class on a div */}
      <div id="bankFilter" className="chips" role="group" aria-label={t('bank.filter')}>
        {FILTER_PICKS.map(([value, key]) => (
          <button
            key={value}
            type="button"
            className="chip"
            data-filter={value}
            aria-pressed={q.filter === value}
            onClick={() => setQuery({ filter: q.filter === value ? '' : value })}
          >
            {t(key)}
          </button>
        ))}
      </div>
      <label className="pick grow">
        <span className="pick-lab">{t('bank.find')}</span>
        <input
          id="bankFind"
          type="search"
          value={q.find}
          placeholder={t('bank.findPlaceholder')}
          aria-label={t('bank.find')}
          onChange={(ev) => setQuery({ find: ev.target.value })}
        />
      </label>
      <label className="pick">
        <span className="pick-lab">{t('bank.sort')}</span>
        <select
          id="bankSort"
          value={q.sort}
          aria-label={t('bank.sort')}
          onChange={(ev) => setQuery({ sort: ev.target.value as SortKey })}
        >
          {SORT_PICKS.map(([value, key]) => (
            <option key={value} value={value}>
              {t(key)}
            </option>
          ))}
        </select>
      </label>
      <span id="bankCount" className="mono">
        {total ? `${shown} / ${total}` : ''}
      </span>
    </div>
  );
}

/* What the slot is aimed at and what answers it, in one cell: the aim in words, then the capture
 * in the reference colour where on target, the warning colour where not, quiet where there is
 * none -- most voices have none, and a hundred loud absences would bury the ones that say something. */
function TargetCell({ v }: { v: BankVoiceRow }) {
  const { t } = useT();
  const tg = v.target;
  const st = tg?.state || (v.capture ? 'aimed' : 'uncaptured');
  const off = st === 'off-target' || st === 'unclassified';
  const title = [
    targetWords(tg),
    word(`tst.${st}`, st),
    tg?.approximated ? t('tgt.approximated', { slot: tg.approximated.answered_by }) : '',
  ]
    .filter(Boolean)
    .join(' — ');
  return (
    <span className="target-cell" title={title}>
      <span className="aim">{targetShort(tg) || NONE}</span>
      <span className={`oracle${v.capture ? '' : ' absent'}${off ? ' off' : ''}`}>
        {v.capture || word(`tst.${st}`, st)}
      </span>
    </span>
  );
}

function BankRow({
  v,
  voices,
  active,
  ctx,
  onSelect,
}: {
  v: BankVoiceRow;
  voices: BankVoiceRow[];
  active: boolean;
  ctx: PageContext;
  onSelect: (slug: string) => void;
}) {
  const { t, lang } = useT();
  const ships = useApp((s) => shipsOf(s, v.slug));
  const open = v.open_candidates;
  return (
    // biome-ignore lint/a11y/useKeyWithClickEvents: keys are handled by the listbox that owns the cursor
    <div
      id={`v-${v.slug}`}
      className="bank-row"
      role="option"
      tabIndex={-1}
      aria-selected={active}
      aria-current={active ? 'true' : undefined}
      onClick={() => onSelect(v.slug)}
      onDoubleClick={() => openRow(v)}
    >
      <span className="addr">{addressOf(v)}</span>
      <SlotTag voice={v} voices={voices} short />
      <span className="who">
        {v.name}
        {v.patch && <span className="patch">{`  ${v.patch}`}</span>}
      </span>
      <span className="engine">
        <MethodTag engine={v.engine} short />
        <span>{v.engine || t('bank.notReported')}</span>
      </span>
      <span className="stage" title={`${t(`stage.${stageIndex(v)}`)} — ${v.next}`}>
        <StageBar stage={v.stage} />
        <span>{oneDecimal(v.stage, lang)}</span>
      </span>
      <TargetCell v={v} />
      <span className="page-cell">{ctx.hasPage(v.slug) && <ShipChip ships={ships} short />}</span>
      <HeardChip id={v.slug} />
      <SignoffChip voice={v} />
      <span className="bank-tail">
        {open.length > 0 && (
          <span className="badge warn" title={open.join(', ')}>
            {t('bank.nUnwritten', { n: open.length })}
          </span>
        )}
        {/* Only past the oracle step: below it every voice says one sentence and 150 copies bury the rest. */}
        {v.stage > 0.2 && v.stage < 1 && <span className="next">{v.next}</span>}
      </span>
    </div>
  );
}

function Ladder({ here }: { here: Rung | null }) {
  const { t, lang } = useT();
  return (
    <div className="ladder">
      {RUNGS.map((i) => {
        let cls = 'rung';
        if (here === null || i < here) {
          cls = 'rung done';
        } else if (i === here) {
          cls = 'rung here';
        }
        return (
          <div key={i} className={cls} title={t(`stage.${i}.of`)}>
            <span className="mark">{i === here ? '▸' : '·'}</span>
            <span className="at">{oneDecimal(i / 5, lang)}</span>
            <span>{t(`stage.${i}`)}</span>
          </div>
        );
      })}
    </div>
  );
}

function KeyHint() {
  const { t } = useT();
  return (
    <p className="key-hint">
      {`↑ ↓ ${t('keys.bankMove')}   enter ${t('keys.bankOpen')}   / ${t('keys.bankFind')}`}
    </p>
  );
}

function Fact({
  term,
  value,
  aside,
  warn,
}: {
  term: string;
  value: string;
  aside?: string;
  warn?: boolean;
}) {
  return (
    <>
      <dt>{term}</dt>
      <dd className={warn ? 'warn' : undefined}>
        {value}
        {aside && <span className="aside">{`  ${aside}`}</span>}
      </dd>
    </>
  );
}

function Legend() {
  const { t } = useT();
  const methods: [Method, string | undefined][] = [
    ['physical', 'piano'],
    ['fm', 'fm'],
    ['classic', 'subtractive'],
  ];
  return (
    <>
      <span className="lab">{t('bank.ladder')}</span>
      <Ladder here={null} />
      <span className="lab">{t('bank.methodLegend')}</span>
      <div className="legend">
        {methods.map(([m, engine]) => (
          <div key={m}>
            <MethodTag engine={engine} />
            <span>{t(`method.${m}.legend`)}</span>
          </div>
        ))}
      </div>
      <p className="hint">{t('bank.selectOne')}</p>
      <KeyHint />
    </>
  );
}

/* The voice's identity in the words of the listening surface's strip: where it sits on the map,
 * how it is made, what it is aimed at, what ships, and whether its page still plays that. */
function IdentityFacts({
  v,
  voices,
  ctx,
}: {
  v: BankVoiceRow;
  voices: BankVoiceRow[];
  ctx: PageContext;
}) {
  const { t } = useT();
  const ships = useApp((s) => shipsOf(s, v.slug));
  const tg = v.target;
  const st = targetState(tg);
  const rendered = readShips(ships)?.rendered;
  return (
    <dl className="facts idfacts">
      <dt>{t('id.map')}</dt>
      <dd>
        <SlotTag voice={v} voices={voices} />
        <span className="aside">{`  ${slotLong(v, voices)}`}</span>
      </dd>
      <dt>{t('id.method')}</dt>
      <dd>
        <MethodTag engine={v.engine} />
        <span className="aside">{`  ${v.engine || t('bank.notReported')}`}</span>
      </dd>
      <dt>{t('id.target')}</dt>
      <dd>
        {targetWords(tg) || NONE}
        {st && (
          <>
            {' '}
            <span className="idstate" data-state={st.key} title={st.title}>
              {st.text}
            </span>
          </>
        )}
        {tg?.approximated && (
          <span className="aside warn">
            {`  ${t('tgt.approximated', { slot: tg.approximated.answered_by })}`}
          </span>
        )}
      </dd>
      {v.ships && (
        <>
          <dt>{t('id.shipsNow')}</dt>
          <dd>
            {t('ship.versionOf', { unit: v.ships.unit, v: v.ships.version, date: v.ships.date })}
          </dd>
        </>
      )}
      {ctx.hasPage(v.slug) && (
        <>
          <dt>{t('id.page')}</dt>
          <dd>
            <ShipChip ships={ships} />
            {rendered && (
              <span className="aside">{`  ${t('ship.rendered', { date: rendered })}`}</span>
            )}
          </dd>
        </>
      )}
    </dl>
  );
}

function InspectVoice({
  v,
  voices,
  ctx,
}: {
  v: BankVoiceRow;
  voices: BankVoiceRow[];
  ctx: PageContext;
}) {
  const { t, lang } = useT();
  const here = stageIndex(v);
  const ax = v.axes;
  const cov = ax?.coverage;
  const ag = ax?.agreement;
  const outside = Object.entries(ag?.outside ?? {}).map(([d, m]) => `${d} ${m}×`);
  const open = v.open_candidates;
  return (
    <>
      <h2>{v.name}</h2>
      <p className="addr-line">
        {[addressOf(v), v.patch, v.tone_class].filter(Boolean).join('  ·  ')}
      </p>
      <IdentityFacts v={v} voices={voices} ctx={ctx} />
      <span className="lab">
        {t('bank.stageOf', { n: oneDecimal(v.stage, lang), name: t(`stage.${here}`) })}
      </span>
      {/* Names only, the rung marked; the predicate is the tooltip and the next move is spelled out below. */}
      <Ladder here={here} />
      <span className="lab">{t('bank.measurement')}</span>
      <dl className="facts">
        {v.capture ? (
          <Fact
            term={t('bank.oracle')}
            value={v.capture}
            aside={t('bank.timbres', {
              timbres: ax?.timbres ?? 0,
              rows: ax?.profile_rows ?? 0,
              gate: ax?.gate_state || t('bank.gateNotRecorded'),
            })}
          />
        ) : (
          <Fact term={t('bank.oracle')} value={t('bank.notCaptured')} warn />
        )}
        {cov?.canonical ? (
          <>
            <Fact
              term={t('bank.coverage')}
              value={t('bank.coverageOf', { gated: cov.gated, canonical: cov.canonical })}
              aside={
                cov.excused.length
                  ? t('bank.excused', { n: cov.excused.length, list: cov.excused.join(', ') })
                  : ''
              }
              warn={!cov.complete}
            />
            {cov.gaps.length > 0 && (
              <Fact
                term={t('bank.gaps')}
                value={String(cov.gaps.length)}
                aside={cov.gaps.join(', ')}
                warn
              />
            )}
          </>
        ) : null}
        {ag?.total ? (
          <Fact
            term={t('bank.agreement')}
            value={t('bank.agreementOf', { inside: ag.inside, total: ag.total })}
            aside={outside.join(', ')}
          />
        ) : (
          ag &&
          ag.unjudgeable.length > 0 && (
            <Fact
              term={t('bank.agreement')}
              value={t('bank.noSpread')}
              aside={t('bank.unjudgeable', { n: ag.unjudgeable.length })}
              warn
            />
          )
        )}
        {open.length > 0 && (
          <Fact
            term={t('bank.unwritten')}
            value={String(open.length)}
            aside={open.join(', ')}
            warn
          />
        )}
      </dl>
      <span className="lab">{t('bank.next')}</span>
      <p className="next-text">{v.next}</p>
      {ctx.hasPage(v.slug) && (
        <button type="button" className="go" onClick={() => openRow(v)}>
          {t('bank.listen')}
        </button>
      )}
      <KeyHint />
    </>
  );
}

export function BankView({ onOpen }: { onOpen: (slug: string) => void }) {
  const { t, lang } = useT();
  const voices = useBank((s) => s.voices);
  const query = useBank((s) => s.query);
  const cursor = useBank((s) => s.cursor);
  const sets = useApp((s) => s.sets);
  const rowsRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    openVoiceFn = onOpen;
  }, [onOpen]);

  const ctx = useMemo(() => pageContext(sets), [sets]);
  const shown = useMemo(() => shownVoices(voices, query, ctx, lang), [voices, query, ctx, lang]);
  const at = cursorIndex(shown, cursor);
  const current = shown[at] ?? null;
  const currentSlug = current?.slug ?? null;

  // Keyboard moves keep the cursor row in view; a filter that leaves it in place does not scroll.
  useEffect(() => {
    if (currentSlug) {
      rowsRef.current
        ?.querySelector(`[id="v-${currentSlug}"]`)
        ?.scrollIntoView?.({ block: 'nearest' });
    }
  }, [currentSlug]);

  const onKeyDown = (ev: KeyboardEvent) => {
    if (bankKey(ev.nativeEvent)) {
      ev.stopPropagation();
    }
  };

  const select = (slug: string) => {
    rowsRef.current?.focus();
    setCursor(slug);
  };

  // The GM families are the address order's own headings; under another sort they would interleave.
  const grouped = query.sort === 'address';
  let group: string | null = null;

  return (
    // biome-ignore lint/a11y/noStaticElementInteractions: the bank owns its keys while it has focus
    <section id="bank" className="bank" onKeyDown={onKeyDown}>
      {voices.length > 0 && <Masthead ctx={ctx} />}
      <Toolbar shown={shown.length} total={voices.length} />
      <div className="bank-body">
        <div className="bank-table">
          <div className="bank-cols" aria-hidden="true">
            <span>{t('bank.colProg')}</span>
            <span>{t('bank.colSlot')}</span>
            <span>{t('bank.colVoice')}</span>
            <span>{t('bank.colEngine')}</span>
            <span>{t('bank.colStage')}</span>
            <span>{t('bank.colOracle')}</span>
            <span>{t('bank.colPage')}</span>
            <span>{t('bank.colHeard')}</span>
            <span>{t('bank.colSignoff')}</span>
            <span>{t('bank.colNext')}</span>
          </div>
          <div
            id="bankRows"
            ref={rowsRef}
            role="listbox"
            tabIndex={0}
            aria-label={t('bank.voices')}
            aria-activedescendant={currentSlug ? `v-${currentSlug}` : ''}
          >
            {!voices.length && <p className="bank-empty">{t('bank.notGenerated')}</p>}
            {voices.length > 0 && !shown.length && (
              <p className="bank-empty">{t('bank.nothingMatches')}</p>
            )}
            {shown.flatMap((v, i) => {
              const out = [];
              if (grouped && v.group !== group) {
                group = v.group;
                const mine = shown.filter((x) => x.group === v.group);
                out.push(
                  <h2 key={`g-${v.slug}`} className="bank-group">
                    <span>{v.group}</span>
                    <span className="n">{mine.length}</span>
                    <DistBar voices={mine} className="mini" />
                  </h2>,
                );
              }
              out.push(
                <BankRow
                  key={v.slug}
                  v={v}
                  voices={voices}
                  active={i === at}
                  ctx={ctx}
                  onSelect={select}
                />,
              );
              return out;
            })}
          </div>
        </div>
        <aside id="bankInspect" className="inspect" aria-live="polite">
          {current ? <InspectVoice v={current} voices={voices} ctx={ctx} /> : <Legend />}
        </aside>
      </div>
    </section>
  );
}
