import type { MidiFxConfig } from '../public_types_engine.js';

/**
 * Serialize a {@link MidiFxConfig} to the JSON document the native MIDI-FX
 * reader takes. The core refuses the document's content by key; only a config
 * that is not an object is refused here.
 *
 * @throws TypeError when `config` is not an object
 */
export function midiFxConfigJson(config: MidiFxConfig, fnName: string): string {
  if (typeof config !== 'object' || config === null || Array.isArray(config)) {
    throw new TypeError(`${fnName}: config must be an object`);
  }
  return JSON.stringify(config);
}
