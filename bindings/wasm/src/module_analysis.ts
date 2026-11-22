/**
 * Declaration of the `@libraz/libsonare/module/analysis` entry: the analysis-only
 * build's module factory, for `init({ moduleFactory, wasmBinary })` from
 * `@libraz/libsonare/analysis` where the realm cannot fetch. The exports map points
 * this entry's runtime at the emscripten glue (`dist/sonare-analysis.js`) itself, so
 * this file supplies only its type and is never bundled.
 */

import type { ModuleFactory } from './public_types.js';
import createModule from './sonare-analysis.js';

/** Builds the analysis-only module from `wasmBinary`, the same factory its `init()` loads by default. */
const moduleFactory: ModuleFactory = createModule as unknown as ModuleFactory;

export default moduleFactory;
