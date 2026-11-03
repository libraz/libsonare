/* The small repeated readouts shared by the bank, the palette and the listening surface: a dot
 * for the method, a bar for the stage, chips for what the ear and the ledger have said.
 */

import type { CSSProperties } from 'react';
import type { Ships } from '../api/schemas';
import { stageIndex } from '../bank/logic';
import type { BankVoiceRow, ShipsView, Target } from '../bank/schema';
import { useT } from '../i18n';
import { useApp } from '../state/store';
import { methodOf, methodTag, type SlotRef, shipTag, slotTag, targetState, word } from './identity';

export const stageColor = (i: number): string => `var(--s${i})`;

export function MethodDot({ engine }: { engine: string | null | undefined }) {
  return <span className={`dot ${methodOf(engine)}`} />;
}

/** Five segments filled to the voice's rung, in that rung's colour. */
export function StageBar({ stage }: { stage: number }) {
  const filled = stageIndex({ stage });
  return (
    <span className="bar" style={{ '--sc': stageColor(filled) } as CSSProperties}>
      {[0, 1, 2, 3, 4].map((i) => (
        <span key={i} className={i < filled ? 'seg on' : 'seg'} />
      ))}
    </span>
  );
}

const gradeWord = (name: string): string =>
  word(name === 'wrong-instrument' ? 'grade.wrongInstrument' : `grade.${name}`, name);

/* What the ear has said about a voice, as one chip. Empty where nothing has been said, which is
 * most of the bank and the point: a list whose every row claims a status says nothing. */
export function HeardChip({ id }: { id: string }) {
  const { t } = useT();
  const got = useApp((s) => s.feedbackIndex[id]);
  if (!got) {
    return <span className="heard" />;
  }
  let tone = '';
  if (got.worst === 'broken' || got.worst === 'wrong-instrument') {
    tone = ' bad';
  } else if (got.worst === 'acceptable' || got.worst === 'off') {
    tone = ' mid';
  } else if (got.worst === 'ok') {
    tone = ' good';
  }
  const said = [t('pick.notes', { count: got.n })];
  if (got.worst) {
    said.push(gradeWord(got.worst));
  }
  if (got.prefer) {
    said.push(t('pick.prefer', { n: got.prefer }));
  }
  return (
    <span className={`heard on${tone}`} title={said.join(' · ')}>
      {got.n}
    </span>
  );
}

/* The two claims nothing on disk implies: that the residual was diagnosed, and that somebody
 * listened and said it is the instrument. An empty column is the bank's own answer. */
export function SignoffChip({ voice }: { voice: BankVoiceRow | null | undefined }) {
  const { t } = useT();
  return (
    <span className="signoff">
      {voice &&
        (['structure', 'music'] as const).map((which) => {
          const state = voice.axes?.[which]?.state;
          if (!state) {
            return null;
          }
          let tone = 'mid';
          if (state === 'current') {
            tone = 'good';
          } else if (state === 'stale') {
            tone = 'bad';
          }
          return (
            <span
              key={which}
              className={`sg ${tone}`}
              title={`${t(`pick.${which}.long`)} — ${word(`pick.${state}`, state)}`}
            >
              {t(`pick.${which}`)}
            </span>
          );
        })}
    </span>
  );
}

/** Whether the page plays today's voice. */
export function ShipChip({
  ships,
  short = false,
}: {
  ships: Ships | ShipsView | null | undefined;
  short?: boolean;
}) {
  useT();
  const tag = shipTag(ships, short);
  return (
    <span className="idstate" data-state={tag.key} title={tag.title}>
      {tag.text}
    </span>
  );
}

/** Which part of the GM/GS map a slot belongs to. */
export function SlotTag({
  voice,
  voices,
  short = false,
}: {
  voice: SlotRef | null | undefined;
  voices: SlotRef[];
  short?: boolean;
}) {
  useT();
  const tag = slotTag(voice, voices, short);
  return (
    <span className="idtag" data-slot={tag.key} title={tag.title}>
      {tag.text}
    </span>
  );
}

/** How a voice is synthesised. */
export function MethodTag({
  engine,
  short = false,
}: {
  engine: string | null | undefined;
  short?: boolean;
}) {
  useT();
  const tag = methodTag(engine, short);
  return (
    <span className="idtag" data-method={tag.key} title={tag.title}>
      {tag.text}
    </span>
  );
}

/** How the captured reference stands against the slot's aim; nothing without a state. */
export function TargetStateChip({ target }: { target: Target | null | undefined }) {
  useT();
  const tag = targetState(target);
  if (!tag) {
    return null;
  }
  return (
    <span className="idstate" data-state={tag.key} title={tag.title}>
      {tag.text}
    </span>
  );
}
