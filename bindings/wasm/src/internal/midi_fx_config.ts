import type { MidiFxConfig } from '../public_types_engine.js';
import { refuseType } from './entry.js';
import { requireListBudget, stringifyWithinBudget } from './list_budget.js';
import { isNonArrayObject } from './value_guards.js';

/**
 * Serialize a {@link MidiFxConfig} to the JSON document the native MIDI-FX
 * reader takes. The core refuses the document's content by key; only a config
 * that is not an object is refused here.
 *
 * @throws TypeError when `config` is not an object
 */
export function midiFxConfigJson(config: MidiFxConfig): string {
  if (!isNonArrayObject(config)) {
    throw refuseType(`config must be an object`);
  }
  for (const list of [config.chordIntervals, config.arpeggiatorIntervals]) {
    if (Array.isArray(list)) {
      requireListBudget(list, 'config');
    }
  }
  return stringifyWithinBudget(config, 'config');
}
