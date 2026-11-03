/* Where the page's voice stands in the bank, carried into the header: its method, its rung, and
 * what is still unwritten. Without it a page says what a voice sounds like and nothing about why
 * it is open. Empty when the bank has no row for the set.
 */

import { stageIndex } from '../bank/logic';
import { useBank } from '../bank/store';
import { useT } from '../i18n';
import { useApp } from '../state/store';
import { MethodDot, StageBar } from './chips';
import { oneDecimal } from './identity';

export function HeadStage() {
  const { t, lang } = useT();
  const setId = useApp((s) => s.setId);
  const v = useBank((s) => s.voices.find((x) => x.slug === setId));
  if (!v) {
    return <span id="headStage" className="head-stage listen-only" />;
  }
  const open = v.open_candidates;
  return (
    <span id="headStage" className="head-stage listen-only" title={v.next}>
      <MethodDot engine={v.engine} />
      <span>{v.engine || t('bank.notReported')}</span>
      <StageBar stage={v.stage} />
      <span>{`${oneDecimal(v.stage, lang)} ${t(`stage.${stageIndex(v)}`)}`}</span>
      {open.length > 0 && (
        <span className="badge warn" title={open.join(', ')}>
          {t('bank.nUnwritten', { n: open.length })}
        </span>
      )}
    </span>
  );
}
