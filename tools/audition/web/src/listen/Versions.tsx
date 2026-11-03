/* The version switch, and the words and colours that say which side is sounding.
 *
 * A role owns a colour and a sentence: the banner names it, the block is
 * labelled with it, the button carries its dot. One block per QUESTION, keyed by
 * role and scope together — which recording is the target, which signal path,
 * which candidate are different questions, and eighteen buttons in one strip
 * answer none of them.
 */

import type { CSSProperties, ReactNode } from 'react';
import { recordPreference } from '../feedback/actions';
import { type TKey, useT } from '../i18n';
import {
  activeKey,
  blindLetter,
  blockOf,
  comparisons,
  roleClass,
  roleOf,
  scopeOf,
  selectedComparison,
  shipsOf,
  sourceLabel,
  sourceOf,
} from '../state/selectors';
import { type AppState, useApp } from '../state/store';
import { ShipChip } from '../voice/chips';
import { selectComparison, setVersion, statusWord, swapRole } from './actions';
import { slotsInRole } from './display';

/** Buttons past which a label stops fitting in a shared row, so the block wraps instead. */
const CROWDED = 6;

/** The unmodified build is not a recorded setting, so its two keys carry their words here. */
const BASELINE: Record<string, { title: TKey; desc: TKey }> = {
  model: { title: 'ver.baseline', desc: 'ver.baselineDesc' },
  'model-di': { title: 'ver.baselineDirect', desc: 'ver.baselineDirectDesc' },
};

type Translate = ReturnType<typeof useT>;

const roleWord = (t: Translate['t'], role: string): string => {
  switch (role) {
    case 'model':
      return t('role.model');
    case 'reference':
      return t('role.reference');
    case 'comparison':
      return t('role.comparison');
    default:
      return t('role.other');
  }
};

const roleLong = (t: Translate['t'], role: string): string => {
  switch (role) {
    case 'model':
      return t('role.model.long');
    case 'reference':
      return t('role.reference.long');
    case 'comparison':
      return t('role.comparison.long');
    default:
      return '';
  }
};

/** A version's title: the registered one, the baseline's, or a capture's own label. */
export function sourceTitle(s: AppState, key: string, { t, phrase }: Translate): string {
  const base = BASELINE[key];
  if (base) {
    return t(base.title);
  }
  const src = sourceOf(s, key);
  const title = phrase(src.title);
  if (title) {
    return title;
  }
  return roleOf(s, key) !== 'model' ? src.label || '' : '';
}

/* What a block asks: distance from the target where there is one, whether it
 * could pass for the instrument where there is none. A comparison capture only
 * says what it is. */
function hintKey(s: AppState, role: string, scope: string): TKey {
  if (s.blind) {
    return 'ver.hintBlind';
  }
  if (scope === 'instrument' && !selectedComparison(s)) {
    return 'ver.hintDirect';
  }
  if (role === 'reference') {
    return 'ver.hintReference';
  }
  if (role === 'comparison') {
    return 'ver.hintComparison';
  }
  if (!role) {
    return 'ver.hint';
  }
  const hasReference = s.display.some((slot) => roleOf(s, s.take?.keys[slot]) === 'reference');
  return hasReference ? 'ver.hintModel' : 'ver.hintModelAlone';
}

interface Block {
  role: string;
  scope: string;
  block: string;
  slots: number[];
}

/** The shown slots grouped into blocks, each one question. */
export function blocksOf(s: AppState): Block[] {
  const out: Block[] = [];
  for (const slot of s.display) {
    const key = s.blind ? '' : (s.take?.keys[slot] ?? '');
    const block = key ? blockOf(s, key) : '';
    const last = out[out.length - 1];
    if (last && last.block === block) {
      last.slots.push(slot);
    } else {
      out.push({
        role: key ? roleOf(s, key) : '',
        scope: key ? scopeOf(s, key) : '',
        block,
        slots: [slot],
      });
    }
  }
  return out;
}

