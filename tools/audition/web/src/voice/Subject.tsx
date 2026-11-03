/* What the page is about, as opposed to what is sounding: the voice and its place on the map,
 * what it is aimed at, and whether the render is the voice that ships. Three cells, because they
 * are three questions a listener asks before trusting anything heard. The target cell's product
 * half is withheld when blind, where naming the product would name the version.
 */

import { type ReactNode, useMemo } from 'react';
import type { Comparison, Manifest } from '../api/schemas';
import { type BankVoiceRow, Provenance } from '../bank/schema';
import { useBank } from '../bank/store';
import { useT } from '../i18n';
import { roleClass, selectedComparison, shipsOf } from '../state/selectors';
import { useApp } from '../state/store';
import { MethodTag, ShipChip, SlotTag, TargetStateChip } from './chips';
import { readShips, rerenderCommand, shipReasons, slotLong, targetWords, word } from './identity';

function Cell({ kind, label, children }: { kind: string; label: string; children: ReactNode }) {
  return (
    <section className={`idcell ${kind}`.trim()}>
      <h3 className="idhead">{label}</h3>
      {children}
    </section>
  );
}

type ManifestVoice = NonNullable<Manifest['voice']>;
type TakeNotes = { start: number; note: number }[] | undefined;

export function Subject() {
  const { t } = useT();
  const voice = useApp((s) => s.manifest?.voice);
  const rawProv = useApp((s) => s.manifest?.provenance);
  const setId = useApp((s) => s.setId);
  const blind = useApp((s) => s.blind);
  const comparison = useApp(selectedComparison);
  const ships = useApp((s) => shipsOf(s, s.setId));
  const item = useApp((s) => s.items[s.itemIndex]);
  const voices = useBank((s) => s.voices);

  const prov = useMemo(() => {
    const got = Provenance.safeParse(rawProv);
    return got.success ? got.data : null;
  }, [rawProv]);
  const row = voices.find((x) => x.slug === setId) ?? null;

  if (!voice) {
    return <div id="subject" className="subject" />;
  }

  const shipsNow = row?.ships ?? null;
  const detail = readShips(ships);
  const unitCount = detail?.units?.length ?? 0;
  const then = detail?.then ?? {};
  const thenUnit = Object.keys(then)[0];

  return (
    <div id="subject" className="subject">
      <Cell kind="" label={t('id.voice')}>
        <VoiceCell
          voice={voice}
          row={row}
          setId={setId}
          voices={voices}
          notes={item?.meta?.notes}
          drumNames={prov?.drum_names ?? {}}
        />
      </Cell>
      <Cell kind={`id-target ${roleClass('reference')}`} label={t('id.target')}>
        <TargetCell row={row} prov={prov} blind={blind} comparison={comparison} />
      </Cell>
      <Cell kind="" label={t('id.ships')}>
        <p className="idbig">
          <ShipChip ships={ships} />
        </p>
        {shipsNow ? (
          <p className="idline">
            {t('ship.shipping', { unit: shipsNow.unit, v: shipsNow.version, date: shipsNow.date })}
          </p>
        ) : (
          unitCount > 1 && <p className="idline">{t('ship.kitUnits', { n: unitCount })}</p>
        )}
        {detail?.rendered && (
          <p className="idline dim">
            {thenUnit && Object.keys(then).length === 1
              ? t('ship.renderedThen', {
                  unit: thenUnit,
                  v: then[thenUnit] ?? '',
                  date: detail.rendered,
                })
              : t('ship.rendered', { date: detail.rendered })}
            {detail.since.length > 0 && (
              <span className="idsince" title={shipReasons(detail)}>
                {t('ship.since', { n: detail.since.length })}
              </span>
            )}
          </p>
        )}
        {detail?.state === 'stale' && (
          <code className="idcmd" title={t('ship.rerenderTitle')}>
            {rerenderCommand(voice)}
          </code>
        )}
      </Cell>
    </div>
  );
}

/** The distinct drum notes this take strikes, in the order struck, named where the map names them. */
function struckInstruments(
  notes: TakeNotes,
  names: Record<string, string>,
  noteLabel: (key: string) => string,
): string[] {
  if (!notes?.length) {
    return [];
  }
  const seen: string[] = [];
  for (const n of [...notes].sort((a, b) => a.start - b.start)) {
    const key = String(n.note);
    if (!seen.includes(key)) {
      seen.push(key);
    }
  }
  return seen.map((key) => (names[key] ? `${names[key]} ${key}` : noteLabel(key)));
}

