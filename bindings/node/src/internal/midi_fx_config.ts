import type { MidiFxConfig } from '../types_engine.js';
import { refuseType } from './entry.js';

/**
 * Serialize a {@link MidiFxConfig} to the JSON document the native MIDI-FX
 * reader takes. The core refuses the document's content by key; only a config
 * that is not an object is refused here.
 *
 * @throws TypeError when `config` is not an object
 */
export function midiFxConfigJson(config: MidiFxConfig): string {
  if (typeof config !== 'object' || config === null || Array.isArray(config)) {
    throw refuseType(`config must be an object`);
  }
  return JSON.stringify(config);
}
