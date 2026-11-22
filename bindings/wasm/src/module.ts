/**
 * Declaration of the `@libraz/libsonare/module` entry: the full build's module
 * factory, which an AudioWorklet processor hands to `init({ moduleFactory, wasmBinary })`
 * because that realm cannot fetch. The exports map points this entry's runtime
 * at the emscripten glue (`dist/sonare.js`) itself, so this file supplies only
 * its type and is never bundled.
 */

import type { ModuleFactory } from './public_types.js';
import createModule from './sonare.js';

/** Builds the full module from `wasmBinary`, the same factory `init()` loads by default. */
const moduleFactory: ModuleFactory = createModule as unknown as ModuleFactory;

export default moduleFactory;