function VoiceCell({
  voice,
  row,
  setId,
  voices,
  notes,
  drumNames,
}: {
  voice: ManifestVoice;
  row: BankVoiceRow | null;
  setId: string | null;
  voices: BankVoiceRow[];
  notes: TakeNotes;
  drumNames: Record<string, string>;
}) {
  const { t } = useT();
  // Both numbers: the printed map counts from one and every MIDI file from zero.
  const program = voice.program ?? 0;
  const addr = voice.kit
    ? [t('subj.kit', { n: program }), t('subj.channel10')]
    : [
        t('subj.gm', { n: program + 1 }),
        t('subj.program', { n: program }),
        t('subj.bank', { n: voice.bank || 0 }),
      ];
  const slot = row ?? voice;
  const engine = row?.engine ?? '';
  // A kit is one part holding forty-odd instruments; the take decides which are struck.
  const struck = voice.kit
    ? struckInstruments(notes, drumNames, (k) => t('subj.note', { n: k }))
    : [];
  return (
    <>
      <p className="idname">
        {voice.name || setId || ''}
        <span className="idaddr">{addr.join(' · ')}</span>
      </p>
      <p className="idtags">
        <SlotTag voice={slot} voices={voices} />
        <span className="idlong">{slotLong(slot, voices)}</span>
      </p>
      <p className="idtags">
        <MethodTag engine={engine} />
        <span className="idmono">
          {[
            engine || t('bank.notReported'),
            voice.patch ? t('subj.patch', { name: voice.patch }) : t('subj.noPatch'),
          ].join(' · ')}
        </span>
      </p>
      {struck.length > 0 && (
        <p className="idtags">
          <span className="lab">{t('subj.struck')}</span>
          <span className="idmono">{struck.join(' · ')}</span>
        </p>
      )}
    </>
  );
}

/** The room a capture carries. `room` is measured and `dry` only says whether an effect section
 * existed to switch off, so a capture that answered the first question is read on it. */
function roomKey(cap: { room?: unknown; dry?: boolean | null }) {
  if (cap.room === 'none') {
    return 'room.none';
  }
  if (cap.room) {
    return 'room.present';
  }
  return cap.dry ? 'room.dry' : 'room.undeclared';
}

function TargetCell({
  row,
  prov,
  blind,
  comparison,
}: {
  row: BankVoiceRow | null;
  prov: Provenance | null;
  blind: boolean;
  comparison: Comparison | null;
}) {
  const { t } = useT();
  const target = row?.target ?? (prov?.want ? { ...prov.want, state: prov.state } : null);
  if (!target?.branch) {
    return <p className="idline dim">{t('id.noPolicy')}</p>;
  }
  const shown = { ...target, state: prov?.state || target.state };
  const approx = prov?.approximated ?? row?.target?.approximated;
  const answers = prov?.answers ?? row?.target?.answers;
  return (
    <>
      <p className="idbig" title={prov?.want?.reason || ''}>
        {targetWords(target)}
      </p>
      <p className="idtags">
        {blind ? (
          <span className="idlong">{t('id.blindHidden')}</span>
        ) : (
          <>
            <TargetStateChip target={shown} />
            <CaptureLine prov={prov} comparison={comparison} />
          </>
        )}
      </p>
      {!blind && approx && (
        <p className="idline warn" title={approx.reason || ''}>
          {t('tgt.approximated', { slot: approx.answered_by })}
        </p>
      )}
      {!blind && answers && answers.length > 0 && (
        <p className="idline dim">{t('tgt.answers', { slots: answers.join(', ') })}</p>
      )}
    </>
  );
}

/** What answers the aim: the capture, or why a comparison or the policy has none. */
function CaptureLine({
  prov,
  comparison,
}: {
  prov: Provenance | null;
  comparison: Comparison | null;
}) {
  const { t } = useT();
  // The capture answers only comparisons that list it; one with no oracle must not name another's.
  const cap = comparison && !comparison.oracle_sources.length ? null : prov?.capture;
  if (cap) {
    const line = [
      word(`src.${cap.source_class || 'unclassified'}`, cap.source_class || ''),
      cap.product,
      t(roomKey(cap)),
    ].filter(Boolean);
    return <span className="idlong">{line.join(' · ')}</span>;
  }
  if (comparison && prov?.capture) {
    return <span className="idlong">{t('compare.noReference')}</span>;
  }
  const d = prov?.declined;
  if (d) {
    return (
      <span className="idlong" title={d.reason || ''}>
        {d.names && d.carries
          ? t('subj.declined', { names: d.names, carries: d.carries })
          : t('subj.declinedBare')}
      </span>
    );
  }
  return null;
}