function VersionButton({ slot, position }: { slot: number; position: number }) {
  const tr = useT();
  const { t, phrase } = tr;
  const s = useApp((x) => x);
  const item = s.items[s.itemIndex];
  const key = s.take?.keys[s.blind ? (s.blindOrder[slot] ?? -1) : slot] ?? '';
  let name = key;
  let sub = '';
  let tag: ReactNode = null;
  let title = '';
  if (s.blind) {
    const letter = blindLetter(s, slot);
    const pick = item ? s.picks[item.id] : undefined;
    name = pick?.revealed ? `${letter} · ${key}` : letter;
  } else {
    const named = sourceTitle(s, key, tr);
    if (named) {
      name = named;
      sub = roleOf(s, key) === 'model' ? key : '';
    }
    // A candidate is not what ships, and an unmodified render older than the bank is not either.
    if (roleOf(s, key) === 'model') {
      if (!BASELINE[key]) {
        tag = <span className="vtag">{t('ver.tagCandidate')}</span>;
      } else if (shipsOf(s, s.setId)?.state === 'stale') {
        tag = <span className="vtag warn">{t('ver.tagStale')}</span>;
      }
    }
    const src = sourceOf(s, key);
    title = [key, sourceTitle(s, key, tr), phrase(src.desc), src.label, src.detail]
      .filter(Boolean)
      .join('\n');
  }
  return (
    <button
      type="button"
      data-slot={slot}
      title={title}
      aria-pressed={slot === s.versionIndex}
      onClick={() => setVersion(slot, { play: true })}
    >
      <span className="key">{position + 1}</span>
      <span className="vtext">
        <span className="vname">{name}</span>
        {(sub || tag) && (
          <span className="vsub">
            {sub && <span className="vkey">{sub}</span>}
            {tag}
          </span>
        )}
      </span>
    </button>
  );
}

function BlockHead({ block }: { block: Block }) {
  const { t } = useT();
  const s = useApp((x) => x);
  const { role, scope } = block;
  const instrument = scope === 'instrument';
  const here = s.take && !s.blind ? blockOf(s, activeKey(s)) : null;
  const many = block.slots.length > 1;
  const hint = many ? t(hintKey(s, role, scope)) : '';
  // "Keep this one" records whatever is sounding, so it stands only in the block that is.
  const prefer = many && !s.blind && role !== 'reference' && role !== 'comparison';
  return (
    <div className="vhead">
      {role && (
        <span className="role" title={instrument ? t('scope.instrument.long') : roleLong(t, role)}>
          <span className="dot" />
          <span>{roleWord(t, role)}</span>
          {instrument && <span className="vpath">{t('scope.instrument')}</span>}
        </span>
      )}
      {hint && (
        <span className="vhint" title={hint}>
          {hint}
        </span>
      )}
      {prefer && (
        <button
          type="button"
          className="ghost"
          data-block={`${role}|${scope}`}
          title={t('fb.preferTitle')}
          hidden={here !== `${role}|${scope}`}
          onClick={() => void recordPreference()}
        >
          {t('fb.prefer')}
        </button>
      )}
    </div>
  );
}

/* The comparison selector, in the same row as the blocks it decides. Its
 * standing is shown blind or not; what it is compared against is hidden blind. */
function ComparisonRow() {
  const { t } = useT();
  const s = useApp((x) => x);
  const all = comparisons(s);
  const cur = selectedComparison(s);
  if (!all.length || !cur) {
    return null;
  }
  const refs = cur.oracle_sources.map((k) => sourceLabel(s, k)).join(', ');
  let line = t('compare.noReference');
  if (refs) {
    line = s.blind
      ? statusWord(cur.status)
      : `${statusWord(cur.status)} · ${t('compare.against', { ref: refs })}`;
  }
  return (
    <div className={`vrow ${roleClass('')}`} style={{ '--n': String(all.length) } as CSSProperties}>
      <div className="vhead">
        <span className="role" title={comparisonLong(t, cur.id)}>
          <span className="dot" />
          <span>{t('compare.label')}</span>
        </span>
        <span className="vhint" title={cur.reasons.join('\n')}>
          {line}
        </span>
      </div>
      {/* biome-ignore lint/a11y/useSemanticElements: a fieldset's border and legend fight the segmented layout. */}
      <div className="segmented" role="group" aria-label={t('compare.label')}>
        {all.map((c) => (
          <button
            key={c.id}
            type="button"
            aria-pressed={c.id === cur.id}
            title={[comparisonLong(t, c.id), ...c.reasons].join('\n')}
            onClick={() => selectComparison(c.id)}
          >
            <span className="vtext">
              <span className="vname">{comparisonName(t, c.id)}</span>
              <span className="vkey">{statusWord(c.status)}</span>
            </span>
          </button>
        ))}
      </div>
    </div>
  );
}

const COMPARISON_WORDS = {
  gm_gs_product: { name: 'compare.gm_gs_product', long: 'compare.gm_gs_product.long' },
  instrument_di: { name: 'compare.instrument_di', long: 'compare.instrument_di.long' },
} as const satisfies Record<string, { name: TKey; long: TKey }>;

const comparisonName = (t: Translate['t'], id: string): string =>
  id in COMPARISON_WORDS ? t(COMPARISON_WORDS[id as keyof typeof COMPARISON_WORDS].name) : id;

const comparisonLong = (t: Translate['t'], id: string): string =>
  id in COMPARISON_WORDS ? t(COMPARISON_WORDS[id as keyof typeof COMPARISON_WORDS].long) : '';

export function Versions() {
  const { t } = useT();
  const s = useApp((x) => x);
  if (!s.take) {
    return <div id="versions" className="versions compare-only" />;
  }
  let position = 0;
  return (
    <div id="versions" className="versions compare-only">
      <ComparisonRow />
      {blocksOf(s).map((block) => (
        <div
          key={`${block.block}:${block.slots[0]}`}
          className={`vrow ${roleClass(block.role)}`}
          style={{ '--n': String(block.slots.length) } as CSSProperties}
        >
          <BlockHead block={block} />
          {/* biome-ignore lint/a11y/useSemanticElements: a fieldset's border and legend fight the segmented layout. */}
          <div
            className={`segmented${block.slots.length > CROWDED ? ' many' : ''}`}
            role="group"
            aria-label={roleWord(t, block.role)}
          >
            {block.slots.map((slot) => (
              <VersionButton key={slot} slot={slot} position={position++} />
            ))}
          </div>
        </div>
      ))}
    </div>
  );
}

/* The banner: one line that answers "what am I listening to" in words — the
 * role in its colour, the registered title, and what it is for. */
export function NowLine() {
  const tr = useT();
  const { t, phrase } = tr;
  const s = useApp((x) => x);
  let banner: ReactNode = null;
  if (s.take && s.blind) {
    banner = (
      <div id="nowRole" className={`now-role ${roleClass('')}`}>
        <span className="dot" />
        <span className="now-what">{blindLetter(s, s.versionIndex)}</span>
        <span className="now-detail">{t('now.hidden')}</span>
      </div>
    );
  } else if (s.take) {
    const key = activeKey(s);
    const role = roleOf(s, key);
    const src = sourceOf(s, key);
    const title = sourceTitle(s, key, tr);
    const base = BASELINE[key];
    const desc = base ? t(base.desc) : phrase(src.desc);
    const detail = [src.label, src.detail].filter((x) => x && x !== key).join('  ·  ');
    banner = (
      <div id="nowRole" className={`now-role ${roleClass(role)}`}>
        <span className="dot" />
        <span className="now-what">{roleWord(t, role)}</span>
        {title && <span className="now-title">{title}</span>}
        <span className="now-detail">{desc || detail || sourceLabel(s, key)}</span>
        <NowStatus versionKey={key} role={role} />
      </div>
    );
  } else {
    banner = <div id="nowRole" className="now-role" />;
  }
  return (
    <div className="now">
      {banner}
      <SwapButton />
    </div>
  );
}

const NOW_IS = {
  candidate: { short: 'now.is.candidate', long: 'now.is.candidate.long' },
  target: { short: 'now.is.target', long: 'now.is.target.long' },
  context: { short: 'now.is.context', long: 'now.is.context.long' },
} as const satisfies Record<string, { short: TKey; long: TKey }>;

/** What the sounding version stands for: what ships, a candidate, the target, or context. */
function NowStatus({ versionKey, role }: { versionKey: string; role: string }) {
  const { t } = useT();
  const ships = useApp((x) => shipsOf(x, x.setId));
  if (role === 'model' && BASELINE[versionKey]) {
    return <ShipChip ships={ships} />;
  }
  let which: keyof typeof NOW_IS = 'context';
  if (role === 'model') {
    which = 'candidate';
  } else if (role === 'reference') {
    which = 'target';
  }
  return (
    <span className="idstate" data-state={which} title={t(NOW_IS[which].long)}>
      {t(NOW_IS[which].short)}
    </span>
  );
}

/** The other side of the comparison, offered, coloured for where it goes. */
export function SwapButton() {
  const { t } = useT();
  const s = useApp((x) => x);
  const has =
    Boolean(s.take) &&
    !s.blind &&
    slotsInRole(s, 'reference').length > 0 &&
    slotsInRole(s, 'model').length > 0;
  if (!has) {
    return <button id="swapBtn" type="button" className="swap compare-only" hidden />;
  }
  const atReference = roleOf(s, activeKey(s)) === 'reference';
  return (
    <button
      id="swapBtn"
      type="button"
      className={`swap compare-only ${roleClass(atReference ? 'model' : 'reference')}`}
      onClick={swapRole}
    >
      {atReference ? t('now.swapBack') : t('now.swap')}
    </button>
  );
}
